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


#include "simrun.h"
#include "uvdev.h"
#include "main.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#if !CONFIG_TARGET_WIN

#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <uv_rtos.h>
#include <uv_canopen.h>
#include "find.h"
#include "loadparam.h"
#include "uvstdin.h"


// A single running simulator child process.
typedef struct {
	// the extracted .uvdev package; pkg.dir is the simulator's working directory
	// (where it keeps its .eeprom / .nvconf), removed when the simulator stops
	uvdev_st pkg;
	// child process id, 0 when the slot is empty
	int pid;
	// device name, for display
	char name[128];
	// CANopen node id the simulator was started with
	uint8_t nodeid;
	// the CAN channel it was started on and whether its node id was pinned with
	// -n: kept so simrun_restart() can launch it again exactly the same way
	char can_channel[64];
	bool set_nodeid;
	// running / killed / stopped. Stopped entries are kept (so their final state
	// and log stay visible) until the next simrun_start_system().
	simrun_state_e state;
} simproc_st;


static simproc_st procs[SIMRUN_MAX];
static uint8_t proc_count;
static bool atexit_registered;

// the system the running simulators were started from, so simrun_restart() finds
// the device (and its parameters) belonging to a simulator
static system_st *sim_sys;

// set whenever any simulator's state changes, so the UI can refresh only when
// needed; read and cleared by simrun_poll_changed()
static volatile bool state_changed;

// set by simrun_kill_all() (the Force-stop button) so a running post-launch load
// stops waiting for / loading devices that are about to be killed. Cleared when a
// new load is started with simrun_load_params_async().
static volatile bool pp_cancel;


// True while the simulator is alive (running, just started, or loading params).
static bool state_is_alive(simrun_state_e s) {
	return s <= SIMRUN_RUNNING;
}


// Sets *p* to *st* and flags a state change for the UI.
static void set_state(simproc_st *p, simrun_state_e st) {
	if (p->state != st) {
		p->state = st;
		state_changed = true;
	}
}


bool simrun_poll_changed(void) {
	bool ret = state_changed;
	state_changed = false;
	return ret;
}


// How long a simulator is given to exit on SIGTERM before it is killed, and how
// often it is checked on while that runs out.
#define SIMRUN_TERM_GRACE_MS	500
#define SIMRUN_TERM_POLL_MS		10


// True when *p* has a process of its own to stop.
static bool proc_is_running(const simproc_st *p) {
	return (state_is_alive(p->state) && (p->pid > 0));
}


// Reaps *p*'s process if it has exited. Returns true once it is gone (or was
// never ours to reap), false while it is still running.
static bool reap_proc(simproc_st *p) {
	int status;
	pid_t r = waitpid(p->pid, &status, WNOHANG);
	return ((r == p->pid) || ((r == -1) && (errno == ECHILD)));
}


// Stops *p*'s process if it is running: SIGTERM, then SIGKILL if it does not
// exit, and reaps it. Does not change *p*'s state or remove its temp dir.
static void terminate_proc(simproc_st *p) {
	if (proc_is_running(p)) {
		kill(p->pid, SIGTERM);
		bool reaped = false;
		for (uint16_t t = 0; (t < SIMRUN_TERM_GRACE_MS) && !reaped;
				t += SIMRUN_TERM_POLL_MS) {
			reaped = reap_proc(p);
			if (!reaped) {
				usleep(SIMRUN_TERM_POLL_MS * 1000);
			}
			else {
			}
		}
		if (!reaped) {
			kill(p->pid, SIGKILL);
			while ((waitpid(p->pid, NULL, 0) == -1) && (errno == EINTR)) {
			}
		}
		else {
		}
	}
	else {
	}
}


// Stops every tracked simulator: SIGTERM to all of them first, then one shared
// wait for them to go, then SIGKILL to whatever is left. The wait is shared
// rather than taken one simulator at a time so stopping a whole system takes the
// same half second as stopping a single simulator - this is also the wait the
// signal handler sits in when the user closes the window, where every extra half
// second is one the program spends not ending.
static void terminate_all(void) {
	for (uint8_t i = 0; i < proc_count; i++) {
		if (proc_is_running(&procs[i])) {
			kill(procs[i].pid, SIGTERM);
		}
		else {
		}
	}

	bool all_reaped = false;
	for (uint16_t t = 0; (t < SIMRUN_TERM_GRACE_MS) && !all_reaped;
			t += SIMRUN_TERM_POLL_MS) {
		all_reaped = true;
		for (uint8_t i = 0; i < proc_count; i++) {
			if (proc_is_running(&procs[i])) {
				if (reap_proc(&procs[i])) {
					// gone: nothing left to wait for, and nothing to kill below
					procs[i].pid = 0;
				}
				else {
					all_reaped = false;
				}
			}
			else {
			}
		}
		if (!all_reaped) {
			usleep(SIMRUN_TERM_POLL_MS * 1000);
		}
		else {
		}
	}

	for (uint8_t i = 0; i < proc_count; i++) {
		if (proc_is_running(&procs[i])) {
			kill(procs[i].pid, SIGKILL);
			while ((waitpid(procs[i].pid, NULL, 0) == -1) && (errno == EINTR)) {
			}
		}
		else {
		}
	}
}


