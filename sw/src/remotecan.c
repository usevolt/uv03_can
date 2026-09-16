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
#include <uv_rtos.h>
#include <uv_remote_proto.h>
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
// Narrow the device's end to the SDO conversation and the frames that say the
// node is alive. Off by default: the bridge is a window onto a bus, and one
// that showed only half of it without being asked would be a trap.
static bool sdo_only;

// How often the bridge steps: reads what has been written to the netdev and
// hands it to the broker. Short on purpose. Every frame of an SDO conversation
// waits for this twice - once here, once for the reply - and at the 20 ms UI
// cycle this used to run on, that alone was most of the transfer's round trip.
#define REMOTECAN_STEP_MS	2

// How many steps between checks that the device is still actually forwarding;
// a couple of seconds, whatever the step period is.
#define REASSERT_STEPS		(2000 / REMOTECAN_STEP_MS)
static uint16_t reassert_ticks;

// The bridge's own task. Started with the first bridge and then left running:
// remotecan_step() does nothing while no bridge is up, and a task that outlives
// the bridge cannot be caught mid-step by one being torn down under it.
static bool task_started;

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


// defined with the other frame conversion below; the offload writes synthesised
// answers onto the netdev with it
static void msg_to_frame(const uv_can_msg_st *msg, struct can_frame *frame);


// ---- offloaded SDO transfers -----------------------------------------------
//
// An SDO transfer written to the netdev is a conversation: the client sends one
// frame and waits for its answer before sending the next. Forwarded frame by
// frame, every frame of it pays the link's latency - about a tenth of a second
// - so a transfer of a hundred bytes costs some twenty round trips to move a
// hundred bytes, and a parameter load takes minutes.
//
// So the bridge recognises the conversation instead of relaying it: it collects
// what the client is asking for, has the device run the whole transfer on its
// own bus (REMOTE_MSG_TYPE_SDO_REQ) and answers the client here, out of the
// result. One round trip per transfer, whatever the transfer.
//
// It is done at this level, rather than in uvcan's own SDO client, so that
// everything speaking SocketCAN to the interface gets it: candump and cansend,
// a second uvcan started with -c uvremote0, anything else on the bus.
//
// All of the state below belongs to the bridge task. The result arrives on the
// broker's pump task, which does nothing but park it and set *sdo_res_ready*.

// COB-ids of the two halves of an SDO conversation, and the command bytes of
// the frames exchanged. Plain CANopen (CiA 301), spelled out here rather than
// taken from the stack's internals: this parses someone else's frames off a
// socket, not the stack's own.
#define SDO_REQUEST_ID			0x600
#define SDO_RESPONSE_ID			0x580
// client -> server, the top three bits of the command byte
#define SDO_CCS_DOWNLOAD_SEG	0x00
#define SDO_CCS_DOWNLOAD_INIT	0x20
#define SDO_CCS_UPLOAD_INIT		0x40
#define SDO_CCS_UPLOAD_SEG		0x60
#define SDO_CS_ABORT			0x80
// server -> client
#define SDO_SCS_DOWNLOAD_SEG	0x20
#define SDO_SCS_DOWNLOAD_INIT	0x60
#define SDO_SCS_UPLOAD_INIT		0x40
#define SDO_SCS_UPLOAD_SEG		0x00
#define SDO_CMD_TOGGLE			0x10

/// @brief: How long the bridge waits for the device before telling the client
/// the transfer failed. The client's own timeout is shorter - a second is the
/// usual CANopen default - so it will often have given up and retried first;
/// this is only what stops this end waiting for ever on an answer that is not
/// coming.
#define SDO_OFFLOAD_TIMEOUT_MS	5000

typedef enum {
	SDO_IDLE = 0,
	// collecting the segments of a download the client is still sending
	SDO_DL_COLLECT,
	// the whole transfer is at the device
	SDO_WAIT,
	// handing an upload's result back to the client, segment by segment
	SDO_UL_SERVE,
} sdo_state_e;

// whether the bridged device reported the offload applied; without it every
// transfer is relayed frame by frame as before
static bool sdo_offload_on;

