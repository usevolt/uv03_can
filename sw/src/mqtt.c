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
#include "uv_remote_stream.h"

#define TOPIC_ROOT			"fleets/"
#define TOPIC_ROOT_LEN		(sizeof(TOPIC_ROOT) - 1)

// Keepalive sent to the broker, in seconds.
#define KEEPALIVE_S			30

// How many times a connect interrupted by a signal is retried before it is
// reported as a failure (see mqtt_connect_task()).
#define EINTR_RETRIES		5


// Largest mirrored UI frame we will reassemble. The device drops any frame that
// does not fit its own 4 kB encoder buffer, so anything beyond that is a
// desynchronised stream rather than a real frame.
#define UI_FRAME_MAX		8192


// One device (an MQTT client id) seen inside a fleet.
typedef struct {
	char name[MQTT_NAME_MAX];
	uint32_t msg_count;
	time_t last_seen;

	// what the device says about itself in its heartbeat
	char devname[MQTT_NAME_MAX];
	uint32_t uptime_s;
	uint8_t features;
	uint8_t dev_state;

	// REMOTE messages arrive on this device's own to_admin topic, so each
	// device needs its own framer: the streams are independent.
	remote_stream_st rx;

	// geometry of the mirrored display, from REMOTE_MSG_TYPE_UI_INFO
	uint16_t ui_w;
	uint16_t ui_h;
	bool ui_size_known;

	// Mirrored UI frame being reassembled from its chunks. Allocated only while
	// a device is actually being mirrored - one buffer per known device would
	// be megabytes for a fleet that is merely listed.
	bool ui_active;
	uint8_t *ui_buf;
	uint32_t ui_len;
	bool ui_dropping;
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

// Sink for reassembled mirrored UI frames.
static mqtt_ui_frame_callb_t ui_frame_callb;
static void *ui_frame_user;


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



// --- the device heartbeat ---------------------------------------------------
//
// {"n":"UV0D Display","id":"aa:bb:cc:dd:ee:ff","up":25,"en":3,"st":2}
//
// Flat, ours, and only ever these keys, so a full JSON parser would be more
// machinery than the payload deserves. These two helpers are deliberately
// strict: anything unexpected simply does not match and the field is left
// alone, rather than half-parsed.

/// @brief: Extracts a quoted string value for *key*. Returns false when the key
/// is absent or the value is not a string.
static bool json_get_str(const char *payload, const char *key,
		char *out, size_t outlen) {
	bool ret = false;
	char pattern[32];
	snprintf(pattern, sizeof(pattern), "\"%s\":", key);
	const char *p = strstr(payload, pattern);
	if (p != NULL) {
		p += strlen(pattern);
		if (*p == '"') {
			p++;
			const char *end = strchr(p, '"');
			if (end != NULL) {
				size_t len = (size_t) (end - p);
				if (len > outlen - 1) {
					len = outlen - 1;
				}
				memcpy(out, p, len);
				out[len] = '\0';
				ret = true;
			}
		}
	}
	return ret;
}


/// @brief: Extracts an unsigned number value for *key*.
static bool json_get_uint(const char *payload, const char *key,
		uint32_t *out) {
	bool ret = false;
	char pattern[32];
	snprintf(pattern, sizeof(pattern), "\"%s\":", key);
	const char *p = strstr(payload, pattern);
	if (p != NULL) {
		p += strlen(pattern);
		char *end = NULL;
		unsigned long v = strtoul(p, &end, 10);
		if (end != p) {
			*out = (uint32_t) v;
			ret = true;
		}
	}
	return ret;
}


/// @brief: Finds a device by fleet and client id, or NULL.
static mqtt_dev_st *dev_find(const char *fleet, const char *dev) {
	mqtt_dev_st *ret = NULL;
	for (uint8_t i = 0; (i < fleet_count) && (ret == NULL); i++) {
		if (strcmp(fleets[i].name, fleet) == 0) {
			for (uint8_t j = 0; j < fleets[i].dev_count; j++) {
				if (strcmp(fleets[i].devs[j].name, dev) == 0) {
					ret = &fleets[i].devs[j];
					break;
				}
			}
		}
	}
	return ret;
}


/// @brief: Handles a heartbeat published on a fleet's announce topic. The
/// announce *topic* does not say who published, so the client id in the payload
/// is what puts the device on the list - without this an idle device, which
/// only heartbeats, would never appear at all.
static void announce_parse(const char *fleet, const char *payload,
		uint16_t len) {
	char buf[512];
	uint16_t n = (len < sizeof(buf) - 1) ? len : (uint16_t) (sizeof(buf) - 1);
	memcpy(buf, payload, n);
	buf[n] = '\0';

	char id[MQTT_NAME_MAX];
	if (json_get_str(buf, "id", id, sizeof(id))) {
		dev_seen(fleet, id);
		mqtt_dev_st *d = dev_find(fleet, id);
		if (d != NULL) {
			char name[MQTT_NAME_MAX];
			if (json_get_str(buf, "n", name, sizeof(name))) {
				if (strcmp(d->devname, name) != 0) {
					strncpy(d->devname, name, sizeof(d->devname) - 1);
					d->devname[sizeof(d->devname) - 1] = '\0';
					changed = true;
				}
			}
			uint32_t v;
			if (json_get_uint(buf, "up", &v)) {
				d->uptime_s = v;
			}
			if (json_get_uint(buf, "en", &v)) {
				d->features = (uint8_t) v;
			}
			if (json_get_uint(buf, "st", &v)) {
				d->dev_state = (uint8_t) v;
			}
		}
	}
	else {
		// not one of ours, or an older device that announces something else
	}
}


// --- REMOTE messages on a device's to_admin topic ---------------------------

/// @brief: Appends a mirrored UI chunk to the frame being reassembled, and
/// hands the finished frame to the sink on FRAME_END.
static void ui_chunk(mqtt_dev_st *d, const uint8_t *msg, uint8_t len,
		uint8_t fleet_index, uint8_t dev_index) {
	uint8_t chunk_len = msg[2];
	uint8_t flags = msg[3];
	const uint8_t *cmds = &msg[4];
	if (chunk_len > (uint8_t) (len - 4)) {
		chunk_len = (uint8_t) (len - 4);
	}

	if ((flags & REMOTE_UI_FLAG_FRAME_START) != 0) {
		// a new frame supersedes whatever was half-collected
		d->ui_len = 0;
		d->ui_dropping = false;
	}

	if (d->ui_buf == NULL) {
		// not mirroring this device
	}
	else if (d->ui_dropping) {
		// already over the limit; keep discarding until the next frame start
	}
	else if ((d->ui_len + chunk_len) > UI_FRAME_MAX) {
		// The device never sends a frame this large, so the stream is out of
		// step. Drop the rest of it rather than render garbage.
		d->ui_dropping = true;
		d->ui_len = 0;
	}
	else {
		memcpy(&d->ui_buf[d->ui_len], cmds, chunk_len);
		d->ui_len += chunk_len;

		if (((flags & REMOTE_UI_FLAG_FRAME_END) != 0) &&
				(ui_frame_callb != NULL) &&
				(d->ui_len > 0)) {
			ui_frame_callb(fleet_index, dev_index, d->ui_buf, d->ui_len,
					ui_frame_user);
			d->ui_len = 0;
		}
	}
}


// Which device the framer is currently decoding for, so the callback can find
// it again. The frame callback carries a void* but the decode is strictly
// sequential inside on_message, so a small context struct is enough.
typedef struct {
	mqtt_dev_st *dev;
	uint8_t fleet_index;
	uint8_t dev_index;
} rx_ctx_st;


/// @brief: Handles one complete REMOTE message from a device.
static void dev_frame_callb(void *user, remote_msg_types_e type,
		const uint8_t *data, uint8_t len) {
	rx_ctx_st *ctx = user;
	mqtt_dev_st *d = ctx->dev;

	switch (type) {
	case REMOTE_MSG_TYPE_IOT_STATUS:
		if (d->features != data[2]) {
			d->features = data[2];
			changed = true;
		}
		d->dev_state = data[3];
		break;

	case REMOTE_MSG_TYPE_UI_INFO:
	{
		uint16_t w;
		uint16_t h;
		memcpy(&w, &data[2], sizeof(w));
		memcpy(&h, &data[4], sizeof(h));
		if ((w != d->ui_w) || (h != d->ui_h) || !d->ui_size_known) {
			d->ui_w = w;
			d->ui_h = h;
			d->ui_size_known = true;
			changed = true;
			printf("MQTT: device '%s' mirrors a %ux%u display\n",
					d->name, (unsigned int) w, (unsigned int) h);
			fflush(stdout);
		}
		break;
	}

	case REMOTE_MSG_TYPE_UI:
		ui_chunk(d, data, len, ctx->fleet_index, ctx->dev_index);
		break;

	default:
		// CAN traffic and the rest are not consumed by the fleet view yet
		break;
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

		// beyond discovery, the payload matters: the announce topic carries the
		// heartbeat that names the device, and to_admin carries REMOTE messages
		if (strncmp(msg->topic, TOPIC_ROOT, TOPIC_ROOT_LEN) == 0) {
			char fleet[MQTT_NAME_MAX];
			const char *rest = level_copy(msg->topic + TOPIC_ROOT_LEN,
					fleet, sizeof(fleet));
			if (rest != NULL) {
				char what[32];
				const char *rest2 = level_copy(rest, what, sizeof(what));
				if ((strcmp(what, "announce") == 0) &&
						(msg->payload != NULL)) {
					announce_parse(fleet, (const char *) msg->payload,
							(uint16_t) msg->payloadlen);
				}
				else if ((strcmp(what, "clients") == 0) &&
						(rest2 != NULL) &&
						(msg->payload != NULL)) {
					char dev[MQTT_NAME_MAX];
					const char *rest3 = level_copy(rest2, dev, sizeof(dev));
					char leaf[32];
					if (rest3 != NULL) {
						(void) level_copy(rest3, leaf, sizeof(leaf));
					}
					else {
						leaf[0] = '\0';
					}
					if (strcmp(leaf, "to_admin") == 0) {
						rx_ctx_st ctx = { NULL, 0, 0 };
						for (uint8_t i = 0; i < fleet_count; i++) {
							if (strcmp(fleets[i].name, fleet) != 0) {
								continue;
							}
							for (uint8_t j = 0; j < fleets[i].dev_count; j++) {
								if (strcmp(fleets[i].devs[j].name, dev) == 0) {
									ctx.dev = &fleets[i].devs[j];
									ctx.fleet_index = i;
									ctx.dev_index = j;
									break;
								}
							}
						}
						if (ctx.dev != NULL) {
							remote_stream_feed(&ctx.dev->rx,
									(const uint8_t *) msg->payload,
									(uint16_t) msg->payloadlen,
									&dev_frame_callb, &ctx);
						}
					}
				}
				else {
					// another topic under this fleet
				}
			}
		}
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


// --- device detail and control ----------------------------------------------

/// @brief: Bounds-checked lookup used by every getter below.
static mqtt_dev_st *dev_at(uint8_t fleet_index, uint8_t dev_index) {
	mqtt_dev_st *ret = NULL;
	if ((fleet_index < fleet_count) &&
			(dev_index < fleets[fleet_index].dev_count)) {
		ret = &fleets[fleet_index].devs[dev_index];
	}
	return ret;
}


const char *mqtt_get_dev_devname(uint8_t fleet_index, uint8_t dev_index) {
	mqtt_dev_st *d = dev_at(fleet_index, dev_index);
	return (d != NULL) ? d->devname : "";
}


uint32_t mqtt_get_dev_uptime_s(uint8_t fleet_index, uint8_t dev_index) {
	mqtt_dev_st *d = dev_at(fleet_index, dev_index);
	return (d != NULL) ? d->uptime_s : 0;
}


uint8_t mqtt_get_dev_features(uint8_t fleet_index, uint8_t dev_index) {
	mqtt_dev_st *d = dev_at(fleet_index, dev_index);
	return (d != NULL) ? d->features : 0;
}


uint8_t mqtt_get_dev_state(uint8_t fleet_index, uint8_t dev_index) {
	mqtt_dev_st *d = dev_at(fleet_index, dev_index);
	return (d != NULL) ? d->dev_state : 0;
}


bool mqtt_get_dev_ui_size(uint8_t fleet_index, uint8_t dev_index,
		uint16_t *width, uint16_t *height) {
	mqtt_dev_st *d = dev_at(fleet_index, dev_index);
	bool ret = false;
	if ((d != NULL) && d->ui_size_known) {
		if (width != NULL) {
			*width = d->ui_w;
		}
		if (height != NULL) {
			*height = d->ui_h;
		}
		ret = true;
	}
	return ret;
}


bool mqtt_dev_set_features(uint8_t fleet_index, uint8_t dev_index,
		uint8_t features) {
	mqtt_dev_st *d = dev_at(fleet_index, dev_index);
	bool ret = false;
	if ((d != NULL) && (mosq != NULL) && (state == MQTT_STATE_CONNECTED)) {
		char topic[MQTT_NAME_MAX * 2 + 32];
		snprintf(topic, sizeof(topic), "%s%s/clients/%s/to_dev",
				TOPIC_ROOT, fleets[fleet_index].name, d->name);
		uint8_t frame[REMOTE_MSG_TYPE_IOT_CTRL_LEN] = {
				REMOTE_MSG_START_BYTE,
				REMOTE_MSG_TYPE_IOT_CTRL,
				features
		};
		int rc = mosquitto_publish(mosq, NULL, topic, (int) sizeof(frame),
				frame, 0, false);
		if (rc == MOSQ_ERR_SUCCESS) {
			ret = true;
		}
		else {
			printf("MQTT: could not ask '%s' for features 0x%x: %s\n",
					d->name, (unsigned int) features, mosquitto_strerror(rc));
			fflush(stdout);
		}
	}
	return ret;
}


void mqtt_set_ui_frame_callb(mqtt_ui_frame_callb_t callb, void *user) {
	ui_frame_callb = callb;
	ui_frame_user = user;
}


bool mqtt_dev_set_ui_active(uint8_t fleet_index, uint8_t dev_index,
		bool active) {
	mqtt_dev_st *d = dev_at(fleet_index, dev_index);
	bool ret = false;
	if (d != NULL) {
		if (active && (d->ui_buf == NULL)) {
			// only mirrored devices carry a reassembly buffer; one per known
			// device would be megabytes for a fleet that is merely listed
			d->ui_buf = malloc(UI_FRAME_MAX);
		}
		else {
		}
		if (active && (d->ui_buf == NULL)) {
			printf("MQTT: out of memory for the mirrored frame buffer\n");
			fflush(stdout);
		}
		else {
			d->ui_active = active;
			d->ui_len = 0;
			d->ui_dropping = false;
			if (!active) {
				free(d->ui_buf);
				d->ui_buf = NULL;
				d->ui_size_known = false;
			}
			else {
			}
			// ask the device to start or stop mirroring. The CAN feature is
			// left as it is: it is switched independently and nothing here owns
			// it.
			uint8_t mask = d->features;
            if (active) {
                mask |= REMOTE_IOT_FEATURE_UI;
            }
            else {
                mask &= (uint8_t) ~REMOTE_IOT_FEATURE_UI;
            }
			ret = mqtt_dev_set_features(fleet_index, dev_index, mask);
			changed = true;
		}
	}
	return ret;
}


bool mqtt_dev_get_ui_active(uint8_t fleet_index, uint8_t dev_index) {
	mqtt_dev_st *d = dev_at(fleet_index, dev_index);
	return (d != NULL) ? d->ui_active : false;
}
