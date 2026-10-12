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


/*
 * An MQTT 3.1.1 client over a WebSocket, behind the part of libmosquitto's
 * interface that src/mqtt.c uses (see mosquitto.h for how it differs).
 *
 * The browser delivers the WebSocket's events whenever the page's thread is
 * handed back to it, which is between the FreeRTOS tasks' turns. The handlers
 * below only record what happened: the bytes received, that the socket opened
 * or closed. Everything else - parsing the packets, the callbacks into
 * mqtt.c, the keepalive - happens in mosquitto_loop(), on the task that pumps
 * the client, just as with libmosquitto.
 */

#if CONFIG_TARGET_WEB

#include "mosquitto.h"
#include <emscripten.h>
#include <emscripten/websocket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


// MQTT control packet types, in the high nibble of the first byte
#define PKT_CONNECT		0x10
#define PKT_CONNACK		0x20
#define PKT_PUBLISH		0x30
#define PKT_PUBACK		0x40
#define PKT_SUBSCRIBE	0x82
#define PKT_SUBACK		0x90
#define PKT_PINGREQ		0xC0
#define PKT_PINGRESP	0xD0
#define PKT_DISCONNECT	0xE0

// What mqtt.c's connect callback is given when the socket closes before the
// broker has answered the CONNECT. No CONNACK code is this large, so
// mosquitto_connack_string() can tell it apart.
#define CONNACK_UNREACHABLE		256

// Largest packet accepted from the broker. Fleet traffic is at most a few
// kilobytes; anything near this is a broken stream.
#define RX_PACKET_MAX			(1024U * 1024U)


typedef enum {
	STATE_IDLE = 0,
	// the WebSocket is opening
	STATE_WS_CONNECTING,
	// CONNECT sent, waiting for the CONNACK
	STATE_MQTT_CONNECTING,
	STATE_CONNECTED,
	STATE_CLOSED
} state_e;


struct mosquitto {
	char id[128];
	char user[256];
	char pass[256];
	bool have_user;
	void *obj;
	void (*on_connect)(struct mosquitto *, void *, int);
	void (*on_disconnect)(struct mosquitto *, void *, int);
	void (*on_message)(struct mosquitto *, void *,
			const struct mosquitto_message *);

	EMSCRIPTEN_WEBSOCKET_T ws;
	state_e state;
	uint16_t keepalive_s;
	uint16_t next_mid;
	double last_tx_ms;
	double ping_sent_ms;
	bool ping_outstanding;
	// we closed it ourselves, so its closing is no news
	bool closing;

	// set by the WebSocket's event handlers, taken by mosquitto_loop()
	volatile bool ev_open;
	volatile bool ev_closed;

	// the byte stream received, not yet parsed
	uint8_t *rx;
	uint32_t rx_len;
	uint32_t rx_cap;
};


// ---- WebSocket events --------------------------------------------------------

static bool ws_on_open(int type, const EmscriptenWebSocketOpenEvent *e,
		void *user) {
	(void) type;
	(void) e;
	struct mosquitto *m = user;
	m->ev_open = true;
	return true;
}


static bool ws_on_message(int type, const EmscriptenWebSocketMessageEvent *e,
		void *user) {
	(void) type;
	struct mosquitto *m = user;
	if (!e->isText && (e->numBytes > 0U)) {
		if ((m->rx_len + e->numBytes) > m->rx_cap) {
			uint32_t cap = (m->rx_cap != 0U) ? m->rx_cap : 4096U;
			while (cap < (m->rx_len + e->numBytes)) {
				cap *= 2U;
			}
			uint8_t *p = realloc(m->rx, cap);
			if (p != NULL) {
				m->rx = p;
				m->rx_cap = cap;
			}
		}
		if ((m->rx_len + e->numBytes) <= m->rx_cap) {
			memcpy(&m->rx[m->rx_len], e->data, e->numBytes);
			m->rx_len += e->numBytes;
		}
	}
	return true;
}


static bool ws_on_close(int type, const EmscriptenWebSocketCloseEvent *e,
		void *user) {
	(void) type;
	(void) e;
	struct mosquitto *m = user;
	m->ev_closed = true;
	return true;
}


static bool ws_on_error(int type, const EmscriptenWebSocketErrorEvent *e,
		void *user) {
	(void) type;
	(void) e;
	// an error is always followed by a close, which is what is acted on
	(void) user;
	return true;
}


// ---- packets -----------------------------------------------------------------