// Stops every tracked simulator and removes all their temp dirs, emptying the
// list. Used before a fresh run and on exit.
static void clear_all(void) {
	terminate_all();
	for (uint8_t i = 0; i < proc_count; i++) {
		if (strlen(procs[i].pkg.dir) != 0) {
			uvdev_close(&procs[i].pkg);
		}
		else {
		}
	}
	proc_count = 0;
	memset(procs, 0, sizeof(procs));
	state_changed = true;
}


// Forks and execs the simulator of *p* in its own working directory, passing the
// CAN channel as -c and, when *set_nodeid* is true, the node id as -n. Without
// -n the simulator boots with the node id stored in its own package (its
// default) and is free to move itself to another one if that one is already
// taken on the bus. Returns true on success.
static bool spawn_proc(simproc_st *p, const char *can_channel, bool set_nodeid) {
	bool ret = false;

	char binpath[2048];
	snprintf(binpath, sizeof(binpath), "%s/%s", p->pkg.dir, p->pkg.linux_bin);
	// the simulator is extracted from a zip; make sure it is executable
	chmod(binpath, 0755);

	char nodeid_str[16];
	snprintf(nodeid_str, sizeof(nodeid_str), "0x%x", (unsigned int) p->nodeid);
	char logpath[2100];
	snprintf(logpath, sizeof(logpath), "%s/sim.log", p->pkg.dir);

	// build the argument vectors before forking: the child may only call
	// async-signal-safe functions between fork() and exec(). The second vector
	// runs the same command under stdbuf (see the exec calls below).
	char *argv[8];
	char *sargv[11];
	uint8_t argc = 0;
	argv[argc++] = binpath;
	argv[argc++] = "-c";
	argv[argc++] = (char *) can_channel;
	if (set_nodeid) {
		argv[argc++] = "-n";
		argv[argc++] = nodeid_str;
	}
	argv[argc] = NULL;
	sargv[0] = "stdbuf";
	sargv[1] = "-oL";
	sargv[2] = "-eL";
	for (uint8_t i = 0; i <= argc; i++) {
		sargv[i + 3] = argv[i];
	}

	// log the exact command so the user can see which CAN device (-c) and node id
	// (-n) each simulator is started with
	PRINT("Simulator command: \"%s\" -c %s%s%s  (cwd: %s)\n",
			binpath, can_channel, set_nodeid ? " -n " : "",
			set_nodeid ? nodeid_str : "", p->pkg.dir);

	pid_t pid = fork();
	if (pid == 0) {
		// child: only async-signal-safe calls until exec
		// ask the kernel to kill us if uvcan (our parent) dies, even on a crash
		prctl(PR_SET_PDEATHSIG, SIGKILL);
		if (getppid() == 1) {
			// parent already died between fork and prctl
			_exit(1);
		}
		// uvcan is itself a FreeRTOS POSIX-port program whose scheduler tick runs
		// on ITIMER_REAL / SIGALRM. The fork inherits that running timer; stop it
		// and ignore SIGALRM (SIG_IGN survives execve) so the inherited tick cannot
		// fire into the simulator before it installs its own handler - otherwise it
		// kills the simulator during the gap right after its first self-restart.
		// The simulator reinstalls its own timer and handler during init.
		struct itimerval notimer = { { 0, 0 }, { 0, 0 } };
		setitimer(ITIMER_REAL, &notimer, NULL);
		signal(SIGALRM, SIG_IGN);
		// the simulator drives its scheduler from timer signals; clear any signal
		// mask inherited from uvcan's threads so those signals are not blocked
		sigset_t set;
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, NULL);
		if (chdir(p->pkg.dir) != 0) {
			_exit(1);
		}
		// detach into a new session so the simulator does not share uvcan's
		// controlling terminal or process group
		setsid();
		// keep the simulator's noisy output out of uvcan's console: send it to a
		// log file inside the run directory
		int fd = open(logpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd >= 0) {
			dup2(fd, STDOUT_FILENO);
			dup2(fd, STDERR_FILENO);
			if (fd > STDERR_FILENO) {
				close(fd);
			}
		}
		// close every other descriptor inherited from uvcan so none of its open
		// files (CAN socket, FreeRTOS-port internals, ...) leak into the simulator,
		// which opens its own
		for (int fdn = STDERR_FILENO + 1; fdn < 1024; fdn++) {
			close(fdn);
		}
		// run under stdbuf so the simulator's stdout/stderr are line-buffered: its
		// output then reaches sim.log immediately (instead of only when a full
		// buffer flushes), so the log terminal shows it live from the start
		execvp("stdbuf", sargv);
		// stdbuf not available: run the simulator directly
		execv(binpath, argv);
		// exec failed
		_exit(127);
	}
	else if (pid > 0) {
		p->pid = pid;
		p->state = SIMRUN_STARTED;
		state_changed = true;
		ret = true;
	}
	else {
		// fork failed
	}
	return ret;
}


