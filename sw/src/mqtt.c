/*
 * This file is part of the uv_hal distribution (www.usevolt.fi).
 * Copyright (c) 2017 Usevolt Oy.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
*/


#include "mqtt.h"
#include "credentials.h"
#include <stdio.h>
#include <string.h>
#include <time.h>


// The real client is built on libmosquitto. That is always available on Linux; on
// Windows the apt mingw toolchain ships no libmosquitto (and no OpenSSL for its
// TLS backend), so the Fleet-tab connection is only compiled in once a mingw
// libmosquitto has been staged into thirdparty_win/ and the build opts in with
// -DMOSQUITTO_WIN (make win MOSQUITTO=1). Otherwise the Windows build gets the
// stub at the bottom of this file, so it still links.
#if !CONFIG_TARGET_WIN || defined(MOSQUITTO_WIN)
#define MQTT_ENABLED 1
#else
#define MQTT_ENABLED 0
#endif


#if MQTT_ENABLED

#include <mosquitto.h>
#include <uv_rtos.h>
#include <errno.h>
#if CONFIG_TARGET_WIN
#include <process.h>	// getpid()
#else
#include <unistd.h>		// getpid()
#endif
// The broker's private CA certificate, compiled in as a byte array
// (uvca_crt / uvca_crt_len). Generated from certs/uvca.crt by the makefile.
#include "cert_uvca.h"


// Topic prefix every fleet topic starts with.
#define TOPIC_ROOT			"fleets/"
#define TOPIC_ROOT_LEN		(sizeof(TOPIC_ROOT) - 1)

// Keepalive sent to the broker, in seconds.
#define KEEPALIVE_S			30

// How many times a connect interrupted by a signal is retried before it is
// reported as a failure (see mqtt_connect_task()).
#define EINTR_RETRIES		5


// One device (an MQTT client id) seen inside a fleet.
typedef struct {
	char name[MQTT_NAME_MAX];
	uint32_t msg_count;
	time_t last_seen;
} mqtt_dev_st;

// One fleet, with the devices seen in it.
typedef struct {
	char name[MQTT_NAME_MAX];
	mqtt_dev_st devs[MQTT_MAX_DEVS];
	uint8_t dev_count;
} mqtt_fleet_st;


static struct mosquitto *mosq;
static bool lib_inited;

// The client the connect task is working on. It owns *task_mosq* for the duration
// of the blocking mosquitto_connect(), so nothing else may destroy it meanwhile.
static struct mosquitto *task_mosq;
// Set when mqtt_disconnect() is called while that connect is still running: the
// task then throws its client away instead of handing it over. Needed because a
// connect to an unreachable host runs into the TCP timeout, which is far too long
// to keep the UI waiting for.
static volatile bool task_abandoned;

// Connection state. Written by the connect task as well as by the callbacks that
// run inside mqtt_step(), hence volatile.
static volatile mqtt_state_e state = MQTT_STATE_DISCONNECTED;
// True while the connect task is inside the blocking mosquitto_connect(); the
// client must not be pumped from the UI thread until it is done.
static volatile bool connecting_task_busy;
// Set by the connect task when mosquitto_connect() succeeded, so mqtt_step() may
// start pumping the client and waiting for the CONNACK.
static volatile bool connect_started;

static char err_str[256];
static char host_str[CREDENTIALS_MAX];
// Path of the CA certificate file written out for libmosquitto.
static char ca_path[1024];

// Fleet / device tree, built from the topics seen. Only touched from the UI
// thread (mqtt_step() runs the callbacks, the getters read them).
static mqtt_fleet_st fleets[MQTT_MAX_FLEETS];
static uint8_t fleet_count;

// Set whenever the state or the tree changed, cleared by mqtt_poll_changed().
static bool changed;


// Connection parameters, copied here so the connect task can use them after
// mqtt_connect() returned.
static char conn_user[CREDENTIALS_MAX];
static char conn_pass[CREDENTIALS_MAX];


bool mqtt_is_supported(void) {
	return true;
}


// Fills err_str with *msg*, logs it and moves to the error state.
static void mqtt_fail(const char *msg) {
	strncpy(err_str, msg, sizeof(err_str) - 1);
	err_str[sizeof(err_str) - 1] = '\0';
	state = MQTT_STATE_ERROR;
	changed = true;
	printf("MQTT: %s\n", err_str);
	fflush(stdout);
}


