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
	// running / killed / stopped. Stopped entries are kept (so their final state
	// and log stay visible) until the next simrun_start_system().
	simrun_state_e state;
} simproc_st;


static simproc_st procs[SIMRUN_MAX];
static uint8_t proc_count;
static bool atexit_registered;


// Stops *p*'s process if it is running: SIGTERM, then SIGKILL if it does not
// exit, and reaps it. Does not change *p*'s state or remove its temp dir.
static void terminate_proc(simproc_st *p) {
	if ((p->state == SIMRUN_RUNNING) && (p->pid > 0)) {
		kill(p->pid, SIGTERM);
		// wait up to ~500 ms for a graceful exit
		bool reaped = false;
		for (int i = 0; (i < 50) && !reaped; i++) {
			int status;
			pid_t r = waitpid(p->pid, &status, WNOHANG);
			if ((r == p->pid) || ((r == -1) && (errno == ECHILD))) {
				reaped = true;
			}
			else {
				usleep(10000);
			}
		}
		if (!reaped) {
			kill(p->pid, SIGKILL);
			waitpid(p->pid, NULL, 0);
		}
	}
}


// Stops every tracked simulator and removes all their temp dirs, emptying the
// list. Used before a fresh run and on exit.
static void clear_all(void) {
	for (uint8_t i = 0; i < proc_count; i++) {
		terminate_proc(&procs[i]);
		if (strlen(procs[i].pkg.dir) != 0) {
			uvdev_close(&procs[i].pkg);
		}
	}
	proc_count = 0;
	memset(procs, 0, sizeof(procs));
}


// Forks and execs the simulator of *p* in its own working directory, passing the
// CAN channel and node id as -c / -n. Returns true on success.
static bool spawn_proc(simproc_st *p, const char *can_channel) {
	bool ret = false;

	char binpath[2048];
	snprintf(binpath, sizeof(binpath), "%s/%s", p->pkg.dir, p->pkg.linux_bin);
	// the simulator is extracted from a zip; make sure it is executable
	chmod(binpath, 0755);

	char nodeid_str[16];
	snprintf(nodeid_str, sizeof(nodeid_str), "0x%x", (unsigned int) p->nodeid);
	char logpath[2100];
	snprintf(logpath, sizeof(logpath), "%s/sim.log", p->pkg.dir);

	// log the exact command so the user can see which CAN device (-c) and node id
	// (-n) each simulator is started with
	PRINT("Simulator command: \"%s\" -c %s -n %s  (cwd: %s)\n",
			binpath, can_channel, nodeid_str, p->pkg.dir);

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
		execlp("stdbuf", "stdbuf", "-oL", "-eL", binpath,
				"-c", can_channel, "-n", nodeid_str, (char *) NULL);
		// stdbuf not available: run the simulator directly
		execl(binpath, binpath, "-c", can_channel, "-n", nodeid_str,
				(char *) NULL);
		// exec failed
		_exit(127);
	}
	else if (pid > 0) {
		p->pid = pid;
		p->state = SIMRUN_RUNNING;
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


uint8_t simrun_start_system(system_st *sys, const char *can_channel) {
	// the list is cleared only here: drop every previous entry (running, killed
	// or stopped) and start fresh
	clear_all();

	uint8_t started = 0;
	for (uint8_t i = 0; (i < system_get_dev_count(sys)) &&
			(proc_count < SIMRUN_MAX); i++) {
		device_st *d = system_get_dev(sys, i);
		// only devices that carry a configuration package can be simulated
		if ((d == NULL) || (strlen(d->filepath) == 0)) {
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

		if (spawn_proc(p, can_channel)) {
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
		if (p->state != SIMRUN_RUNNING) {
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
			p->state = by_term ? SIMRUN_KILLED : SIMRUN_STOPPED;
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


bool simrun_any_running(void) {
	bool ret = false;
	for (uint8_t i = 0; i < proc_count; i++) {
		if (procs[i].state == SIMRUN_RUNNING) {
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
	if ((index < proc_count) && (procs[index].state == SIMRUN_RUNNING)) {
		terminate_proc(&procs[index]);
		procs[index].state = SIMRUN_KILLED;
	}
}


void simrun_kill_all(void) {
	clear_all();
}


#else /* CONFIG_TARGET_WIN: simulators are a Linux-only feature */


void simrun_init(void) {
}

uint8_t simrun_start_system(system_st *sys, const char *can_channel) {
	(void) sys;
	(void) can_channel;
	return 0;
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

bool simrun_any_running(void) {
	return false;
}

void simrun_open_log(uint8_t index) {
	(void) index;
}

void simrun_kill(uint8_t index) {
	(void) index;
}

void simrun_kill_all(void) {
}


#endif
