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


#ifndef SIMRUN_H_
#define SIMRUN_H_


#include <stdbool.h>
#include <stdint.h>
#include "system.h"


/// @brief: Runs the Linux simulator executables bundled in the devices' .uvdev
/// packages as separate child processes, and tracks them so they can be
/// monitored and killed.
///
/// Each simulator runs in its own temporary directory (the .uvdev extraction
/// dir), where it keeps its .eeprom / .nvconf state, isolated from the others.
/// The directory is removed when the simulator stops. Every child is launched
/// with PR_SET_PDEATHSIG so the kernel kills it if uvcan dies unexpectedly, and
/// the temp dirs are swept on the next startup (see system_init_tmp_cleanup()).
///
/// This is a Linux-only feature; on other targets every function is a no-op.


/// @brief: Maximum number of simulator processes (one per system device).
#define SIMRUN_MAX			SYSTEM_DEV_MAX_COUNT


/// @brief: State of a tracked simulator. Stopped simulators are kept in the list
/// (so their final state and log stay visible) until the next simrun_start_system().
typedef enum {
	/// @brief: the process is running
	SIMRUN_RUNNING = 0,
	/// @brief: stopped by the user (Kill button, or Ctrl-C in its log terminal)
	SIMRUN_KILLED,
	/// @brief: exited on its own (crash, or normal exit for any other reason)
	SIMRUN_STOPPED,
} simrun_state_e;


/// @brief: Registers an atexit() handler that kills every running simulator and
/// removes its temp dir when uvcan exits. Call once at startup.
void simrun_init(void);


/// @brief: Launches the Linux simulator of every device in *sys* that has a
/// configuration package (.uvdev), each in its own temp directory and connected
/// to *can_channel* with the device's node id (passed as the simulator's -c and
/// -n arguments). Any already-running simulators are killed first. Returns the
/// number of simulators started.
uint8_t simrun_start_system(system_st *sys, const char *can_channel);


/// @brief: Reaps simulator processes that have exited and frees their slots and
/// temp dirs. Call periodically (e.g. from the UI step). Returns true when the
/// running set changed (a process exited), so the caller can refresh its view.
bool simrun_step(void);


/// @brief: Number of tracked simulator processes.
uint8_t simrun_get_count(void);


/// @brief: Display name (device name) of simulator *index*, or "" if invalid.
const char *simrun_get_name(uint8_t index);


/// @brief: OS process id of simulator *index*, or 0 if invalid.
int simrun_get_pid(uint8_t index);


/// @brief: CANopen node id simulator *index* was started with, or 0 if invalid.
uint8_t simrun_get_nodeid(uint8_t index);


/// @brief: State of simulator *index* (running / killed / stopped).
simrun_state_e simrun_get_state(uint8_t index);


/// @brief: Human-readable state of simulator *index*: "Running", "Killed" or
/// "Stopped".
const char *simrun_get_state_str(uint8_t index);


/// @brief: True when at least one simulator is currently running.
bool simrun_any_running(void);


/// @brief: Opens a terminal window showing simulator *index*'s live output (its
/// stdout/stderr log), as if the binary were run directly from a shell. The
/// terminal follows the output until the simulator exits, and Ctrl-C in it stops
/// the simulator. No-op when *index* is invalid or no terminal emulator is found.
void simrun_open_log(uint8_t index);


/// @brief: Kills simulator *index* (SIGTERM, then SIGKILL if needed) and removes
/// its temp dir. Remaining simulators keep their order, compacted down.
void simrun_kill(uint8_t index);


/// @brief: Kills every running simulator and removes all temp dirs.
void simrun_kill_all(void);


#endif /* SIMRUN_H_ */