// Writes the compiled-in CA certificate next to the account file, so
// libmosquitto (which only takes file paths) can verify the broker. Rewritten on
// every connect, so a uvcan carrying a renewed CA replaces an older copy.
// Returns false - and fills err_str - when the file could not be written.
static bool mqtt_write_ca(void) {
	bool ret = false;
	if (!credentials_config_path(ca_path, sizeof(ca_path), "uvca.crt")) {
		mqtt_fail("No home directory for the CA certificate.");
	}
	else {
		FILE *f = fopen(ca_path, "wb");
		if (f == NULL) {
			mqtt_fail("Could not write the CA certificate.");
		}
		else {
			size_t wr = fwrite(uvca_crt, 1, (size_t) uvca_crt_len, f);
			fclose(f);
			if (wr != (size_t) uvca_crt_len) {
				mqtt_fail("Could not write the CA certificate.");
			}
			else {
				ret = true;
			}
		}
	}
	return ret;
}


/// @brief: Returns the fleet named *name*, adding it when it is not known yet.
/// Returns NULL when the fleet table is full or *name* is empty.
static mqtt_fleet_st *fleet_get(const char *name) {
	mqtt_fleet_st *ret = NULL;
	if (name[0] != '\0') {
		for (uint8_t i = 0; (i < fleet_count) && (ret == NULL); i++) {
			if (strcmp(fleets[i].name, name) == 0) {
				ret = &fleets[i];
			}
		}
		if ((ret == NULL) && (fleet_count < MQTT_MAX_FLEETS)) {
			ret = &fleets[fleet_count];
			memset(ret, 0, sizeof(*ret));
			strncpy(ret->name, name, sizeof(ret->name) - 1);
			ret->name[sizeof(ret->name) - 1] = '\0';
			fleet_count++;
			changed = true;
			printf("MQTT: discovered fleet '%s'\n", ret->name);
			fflush(stdout);
		}
	}
	return ret;
}


/// @brief: Records that device *dev* of fleet *fleet* was heard from, adding
/// either to the tree when they are new.
static void dev_seen(const char *fleet, const char *dev) {
	mqtt_fleet_st *f = fleet_get(fleet);
	if ((f != NULL) && (dev[0] != '\0')) {
		mqtt_dev_st *d = NULL;
		for (uint8_t i = 0; (i < f->dev_count) && (d == NULL); i++) {
			if (strcmp(f->devs[i].name, dev) == 0) {
				d = &f->devs[i];
			}
		}
		if ((d == NULL) && (f->dev_count < MQTT_MAX_DEVS)) {
			d = &f->devs[f->dev_count];
			memset(d, 0, sizeof(*d));
			strncpy(d->name, dev, sizeof(d->name) - 1);
			d->name[sizeof(d->name) - 1] = '\0';
			f->dev_count++;
			changed = true;
			printf("MQTT: device '%s' joined fleet '%s'\n", d->name, f->name);
			fflush(stdout);
		}
		if (d != NULL) {
			d->msg_count++;
			d->last_seen = time(NULL);
		}
	}
}


/// @brief: Copies the topic level starting at *src* (up to the next '/' or the
/// end of the string) into *out*, and returns a pointer to the character after
/// the separator, or NULL when this was the last level.
static const char *level_copy(const char *src, char *out, size_t outlen) {
	const char *slash = strchr(src, '/');
	size_t len = (slash != NULL) ? (size_t) (slash - src) : strlen(src);
	if (len > outlen - 1) {
		len = outlen - 1;
	}
	memcpy(out, src, len);
	out[len] = '\0';
	return (slash != NULL) ? (slash + 1) : NULL;
}


