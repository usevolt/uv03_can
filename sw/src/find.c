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


#include "find.h"
#include <stdio.h>
#include <string.h>
#include <uv_rtos.h>
#include <uv_can.h>
#include <uv_canopen.h>
#include "main.h"
#include "system.h"


// Duration of the heartbeat listen window in milliseconds.
#define FIND_LISTEN_MS		2000
// Polling step while listening.
#define FIND_STEP_MS		20

// CANopen Identity object (0x1018) and the sub-indexes read from it. Per the
// CANopen standard the array is: 1 = vendor id, 2 = product code, 3 = revision
// number. (The object's array_max_size is larger than the populated struct, so
// reading past sub 3 returns garbage — which is why the wrong sub-indexes used
// to show "random" values.)
#define IDENTITY_INDEX			0x1018
#define IDENTITY_SUB_VENDOR		1
#define IDENTITY_SUB_PRODUCT	2
// Identity sub 3 holds the device's revision number, shown as the device's
// "software version" for an online device.
#define IDENTITY_SUB_REVISION	3

// Highest valid CANopen node id.
#define NODEID_MAX				127

// A device is considered offline if no OPERATIONAL heartbeat has been seen from
// it within this many milliseconds.
#define FIND_ONLINE_TIMEOUT_MS	2000


// Bitmap (one bit per node id 0..127) of nodes seen sending a heartbeat (any NMT
// state). Written by the CAN rx callback (in the HAL task), read from the
// application. It accumulates: a node that sends a heartbeat at any time after
// the monitor is installed leaves its bit set, which find_devices() uses to know
// which nodes to query during a scan.
static uint8_t operational_nodes[(NODEID_MAX + 1 + 7) / 8];
// Per-node RTOS tick of the most recently received heartbeat (0 = never seen).
// Written by the CAN rx callback, read by the application to age devices out to
// OFFLINE once they stop sending heartbeats.
static volatile uint32_t last_seen_tick[NODEID_MAX + 1];
// Per-node NMT state byte from the most recently received heartbeat (the
// CANOPEN_* node state, toggle bit masked off). Only meaningful once the node's
// last_seen_tick is non-zero. Mapped onto dev_state_e by nmt_to_dev_state().
static volatile uint8_t last_nmt_state[NODEID_MAX + 1];
// True once the CAN rx sniffer has been installed, so it is only installed once.
static bool monitor_installed;
// Optional additional CAN rx sniffer, called from find_can_callb() for every
// frame (see find_set_extra_can_callback()). NULL when none is registered.
static void (*extra_can_callb)(void *user_ptr, uv_can_message_st *msg);
// Node ids blocked from live auto-discovery (find_poll_new_devices). A device
// removed from the UI is blacklisted so it does not immediately reappear; the
// System tab's search clears the list.
static uint8_t blacklist_bits[(NODEID_MAX + 1 + 7) / 8];
// Per-node live-discovery state: 0 = not handled by the poll, 1 = added as a
// BOOT-UP placeholder (identity unread), 2 = added / attempted with a full
// identity read. Reset to 0 when the node goes offline so a reconnect is
// rediscovered.
static uint8_t live_added[NODEID_MAX + 1];
// True once an asynchronous UI search (see find_search_async()) has finished.
static volatile bool search_finished = true;
// Remaining listen time of the search in progress, in milliseconds (0 when no
// search is running). Updated by find_devices(), read by the UI for a countdown.
static volatile uint16_t search_remaining_ms;


static inline void node_set(uint8_t nodeid) {
	operational_nodes[nodeid / 8] |= (uint8_t) (1u << (nodeid % 8));
}

static inline bool node_is_set(uint8_t nodeid) {
	return (operational_nodes[nodeid / 8] & (uint8_t) (1u << (nodeid % 8))) != 0;
}


bool find_node_is_online(uint8_t nodeid) {
	return (nodeid <= NODEID_MAX) && node_is_set(nodeid);
}


