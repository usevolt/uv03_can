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


#include "remotecan.h"

#include <stdio.h>
#include <string.h>
#include "mqtt.h"

#if CONFIG_TARGET_LINUX

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <linux/can.h>
#include <linux/can/raw.h>


// The one socket is deliberate. A raw CAN socket does not receive what it sends
// itself (CAN_RAW_RECV_OWN_MSGS is off unless asked for), so a frame written
// here on the device's behalf is not read back and sent to the device again. A
// second socket for reading would see exactly those frames, and the bridge
// would feed itself for as long as it was up.
static int sock = -1;

static bool active;
static int16_t bridged_fleet = -1;
static int16_t bridged_dev = -1;

static char err_str[256];

static uint32_t rx_count;
static uint32_t tx_count;
static uint32_t filtered_count;
static uint32_t error_count;

// Standard frames are what a Usevolt machine talks CANopen over, so they are
// carried unless asked otherwise; extended ones are opt-in (see remotecan.h).
static bool allow_std = true;
static bool allow_ext;

// How many steps between checks that the device is still actually forwarding.
// The step runs on the UI cycle (20 ms), so this is a couple of seconds.
#define REASSERT_STEPS		100
static uint16_t reassert_ticks;

static remotecan_filter_st filters[REMOTECAN_FILTER_MAX];
static uint8_t filter_count;
// a table someone set by hand, which the checkboxes' default must not throw
// away when the next bridge starts
static bool filters_custom;


/// @brief: Fills the filter table with what is wanted by default: every frame
/// of whichever types are allowed. A mask of zero is what "every id" is.
static void filters_set_default(void) {
	filter_count = 0;
	filters_custom = false;
	if (allow_std) {
		filters[filter_count].id = 0;
		filters[filter_count].mask = 0;
		filters[filter_count].type = CAN_STD;
		filter_count++;
	}
	else {
	}
	if (allow_ext) {
		filters[filter_count].id = 0;
		filters[filter_count].mask = 0;
		filters[filter_count].type = CAN_EXT;
		filter_count++;
	}
	else {
	}
	// An empty table carries nothing, in either direction — which is what
	// clearing both boxes asks for, so it is left empty rather than quietly
	// filled in with something.
}


/// @brief: True when a frame is one the filter table carries. The same test
/// both ways: what the device is asked to forward and what is forwarded to it
/// are the same set, so the bridge cannot be a one-way mirror by accident.
static bool filter_passes(uint32_t id, uv_can_msg_types_e type) {
	bool ret = false;
	for (uint8_t i = 0; (i < filter_count) && !ret; i++) {
		if ((filters[i].type == type) &&
				((id & filters[i].mask) == filters[i].id)) {
			ret = true;
		}
		else {
		}
	}
	return ret;
}


/// @brief: Tells the device what to forward: clear, then every filter, then
/// done. The device applies each one as it arrives and has no other way of
/// knowing when the set is complete.
static void send_filters(void) {
	if ((bridged_fleet >= 0) && (bridged_dev >= 0)) {
		(void) mqtt_dev_send_rxclear((uint8_t) bridged_fleet,
				(uint8_t) bridged_dev);
		for (uint8_t i = 0; i < filter_count; i++) {
			(void) mqtt_dev_send_rxconf((uint8_t) bridged_fleet,
					(uint8_t) bridged_dev,
					filters[i].id, filters[i].mask, filters[i].type);
		}
		(void) mqtt_dev_send_rxdone((uint8_t) bridged_fleet,
				(uint8_t) bridged_dev);
	}
	else {
	}
}


static void frame_to_msg(const struct can_frame *frame, uv_can_msg_st *msg) {
	if ((frame->can_id & CAN_EFF_FLAG) != 0) {
		msg->type = CAN_EXT;
		msg->id = frame->can_id & CAN_EFF_MASK;
	}
	else {
		msg->type = CAN_STD;
		msg->id = frame->can_id & CAN_SFF_MASK;
	}
	msg->data_length = frame->can_dlc;
	if (msg->data_length > 8u) {
		msg->data_length = 8u;
	}
	else {
	}
	memcpy(msg->data_8bit, frame->data, msg->data_length);
}


