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


#ifndef FIND_H_
#define FIND_H_


#include <stdbool.h>
#include <stdint.h>
#include "system.h"


/// @brief: --find command callback. Registers a task that performs a device
/// search (see find_devices()).
bool cmd_find(const char *arg);


/// @brief: Installs the CAN heartbeat sniffer (and brings the CAN bus up) so
/// OPERATIONAL nodes are recorded as they appear. Idempotent. Used by the --ui
/// live monitor; find_devices() installs it implicitly too.
void find_start_monitor(void);


/// @brief: Returns true when *nodeid* has been seen sending an OPERATIONAL
/// heartbeat since the monitor was installed.
bool find_node_is_online(uint8_t nodeid);


/// @brief: Reads *device*'s own CAN IF VERSION (revision) from the CAN IF VERSION
/// object on the bus, using the object location captured from its .uvdev database.
/// Returns 0 when the device is offline, has no configuration file, or the read
/// fails. Blocks for the SDO read.
uint32_t find_read_device_revision(device_st *device);


/// @brief: Updates every device of *sys* to its live CAN-bus state: OFFLINE when
/// no heartbeat has been seen within the timeout, otherwise the NMT state from the
/// last heartbeat (BOOTUP / PREOP / OP). Reads the device's software version once
/// when it first comes online. Returns true if any device's state changed, so the
/// caller can refresh the UI.
bool find_update_device_states(system_st *sys);


/// @brief: Searches the CAN bus for devices and adds any newly found ones to the
/// system, keeping the devices already present.
///
/// Brings the CAN bus up (no-op if already up), then listens for CANopen
/// heartbeats for a fixed window. For every node seen in a readable state
/// (OPERATIONAL / PRE-OPERATIONAL) it reads the Identity object (0x1018) and adds
/// the node as a new device with an empty configuration-file path. Nodes seen
/// only in BOOT-UP (e.g. a device running the bootloader, typically node 0x7f)
/// are also added, but WITHOUT reading their Identity object or any other data —
/// a bootloading device does not answer application SDO reads. Such a device
/// shows up with its node id and a BOOT-UP state until it becomes readable (then
/// a later scan reads its identity). Node ids already present in the system are
/// skipped (no duplicate device is added); their live state is kept up to date by
/// the UI monitor.
///
/// Blocks for the duration of the listen window plus the SDO identity reads, so
/// it must be called from a task/UI context where blocking is acceptable. Safe
/// to call both from the --find command task and from the UI thread.
///
/// @return: the number of devices found and added.
uint8_t find_devices(void);


/// @brief: Runs find_devices() on its own task so the caller's thread (e.g. the
/// UI) stays live and can render device tabs as they are discovered. Poll
/// find_search_is_finished() to know when it is done.
void find_search_async(void);


/// @brief: Returns true when no asynchronous search is running (i.e. the last
/// find_search_async() has completed). Returns true before any search is started.
bool find_search_is_finished(void);


/// @brief: Returns the remaining listen time of the search in progress, in
/// milliseconds, or 0 when no search is running. Used by the UI to show a live
/// "Searching Xs..." countdown on the search button.
uint16_t find_search_remaining_ms(void);


#endif /* FIND_H_ */