void simrun_init(void) {
	if (!atexit_registered) {
		atexit(&simrun_kill_all);
		atexit_registered = true;
	}
}


bool simrun_can_is_virtual(const char *can_channel) {
	// SocketCAN virtual interfaces are conventionally named vcanN / vxcanN. They
	// loop every frame back to all local sockets regardless of bus acknowledgement,
	// so the simulators can talk to each other and to uvcan with no real hardware.
	return (can_channel != NULL) &&
			((strncmp(can_channel, "vcan", 4) == 0) ||
			 (strncmp(can_channel, "vxcan", 5) == 0));
}


uint8_t simrun_start_system(system_st *sys, const char *can_channel) {
	// the list is cleared only here: drop every previous entry (running, killed
	// or stopped) and start fresh
	clear_all();
	sim_sys = sys;

	// With no system configuration file the devices come from --dev (or from the
	// UI) and nothing is loaded onto them afterwards, so nothing depends on a
	// simulator keeping the node id it was started with. A device still sitting on
	// its package default is then started without -n, leaving it free to move
	// itself to another node id when that default is already taken on the bus
	// (two simulators of the same device, or a real device answering there). A
	// device given an explicit node id keeps it: that id is the reason it was
	// given. With a system file the node ids are the system's own and the
	// parameters are loaded per node id, so every simulator is pinned with -n.
	bool free_default_nodeids = !system_is_sysfile_loaded(sys);

	uint8_t started = 0;
	for (uint8_t i = 0; (i < system_get_dev_count(sys)) &&
			(proc_count < SIMRUN_MAX); i++) {
		device_st *d = system_get_dev(sys, i);
		// only devices that carry a configuration package can be simulated
		if ((d == NULL) || (strlen(d->filepath) == 0)) {
			continue;
		}
		// a device that is present on the bus (real hardware) is not simulated: a
		// simulator would clash with it on the same node id. The caller handles
		// those devices separately (restores their defaults and loads the system
		// parameters onto them). The caller refreshes the device states with
		// find_update_device_states() before calling, so this reflects the bus.
		if (d->state != DEV_STATE_OFFLINE) {
			PRINT("Device '%s' (node 0x%x) is already online; not simulating it.\n",
					(strlen(d->devname) > 0) ? d->devname : d->name,
					(unsigned int) d->nodeid);
			continue;
		}

		simproc_st *p = &procs[proc_count];
		memset(p, 0, sizeof(*p));
		if (!uvdev_open(&p->pkg, d->filepath)) {
			PRINT("Failed to open package '%s' for simulation.\n", d->filepath);
			continue;
		}
		if (strlen(p->pkg.linux_bin) == 0) {
			PRINT("Package '%s' has no LINUX_BIN simulator; skipping.\n",
					d->filepath);
			uvdev_close(&p->pkg);
			memset(p, 0, sizeof(*p));
			continue;
		}

		const char *nm = (strlen(d->devname) > 0) ? d->devname : d->name;
		strncpy(p->name, nm, sizeof(p->name) - 1);
		p->nodeid = d->nodeid;

		// the device is on its package default when no explicit node id was given
		// for it (system_add_device() then adopts the database's own)
		bool is_default = (d->default_nodeid != 0) &&
				(d->nodeid == d->default_nodeid);
		strncpy(p->can_channel, can_channel, sizeof(p->can_channel) - 1);
		p->set_nodeid = !(free_default_nodeids && is_default);
		if (spawn_proc(p, p->can_channel, p->set_nodeid)) {
			proc_count++;
			started++;
			PRINT("Started simulator for '%s' (node 0x%x), pid %d in '%s'\n",
					p->name, (unsigned int) p->nodeid, p->pid, p->pkg.dir);
		}
		else {
			PRINT("Failed to launch the simulator for '%s'.\n", d->filepath);
			if (strlen(p->pkg.dir) != 0) {
				uvdev_close(&p->pkg);
			}
			memset(p, 0, sizeof(*p));
		}
	}
	return started;
}


