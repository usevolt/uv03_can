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
} load_st;


#define LOADWFR_WAIT_TIME_MS			10000


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