/// @brief: Maps a CANopen NMT node-state byte (from a heartbeat) onto the device
/// connection state shown in the UI. STOPPED is grouped with PRE-OPERATIONAL: in
/// both the device is reachable over SDO but not exchanging PDOs. Any unknown
/// value is treated as a fresh boot-up.
static dev_state_e nmt_to_dev_state(uint8_t nmt) {
	dev_state_e ret;
	switch (nmt) {
	case CANOPEN_OPERATIONAL:
		ret = DEV_STATE_OP;
		break;
	case CANOPEN_PREOPERATIONAL:
	case CANOPEN_STOPPED:
		ret = DEV_STATE_PREOP;
		break;
	case CANOPEN_BOOT_UP:
	default:
		ret = DEV_STATE_BOOTUP;
		break;
	}
	return ret;
}


/// @brief: CAN rx sniffer. Records the node id and NMT state of every heartbeat
/// (COB-ID 0x700 + nodeid, a single data byte holding the NMT state). Runs in
/// the HAL task, so it only touches the shared bitmap / state arrays; mapping
/// them onto the device structs is left to the application thread.
static void find_can_callb(void *ptr, uv_can_msg_st *msg) {
	if ((msg->type == CAN_STD) &&
			((msg->id & ~0x7Fu) == CANOPEN_HEARTBEAT_ID) &&
			(msg->data_length == 1)) {
		uint8_t nodeid = msg->id & 0x7Fu;
		if ((nodeid >= 1) && (nodeid <= NODEID_MAX)) {
			node_set(nodeid);
			// mask off the node-guarding toggle bit; only the state remains
			last_nmt_state[nodeid] = msg->data_8bit[0] & 0x7Fu;
			// record the heartbeat time; never store 0 so it reads as "seen"
			uint32_t t = uv_rtos_get_tick_count();
			last_seen_tick[nodeid] = (t != 0) ? t : 1;
		}
	}
	// forward the frame to the optional extra sniffer (e.g. the GUI terminal),
	// so it can observe raw traffic without displacing the heartbeat monitor
	if (extra_can_callb != NULL) {
		extra_can_callb(ptr, msg);
	}
}


void find_set_extra_can_callback(void (*callb)(void *user_ptr, uv_can_message_st *msg)) {
	extra_can_callb = callb;
}


void find_start_monitor(void) {
	// make sure the CAN bus is up (no-op if it already is) and install the
	// heartbeat sniffer once
	uv_can_set_up(false);
	if (!monitor_installed) {
		uv_canopen_set_can_callback(&find_can_callb);
		monitor_installed = true;
	}
}


void find_reinstall_monitor(void) {
	uv_canopen_set_can_callback(&find_can_callb);
	monitor_installed = true;
}


/// @brief: Reads the "software version" (Identity object revision number) of an
/// online device. Returns 0 if the read fails.
static uint32_t read_sw_version(uint8_t nodeid) {
	uint32_t sw_version = 0;
	if (uv_canopen_sdo_read(nodeid, IDENTITY_INDEX, IDENTITY_SUB_REVISION,
			sizeof(sw_version), &sw_version) != ERR_NONE) {
		sw_version = 0;
	}
	return sw_version;
}


// All Usevolt devices expose their CAN interface version (object-dictionary
// revision) at a fixed object, so it can be read straight from the device even
// when no configuration package is attached to point at it.
#define CAN_IF_VERSION_INDEX		0x2FFF
#define CAN_IF_VERSION_SUBINDEX		0
#define CAN_IF_VERSION_SIZE			2


uint32_t find_read_device_revision(device_st *device) {
	uint32_t rev = 0;
	if ((device != NULL) &&
			(device->state != DEV_STATE_OFFLINE) &&
			(device->nodeid != 0)) {
		// prefer the object location captured from the configuration database;
		// fall back to the fixed Usevolt CAN IF VERSION object when there is none
		// (e.g. a device discovered on the bus without a configuration file)
		uint16_t mindex = device->if_version_mindex;
		uint8_t sindex = device->if_version_sindex;
		uint8_t size = device->if_version_size;
		if ((mindex == 0) || (size == 0)) {
			mindex = CAN_IF_VERSION_INDEX;
			sindex = CAN_IF_VERSION_SUBINDEX;
			size = CAN_IF_VERSION_SIZE;
		}
		uint32_t v = 0;
		if (uv_canopen_sdo_read(device->nodeid, mindex, sindex, size, &v) ==
						ERR_NONE) {
			rev = v;
		}
	}
	return rev;
}