bool simrun_step(void) {
	bool changed = false;
	// reap simulators that have exited on their own, but keep their entries: the
	// list is only cleared by simrun_start_system(). SIGTERM means the user
	// stopped it (Kill button reaps before this runs, Ctrl-C in the log terminal
	// sends SIGTERM); anything else is treated as a crash / other stop.
	for (uint8_t i = 0; i < proc_count; i++) {
		simproc_st *p = &procs[i];
		if (!state_is_alive(p->state)) {
			continue;
		}
		int status;
		pid_t r = waitpid(p->pid, &status, WNOHANG);
		bool exited = false;
		bool by_term = false;
		if (r == p->pid) {
			exited = true;
			if (WIFSIGNALED(status) && (WTERMSIG(status) == SIGTERM)) {
				by_term = true;
			}
		}
		else if ((r == -1) && (errno == ECHILD)) {
			// already reaped elsewhere; cause unknown
			exited = true;
		}
		else if ((r == 0) && (kill(p->pid, 0) != 0) && (errno == ESRCH)) {
			exited = true;
		}
		else {
			// still running
		}
		if (exited) {
			set_state(p, by_term ? SIMRUN_KILLED : SIMRUN_STOPPED);
			PRINT("Simulator '%s' (pid %d) %s.\n", p->name, p->pid,
					by_term ? "was stopped" : "stopped");
			changed = true;
		}
	}
	return changed;
}


uint8_t simrun_get_count(void) {
	return proc_count;
}


const char *simrun_get_name(uint8_t index) {
	return (index < proc_count) ? procs[index].name : "";
}


int simrun_get_pid(uint8_t index) {
	return (index < proc_count) ? procs[index].pid : 0;
}


uint8_t simrun_get_nodeid(uint8_t index) {
	return (index < proc_count) ? procs[index].nodeid : 0;
}


simrun_state_e simrun_get_state(uint8_t index) {
	return (index < proc_count) ? procs[index].state : SIMRUN_STOPPED;
}


const char *simrun_get_state_str(uint8_t index) {
	const char *ret = "Stopped";
	if (index < proc_count) {
		switch (procs[index].state) {
		case SIMRUN_STARTED:
			ret = "Started";
			break;
		case SIMRUN_PARAM:
			ret = "Loading parameters";
			break;
		case SIMRUN_RUNNING:
			ret = "Running";
			break;
		case SIMRUN_KILLED:
			ret = "Killed";
			break;
		case SIMRUN_STOPPED:
		default:
			ret = "Stopped";
			break;
		}
	}
	return ret;
}


bool simrun_is_alive(uint8_t index) {
	return (index < proc_count) && state_is_alive(procs[index].state);
}


bool simrun_any_running(void) {
	bool ret = false;
	for (uint8_t i = 0; i < proc_count; i++) {
		if (state_is_alive(procs[i].state)) {
			ret = true;
			break;
		}
	}
	return ret;
}


void simrun_open_log(uint8_t index) {
	if (index >= proc_count) {
		return;
	}
	simproc_st *p = &procs[index];
	if (p->pid <= 0) {
		return;
	}
	// a stopped simulator still has its sim.log in its (kept) temp dir, so the
	// terminal can still show the final output

	// Write a small viewer script into the simulator's run directory. Generating
	// a script file (rather than a deeply nested -e command line) keeps the shell
	// quoting simple. It traps Ctrl-C to stop the simulator, then follows its log
	// until the process exits.
	// p->pkg.dir is at most sizeof(uvdev_st::dir) (1024) chars
	char scriptpath[1100];
	snprintf(scriptpath, sizeof(scriptpath), "%s/showlog.sh", p->pkg.dir);
	FILE *f = fopen(scriptpath, "w");
	if (f == NULL) {
		PRINT("Could not write the simulator log viewer script for '%s'.\n",
				p->name);
		return;
	}
	fprintf(f,
			"#!/bin/sh\n"
			"trap 'kill %d 2>/dev/null' INT\n"
			"echo 'Simulator %s (pid %d). Press Ctrl-C to stop it.'\n"
			"echo '----------------------------------------'\n"
			"tail -n +1 --pid=%d -f '%s/sim.log'\n"
			"echo\n"
			"echo '[simulator exited - press Enter to close]'\n"
			"read dummy\n",
			p->pid, p->name, p->pid, p->pid, p->pkg.dir);
	fclose(f);
	chmod(scriptpath, 0755);

	// Launch a terminal emulator running the script, trying the common ones in
	// turn. Backgrounded so uvcan is not blocked; output of the launcher itself is
	// discarded.
	char cmd[8192];
	snprintf(cmd, sizeof(cmd),
			"{ if command -v x-terminal-emulator >/dev/null 2>&1; then "
			"x-terminal-emulator -e sh '%s'; "
			"elif command -v gnome-terminal >/dev/null 2>&1; then "
			"gnome-terminal -- sh '%s'; "
			"elif command -v konsole >/dev/null 2>&1; then "
			"konsole -e sh '%s'; "
			"elif command -v xterm >/dev/null 2>&1; then "
			"xterm -e sh '%s'; "
			"else echo 'simrun: no terminal emulator found' >&2; fi; } "
			">/dev/null 2>&1 &",
			scriptpath, scriptpath, scriptpath, scriptpath);
	if (system(cmd) != 0) {
		// best effort; the launcher was backgrounded so this rarely reports
	}
	PRINT("Opened log terminal for simulator '%s' (pid %d).\n",
			p->name, p->pid);
}