/// @brief: Writes the MQTT variable length encoding of *len* into *dst*.
/// Returns the number of bytes written.
static uint32_t put_remaining_len(uint8_t *dst, uint32_t len) {
	uint32_t n = 0;
	do {
		uint8_t b = (uint8_t) (len % 128U);
		len /= 128U;
		if (len > 0U) {
			b |= 0x80U;
		}
		dst[n++] = b;
	} while (len > 0U);
	return n;
}


static uint32_t put_str(uint8_t *dst, const char *s, uint32_t len) {
	dst[0] = (uint8_t) (len >> 8);
	dst[1] = (uint8_t) len;
	memcpy(&dst[2], s, len);
	return len + 2U;
}


/// @brief: Sends a packet whose header byte is *header* and whose rest is the
/// *body_len* bytes of *body*.
static int send_packet(struct mosquitto *m, uint8_t header, const uint8_t *body,
		uint32_t body_len) {
	int ret = MOSQ_ERR_NOMEM;
	uint8_t *pkt = malloc(body_len + 5U);
	if (pkt != NULL) {
		uint32_t n = 0;
		pkt[n++] = header;
		n += put_remaining_len(&pkt[n], body_len);
		if (body_len > 0U) {
			memcpy(&pkt[n], body, body_len);
		}
		n += body_len;
		if (emscripten_websocket_send_binary(m->ws, pkt, n) ==
				EMSCRIPTEN_RESULT_SUCCESS) {
			m->last_tx_ms = emscripten_get_now();
			ret = MOSQ_ERR_SUCCESS;
		}
		else {
			ret = MOSQ_ERR_CONN_LOST;
		}
		free(pkt);
	}
	return ret;
}


static int send_connect(struct mosquitto *m) {
	uint32_t id_len = (uint32_t) strlen(m->id);
	uint32_t user_len = (uint32_t) strlen(m->user);
	uint32_t pass_len = (uint32_t) strlen(m->pass);
	uint8_t body[10 + 2 + sizeof(m->id) + 2 + sizeof(m->user) + 2 + sizeof(m->pass)];
	uint32_t n = 0;
	n += put_str(&body[n], "MQTT", 4);
	// protocol level 4 is MQTT 3.1.1
	body[n++] = 4;
	uint8_t flags = 0x02;	// clean session
	if (m->have_user) {
		flags |= 0x80;
		if (pass_len > 0U) {
			flags |= 0x40;
		}
	}
	body[n++] = flags;
	body[n++] = (uint8_t) (m->keepalive_s >> 8);
	body[n++] = (uint8_t) m->keepalive_s;
	n += put_str(&body[n], m->id, id_len);
	if (m->have_user) {
		n += put_str(&body[n], m->user, user_len);
		if (pass_len > 0U) {
			n += put_str(&body[n], m->pass, pass_len);
		}
	}
	return send_packet(m, PKT_CONNECT, body, n);
}


/// @brief: Forgets the socket. Its events stop with it.
static void ws_drop(struct mosquitto *m) {
	if (m->ws > 0) {
		(void) emscripten_websocket_close(m->ws, 1000, "");
		(void) emscripten_websocket_delete(m->ws);
		m->ws = 0;
	}
	m->rx_len = 0;
	m->ev_open = false;
	m->ev_closed = false;
	m->ping_outstanding = false;
}


/// @brief: Handles one complete packet: *type* is its first byte, *p* the
/// *len* bytes after the remaining length.
static void handle_packet(struct mosquitto *m, uint8_t type, const uint8_t *p,
		uint32_t len) {
	switch (type & 0xF0U) {
	case PKT_CONNACK:
		if ((m->state == STATE_MQTT_CONNECTING) && (len >= 2U)) {
			int rc = p[1];
			if (rc == 0) {
				m->state = STATE_CONNECTED;
			}
			else {
				// the broker closes the connection after a refusal
				m->closing = true;
				m->state = STATE_CLOSED;
			}
			if (m->on_connect != NULL) {
				m->on_connect(m, m->obj, rc);
			}
		}
		break;
	case PKT_PUBLISH:
		if (len >= 2U) {
			uint32_t topic_len = ((uint32_t) p[0] << 8) | p[1];
			uint32_t qos = (type >> 1) & 0x03U;
			uint32_t pos = 2U + topic_len;
			uint16_t mid = 0;
			if ((qos > 0U) && ((pos + 2U) <= len)) {
				mid = (uint16_t) (((uint16_t) p[pos] << 8) | p[pos + 1U]);
				pos += 2U;
			}
			if (pos <= len) {
				char *topic = malloc(topic_len + 1U);
				if (topic != NULL) {
					memcpy(topic, &p[2], topic_len);
					topic[topic_len] = '\0';
					struct mosquitto_message msg = {
							.mid = mid,
							.topic = topic,
							.payload = (void *) &p[pos],
							.payloadlen = (int) (len - pos),
							.qos = (int) qos,
							.retain = ((type & 0x01U) != 0U)
					};
					if (m->on_message != NULL) {
						m->on_message(m, m->obj, &msg);
					}
					free(topic);
				}
				if (qos == 1U) {
					uint8_t ack[2] = { (uint8_t) (mid >> 8), (uint8_t) mid };
					(void) send_packet(m, PKT_PUBACK, ack, sizeof(ack));
				}
			}
		}
		break;
	case PKT_PINGRESP:
		m->ping_outstanding = false;
		break;
	default:
		// SUBACK, and anything a QoS 0 client does not ask for
		break;
	}
}