/// @brief: Turns a received topic into fleet / device entries. Understands
///   fleets/<fleet>/announce
///   fleets/<fleet>/clients/<client>/to_admin | to_dev
/// and ignores anything else.
static void topic_parse(const char *topic) {
	if (strncmp(topic, TOPIC_ROOT, TOPIC_ROOT_LEN) == 0) {
		char fleet[MQTT_NAME_MAX];
		const char *rest = level_copy(topic + TOPIC_ROOT_LEN, fleet, sizeof(fleet));
		if (rest != NULL) {
			char what[32];
			const char *rest2 = level_copy(rest, what, sizeof(what));
			if (strcmp(what, "announce") == 0) {
				// a device joining: the fleet exists, but the announce topic does
				// not name the device (the payload is a binary blob, not a name)
				(void) fleet_get(fleet);
			}
			else if ((strcmp(what, "clients") == 0) && (rest2 != NULL)) {
				char dev[MQTT_NAME_MAX];
				(void) level_copy(rest2, dev, sizeof(dev));
				dev_seen(fleet, dev);
			}
			else {
				// some other topic under this fleet: still tells us it exists
				(void) fleet_get(fleet);
			}
		}
		else {
			// "fleets/<something>" with no further level: nothing to record
		}
	}
	else {
		// not a fleet topic
	}
}


static void on_connect(struct mosquitto *m, void *obj, int rc) {
	(void) obj;
	if (rc == 0) {
		state = MQTT_STATE_CONNECTED;
		err_str[0] = '\0';
		changed = true;
		printf("MQTT: connected to %s as '%s', subscribing to the fleet topics\n",
				host_str, (conn_user[0] != '\0') ? conn_user : "(anonymous)");
		fflush(stdout);
		// Listen to everything the account is allowed to see. The broker's ACL
		// confines a fleet admin to their own fleet, so these wildcards resolve to
		// one fleet for an ordinary account and to all of them for `uvadmin`; a
		// denied subscription is reported per-topic and does not drop the session.
		(void) mosquitto_subscribe(m, NULL, "fleets/+/announce", 0);
		(void) mosquitto_subscribe(m, NULL, "fleets/+/clients/+/to_admin", 0);
	}
	else {
		// the broker refused the connection; rc 4/5 are bad credentials
		mqtt_fail((rc == 5) ? "Broker refused the connection: not authorised." :
				((rc == 4) ? "Broker refused the connection: wrong username or "
						"password." : mosquitto_connack_string(rc)));
	}
}


static void on_disconnect(struct mosquitto *m, void *obj, int rc) {
	(void) m;
	(void) obj;
	if (rc != 0) {
		// unexpected drop (network lost, broker restarted, kicked out)
		mqtt_fail("Connection to the broker was lost.");
	}
	else if (state != MQTT_STATE_DISCONNECTED) {
		state = MQTT_STATE_DISCONNECTED;
		changed = true;
	}
	else {
		// a disconnect we asked for, already reflected in the state
	}
}


static void on_message(struct mosquitto *m, void *obj,
		const struct mosquitto_message *msg) {
	(void) m;
	(void) obj;
	if ((msg != NULL) && (msg->topic != NULL)) {
		topic_parse(msg->topic);
	}
}


/// @brief: Task body performing the blocking part of the connect (DNS lookup, TCP
/// connect and the TLS handshake). Run on its own task so the UI stays live even
/// when the broker is unreachable and the attempt runs into its timeout.
static void mqtt_connect_task(void *ptr) {
	(void) ptr;
	int rc = MOSQ_ERR_SUCCESS;
	// The FreeRTOS POSIX port drives its scheduler with signals, so the blocking
	// connect() and TLS handshake inside libmosquitto get interrupted regularly.
	// That is not a connection failure - retry a few times before believing it.
	for (uint8_t try = 0; try < EINTR_RETRIES; try++) {
		rc = mosquitto_connect(task_mosq, host_str, MQTT_PORT, KEEPALIVE_S);
		if (!((rc == MOSQ_ERR_ERRNO) && (errno == EINTR))) {
			break;
		}
	}

	if (task_abandoned) {
		// the user disconnected (or changed the settings) while we were waiting:
		// this client was handed over to us, so throw it away here
		mosquitto_destroy(task_mosq);
		task_mosq = NULL;
	}
	else if (rc == MOSQ_ERR_SUCCESS) {
		// connected at the socket level; the CONNACK is picked up by mqtt_step()
		connect_started = true;
	}
	else {
		char msg[512];
		snprintf(msg, sizeof(msg), "Could not reach %s: %s", host_str,
				(rc == MOSQ_ERR_ERRNO) ? strerror(errno) : mosquitto_strerror(rc));
		mqtt_fail(msg);
	}
	connecting_task_busy = false;
}