bool find_update_device_states(system_st *sys) {
	bool changed = false;
	uint32_t now = uv_rtos_get_tick_count();
	for (uint8_t i = 0; i < system_get_dev_count(sys); i++) {
		device_st *device = system_get_dev(sys, i);
		if (device->nodeid == 0) {
			continue;
		}
		// online while a heartbeat has been seen within the timeout window; the
		// live state then follows the last heartbeat's NMT state
		uint32_t seen = last_seen_tick[device->nodeid];
		bool online = (seen != 0) &&
				(((now - seen) * UV_RTOS_TICK_PERIOD_MS) < FIND_ONLINE_TIMEOUT_MS);
		dev_state_e newstate = online ?
				nmt_to_dev_state(last_nmt_state[device->nodeid]) : DEV_STATE_OFFLINE;

		// read the software version once it is missing and the device is in a state
		// that serves the object dictionary (OPERATIONAL / PRE-OPERATIONAL). A
		// BOOT-UP device runs the bootloader and would only stall the SDO read, so
		// it is skipped here and picked up later once the device reaches OP/PRE-OP.
		// The attempt is latched: if the read returns nothing (e.g. a device whose
		// Identity object does not answer with a plain 4-byte value), it is NOT
		// retried every cycle — that would spam the bus with the same failing SDO
		// transfer forever. The latch is cleared below when the device is not in a
		// readable state, so a fresh read is taken after a reboot/reconnect.
		if ((newstate == DEV_STATE_OP) || (newstate == DEV_STATE_PREOP)) {
			if ((device->sw_version == 0) && !device->sw_version_tried) {
				device->sw_version = read_sw_version(device->nodeid);
				device->sw_version_tried = true;
			}
		}
		else {
			device->sw_version_tried = false;
		}

		if (newstate != device->state) {
			device->state = newstate;
			changed = true;
		}
		else {
			// state unchanged this cycle
		}
	}
	return changed;
}


/// @brief: Reads the Identity object of *nodeid* and adds it to the system with
/// its live NMT state and software version. Usevolt nodes (matching vendor id)
/// are added as normal devices; other manufacturers' nodes are added flagged as
/// third-party. Returns true if a device was added. Called as soon as a node's
/// heartbeat appears so the UI can show its tab live.
static bool find_try_add_node(uint8_t nodeid) {
	bool added = false;
	uint32_t vendor_id = 0;
	uv_errors_e e = uv_canopen_sdo_read(nodeid, IDENTITY_INDEX,
			IDENTITY_SUB_VENDOR, sizeof(vendor_id), &vendor_id);
	if (e == ERR_NONE) {
		uint32_t product_code = 0;
		uv_canopen_sdo_read(nodeid, IDENTITY_INDEX,
				IDENTITY_SUB_PRODUCT, sizeof(product_code), &product_code);
		device_st *device = system_add_found_device(&dev.system, nodeid,
				vendor_id, product_code);
		if (device != NULL) {
			bool thirdparty = (vendor_id != CANOPEN_USEVOLT_VENDOR_ID);
			device->thirdparty = thirdparty;
			// record the device's live NMT state from its last heartbeat
			device->state = nmt_to_dev_state(last_nmt_state[nodeid]);
			device->sw_version = read_sw_version(nodeid);
			printf("Found %s device at node 0x%x "
					"(vendor 0x%x, product code 0x%x)\n",
					thirdparty ? "third-party" : "Usevolt",
					(unsigned int) nodeid, (unsigned int) vendor_id,
					(unsigned int) product_code);
			added = true;
		}
		else {
			printf("Found device at node 0x%x but the system is "
					"full; ignoring it.\n", (unsigned int) nodeid);
		}
	}
	else {
		// no readable Identity object (not a CANopen device, or unreachable): skip
	}
	return added;
}


/// @brief: Adds a node that is only sending BOOT-UP heartbeats (e.g. a device
/// running the bootloader, typically at node 0x7f) to the system, WITHOUT reading
/// its Identity object or any other data: a bootloading device does not answer
/// application SDO reads, so a read would only stall. The device shows up with its
/// node id and a BOOT-UP state so the user can see it (and e.g. flash it); the
/// vendor id / product code / software version stay unknown until it reaches a
/// readable state and a later scan reads them. Returns true if a device was added.
static bool find_add_bootup_node(uint8_t nodeid) {
	bool added = false;
	device_st *device = system_add_found_device(&dev.system, nodeid, 0, 0);
	if (device != NULL) {
		device->state = DEV_STATE_BOOTUP;
		printf("Found device at node 0x%x in BOOT-UP state (bootloader); "
				"its identity is not read while it is booting.\n",
				(unsigned int) nodeid);
		added = true;
	}
	else {
		printf("Found a booting device at node 0x%x but the system is "
				"full; ignoring it.\n", (unsigned int) nodeid);
	}
	return added;
}