static sdo_state_e sdo_state;
static uint8_t sdo_node;
static uint16_t sdo_mindex;
static uint8_t sdo_sindex;
static bool sdo_write;
static uint8_t sdo_buf[REMOTE_SDO_DATA_MAX];
static uint16_t sdo_len;
// how far through *sdo_buf* the upload being served has got, and the toggle bit
// the next segment carries
static uint16_t sdo_pos;
static bool sdo_toggle;
// the answer a finished transfer still owes the client: a download's last
// segment is answered with a segment reply, its first frame with an initiate
// reply, and the toggle has to match the request being answered
static bool sdo_reply_is_segment;
static bool sdo_reply_toggle;
static uint32_t sdo_wait_ms;
static uint32_t sdo_count;

// The result, written by the broker's pump task and read by the bridge task.
static volatile bool sdo_res_ready;
static uint32_t sdo_res_abort;
static uint8_t sdo_res_buf[REMOTE_SDO_DATA_MAX];
static uint16_t sdo_res_len;
static uint8_t sdo_res_node;
static uint16_t sdo_res_mindex;
static uint8_t sdo_res_sindex;
static bool sdo_res_write;


/// @brief: Writes one synthesised SDO response onto the netdev, as though the
/// device on *sdo_node* had answered it.
static void sdo_to_client(const uint8_t *data) {
	uv_can_msg_st msg;
	memset(&msg, 0, sizeof(msg));
	msg.type = CAN_STD;
	msg.id = (uint32_t) (SDO_RESPONSE_ID + sdo_node);
	msg.data_length = 8;
	memcpy(msg.data_8bit, data, 8);

	struct can_frame frame;
	msg_to_frame(&msg, &frame);
	if (write(sock, &frame, sizeof(frame)) == (ssize_t) sizeof(frame)) {
		rx_count++;
	}
	else {
		error_count++;
	}
}


/// @brief: Tells the client the transfer failed, with *code* as the reason.
static void sdo_abort_to_client(uint32_t code) {
	uint8_t d[8];
	d[0] = SDO_CS_ABORT;
	d[1] = (uint8_t) (sdo_mindex & 0xFFu);
	d[2] = (uint8_t) ((sdo_mindex >> 8) & 0xFFu);
	d[3] = sdo_sindex;
	d[4] = (uint8_t) (code & 0xFFu);
	d[5] = (uint8_t) ((code >> 8) & 0xFFu);
	d[6] = (uint8_t) ((code >> 16) & 0xFFu);
	d[7] = (uint8_t) ((code >> 24) & 0xFFu);
	sdo_to_client(d);
}


/// @brief: Answers one download frame: the initiate, or a segment carrying
/// *toggle*.
static void sdo_download_reply(bool segment, bool toggle) {
	uint8_t d[8];
	memset(d, 0, sizeof(d));
	if (segment) {
		d[0] = (uint8_t) (SDO_SCS_DOWNLOAD_SEG | (toggle ? SDO_CMD_TOGGLE : 0));
	}
	else {
		d[0] = SDO_SCS_DOWNLOAD_INIT;
		d[1] = (uint8_t) (sdo_mindex & 0xFFu);
		d[2] = (uint8_t) ((sdo_mindex >> 8) & 0xFFu);
		d[3] = sdo_sindex;
	}
	sdo_to_client(d);
}


/// @brief: Answers an upload's initiate with the data itself when it fits in
/// the frame, or with its size when the client has to come back for segments.
static void sdo_upload_init_reply(void) {
	uint8_t d[8];
	memset(d, 0, sizeof(d));
	d[1] = (uint8_t) (sdo_mindex & 0xFFu);
	d[2] = (uint8_t) ((sdo_mindex >> 8) & 0xFFu);
	d[3] = sdo_sindex;
	if (sdo_len <= 4) {
		// expedited: e and s set, and n says how many of the four bytes are
		// not data
		d[0] = (uint8_t) (SDO_SCS_UPLOAD_INIT |
				(((4u - sdo_len) & 0x03u) << 2) | 0x03u);
		memcpy(&d[4], sdo_buf, sdo_len);
	}
	else {
		// segmented, size indicated
		d[0] = (uint8_t) (SDO_SCS_UPLOAD_INIT | 0x01u);
		d[4] = (uint8_t) (sdo_len & 0xFFu);
		d[5] = (uint8_t) ((sdo_len >> 8) & 0xFFu);
		d[6] = 0;
		d[7] = 0;
	}
	sdo_to_client(d);
}


