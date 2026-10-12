/*
 * This file is part of the uv_hal distribution (www.usevolt.fi).
 * Copyright (c) 2026 Usevolt Oy.
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

#ifndef UVCAN_WEB_MOSQUITTO_H_
#define UVCAN_WEB_MOSQUITTO_H_


/// @file: The part of libmosquitto's interface src/mqtt.c uses, for the web
/// browser build, where it is implemented by src/web/mosquitto_web.c. A page
/// cannot open a TCP connection, so the client speaks MQTT 3.1.1 over a
/// WebSocket instead, to a broker listener for WebSockets.
///
/// What differs from libmosquitto:
/// * The *host* given to mosquitto_connect_async() is a WebSocket URL when it
///   has a scheme (ws:// or wss://). A bare host name is reached at
///   wss://<host>/mqtt. The port is not used: the URL says it.
/// * TLS is the browser's, against the certificates the browser trusts, so
///   mosquitto_tls_set() only checks its arguments.
/// * Only QoS 0 is published and subscribed to, which is all mqtt.c uses.
/// * The client id is made unique by appending a random suffix. A page has no
///   process id worth the name, and two browser tabs logged in with the same
///   id would keep throwing each other off the broker.

#include <stdbool.h>
#include <stdint.h>


enum mosq_err_t {
	MOSQ_ERR_SUCCESS = 0,
	MOSQ_ERR_NOMEM = 1,
	MOSQ_ERR_PROTOCOL = 2,
	MOSQ_ERR_INVAL = 3,
	MOSQ_ERR_NO_CONN = 4,
	MOSQ_ERR_CONN_REFUSED = 5,
	MOSQ_ERR_NOT_FOUND = 6,
	MOSQ_ERR_CONN_LOST = 7,
	MOSQ_ERR_TLS = 8,
	MOSQ_ERR_PAYLOAD_SIZE = 9,
	MOSQ_ERR_NOT_SUPPORTED = 10,
	MOSQ_ERR_AUTH = 11,
	MOSQ_ERR_ACL_DENIED = 12,
	MOSQ_ERR_UNKNOWN = 13,
	MOSQ_ERR_ERRNO = 14
};


struct mosquitto;

struct mosquitto_message {
	int mid;
	char *topic;
	void *payload;
	int payloadlen;
	int qos;
	bool retain;
};


int mosquitto_lib_init(void);

int mosquitto_lib_cleanup(void);

struct mosquitto *mosquitto_new(const char *id, bool clean_session, void *obj);

void mosquitto_destroy(struct mosquitto *mosq);

int mosquitto_username_pw_set(struct mosquitto *mosq, const char *username,
		const char *password);

int mosquitto_tls_set(struct mosquitto *mosq, const char *cafile,
		const char *capath, const char *certfile, const char *keyfile,
		int (*pw_callback)(char *buf, int size, int rwflag, void *userdata));

void mosquitto_connect_callback_set(struct mosquitto *mosq,
		void (*on_connect)(struct mosquitto *, void *, int));

void mosquitto_disconnect_callback_set(struct mosquitto *mosq,
		void (*on_disconnect)(struct mosquitto *, void *, int));

void mosquitto_message_callback_set(struct mosquitto *mosq,
		void (*on_message)(struct mosquitto *, void *,
				const struct mosquitto_message *));

int mosquitto_connect_async(struct mosquitto *mosq, const char *host, int port,
		int keepalive);

int mosquitto_disconnect(struct mosquitto *mosq);

/// @brief: Handles what has arrived and keeps the session alive. Never waits:
/// *timeout* is not used, and *max_packets* bounds the packets handled per call.
int mosquitto_loop(struct mosquitto *mosq, int timeout, int max_packets);

int mosquitto_publish(struct mosquitto *mosq, int *mid, const char *topic,
		int payloadlen, const void *payload, int qos, bool retain);

int mosquitto_subscribe(struct mosquitto *mosq, int *mid, const char *sub,
		int qos);

const char *mosquitto_strerror(int mosq_errno);

const char *mosquitto_connack_string(int connack_code);


#endif /* UVCAN_WEB_MOSQUITTO_H_ */