void simrun_kill(uint8_t index) {
	// stop the process but keep its entry (marked Killed) and its temp dir, so it
	// stays in the list with its log available until the next run
	if ((index < proc_count) && state_is_alive(procs[index].state)) {
		terminate_proc(&procs[index]);
		set_state(&procs[index], SIMRUN_KILLED);
	}
}


void simrun_kill_all(void) {
	// cancel any in-progress post-launch parameter load so it stops waiting for
	// (or loading) the devices that are about to be killed
	pp_cancel = true;
	clear_all();
}


// ---- post-launch parameter load ------------------------------------------

// How long to wait for the simulated devices to come operational before loading.
#define SIMRUN_OP_WAIT_MS		15000

// How often the running parameter load is checked on.
#define SIMRUN_PARAM_POLL_MS	200

// How long the parameter load may go without any progress before it is given up
// on. There is no sensible upper bound for the load itself - a full system's
// parameters take minutes over a 250 kbit bus - so what is watched is progress
// rather than total time: loadparam_get_progress_counter() advances on every SDO
// transfer, and only a load which stops advancing for this long counts as stuck.
// Long enough to cover the SDO client's own retries and a device which is busy
// storing, short enough that a load which will never finish does not leave the
// user staring at "Loading params" with no way to stop the simulators.
#define SIMRUN_PARAM_STALL_MS	30000

static system_st *pp_sys;
static volatile bool pp_finished = true;
// node ids of the online real devices that this load manages instead of
// simulating: each is restored to its defaults and reset, then has the system
// parameters loaded onto it alongside the simulated devices
static uint8_t pp_restore[SIMRUN_MAX];
static uint8_t pp_restore_count;


bool simrun_load_params_is_finished(void) {
	return pp_finished;
}


// True when *nodeid* is one of the online real devices managed (not simulated)
// by the current post-launch load.
static bool is_restore_node(uint8_t nodeid) {
	bool ret = false;
	for (uint8_t i = 0; i < pp_restore_count; i++) {
		if (pp_restore[i] == nodeid) {
			ret = true;
			break;
		}
	}
	return ret;
}


// True when some alive simulator carries device *nodeid*.
static bool has_alive_sim(uint8_t nodeid) {
	bool ret = false;
	for (uint8_t i = 0; i < proc_count; i++) {
		if ((procs[i].nodeid == nodeid) && state_is_alive(procs[i].state)) {
			ret = true;
			break;
		}
	}
	return ret;
}


// Moves the alive simulator of device *nodeid* to *st*.
static void set_sim_state_by_nodeid(uint8_t nodeid, simrun_state_e st) {
	for (uint8_t i = 0; i < proc_count; i++) {
		if ((procs[i].nodeid == nodeid) && state_is_alive(procs[i].state)) {
			set_state(&procs[i], st);
		}
	}
}


