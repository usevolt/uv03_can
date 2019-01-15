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

/// @brief: Can be used to trigger the loading of the binary via uvcan, for example from ui
void loadbin(char *filepath, uint8_t nodeid, bool wfr, bool uv, bool block_transfer);


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
