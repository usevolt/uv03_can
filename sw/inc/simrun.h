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


/// @brief: State of a tracked simulator. The states up to (and including)
/// SIMRUN_RUNNING are "alive" (the process is running); KILLED and STOPPED are
/// terminal. Stopped/killed simulators are kept in the list (so their final state
/// and log stay visible) until the next simrun_start_system().
typedef enum {
	/// @brief: just launched, not yet confirmed running / before parameters
	SIMRUN_STARTED = 0,
	/// @brief: parameters are being loaded onto the device
	SIMRUN_PARAM,
	/// @brief: running normally
	SIMRUN_RUNNING,
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
///
/// Devices that are already online (present on the bus as real hardware) are NOT
/// simulated - a simulator would clash with the real device on its node id - so
/// they are skipped. The caller must refresh the device states with
/// find_update_device_states() first, and handle the online devices itself (see
/// simrun_load_params_async()'s restore list).
uint8_t simrun_start_system(system_st *sys, const char *can_channel);


/// @brief: True when *can_channel* names a virtual SocketCAN interface
/// (vcan / vxcan). On a virtual bus the simulators can exchange frames freely;
/// on a real CAN device every frame must be acknowledged by another node, so at
/// least one real device must be present on the bus for the simulators to
/// communicate (and for their parameters to load).
bool simrun_can_is_virtual(const char *can_channel);


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


/// @brief: Human-readable state of simulator *index*: "Started", "Loading
/// params", "Running", "Killed" or "Stopped".
const char *simrun_get_state_str(uint8_t index);


/// @brief: Returns (and clears) whether any simulator's state changed since the
/// last call. Lets the UI rebuild the simulator list only when it actually
/// changed (a sim started, loaded params, became running, was killed or exited).
bool simrun_poll_changed(void);


/// @brief: True when at least one simulator is alive (started, loading params or
/// running).
bool simrun_any_running(void);


/// @brief: Starts the post-launch parameter load on its own task: waits until the
/// managed devices are operational on the bus, then loads each device's bundled
/// parameters (the EMCY-suppress / write / store / reset sequence), moving each
/// simulator through PARAM and finally to RUNNING. Devices without a param file
/// just transition from STARTED to RUNNING once online. Call after
/// simrun_start_system(). Poll simrun_load_params_is_finished().
///
/// *restore_nodeids* (of length *restore_count*) lists the online real devices
/// that were not simulated (they were skipped by simrun_start_system() because
/// they are present on the bus): each is first restored to its defaults and reset,
/// then has the system parameters loaded onto it alongside the simulated devices.
/// Pass NULL / 0 when every device is simulated.
///
/// simrun_kill_all() cancels an in-progress load (used by the Force-stop button).
void simrun_load_params_async(system_st *sys,
		const uint8_t *restore_nodeids, uint8_t restore_count);


/// @brief: Returns true when no post-launch parameter load is running (i.e. the
/// last simrun_load_params_async() has completed). Returns true before any such
/// load is started.
bool simrun_load_params_is_finished(void);


/// @brief: Opens a terminal window showing simulator *index*'s live output (its
/// stdout/stderr log), as if the binary were run directly from a shell. The
/// terminal follows the output until the simulator exits, and Ctrl-C in it stops
/// the simulator. No-op when *index* is invalid or no terminal emulator is found.
void simrun_open_log(uint8_t index);


/// @brief: Kills simulator *index* (SIGTERM, then SIGKILL if needed) and removes
/// its temp dir. Remaining simulators keep their order, compacted down.
void simrun_kill(uint8_t index);


/// @brief: Kills every running simulator and removes all temp dirs. Also cancels
/// an in-progress post-launch parameter load (see simrun_load_params_async()), so
/// this doubles as the Force-stop action while the simulators are starting up or
/// loading parameters.
void simrun_kill_all(void);


#endif /* SIMRUN_H_ */