/// @brief: Waits for the running loadparam system load to finish, but only while
/// it is getting somewhere: a load which stops making progress (a device that
/// never answers, a bug in the load itself) would otherwise keep the caller - and
/// with it the UI's busy state - waiting forever, with no way to stop the
/// simulators. Shared by the post-launch load and a single simulator's restart.
static void wait_for_param_load(void) {
	uint32_t last_progress = loadparam_get_progress_counter();
	uint32_t stalled = 0;
	while (!loadparam_load_system_is_finished() &&
			(stalled < SIMRUN_PARAM_STALL_MS) && !pp_cancel) {
		uv_rtos_task_delay(SIMRUN_PARAM_POLL_MS);
		uint32_t progress = loadparam_get_progress_counter();
		// a load which is waiting for the user to answer a prompt is not stalled,
		// however long the user takes to answer it
		if ((progress != last_progress) || uv_stdin_is_waiting()) {
			last_progress = progress;
			stalled = 0;
		}
		else {
			stalled += SIMRUN_PARAM_POLL_MS;
		}
	}
	if (!loadparam_load_system_is_finished() && !pp_cancel) {
		PRINT("ERROR: the parameter load has not made any progress in "
				"%u seconds and\n"
				"is given up on. It is still running in the background; "
				"stop the simulators\n"
				"with \"Force stop simulator\" (or the Kill buttons) if "
				"it does not recover.\n",
				(unsigned int) (SIMRUN_PARAM_STALL_MS / 1000));
		fflush(stdout);
	}
}


/// @brief: Waits until every managed device (a simulator or a restored real
/// device) is operational, then loads the devices' bundled parameters onto them.
/// Called by postparam_task() when there is something to load.
static void postparam_load(system_st *sys) {
	// 1. wait until every managed device (a simulated device or a restored real
	// device) reports OPERATIONAL (or time out). Tell the user we are waiting,
	// since it takes a few seconds while the devices boot.
	PRINT("Waiting for all devices to come online before loading parameters...\n");
	fflush(stdout);
	uint32_t waited = 0;
	bool all_op = false;
	while (!all_op && (waited < SIMRUN_OP_WAIT_MS) && !pp_cancel) {
		find_update_device_states(sys);
		all_op = true;
		for (uint8_t i = 0; i < system_get_dev_count(sys); i++) {
			device_st *d = system_get_dev(sys, i);
			bool managed = has_alive_sim(d->nodeid) || is_restore_node(d->nodeid);
			if (managed && (d->state != DEV_STATE_OP)) {
				all_op = false;
				break;
			}
		}
		if (!all_op) {
			uv_rtos_task_delay(500);
			waited += 500;
		}
	}

	if (!pp_cancel) {
		PRINT(all_op ? "All devices are online. Loading parameters...\n" :
				"Timed out waiting for devices to come online; loading "
				"parameters to those that are ready...\n");
		fflush(stdout);

		// 2. collect operational devices that have a saved parameter file and are
		// managed (an alive simulator or a restored real device), marking each
		// simulator as loading parameters
		device_st *targets[SIMRUN_MAX];
		uint8_t n = 0;
		for (uint8_t i = 0; i < system_get_dev_count(sys); i++) {
			device_st *d = system_get_dev(sys, i);
			bool managed = has_alive_sim(d->nodeid) || is_restore_node(d->nodeid);
			if ((strlen(d->param_file) != 0) && (d->state == DEV_STATE_OP) &&
					managed && (n < SIMRUN_MAX)) {
				set_sim_state_by_nodeid(d->nodeid, SIMRUN_PARAM);
				targets[n] = d;
				n++;
			}
		}

		// 3. load the parameters: suppress EMCY on all, write each device, re-enable
		// EMCY, store and reset all (handled by loadparam_load_system_async)
		if (n == 0) {
			// Nothing to load. Worth saying out loud: the load is what the user
			// is waiting for, and silence here is indistinguishable from a hang.
			// A system package carries no parameters when reading them from the
			// devices failed while it was saved, so point at that.
			PRINT("None of the devices has any parameters saved in the system "
					"configuration;\n"
					"there is nothing to load and the simulators run with their "
					"default settings.\n"
					"Save the system configuration again if this is not what you "
					"expected.\n");
			fflush(stdout);
		}
		else {
			loadparam_load_system_async(targets, n);
			wait_for_param_load();
		}
	}
}