static void msg_to_frame(const uv_can_msg_st *msg, struct can_frame *frame) {
	memset(frame, 0, sizeof(*frame));
	frame->can_id = msg->id;
	if (msg->type == CAN_EXT) {
		frame->can_id |= CAN_EFF_FLAG;
	}
	else {
	}
	frame->can_dlc = msg->data_length;
	memcpy(frame->data, msg->data_8bit, msg->data_length);
}


/// @brief: A frame the device forwarded from its own bus. Goes onto the
/// interface, where anything listening to it can see it.
static void can_from_dev(uint8_t fleet_index, uint8_t dev_index,
		const uv_can_msg_st *msg, void *user) {
	(void) user;
	if (active &&
			(fleet_index == bridged_fleet) &&
			(dev_index == bridged_dev)) {
		if (!filter_passes(msg->id, msg->type)) {
			// the device was asked not to send these, so it is either mid
			// re-negotiation or one arrived from before the filter changed
			filtered_count++;
		}
		else {
			struct can_frame frame;
			msg_to_frame(msg, &frame);
			if (write(sock, &frame, sizeof(frame)) == (ssize_t) sizeof(frame)) {
				rx_count++;
			}
			else {
				error_count++;
			}
		}
	}
	else {
		// a device that is not the bridged one; it has nowhere to go
	}
}