bool mqtt_connect(const char *host, const char *username, const char *password,
		const char *fleet) {
	bool ret = false;

	mqtt_disconnect();

	strncpy(host_str, (host != NULL) ? host : "", sizeof(host_str) - 1);
	host_str[sizeof(host_str) - 1] = '\0';
	strncpy(conn_user, (username != NULL) ? username : "", sizeof(conn_user) - 1);
	conn_user[sizeof(conn_user) - 1] = '\0';
	strncpy(conn_pass, (password != NULL) ? password : "", sizeof(conn_pass) - 1);
	conn_pass[sizeof(conn_pass) - 1] = '\0';
	err_str[0] = '\0';

	if (connecting_task_busy) {
		// a previous attempt is still waiting for its connect to time out; it owns
		// the client and the connection parameters, so it has to finish first
		mqtt_fail("The previous connection attempt is still in progress.");
	}
	else if (host_str[0] == '\0') {
		mqtt_fail("No broker address set.");
	}
	else if (!mqtt_write_ca()) {
		// mqtt_write_ca() filled in the reason
	}
	else {
		if (!lib_inited) {
			mosquitto_lib_init();
			lib_inited = true;
		}
		// a unique-enough client id: the broker keys anonymous device permissions
		// off the client id, but an authenticated admin is scoped by its username
		char client_id[64];
		snprintf(client_id, sizeof(client_id), "uvcan_%d", (int) getpid());
		mosq = mosquitto_new(client_id, true, NULL);
		if (mosq == NULL) {
			mqtt_fail("Out of memory starting the MQTT client.");
		}
		else {
			int rc = MOSQ_ERR_SUCCESS;
			if (conn_user[0] != '\0') {
				rc = mosquitto_username_pw_set(mosq, conn_user, conn_pass);
			}
			if (rc == MOSQ_ERR_SUCCESS) {
				// verify the broker against the compiled-in private CA; the
				// certificate's CN must match the address being dialled
				rc = mosquitto_tls_set(mosq, ca_path, NULL, NULL, NULL, NULL);
			}
			if (rc != MOSQ_ERR_SUCCESS) {
				mqtt_fail(mosquitto_strerror(rc));
				mosquitto_destroy(mosq);
				mosq = NULL;
			}
			else {
				mosquitto_connect_callback_set(mosq, &on_connect);
				mosquitto_disconnect_callback_set(mosq, &on_disconnect);
				mosquitto_message_callback_set(mosq, &on_message);

				// the fleet the user named is listed straight away, so its tab is
				// there before any of its devices has published anything
				if ((fleet != NULL) && (fleet[0] != '\0')) {
					(void) fleet_get(fleet);
				}

				printf("MQTT: connecting to %s:%d as '%s'...\n", host_str, MQTT_PORT,
						(conn_user[0] != '\0') ? conn_user : "(anonymous)");
				fflush(stdout);
				state = MQTT_STATE_CONNECTING;
				changed = true;
				connect_started = false;
				task_abandoned = false;
				task_mosq = mosq;
				connecting_task_busy = true;
				uv_rtos_task_create(&mqtt_connect_task, "mqtt_connect",
						UV_RTOS_MIN_STACK_SIZE * 5, NULL,
						UV_RTOS_IDLE_PRIORITY + 1, NULL);
				ret = true;
			}
		}
	}
	return ret;
}


void mqtt_disconnect(void) {
	if ((state == MQTT_STATE_CONNECTED) || (state == MQTT_STATE_CONNECTING)) {
		printf("MQTT: disconnecting from %s\n", host_str);
		fflush(stdout);
	}
	if (connecting_task_busy) {
		// a connect is still in flight and the task owns the client: hand it over
		// to be destroyed there rather than blocking here until it times out
		task_abandoned = true;
		mosq = NULL;
	}
	else if (mosq != NULL) {
		(void) mosquitto_disconnect(mosq);
		mosquitto_destroy(mosq);
		mosq = NULL;
	}
	else {
		// nothing to close
	}
	connect_started = false;
	if (state != MQTT_STATE_DISCONNECTED) {
		state = MQTT_STATE_DISCONNECTED;
		changed = true;
	}
	if (fleet_count != 0) {
		fleet_count = 0;
		changed = true;
	}
	memset(fleets, 0, sizeof(fleets));
	err_str[0] = '\0';
}