/// @brief: Task body for simrun_load_params_async(). Restores the online real
/// devices to defaults, waits for every managed device (simulated or restored) to
/// come operational, loads each device's bundled parameters and moves the
/// simulators STARTED -> PARAM -> RUNNING. Aborts early if the load is cancelled
/// with simrun_kill_all() (the Force-stop button).
static void postparam_task(void *ptr) {
	system_st *sys = pp_sys;

	// make sure heartbeats are tracked so the operational state can be detected
	find_start_monitor();

	// 0. the online real devices are not simulated; bring each back to the same
	// known state a freshly started simulator would be in: restore its defaults
	// and reset it, so only the parameters loaded below deviate from the defaults.
	if (pp_restore_count > 0) {
		for (uint8_t i = 0; (i < pp_restore_count) && !pp_cancel; i++) {
			PRINT("Restoring online device (node 0x%x) to system defaults...\n",
					(unsigned int) pp_restore[i]);
			uv_canopen_sdo_restore_params(pp_restore[i], MEMORY_ALL_PARAMS);
			uv_canopen_nmt_master_send_cmd(pp_restore[i],
					CANOPEN_NMT_CMD_RESET_NODE);
		}
		// let the reset devices drop off the bus before waiting for them to come
		// back, so the loop below does not see their pre-reset operational state
		// and proceed while they are still resetting
		uv_rtos_task_delay(1500);
	}

	// 1. - 3. wait for the devices and load their parameters. Skipped when there
	// is nothing to load: no online real device was restored and no device carries
	// a parameter file. That is the normal case when no system configuration file
	// is loaded, since the parameters are part of the system file - simulators
	// started from plain device packages just run with their default settings.
	// The wait is skipped along with the load: without a system file a simulator
	// may end up on another node id than the one it was started with (see
	// simrun_start_system()), so waiting for its node to appear would only stall
	// the start until the timeout.
	bool nothing_to_load = (pp_restore_count == 0);
	for (uint8_t i = 0; nothing_to_load && (i < system_get_dev_count(sys)); i++) {
		device_st *d = system_get_dev(sys, i);
		if ((d != NULL) && (strlen(d->param_file) != 0)) {
			nothing_to_load = false;
		}
	}
	if (nothing_to_load) {
		PRINT("No parameters to load; the simulators run with their default "
				"settings.\n");
		fflush(stdout);
	}
	else {
		postparam_load(sys);
	}

	// 4. every simulator that is still alive is now running. Also done when the
	// load above was given up on or cancelled: the state is what enables the per
	// simulator Kill button, so leaving them in "Loading params" would take away
	// the very thing the user needs after a stalled load.
	for (uint8_t i = 0; i < proc_count; i++) {
		if ((procs[i].state == SIMRUN_STARTED) ||
				(procs[i].state == SIMRUN_PARAM)) {
			set_state(&procs[i], SIMRUN_RUNNING);
		}
	}

	pp_finished = true;
	uv_rtos_task_delete(NULL);
}


// ---- restarting a single simulator ---------------------------------------

// the simulator restart_task() is working on
static volatile uint8_t restart_index;


// The system's device sitting on node *nodeid*, or NULL when there is none.
static device_st *sys_dev_by_nodeid(uint8_t nodeid) {
	device_st *ret = NULL;
	if (sim_sys != NULL) {
		for (uint8_t i = 0; i < system_get_dev_count(sim_sys); i++) {
			device_st *d = system_get_dev(sim_sys, i);
			if ((d != NULL) && (d->nodeid == nodeid)) {
				ret = d;
				break;
			}
		}
	}
	return ret;
}


/// @brief: Task body for simrun_restart(): waits for the relaunched simulator to
/// come operational, loads its device's parameters onto it and moves it
/// STARTED -> PARAM -> RUNNING, the same way the post-launch load does after a
/// start. Gives up on a simulator that does not come online (or is killed again
/// while it is waited for), leaving it in whatever state it ended up in.
static void restart_task(void *ptr) {
	simproc_st *p = &procs[restart_index];
	device_st *d = sys_dev_by_nodeid(p->nodeid);

	// As after a start, the wait and the load are skipped when the device carries
	// no parameters - the normal case when no system configuration file is
	// loaded. Such a simulator may also have been started without -n and be free
	// to boot on another node id than the one it is tracked with, so waiting for
	// its node to answer would only stall the restart until the timeout.
	if ((d == NULL) || (strlen(d->param_file) == 0)) {
		PRINT("No parameters to load for '%s'; it runs with the settings stored "
				"in its own run directory.\n", p->name);
		fflush(stdout);
	}
	else {
		// make sure heartbeats are tracked so the operational state can be seen
		find_start_monitor();
		// let the device drop off the bus before waiting for it to come back, so
		// the loop below does not see the heartbeat state it had before it was
		// stopped and load onto a device which is still booting
		uv_rtos_task_delay(1500);

		PRINT("Waiting for '%s' (node 0x%x) to come online...\n",
				p->name, (unsigned int) p->nodeid);
		fflush(stdout);
		uint32_t waited = 0;
		bool op = false;
		while (!op && (waited < SIMRUN_OP_WAIT_MS) && !pp_cancel &&
				state_is_alive(p->state)) {
			find_update_device_states(sim_sys);
			op = (d->state == DEV_STATE_OP);
			if (!op) {
				uv_rtos_task_delay(500);
				waited += 500;
			}
		}

		if (!op) {
			PRINT("Simulator '%s' did not come online; its parameters are not "
					"loaded.\n", p->name);
			fflush(stdout);
		}
		else if (!pp_cancel && state_is_alive(p->state)) {
			// the same EMCY-suppress / write / store / reset sequence the
			// post-launch load runs, for this one device
			set_state(p, SIMRUN_PARAM);
			PRINT("Loading the parameters of '%s' (node 0x%x)...\n",
					p->name, (unsigned int) p->nodeid);
			fflush(stdout);
			loadparam_load_system_async(&d, 1);
			wait_for_param_load();
		}
		else {
			// stopped again, or the whole run was cancelled, while waiting
		}
	}

	// as after a start, a simulator that is still alive is now running - also when
	// the load above was given up on, so the row's button works again. Not after a
	// Force stop: the list has been cleared under us and this slot may already
	// belong to a simulator of the next run.
	if (!pp_cancel && state_is_alive(p->state)) {
		set_state(p, SIMRUN_RUNNING);
	}

	pp_finished = true;
	uv_rtos_task_delete(NULL);
}