bool remotecan_start(uint8_t fleet_index, uint8_t dev_index) {
	bool ret = false;
	remotecan_stop();
	err_str[0] = '\0';

	if (!uv_can_create_vcan(REMOTECAN_IFNAME, err_str, sizeof(err_str))) {
		// err_str already says what went wrong
	}
	else {
		sock = socket(PF_CAN, SOCK_RAW, CAN_RAW);
		if (sock < 0) {
			snprintf(err_str, sizeof(err_str),
					"Could not open a CAN socket: %s.", strerror(errno));
		}
		else {
			struct ifreq ifr;
			memset(&ifr, 0, sizeof(ifr));
			strncpy(ifr.ifr_name, REMOTECAN_IFNAME, sizeof(ifr.ifr_name) - 1);
			struct sockaddr_can addr;
			memset(&addr, 0, sizeof(addr));
			addr.can_family = AF_CAN;

			if (ioctl(sock, SIOCGIFINDEX, &ifr) < 0) {
				snprintf(err_str, sizeof(err_str),
						"'%s' disappeared before it could be opened: %s.",
						REMOTECAN_IFNAME, strerror(errno));
			}
			else {
				addr.can_ifindex = ifr.ifr_ifindex;
				if (bind(sock, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
					snprintf(err_str, sizeof(err_str),
							"Could not bind to '%s': %s.",
							REMOTECAN_IFNAME, strerror(errno));
				}
				else {
					// read must never block the UI loop; there is no thread
					// here, the socket is polled from remotecan_step()
					int flags = fcntl(sock, F_GETFL, 0);
					(void) fcntl(sock, F_SETFL, flags | O_NONBLOCK);
					ret = true;
				}
			}
		}
	}

	if (ret) {
		active = true;
		bridged_fleet = (int16_t) fleet_index;
		bridged_dev = (int16_t) dev_index;
		rx_count = 0;
		tx_count = 0;
		reassert_ticks = 0;
		filtered_count = 0;
		error_count = 0;
		if (!filters_custom) {
			filters_set_default();
		}
		else {
		}
		mqtt_set_can_callb(&can_from_dev, NULL);
		(void) mqtt_dev_set_can_active(fleet_index, dev_index, true);
		send_filters();
		printf("remote CAN: bridging '%s' to netdev %s\n",
				mqtt_get_dev_name(fleet_index, dev_index), REMOTECAN_IFNAME);
		fflush(stdout);
	}
	else {
		if (sock >= 0) {
			close(sock);
			sock = -1;
		}
		else {
		}
		// nothing is bridged, so an interface created a moment ago would only
		// be a bus that does not exist
		(void) uv_can_delete_vcan(REMOTECAN_IFNAME, NULL, 0);
		printf("remote CAN: %s\n", err_str);
		fflush(stdout);
	}
	return ret;
}


void remotecan_stop(void) {
	if (active) {
		active = false;
		mqtt_set_can_callb(NULL, NULL);
		if ((bridged_fleet >= 0) && (bridged_dev >= 0)) {
			(void) mqtt_dev_send_rxclear((uint8_t) bridged_fleet,
					(uint8_t) bridged_dev);
			(void) mqtt_dev_set_can_active((uint8_t) bridged_fleet,
					(uint8_t) bridged_dev, false);
		}
		else {
		}
		bridged_fleet = -1;
		bridged_dev = -1;
		if (sock >= 0) {
			close(sock);
			sock = -1;
		}
		else {
		}
		// The interface goes with the bridge. Left behind it would look like a
		// bus that is still reachable, and nothing would ever be heard on it.
		(void) uv_can_delete_vcan(REMOTECAN_IFNAME, NULL, 0);
		printf("remote CAN: bridge closed, netdev %s removed\n",
				REMOTECAN_IFNAME);
		fflush(stdout);
	}
	else {
	}
}


void remotecan_shutdown(void) {
	if (active || (sock >= 0)) {
		active = false;
		bridged_fleet = -1;
		bridged_dev = -1;
		if (sock >= 0) {
			close(sock);
			sock = -1;
		}
		else {
		}
		(void) uv_can_delete_vcan(REMOTECAN_IFNAME, NULL, 0);
	}
	else {
	}
}


bool remotecan_is_active(void) {
	return active;
}


bool remotecan_shows(uint8_t fleet_index, uint8_t dev_index) {
	return (active &&
			(fleet_index == bridged_fleet) &&
			(dev_index == bridged_dev));
}


void remotecan_step(void) {
	// A device grants nothing on a fresh broker session: every feature goes
	// back off whenever its link drops, by design, and it has no idea a bridge
	// is still open here. This end's intent is the standing one, so it is
	// re-asserted until the device reports CAN in effect again — without it a
	// blink of the device's connection leaves an interface that is up, empty
	// and silent, which is the worst of all worlds.
	if (active && (bridged_fleet >= 0)) {
		reassert_ticks++;
		if (reassert_ticks >= REASSERT_STEPS) {
			reassert_ticks = 0;
			if (!mqtt_dev_get_can_active((uint8_t) bridged_fleet,
					(uint8_t) bridged_dev)) {
				(void) mqtt_dev_set_can_active((uint8_t) bridged_fleet,
						(uint8_t) bridged_dev, true);
				send_filters();
			}
			else {
			}
		}
		else {
		}
	}
	else {
	}

	if (active && (sock >= 0)) {
		// Everything that has been written to the interface since the last
		// step, up to a bound: a flood on the interface must not keep the UI
		// loop in here.
		for (uint16_t i = 0; i < 256u; i++) {
			struct can_frame frame;
			ssize_t n = read(sock, &frame, sizeof(frame));
			if (n != (ssize_t) sizeof(frame)) {
				// nothing left (EAGAIN), or a frame this cannot use
				break;
			}
			else if (((frame.can_id & CAN_ERR_FLAG) != 0) ||
					((frame.can_id & CAN_RTR_FLAG) != 0)) {
				// error and remote-transmission frames say nothing a device
				// could put on its own bus on our behalf
			}
			else {
				uv_can_msg_st msg;
				frame_to_msg(&frame, &msg);
				if (!filter_passes(msg.id, msg.type)) {
					filtered_count++;
				}
				else if (mqtt_dev_send_can((uint8_t) bridged_fleet,
						(uint8_t) bridged_dev, &msg)) {
					tx_count++;
				}
				else {
					error_count++;
				}
			}
		}
	}
	else {
	}
}


const char *remotecan_get_ifname(void) {
	return active ? REMOTECAN_IFNAME : "";
}


const char *remotecan_get_error(void) {
	return err_str;
}


uint32_t remotecan_get_rx_count(void) {
	return rx_count;
}


uint32_t remotecan_get_tx_count(void) {
	return tx_count;
}


uint32_t remotecan_get_filtered_count(void) {
	return filtered_count;
}


uint32_t remotecan_get_error_count(void) {
	return error_count;
}


bool remotecan_get_allow_std(void) {
	return allow_std;
}


void remotecan_set_allow_std(bool value) {
	if (value != allow_std) {
		allow_std = value;
		// the table is regenerated rather than patched: a hand-made table is
		// the caller's to manage, and there is no such caller yet
		filters_set_default();
		if (active) {
			send_filters();
		}
		else {
		}
	}
	else {
	}
}


bool remotecan_get_allow_ext(void) {
	return allow_ext;
}


void remotecan_set_allow_ext(bool value) {
	if (value != allow_ext) {
		allow_ext = value;
		filters_set_default();
		if (active) {
			send_filters();
		}
		else {
		}
	}
	else {
	}
}


uint8_t remotecan_get_filter_count(void) {
	return filter_count;
}


const remotecan_filter_st *remotecan_get_filter(uint8_t index) {
	return (index < filter_count) ? &filters[index] : NULL;
}


void remotecan_set_filters(const remotecan_filter_st *value, uint8_t count) {
	if ((value == NULL) || (count == 0)) {
		filters_set_default();
	}
	else {
		if (count > REMOTECAN_FILTER_MAX) {
			count = REMOTECAN_FILTER_MAX;
		}
		else {
		}
		memcpy(filters, value, (size_t) count * sizeof(filters[0]));
		filter_count = count;
		filters_custom = true;
	}
	if (active) {
		send_filters();
	}
	else {
	}
}


#else /* !CONFIG_TARGET_LINUX */


// No SocketCAN, so there is no interface to put a remote bus on. Every entry
// point is a stub so the UI compiles and can report the limitation.

bool remotecan_start(uint8_t fleet_index, uint8_t dev_index) {
	(void) fleet_index;
	(void) dev_index;
	return false;
}

void remotecan_stop(void) {
}

void remotecan_shutdown(void) {
}

bool remotecan_is_active(void) {
	return false;
}

bool remotecan_shows(uint8_t fleet_index, uint8_t dev_index) {
	(void) fleet_index;
	(void) dev_index;
	return false;
}

void remotecan_step(void) {
}

const char *remotecan_get_ifname(void) {
	return "";
}

const char *remotecan_get_error(void) {
	return "Bridging a remote CAN bus needs SocketCAN, which this build has no "
			"access to.";
}

uint32_t remotecan_get_rx_count(void) {
	return 0;
}

uint32_t remotecan_get_tx_count(void) {
	return 0;
}

uint32_t remotecan_get_filtered_count(void) {
	return 0;
}

uint32_t remotecan_get_error_count(void) {
	return 0;
}

bool remotecan_get_allow_std(void) {
	return false;
}

void remotecan_set_allow_std(bool value) {
	(void) value;
}

bool remotecan_get_allow_ext(void) {
	return false;
}

void remotecan_set_allow_ext(bool value) {
	(void) value;
}

uint8_t remotecan_get_filter_count(void) {
	return 0;
}

const remotecan_filter_st *remotecan_get_filter(uint8_t index) {
	(void) index;
	return NULL;
}

void remotecan_set_filters(const remotecan_filter_st *value, uint8_t count) {
	(void) value;
	(void) count;
}


#endif