/// @brief: Parses complete packets off the front of the receive buffer, up to
/// *max* of them. Returns false when the stream cannot be parsed.
static bool parse_rx(struct mosquitto *m, int max) {
	bool ret = true;
	uint32_t pos = 0;
	int count = 0;
	while (ret && (count < max) && (m->state != STATE_CLOSED)) {
		if ((m->rx_len - pos) < 2U) {
			break;
		}
		uint32_t len = 0;
		uint32_t mult = 1;
		uint32_t i = pos + 1U;
		bool complete_len = false;
		while ((i < m->rx_len) && (i < (pos + 5U))) {
			uint8_t b = m->rx[i++];
			len += (uint32_t) (b & 0x7FU) * mult;
			mult *= 128U;
			if ((b & 0x80U) == 0U) {
				complete_len = true;
				break;
			}
		}
		if (!complete_len) {
			if ((i - pos) >= 5U) {
				ret = false;
			}
			break;
		}
		else if (len > RX_PACKET_MAX) {
			ret = false;
		}
		else if ((m->rx_len - i) < len) {
			// the rest of this packet has not arrived yet
			break;
		}
		else {
			handle_packet(m, m->rx[pos], &m->rx[i], len);
			pos = i + len;
			count++;
		}
	}
	if (pos > 0U) {
		memmove(m->rx, &m->rx[pos], m->rx_len - pos);
		m->rx_len -= pos;
	}
	return ret;
}


// ---- interface ---------------------------------------------------------------

int mosquitto_lib_init(void) {
	return MOSQ_ERR_SUCCESS;
}


int mosquitto_lib_cleanup(void) {
	return MOSQ_ERR_SUCCESS;
}


struct mosquitto *mosquitto_new(const char *id, bool clean_session, void *obj) {
	(void) clean_session;
	struct mosquitto *m = calloc(1, sizeof(struct mosquitto));
	if (m != NULL) {
		uint32_t rnd = 0;
		if (getentropy(&rnd, sizeof(rnd)) != 0) {
			rnd = (uint32_t) emscripten_get_now();
		}
		snprintf(m->id, sizeof(m->id), "%.100s_web%08x",
				(id != NULL) ? id : "uvcan", (unsigned int) rnd);
		m->obj = obj;
		m->state = STATE_IDLE;
		m->next_mid = 1;
	}
	return m;
}


void mosquitto_destroy(struct mosquitto *mosq) {
	if (mosq != NULL) {
		ws_drop(mosq);
		free(mosq->rx);
		// the password does not outlive the client in the heap
		memset(mosq->pass, 0, sizeof(mosq->pass));
		free(mosq);
	}
}


int mosquitto_username_pw_set(struct mosquitto *mosq, const char *username,
		const char *password) {
	int ret = MOSQ_ERR_INVAL;
	if (mosq != NULL) {
		snprintf(mosq->user, sizeof(mosq->user), "%s",
				(username != NULL) ? username : "");
		snprintf(mosq->pass, sizeof(mosq->pass), "%s",
				(password != NULL) ? password : "");
		mosq->have_user = (username != NULL);
		ret = MOSQ_ERR_SUCCESS;
	}
	return ret;
}


int mosquitto_tls_set(struct mosquitto *mosq, const char *cafile,
		const char *capath, const char *certfile, const char *keyfile,
		int (*pw_callback)(char *buf, int size, int rwflag, void *userdata)) {
	(void) cafile;
	(void) capath;
	(void) certfile;
	(void) keyfile;
	(void) pw_callback;
	return (mosq != NULL) ? MOSQ_ERR_SUCCESS : MOSQ_ERR_INVAL;
}


