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


#ifndef LOADPARAM_H_
#define LOADPARAM_H_


#include <uv_utilities.h>
#include <stdbool.h>
#include "system.h"


#define QUERY_NAME_LEN	64
#define QUERY_QUESTION_LEN	128
#define QUERY_ANSWER_COUNT	6
#define QUERY_ANSWER_LEN	64

typedef struct {
	char name[QUERY_NAME_LEN];
	char question[QUERY_QUESTION_LEN];
	char answers[QUERY_ANSWER_COUNT][QUERY_ANSWER_LEN];
	uint8_t answer_count;
	uint8_t correct_answer;
} query_st;


#define QUERY_COUNT			20

typedef struct {
	char files[64][256];
	unsigned int current_file;

	// The --loadparam option's attached argument (empty when none was given).
	// Captured by cmd_loadparam; the dispatch task resolves it (against the
	// non-option arguments and the loaded system) into a .uvsys / prior-system
	// parameter load or a raw parameter-file load.
	char dispatch_arg[1024];

	query_st queries_buffer[QUERY_COUNT];
	uv_vector_st queries;

	uint8_t modified_dev_nodeids[64];
	uint8_t dev_count;

	// When true, loadparam_step only writes the parameters to the target device:
	// it does not suppress/clear EMCY messages, store the parameters or reset the
	// device. Used by the system load (loadparam_load_system_async), which
	// sequences those steps across all devices itself.
	bool sys_load_mode;

	// true if the loading has finished
	bool finished;

	// true if the last finished load completed without a CANopen transfer error.
	// Set by loadparam_step() and returned by loadparam_load_device().
	bool success;
} loadparam_st;



/// @brief: Returns true if the loading has finished. Note that this doesn't separate
/// successful and failure loading from each other
static inline bool loadparam_is_finished(loadparam_st *this) {
	return this->finished;
}



/// @brief: Single entry point for writing parameters to a device from the
/// parameter file at *path*. Used by the loadparam command and intended for
/// reuse from within uvcan (e.g. the UI). Returns false if *path* is NULL.
bool loadparam_load(const char *path);


/// @brief: Loads uvcan parameters with the name of **arg** to device selected
/// previously with command *nodeid*.
bool cmd_loadparam(const char *arg);


/// @brief: Loads the parameter *file* into *device* over the bus, using the
/// device's .uvdev object dictionary and forcing its actual node id as the
/// target (like the --loadparam command but for a single device chosen in the
/// UI). Blocks while the parameters are written over SDO. Returns true on
/// success.
bool loadparam_load_device(device_st *device, const char *file);


/// @brief: Starts loadparam_load_device() on its own task so the caller (the UI)
/// is not blocked while parameters are written over SDO. Poll
/// loadparam_is_finished(&dev.loadparam) for completion.
void loadparam_load_device_async(device_st *device, const char *file);


/// @brief: Loads saved parameters onto a set of devices over the bus, on its own
/// task so the caller (the UI) stays live. Each device's own param_file is used
/// as the source; devices with no param_file are skipped. The device pointers
/// must stay valid for the duration. Poll loadparam_load_params_is_finished().
void loadparam_load_params_async(device_st **devices, uint8_t count);


/// @brief: Returns true when no asynchronous multi-device parameter load is
/// running (i.e. the last loadparam_load_params_async() has completed). Returns
/// true before any such load is started.
bool loadparam_load_params_is_finished(void);


/// @brief: Loads the parameters bundled with a loaded system configuration onto
/// every given device, on its own task so the UI stays live. Each device's own
/// param_file is the source; devices with no param_file are skipped.
///
/// Unlike loadparam_load_params_async() (which runs the full per-device cycle one
/// device at a time), the load is sequenced across all devices: EMCY messages are
/// suppressed on every device first, then each device's parameters are written in
/// turn, and finally every device is stored and reset simultaneously, so a device
/// does not emit EMCY warnings while another device is being written. Poll
/// loadparam_load_system_is_finished().
void loadparam_load_system_async(device_st **devices, uint8_t count);


/// @brief: Returns true when no asynchronous system parameter load is running
/// (i.e. the last loadparam_load_system_async() has completed). Returns true
/// before any such load is started.
bool loadparam_load_system_is_finished(void);


/// @brief: Compares the CAN interface version stored in *device*'s param file
/// against the version read from the device over the bus. Returns true when both
/// are known and differ (a mismatch worth warning about). *file_if* and *dev_if*,
/// when not NULL, receive the param-file and device versions (0 when unknown).
/// Returns false (no mismatch) when either version cannot be determined.
bool loadparam_can_if_mismatch(device_st *device,
		uint32_t *file_if, uint32_t *dev_if);



#endif /* LOAD_H_ */