bool simrun_restart(uint8_t index) {
	bool ret = false;
	// only a stopped simulator can be restarted, and only while no parameter load
	// is running: the load owns the SDO client and the restart would fight it
	if ((index < proc_count) && !state_is_alive(procs[index].state) &&
			pp_finished && (strlen(procs[index].pkg.dir) != 0)) {
		simproc_st *p = &procs[index];
		PRINT("Restarting the simulator of '%s' (node 0x%x)...\n",
				p->name, (unsigned int) p->nodeid);
		// the run directory (and with it the device's stored settings) is still
		// there, so the simulator is simply launched in it again
		pp_cancel = false;
		if (spawn_proc(p, p->can_channel, p->set_nodeid)) {
			PRINT("Restarted simulator for '%s' (node 0x%x), pid %d in '%s'\n",
					p->name, (unsigned int) p->nodeid, p->pid, p->pkg.dir);
			restart_index = index;
			pp_finished = false;
			uv_rtos_task_create(&restart_task, "simrestart",
					UV_RTOS_MIN_STACK_SIZE * 5, NULL,
					UV_RTOS_IDLE_PRIORITY + 1, NULL);
			ret = true;
		}
		else {
			PRINT("Failed to relaunch the simulator of '%s'.\n", p->name);
		}
	}
	else {
		// invalid index, still running, or a parameter load is in progress
	}
	return ret;
}


void simrun_load_params_async(system_st *sys,
		const uint8_t *restore_nodeids, uint8_t restore_count) {
	pp_sys = sys;
	pp_cancel = false;
	if (restore_count > SIMRUN_MAX) {
		restore_count = SIMRUN_MAX;
	}
	pp_restore_count = restore_count;
	for (uint8_t i = 0; i < restore_count; i++) {
		pp_restore[i] = restore_nodeids[i];
	}
	pp_finished = false;
	uv_rtos_task_create(&postparam_task, "simparam",
			UV_RTOS_MIN_STACK_SIZE * 5, NULL, UV_RTOS_IDLE_PRIORITY + 1, NULL);
}


#else /* CONFIG_TARGET_WIN: simulators are a Linux-only feature */


void simrun_init(void) {
}

uint8_t simrun_start_system(system_st *sys, const char *can_channel) {
	(void) sys;
	(void) can_channel;
	return 0;
}

bool simrun_can_is_virtual(const char *can_channel) {
	(void) can_channel;
	return false;
}

bool simrun_step(void) {
	return false;
}

uint8_t simrun_get_count(void) {
	return 0;
}

const char *simrun_get_name(uint8_t index) {
	(void) index;
	return "";
}

int simrun_get_pid(uint8_t index) {
	(void) index;
	return 0;
}

uint8_t simrun_get_nodeid(uint8_t index) {
	(void) index;
	return 0;
}

simrun_state_e simrun_get_state(uint8_t index) {
	(void) index;
	return SIMRUN_STOPPED;
}

const char *simrun_get_state_str(uint8_t index) {
	(void) index;
	return "Stopped";
}

bool simrun_poll_changed(void) {
	return false;
}

bool simrun_is_alive(uint8_t index) {
	(void) index;
	return false;
}

bool simrun_any_running(void) {
	return false;
}

void simrun_load_params_async(system_st *sys,
		const uint8_t *restore_nodeids, uint8_t restore_count) {
	(void) sys;
	(void) restore_nodeids;
	(void) restore_count;
}

bool simrun_load_params_is_finished(void) {
	return true;
}

void simrun_open_log(uint8_t index) {
	(void) index;
}

void simrun_kill(uint8_t index) {
	(void) index;
}

bool simrun_restart(uint8_t index) {
	(void) index;
	return false;
}

void simrun_kill_all(void) {
}


#endif