/// @brief: Answers one upload segment request out of the result already here.
static void sdo_upload_segment_reply(bool toggle) {
	uint8_t d[8];
	memset(d, 0, sizeof(d));
	uint16_t left = (uint16_t) (sdo_len - sdo_pos);
	uint8_t n = (left > 7u) ? 7u : (uint8_t) left;
	bool last = (left <= 7u);
	d[0] = (uint8_t) (SDO_SCS_UPLOAD_SEG |
			(toggle ? SDO_CMD_TOGGLE : 0) |
			(((7u - n) & 0x07u) << 1) |
			(last ? 0x01u : 0u));
	memcpy(&d[1], &sdo_buf[sdo_pos], n);
	sdo_pos = (uint16_t) (sdo_pos + n);
	sdo_to_client(d);
	if (last) {
		sdo_state = SDO_IDLE;
	}
	else {
	}
}


/// @brief: Hands the collected transfer to the device.
static void sdo_offload(void) {
	sdo_wait_ms = 0;
	sdo_state = SDO_WAIT;
	sdo_res_ready = false;
	if (!mqtt_dev_send_sdo_req((uint8_t) bridged_fleet, (uint8_t) bridged_dev,
			sdo_write, sdo_node, sdo_mindex, sdo_sindex,
			sdo_buf, sdo_write ? sdo_len : REMOTE_SDO_DATA_MAX)) {
		// nothing went out, so nothing is coming back
		sdo_abort_to_client(REMOTE_SDO_ABORT_BUSY);
		sdo_state = SDO_IDLE;
	}
	else {
		sdo_count++;
	}
}


/// @brief: Takes one frame the client wrote to the netdev. Returns true when
/// the bridge has dealt with it, i.e. when it must not also be forwarded.
static bool sdo_intercept(const uv_can_msg_st *msg) {
	bool ret = false;
	uint8_t node = (uint8_t) (msg->id & 0x7Fu);
	if (!sdo_offload_on ||
			(msg->type != CAN_STD) ||
			((msg->id & ~0x7Fu) != SDO_REQUEST_ID) ||
			(msg->data_length < 8u) ||
			(node == 0u)) {
		// not an SDO request, or the device cannot run it for us
	}
	else if ((sdo_state != SDO_IDLE) && (node != sdo_node)) {
		// one transfer at a time: another node's conversation is relayed the
		// old way rather than queued behind this one
	}
	else {
		uint8_t cmd = msg->data_8bit[0];
		uint16_t mindex = (uint16_t) ((uint16_t) msg->data_8bit[1] |
				((uint16_t) msg->data_8bit[2] << 8));
		uint8_t sindex = msg->data_8bit[3];
		ret = true;

		if ((cmd & 0xE0u) == SDO_CCS_DOWNLOAD_INIT) {
			bool expedited = ((cmd & 0x02u) != 0);
			bool sized = ((cmd & 0x01u) != 0);
			sdo_node = node;
			sdo_mindex = mindex;
			sdo_sindex = sindex;
			sdo_write = true;
			sdo_toggle = false;
			sdo_pos = 0;
			if (expedited) {
				sdo_len = sized ? (uint16_t) (4u - ((cmd >> 2) & 0x03u)) : 4u;
				memcpy(sdo_buf, &msg->data_8bit[4], sdo_len);
				sdo_reply_is_segment = false;
				sdo_reply_toggle = false;
				sdo_offload();
			}
			else {
				// the client will send the data in segments; answer at once so
				// it starts, and collect them here
				sdo_len = 0;
				sdo_state = SDO_DL_COLLECT;
				sdo_download_reply(false, false);
			}
		}
		else if (((cmd & 0xE0u) == SDO_CCS_DOWNLOAD_SEG) &&
				(sdo_state == SDO_DL_COLLECT)) {
			bool toggle = ((cmd & SDO_CMD_TOGGLE) != 0);
			bool last = ((cmd & 0x01u) != 0);
			uint8_t n = (uint8_t) (7u - ((cmd >> 1) & 0x07u));
			if ((sdo_len + n) > REMOTE_SDO_DATA_MAX) {
				// larger than the offload carries; tell the client so, and let
				// it start again - the next attempt is relayed frame by frame
				sdo_abort_to_client(REMOTE_SDO_ABORT_TOO_LONG);
				sdo_state = SDO_IDLE;
			}
			else {
				memcpy(&sdo_buf[sdo_len], &msg->data_8bit[1], n);
				sdo_len = (uint16_t) (sdo_len + n);
				if (last) {
					// the answer to this one waits for the device
					sdo_reply_is_segment = true;
					sdo_reply_toggle = toggle;
					sdo_offload();
				}
				else {
					sdo_download_reply(true, toggle);
				}
			}
		}
		else if ((cmd & 0xE0u) == SDO_CCS_UPLOAD_INIT) {
			sdo_node = node;
			sdo_mindex = mindex;
			sdo_sindex = sindex;
			sdo_write = false;
			sdo_len = 0;
			sdo_pos = 0;
			sdo_toggle = false;
			sdo_reply_is_segment = false;
			sdo_offload();
		}
		else if (((cmd & 0xE0u) == SDO_CCS_UPLOAD_SEG) &&
				(sdo_state == SDO_UL_SERVE)) {
			sdo_upload_segment_reply((cmd & SDO_CMD_TOGGLE) != 0);
		}
		else if (cmd == SDO_CS_ABORT) {
			// the client gave up; so does this end
			sdo_state = SDO_IDLE;
		}
		else {
			// a block transfer, or a frame that makes no sense where the
			// conversation has got to: relay it and let the two ends sort it
			// out between themselves
			sdo_state = SDO_IDLE;
			ret = false;
		}
	}
	return ret;
}