void mosquitto_connect_callback_set(struct mosquitto *mosq,
		void (*on_connect)(struct mosquitto *, void *, int)) {
	mosq->on_connect = on_connect;
}


void mosquitto_disconnect_callback_set(struct mosquitto *mosq,
		void (*on_disconnect)(struct mosquitto *, void *, int)) {
	mosq->on_disconnect = on_disconnect;
}


void mosquitto_message_callback_set(struct mosquitto *mosq,
		void (*on_message)(struct mosquitto *, void *,
				const struct mosquitto_message *)) {
	mosq->on_message = on_message;
}


int mosquitto_connect_async(struct mosquitto *mosq, const char *host, int port,
		int keepalive) {
	(void) port;
	int ret = MOSQ_ERR_INVAL;
	if ((mosq != NULL) && (host != NULL) && (host[0] != '\0')) {
		ws_drop(mosq);
		char url[512];
		if (strstr(host, "://") != NULL) {
			snprintf(url, sizeof(url), "%s", host);
		}
		else {
			snprintf(url, sizeof(url), "wss://%s/mqtt", host);
		}
		EmscriptenWebSocketCreateAttributes attr;
		emscripten_websocket_init_create_attributes(&attr);
		attr.url = url;
		// the subprotocol a broker's WebSocket listener asks for
		attr.protocols = "mqtt";
		attr.createOnMainThread = true;
		EMSCRIPTEN_WEBSOCKET_T ws = emscripten_websocket_new(&attr);
		if (ws <= 0) {
			ret = MOSQ_ERR_NO_CONN;
		}
		else {
			mosq->ws = ws;
			mosq->keepalive_s = (uint16_t) keepalive;
			mosq->closing = false;
			mosq->state = STATE_WS_CONNECTING;
			emscripten_websocket_set_onopen_callback(ws, mosq, &ws_on_open);
			emscripten_websocket_set_onmessage_callback(ws, mosq, &ws_on_message);
			emscripten_websocket_set_onclose_callback(ws, mosq, &ws_on_close);
			emscripten_websocket_set_onerror_callback(ws, mosq, &ws_on_error);
			ret = MOSQ_ERR_SUCCESS;
		}
	}
	return ret;
}


int mosquitto_disconnect(struct mosquitto *mosq) {
	int ret = MOSQ_ERR_NO_CONN;
	if ((mosq != NULL) && (mosq->ws > 0)) {
		if (mosq->state == STATE_CONNECTED) {
			(void) send_packet(mosq, PKT_DISCONNECT, NULL, 0);
		}
		mosq->closing = true;
		ws_drop(mosq);
		mosq->state = STATE_IDLE;
		ret = MOSQ_ERR_SUCCESS;
	}
	return ret;
}


int mosquitto_loop(struct mosquitto *mosq, int timeout, int max_packets) {
	(void) timeout;
	int ret = MOSQ_ERR_SUCCESS;

	if ((mosq == NULL) || (mosq->ws <= 0)) {
		ret = MOSQ_ERR_NO_CONN;
	}
	else {
		if (mosq->ev_open && (mosq->state == STATE_WS_CONNECTING)) {
			mosq->ev_open = false;
			mosq->state = STATE_MQTT_CONNECTING;
			ret = send_connect(mosq);
		}

		if ((ret == MOSQ_ERR_SUCCESS) &&
				!parse_rx(mosq, (max_packets > 0) ? max_packets : 1)) {
			ret = MOSQ_ERR_PROTOCOL;
		}

		// keepalive: ping halfway through the period, and give up on a broker
		// which has not answered within a whole one
		double now = emscripten_get_now();
		double period = (double) mosq->keepalive_s * 1000.0;
		if ((ret == MOSQ_ERR_SUCCESS) && (mosq->state == STATE_CONNECTED) &&
				(period > 0.0)) {
			if (mosq->ping_outstanding) {
				if ((now - mosq->ping_sent_ms) > period) {
					ret = MOSQ_ERR_CONN_LOST;
				}
			}
			else if ((now - mosq->last_tx_ms) >= (period / 2.0)) {
				ret = send_packet(mosq, PKT_PINGREQ, NULL, 0);
				mosq->ping_outstanding = true;
				mosq->ping_sent_ms = now;
			}
			else {
			}
		}

		if ((ret == MOSQ_ERR_SUCCESS) && mosq->ev_closed) {
			ret = MOSQ_ERR_CONN_LOST;
		}

		if (ret != MOSQ_ERR_SUCCESS) {
			state_e was = mosq->state;
			bool expected = mosq->closing;
			ws_drop(mosq);
			mosq->state = STATE_CLOSED;
			if (expected) {
				// a refused CONNECT, already reported through the connect
				// callback
				ret = MOSQ_ERR_NO_CONN;
			}
			else if ((was == STATE_WS_CONNECTING) || (was == STATE_MQTT_CONNECTING)) {
				// the broker was never reached: report it the way a refused
				// connection is, with a reason of its own
				if (mosq->on_connect != NULL) {
					mosq->on_connect(mosq, mosq->obj, CONNACK_UNREACHABLE);
				}
				ret = MOSQ_ERR_NO_CONN;
			}
			else if (mosq->on_disconnect != NULL) {
				mosq->on_disconnect(mosq, mosq->obj, ret);
			}
			else {
			}
		}
	}
	return ret;
}


