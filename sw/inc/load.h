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


#ifndef LOAD_H_
#define LOAD_H_


#include <uv_utilities.h>
#include <stdbool.h>
#include "system.h"



typedef struct {
	char firmware[256];
	uv_delay_st delay;
	bool response;
	bool wfr;
	bool uv;
	bool block_transfer;

	uint8_t nodeid;
	uint8_t progress;
	// true if the loading has finished
	bool finished;
	// set from the UI to abort the flash: honored while waiting for the device
	// (wfr) and between block-transfer chunks
	volatile bool cancel;
	// true while waiting for the target device's boot-up message (wfr), so the UI
	// can show a "waiting for device" state
	volatile bool waiting;
	// when true the wfr wait does not time out; it waits until the device boots
	// up or the flash is cancelled. Used by the UI to flash an offline device once
	// it is powered on.
	bool wait_forever;
} load_st;


#define LOADWFR_WAIT_TIME_MS			10000


/// @brief: Returns true if the loading has finished. Note that this doesn't separate
/// failed flashing and successful flashing from each other
static inline bool loadbin_is_finished(load_st *this) {
	return this->finished;
}

/// @brief: Returns the progress percent while downloading
static inline uint8_t loadbin_get_progress(load_st *this) {
	return this->progress;
}

/// @brief: Returns true while the flash is waiting for the target device to send
/// its boot-up message (a wfr / offline flash), before the download begins.
static inline bool loadbin_is_waiting(load_st *this) {
	return this->waiting;
}

/// @brief: Requests the in-progress flash to abort. Honored while waiting for the
/// device (wfr) and between block-transfer chunks; a download already committed to
/// a single SDO transfer runs to completion. Safe to call from the UI thread.
void load_cancel(void);

/// @brief: Can be used to trigger the loading of the binary via uvcan, for example from ui
void loadbin(char *filepath, uint8_t nodeid, bool wfr, bool uv, bool block_transfer);


/// @brief: Flashes *device*'s firmware (the FIRMWARE binary from its .uvdev
/// package) to it over the bus, like --loadbin, targeting the device's node id.
/// Starts an asynchronous flash task and returns immediately; poll
/// loadbin_is_finished(&dev.load) for completion. Returns true if the flash was
/// started.
///
/// @param wfr: wait for the device's boot-up message before flashing (used for an
/// offline device that will be powered on), waiting indefinitely until it boots
/// up or the flash is cancelled. When false the device is reset and flashed now.
bool load_flash_device(device_st *device, bool wfr);


/// @brief: Same as load_flash_device(), but flashes *device*'s firmware to the
/// explicit *nodeid* instead of the device's own node id.
bool load_flash_device_to_node(device_st *device, uint8_t nodeid, bool wfr);


/// @brief: Flashes the FIRMWARE binary from an explicit .uvdev package at
/// *uvdev_path* to *nodeid*, without needing a configured device. Used by the UI
/// to flash a device that has no configuration file assigned: the user picks the
/// package and takes responsibility for its suitability. Starts an asynchronous
/// flash task; returns true if the flash was started.
bool load_flash_uvdev_to_node(const char *uvdev_path, uint8_t nodeid, bool wfr);


/// @brief: Single entry point for flashing firmware from a file path. The load
/// command variants are thin wrappers around this, and it is intended for reuse
/// from within uvcan (e.g. the UI). The target node is taken from the loaded
/// database. Returns false if *path* is NULL.
///
/// @param wfr: wait for the node's boot-up message instead of resetting it
/// @param uv: use the legacy, non-CANopen-302 UV bootloader protocol
/// @param block_transfer: use SDO block transfer (false = segmented transfer)
bool load_firmware(const char *path, bool wfr, bool uv, bool block_transfer);


/// @brief: Loads firmware with the name of **arg** to device selected
/// previously with command *nodeid*.
bool cmd_load(const char *arg);


/// @brief: Loads the firmware by not resetting the node. Instead,
/// waits for LOADWFT_WAIT_TIME_MS seconds to receive the boot up message from the node.
/// Can be used when downloading binary to faulty node which is unable to run otherwise
bool cmd_loadwfr(const char *arg);


/// @brief: Loads firmware with the name of **arg** to device selected
/// previously with command *nodeid* with SDO segmented transfer.
bool cmd_segload(const char *arg);


/// @brief: Loads the firmware by not resetting the node with SDO segmented transfer. Instead,
/// waits for LOADWFT_WAIT_TIME_MS seconds to receive the boot up message from the node.
/// Can be used when downloading binary to faulty node which is unable to run otherwise
bool cmd_segloadwfr(const char *arg);


/// @brief: Loads firmware with the name of **arg** to device selected
/// previously with command *nodeid* with older, non-CANopen 302 compatible bootloader.
bool cmd_uvload(const char *arg);


/// @brief: Loads the firmware by not resetting the node. Instead,
/// waits for LOADWFT_WAIT_TIME_MS seconds to receive the boot up message from the node
/// with older, non-CANopen 302 compatible bootloader.
/// Can be used when downloading binary to faulty node which is unable to run otherwise
bool cmd_uvloadwfr(const char *arg);

#endif /* LOAD_H_ */