void mqtt_step(void) {
	// only pump once the connect task has handed the client over, and never while
	// it is still inside mosquitto_connect()
	if ((mosq != NULL) && connect_started && !connecting_task_busy) {
		int rc = mosquitto_loop(mosq, 0, 1);
		if ((rc != MOSQ_ERR_SUCCESS) && (state == MQTT_STATE_CONNECTED)) {
			// EINTR is expected here: the FreeRTOS POSIX port drives its scheduler
			// with signals, so the loop's select() is interrupted regularly. Only a
			// real transport failure ends the session.
			if (!((rc == MOSQ_ERR_ERRNO) && (errno == EINTR))) {
				mqtt_fail(mosquitto_strerror(rc));
			}
		}
	}
}


mqtt_state_e mqtt_get_state(void) {
	return state;
}


bool mqtt_is_connected(void) {
	return state == MQTT_STATE_CONNECTED;
}


const char *mqtt_get_error(void) {
	return err_str;
}


const char *mqtt_get_host(void) {
	return host_str;
}


uint8_t mqtt_get_fleet_count(void) {
	return fleet_count;
}


const char *mqtt_get_fleet_name(uint8_t index) {
	return (index < fleet_count) ? fleets[index].name : "";
}


uint8_t mqtt_get_dev_count(uint8_t fleet_index) {
	return (fleet_index < fleet_count) ? fleets[fleet_index].dev_count : 0;
}


const char *mqtt_get_dev_name(uint8_t fleet_index, uint8_t dev_index) {
	const char *ret = "";
	if ((fleet_index < fleet_count) &&
			(dev_index < fleets[fleet_index].dev_count)) {
		ret = fleets[fleet_index].devs[dev_index].name;
	}
	return ret;
}


uint32_t mqtt_get_dev_msg_count(uint8_t fleet_index, uint8_t dev_index) {
	uint32_t ret = 0;
	if ((fleet_index < fleet_count) &&
			(dev_index < fleets[fleet_index].dev_count)) {
		ret = fleets[fleet_index].devs[dev_index].msg_count;
	}
	return ret;
}


uint32_t mqtt_get_dev_age_s(uint8_t fleet_index, uint8_t dev_index) {
	uint32_t ret = 0;
	if ((fleet_index < fleet_count) &&
			(dev_index < fleets[fleet_index].dev_count)) {
		time_t seen = fleets[fleet_index].devs[dev_index].last_seen;
		if (seen != 0) {
			double d = difftime(time(NULL), seen);
			ret = (d > 0.0) ? (uint32_t) d : 0;
		}
	}
	return ret;
}


bool mqtt_poll_changed(void) {
	bool ret = changed;
	changed = false;
	return ret;
}


#else /* !MQTT_ENABLED */


// No libmosquitto in this build (Windows without a staged mingw libmosquitto):
// the Fleet tab's broker connection is unavailable. Every entry point is a stub
// so the UI compiles and can report the limitation.

bool mqtt_is_supported(void) {
	return false;
}

bool mqtt_connect(const char *host, const char *username, const char *password,
		const char *fleet) {
	(void) host;
	(void) username;
	(void) password;
	(void) fleet;
	return false;
}

void mqtt_disconnect(void) {
}

void mqtt_step(void) {
}

mqtt_state_e mqtt_get_state(void) {
	return MQTT_STATE_DISCONNECTED;
}

bool mqtt_is_connected(void) {
	return false;
}

const char *mqtt_get_error(void) {
	return "This build has no MQTT support (libmosquitto was not linked in).";
}

const char *mqtt_get_host(void) {
	return "";
}

uint8_t mqtt_get_fleet_count(void) {
	return 0;
}

const char *mqtt_get_fleet_name(uint8_t index) {
	(void) index;
	return "";
}

uint8_t mqtt_get_dev_count(uint8_t fleet_index) {
	(void) fleet_index;
	return 0;
}

const char *mqtt_get_dev_name(uint8_t fleet_index, uint8_t dev_index) {
	(void) fleet_index;
	(void) dev_index;
	return "";
}

uint32_t mqtt_get_dev_msg_count(uint8_t fleet_index, uint8_t dev_index) {
	(void) fleet_index;
	(void) dev_index;
	return 0;
}

uint32_t mqtt_get_dev_age_s(uint8_t fleet_index, uint8_t dev_index) {
	(void) fleet_index;
	(void) dev_index;
	return 0;
}

bool mqtt_poll_changed(void) {
	return false;
}


#endif