/// @brief: Removes the first device in the system matching *nodeid*. Used to drop
/// a BOOT-UP placeholder once the same node has become readable and been re-added
/// with its full identity: the placeholder was added earlier, so it is the first
/// (lower-indexed) of the two matching entries and the one removed here.
static void find_drop_device(uint8_t nodeid) {
	for (uint8_t i = 0; i < system_get_dev_count(&dev.system); i++) {
		device_st *d = system_get_dev(&dev.system, i);
		if (d->nodeid == nodeid) {
			system_remove_device(&dev.system, d);
			break;
		}
	}
}


static bool node_blacklisted(uint8_t nodeid) {
	return (blacklist_bits[nodeid / 8] & (uint8_t) (1u << (nodeid % 8))) != 0;
}


void find_blacklist_node(uint8_t nodeid) {
	if ((nodeid >= 1) && (nodeid <= NODEID_MAX)) {
		blacklist_bits[nodeid / 8] |= (uint8_t) (1u << (nodeid % 8));
		// forget any live-discovery state so it is re-evaluated if un-blacklisted
		live_added[nodeid] = 0;
	}
}


void find_clear_blacklist(void) {
	memset(blacklist_bits, 0, sizeof(blacklist_bits));
}


/// @brief: Returns the first device in the system with the given node id, or NULL.
static device_st *find_device_by_nodeid(uint8_t nodeid) {
	device_st *ret = NULL;
	for (uint8_t i = 0; i < system_get_dev_count(&dev.system); i++) {
		device_st *d = system_get_dev(&dev.system, i);
		if (d->nodeid == nodeid) {
			ret = d;
			break;
		}
	}
	return ret;
}


bool find_poll_new_devices(void) {
	bool changed = false;
	uint32_t now = uv_rtos_get_tick_count();
	for (uint8_t nodeid = 1; nodeid <= NODEID_MAX; nodeid++) {
		uint32_t seen = last_seen_tick[nodeid];
		bool online = (seen != 0) &&
				(((now - seen) * UV_RTOS_TICK_PERIOD_MS) < FIND_ONLINE_TIMEOUT_MS);
		if (!online) {
			// forget the node so a later reconnect is rediscovered / re-attempted
			live_added[nodeid] = 0;
			continue;
		}
		if (node_blacklisted(nodeid) || (live_added[nodeid] == 2)) {
			continue;
		}
		device_st *existing = find_device_by_nodeid(nodeid);
		uint8_t nmt = last_nmt_state[nodeid];
		bool readable = (nmt == CANOPEN_OPERATIONAL) ||
				(nmt == CANOPEN_PREOPERATIONAL);
		if (readable) {
			if ((existing == NULL) || (live_added[nodeid] == 1)) {
				// a brand-new readable node, or our BOOT-UP placeholder that has
				// become readable: read its identity and add it as a full device,
				// dropping the placeholder afterwards
				uint8_t prev = live_added[nodeid];
				live_added[nodeid] = 2;
				if (find_try_add_node(nodeid)) {
					if (prev == 1) {
						find_drop_device(nodeid);
					}
					changed = true;
				}
			}
			else {
				// device already present (from a file or an earlier scan): leave it
				live_added[nodeid] = 2;
			}
		}
		else {
			// not readable (BOOT-UP): show a placeholder so the node is visible and
			// can be flashed, but read nothing from it
			if ((existing == NULL) && (live_added[nodeid] == 0)) {
				if (find_add_bootup_node(nodeid)) {
					live_added[nodeid] = 1;
					changed = true;
				}
			}
		}
	}
	return changed;
}