int mosquitto_publish(struct mosquitto *mosq, int *mid, const char *topic,
		int payloadlen, const void *payload, int qos, bool retain) {
	int ret = MOSQ_ERR_NO_CONN;
	if ((mosq != NULL) && (mosq->state == STATE_CONNECTED) && (topic != NULL)) {
		uint32_t topic_len = (uint32_t) strlen(topic);
		uint32_t len = topic_len + 2U + (uint32_t) payloadlen;
		uint8_t *body = malloc(len);
		if (body == NULL) {
			ret = MOSQ_ERR_NOMEM;
		}
		else {
			(void) put_str(body, topic, topic_len);
			if (payloadlen > 0) {
				memcpy(&body[topic_len + 2U], payload, (size_t) payloadlen);
			}
			// QoS 0 only: no packet id
			(void) qos;
			ret = send_packet(mosq, (uint8_t) (PKT_PUBLISH | (retain ? 1U : 0U)),
					body, len);
			free(body);
			if (mid != NULL) {
				*mid = 0;
			}
		}
	}
	return ret;
}


int mosquitto_subscribe(struct mosquitto *mosq, int *mid, const char *sub,
		int qos) {
	int ret = MOSQ_ERR_NO_CONN;
	(void) qos;
	if ((mosq != NULL) && (mosq->state == STATE_CONNECTED) && (sub != NULL)) {
		uint32_t sub_len = (uint32_t) strlen(sub);
		uint8_t *body = malloc(sub_len + 5U);
		if (body == NULL) {
			ret = MOSQ_ERR_NOMEM;
		}
		else {
			uint16_t id = mosq->next_mid++;
			if (mosq->next_mid == 0U) {
				mosq->next_mid = 1U;
			}
			body[0] = (uint8_t) (id >> 8);
			body[1] = (uint8_t) id;
			(void) put_str(&body[2], sub, sub_len);
			body[sub_len + 4U] = 0;		// QoS 0
			ret = send_packet(mosq, PKT_SUBSCRIBE, body, sub_len + 5U);
			free(body);
			if (mid != NULL) {
				*mid = id;
			}
		}
	}
	return ret;
}


const char *mosquitto_strerror(int mosq_errno) {
	const char *ret;
	switch (mosq_errno) {
	case MOSQ_ERR_SUCCESS:
		ret = "No error.";
		break;
	case MOSQ_ERR_NOMEM:
		ret = "Out of memory.";
		break;
	case MOSQ_ERR_PROTOCOL:
		ret = "A network protocol error occurred when communicating with the broker.";
		break;
	case MOSQ_ERR_INVAL:
		ret = "Invalid function arguments provided.";
		break;
	case MOSQ_ERR_NO_CONN:
		ret = "The client is not currently connected.";
		break;
	case MOSQ_ERR_CONN_LOST:
		ret = "The connection was lost.";
		break;
	default:
		ret = "Unknown error.";
		break;
	}
	return ret;
}


const char *mosquitto_connack_string(int connack_code) {
	const char *ret;
	switch (connack_code) {
	case 0:
		ret = "Connection Accepted.";
		break;
	case 1:
		ret = "Connection Refused: unacceptable protocol version.";
		break;
	case 2:
		ret = "Connection Refused: identifier rejected.";
		break;
	case 3:
		ret = "Connection Refused: broker unavailable.";
		break;
	case 4:
		ret = "Connection Refused: bad user name or password.";
		break;
	case 5:
		ret = "Connection Refused: not authorised.";
		break;
	case CONNACK_UNREACHABLE:
		ret = "The broker could not be reached.";
		break;
	default:
		ret = "Connection Refused: unknown reason.";
		break;
	}
	return ret;
}

#endif