/// @brief: Parks the device's answer for the bridge task. Runs on the broker's
/// pump task, so it touches nothing the bridge task owns.
static void sdo_from_dev(uint8_t fleet_index, uint8_t dev_index,
		bool write, uint8_t node, uint16_t mindex, uint8_t sindex,
		uint32_t abort, const uint8_t *data, uint16_t len, void *user) {
	(void) user;
	if (active &&
			(fleet_index == bridged_fleet) &&
			(dev_index == bridged_dev) &&
			!sdo_res_ready) {
		sdo_res_write = write;
		sdo_res_node = node;
		sdo_res_mindex = mindex;
		sdo_res_sindex = sindex;
		sdo_res_abort = abort;
		sdo_res_len = (len > sizeof(sdo_res_buf)) ?
				(uint16_t) sizeof(sdo_res_buf) : len;
		if ((data != NULL) && (sdo_res_len > 0)) {
			memcpy(sdo_res_buf, data, sdo_res_len);
		}
		else {
		}
		sdo_res_ready = true;
	}
	else {
		// not this bridge's, or the last answer has not been dealt with yet
	}
}


/// @brief: Turns a parked result into the client's answer, and gives up on a
/// transfer the device never answered. Called from the bridge's step.
static void sdo_result_step(uint16_t step_ms) {
	if (sdo_res_ready) {
		bool mine = ((sdo_state == SDO_WAIT) &&
				(sdo_res_node == sdo_node) &&
				(sdo_res_mindex == sdo_mindex) &&
				(sdo_res_sindex == sdo_sindex) &&
				(sdo_res_write == sdo_write));
		if (!mine) {
			// an answer to something this end has already given up on
		}
		else if (sdo_res_abort != 0) {
			sdo_abort_to_client(sdo_res_abort);
			sdo_state = SDO_IDLE;
		}
		else if (sdo_write) {
			sdo_download_reply(sdo_reply_is_segment, sdo_reply_toggle);
			sdo_state = SDO_IDLE;
		}
		else {
			sdo_len = sdo_res_len;
			memcpy(sdo_buf, sdo_res_buf, sdo_len);
			sdo_pos = 0;
			// an upload that fits the initiate frame is finished by it; a
			// larger one is served segment by segment from here
			sdo_state = (sdo_len <= 4u) ? SDO_IDLE : SDO_UL_SERVE;
			sdo_upload_init_reply();
		}
		sdo_res_ready = false;
	}
	else if (sdo_state == SDO_WAIT) {
		sdo_wait_ms += step_ms;
		if (sdo_wait_ms >= SDO_OFFLOAD_TIMEOUT_MS) {
			printf("remote CAN: no answer to the offloaded transfer of "
					"0x%x:%u at node 0x%x\n",
					(unsigned int) sdo_mindex, (unsigned int) sdo_sindex,
					(unsigned int) sdo_node);
			fflush(stdout);
			sdo_abort_to_client(REMOTE_SDO_ABORT_BUSY);
			sdo_state = SDO_IDLE;
		}
		else {
		}
	}
	else {
		// idle, collecting, or serving: nothing waits on the device
	}
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


/// @brief: Task body: steps the bridge every REMOTECAN_STEP_MS for the life of
/// the program. The step is what carries a frame written to the netdev on to
/// the device, so how often it runs is half the bridge's latency.
static void remotecan_task(void *ptr) {
	(void) ptr;
	while (true) {
		remotecan_step();
		uv_rtos_task_delay(REMOTECAN_STEP_MS);
	}
}


/// @brief: Starts the task above, once.
static void remotecan_start_task(void) {
	if (!task_started) {
		task_started = true;
		uv_rtos_task_create(&remotecan_task, "remotecan",
				UV_RTOS_MIN_STACK_SIZE * 3, NULL,
				UV_RTOS_IDLE_PRIORITY + 1, NULL);
	}
	else {
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
		remotecan_start_task();
		// a new bridge starts with no transfer in flight, whatever the last one
		// was doing when it was closed
		sdo_state = SDO_IDLE;
		sdo_res_ready = false;
		sdo_offload_on = false;
		mqtt_set_sdo_callb(&sdo_from_dev, NULL);
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
		(void) mqtt_dev_set_can_active(fleet_index, dev_index, true, sdo_only);
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
		mqtt_set_sdo_callb(NULL, NULL);
		sdo_state = SDO_IDLE;
		sdo_res_ready = false;
		sdo_offload_on = false;
		if ((bridged_fleet >= 0) && (bridged_dev >= 0)) {
			(void) mqtt_dev_send_rxclear((uint8_t) bridged_fleet,
					(uint8_t) bridged_dev);
			(void) mqtt_dev_set_can_active((uint8_t) bridged_fleet,
					(uint8_t) bridged_dev, false, sdo_only);
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
						(uint8_t) bridged_dev, true, sdo_only);
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

	if (active && (bridged_fleet >= 0)) {
		// Whether the device is running transfers for us. It says so in the
		// feature mask it reports applied, which drops to nothing on every
		// reconnect - so this follows it rather than being latched on.
		sdo_offload_on = ((mqtt_get_dev_features((uint8_t) bridged_fleet,
				(uint8_t) bridged_dev) & REMOTE_IOT_FEATURE_SDO) != 0);
		// answers that arrived since the last step, and transfers the device
		// never answered at all
		sdo_result_step(REMOTECAN_STEP_MS);
	}
	else {
		sdo_offload_on = false;
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
				else if (sdo_intercept(&msg)) {
					// part of a conversation the device is running for us; it
					// is answered from here, not relayed
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


bool remotecan_get_sdo_only(void) {
	return sdo_only;
}


void remotecan_set_sdo_only(bool value) {
	if (value != sdo_only) {
		sdo_only = value;
		// The filter table is untouched: this narrows by class, which no id
		// mask can express, so it travels as a feature bit rather than as an
		// rxconf round.
		if (active && (bridged_fleet >= 0) && (bridged_dev >= 0)) {
			(void) mqtt_dev_set_can_active((uint8_t) bridged_fleet,
					(uint8_t) bridged_dev, true, sdo_only);
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

bool remotecan_get_sdo_only(void) {
	return false;
}

void remotecan_set_sdo_only(bool value) {
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