uint8_t find_devices(void) {
	uint8_t found = 0;

	// install the heartbeat monitor (also brings the CAN bus up)
	find_start_monitor();

	// keep the existing devices: any node found is added alongside them. Start the
	// listen window from a clean heartbeat bitmap so the scan re-detects the nodes
	// that are live right now.
	memset(operational_nodes, 0, sizeof(operational_nodes));

	// remember how many devices are already present so the summary counts only the
	// newly found ones (the existing devices are kept, see below)
	uint8_t initial_count = system_get_dev_count(&dev.system);

	// use printf (not PRINT) so the search information is shown regardless of the
	// --silent (-s) flag
	printf("Searching for devices on the CAN bus for %u ms...\n",
			(unsigned int) FIND_LISTEN_MS);

	// listen for the whole window, adding each node the moment it is first seen.
	// This way new device tabs appear live during the scan instead of all at once
	// when the window ends. Per-node add status this scan:
	//   0 = not added, 1 = added as a BOOT-UP placeholder (identity unread),
	//   2 = added (or attempted) with a full Identity read.
	uint8_t node_added[NODEID_MAX + 1] = { };
	// pre-mark the node ids of the already-present devices as "done" so the scan
	// neither re-reads nor duplicates them; their live state is kept up to date by
	// the UI monitor (find_update_device_states()).
	for (uint8_t i = 0; i < initial_count; i++) {
		device_st *d = system_get_dev(&dev.system, i);
		if ((d->nodeid >= 1) && (d->nodeid <= NODEID_MAX)) {
			node_added[d->nodeid] = 2;
		}
	}
	for (uint32_t t = 0; t < FIND_LISTEN_MS; t += FIND_STEP_MS) {
		// expose the remaining listen time so the UI can show a live countdown
		search_remaining_ms = (uint16_t) (FIND_LISTEN_MS - t);
		uv_rtos_task_delay(FIND_STEP_MS);
		for (uint8_t nodeid = 1; nodeid <= NODEID_MAX; nodeid++) {
			if (!node_is_set(nodeid) || (node_added[nodeid] == 2)) {
				continue;
			}
			uint8_t nmt = last_nmt_state[nodeid];
			if ((nmt == CANOPEN_OPERATIONAL) ||
					(nmt == CANOPEN_PREOPERATIONAL)) {
				// only PRE-OPERATIONAL / OPERATIONAL nodes answer SDO reads: read
				// the Identity object and add the node as a full device. If it was
				// first seen booting up, drop that placeholder now that the real
				// identity is available. Mark it done either way so a device whose
				// Identity object does not answer is not re-read every cycle.
				bool was_placeholder = (node_added[nodeid] == 1);
				node_added[nodeid] = 2;
				if (find_try_add_node(nodeid)) {
					if (was_placeholder) {
						find_drop_device(nodeid);
					}
				}
			}
			else if (node_added[nodeid] == 0) {
				// BOOT-UP (or any other non-readable NMT state): show the device so
				// the user can see it (and e.g. flash it), but do NOT read anything
				// from it — a bootloading device does not answer application SDOs.
				if (find_add_bootup_node(nodeid)) {
					node_added[nodeid] = 1;
				}
			}
			else {
				// already added as a BOOT-UP placeholder and still not readable:
				// leave it, it is picked up above once it reaches OP/PRE-OP
			}
		}
	}

	// the listen window has elapsed
	search_remaining_ms = 0;

	// report only the net number of newly added devices (the list grew from
	// initial_count), including BOOT-UP placeholders; the pre-existing devices are
	// left in place
	found = system_get_dev_count(&dev.system) - initial_count;
	printf("Device search finished: %u new device(s) found.\n",
			(unsigned int) found);

	return found;
}


bool find_search_is_finished(void) {
	return search_finished;
}


uint16_t find_search_remaining_ms(void) {
	return search_remaining_ms;
}


/// @brief: Task body running find_devices() off the UI thread so the UI stays
/// live and can render new device tabs as they are discovered.
static void find_ui_search_task(void *ptr) {
	find_devices();
	search_finished = true;
	uv_rtos_task_delete(NULL);
}


void find_search_async(void) {
	search_finished = false;
	uv_rtos_task_create(&find_ui_search_task, "find_ui_task",
			UV_RTOS_MIN_STACK_SIZE * 5, NULL, UV_RTOS_IDLE_PRIORITY + 1, NULL);
}


/// @brief: FreeRTOS task body for the --find command.
static void find_task(void *ptr) {
	find_devices();
}


bool cmd_find(const char *arg) {
	add_task(&find_task);
	return true;
}
