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


#include "loadesp32.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#if CONFIG_TARGET_WIN
#include <process.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/wait.h>
#endif
#include <uv_terminal.h>
#include "main.h"
#include "system.h"
#include "uvdev.h"


/// @brief: Baudrate which esptool writes the flash with, as 'make esp' does
#define ESP32_BAUDRATE				"921600"
/// @brief: How long the module's USB port is waited for to (re)appear after
/// the device was asked to leave the module alone. A reset just before may
/// have it mid re-enumeration.
#define ESP32_PORT_WAIT_MS			5000
/// @brief: Delay between the CAN frames of a terminal command, so that the
/// device's terminal keeps up with them
#define TERMINAL_FRAME_DELAY_MS		30
/// @brief: How long the device is given to act on a terminal command
#define TERMINAL_CMD_DELAY_MS		300
/// @brief: Polling interval of the running esptool
#define ESPTOOL_POLL_MS				50


// the serial port given with --esp32port. Empty when it was not given.
static char port[256];

// The argument of --loadesp32 and the node id selection where it stood on the
// command line (see the same fields in loadmedia_st)
static char cmd_file[1024];
static system_nodeids_st cmd_nodeids;

// arguments of the asynchronous flash task
static char async_filepath[1024];
static uint8_t async_nodeid;
static volatile bool async_finished = true;


bool cmd_esp32port(const char *arg) {
	bool ret = false;
	if ((arg == NULL) || (strlen(arg) == 0)) {
		printf(PRINT_BOLDRED "Give the serial port of the ESP32 module.\n"
				PRINT_RESET);
	}
	else if (strlen(arg) >= sizeof(port)) {
		printf(PRINT_BOLDRED "The ESP32 serial port '%s' is too long.\n"
				PRINT_RESET, arg);
	}
	else {
		strcpy(port, arg);
		printf("ESP32 serial port set to '%s'\n", port);
		ret = true;
	}
	return ret;
}


// Writes *line* followed by a newline to the terminal of the device at *nodeid*
// over CAN, four characters per frame, the same way the *terminal* command
// does. The device's answer is not waited for: there may be no device at all.
static void terminal_send(uint8_t nodeid, const char *line) {
	char str[128];
	snprintf(str, sizeof(str), "%s\n", line);
	size_t len = strlen(str);
	for (size_t i = 0; i < len; i += 4) {
		uv_can_msg_st msg;
		uint8_t n = 0;
		msg.type = CAN_STD;
		msg.id = UV_TERMINAL_CAN_RX_ID + nodeid;
		msg.data_8bit[0] = 0x22;
		msg.data_8bit[1] = UV_TERMINAL_CAN_INDEX % 256;
		msg.data_8bit[2] = UV_TERMINAL_CAN_INDEX / 256;
		msg.data_8bit[3] = UV_TERMINAL_CAN_SUBINDEX;
		while ((n < 4) && ((i + n) < len)) {
			msg.data_8bit[4 + n] = (uint8_t) str[i + n];
			n++;
		}
		msg.data_length = 4 + n;
		uv_can_send(dev.can_channel, &msg);
		uv_rtos_task_delay(TERMINAL_FRAME_DELAY_MS);
	}
	uv_rtos_task_delay(TERMINAL_CMD_DELAY_MS);
}


// Asks the device at *nodeid* to stop (false) or resume (true) talking to its
// ESP32 module. Skipped when there is no device to ask.
static void esp32_comm(uint8_t nodeid, bool enable) {
	if (nodeid != 0) {
		printf("%s the ESP32 communication of node 0x%x\n",
				enable ? "Resuming" : "Pausing", (unsigned int) nodeid);
		fflush(stdout);
		terminal_send(nodeid, enable ? "esp \"comm\" 1" : "esp \"comm\" 0");
	}
}


#if CONFIG_TARGET_WIN


// esptool scans the COM ports itself when no port is given
static bool find_port(char *dest, size_t dest_len) {
	bool ret = false;
	if (strlen(port) != 0) {
		snprintf(dest, dest_len, "%s", port);
		ret = true;
	}
	else {
		dest[0] = '\0';
	}
	return ret;
}


// Runs esptool with *argv* and waits for it to finish. _spawnvp() does not go
// through the shell, so no path reaches cmd.exe.
static bool run_esptool(char **argv) {
	fflush(stdout);
	intptr_t rc = _spawnvp(_P_WAIT, "esptool", (const char *const *) argv);
	if (rc == -1) {
		argv[0] = "esptool.py";
		rc = _spawnvp(_P_WAIT, "esptool.py", (const char *const *) argv);
	}
	if (rc == -1) {
		printf(PRINT_BOLDRED "ERROR: esptool was not found. Install it with "
				"'pip install esptool'.\n" PRINT_RESET);
	}
	return (rc == 0);
}


#else


// Finds the module's serial port: the one given with --esp32port, or else an
// Espressif USB device under /dev/serial/by-id. Returns false when no port was
// given and none was found, *dest* then being empty.
static bool find_port(char *dest, size_t dest_len) {
	bool ret = false;
	dest[0] = '\0';
	if (strlen(port) != 0) {
		snprintf(dest, dest_len, "%s", port);
		ret = (access(port, F_OK) == 0);
	}
	else {
		const char *dir = "/dev/serial/by-id";
		DIR *d = opendir(dir);
		if (d != NULL) {
			struct dirent *e;
			while (!ret && ((e = readdir(d)) != NULL)) {
				if (strstr(e->d_name, "Espressif") != NULL) {
					char link[512];
					snprintf(link, sizeof(link), "%s/%s", dir, e->d_name);
					char *real = realpath(link, NULL);
					if (real != NULL) {
						snprintf(dest, dest_len, "%s", real);
						free(real);
						ret = true;
					}
				}
			}
			closedir(d);
		}
	}
	return ret;
}


// Prints the output of esptool which has gathered into *line*. esptool redraws
// its progress on one line with carriage returns, which the UI's line based log
// would show as one enormous line, so a carriage return ends a line too.
static void print_output(char *line, size_t *len, const char *data, size_t n) {
	for (size_t i = 0; i < n; i++) {
		char c = data[i];
		if ((c == '\n') || (c == '\r')) {
			if (*len != 0) {
				line[*len] = '\0';
				printf("%s\n", line);
				fflush(stdout);
				*len = 0;
			}
		}
		else if (*len < 255) {
			line[(*len)++] = c;
		}
		else {
		}
	}
}


// Runs esptool with *argv* and waits for it to finish. It is started with
// fork() and execvp() rather than through the shell, so no path reaches a
// shell, and its output is read through a pipe so that it can be printed line
// by line (see print_output()).
static bool run_esptool(char **argv) {
	bool ret = false;
	int fds[2];
	fflush(stdout);
	if (pipe(fds) != 0) {
		printf(PRINT_BOLDRED "ERROR: failed to start esptool.\n" PRINT_RESET);
	}
	else {
		pid_t pid = fork();
		if (pid == 0) {
			close(fds[0]);
			dup2(fds[1], STDOUT_FILENO);
			dup2(fds[1], STDERR_FILENO);
			close(fds[1]);
			execvp("esptool", argv);
			argv[0] = "esptool.py";
			execvp("esptool.py", argv);
			printf("ERROR: esptool was not found. Install it with "
					"'pip install esptool'.\n");
			fflush(stdout);
			_exit(127);
		}
		else if (pid > 0) {
			close(fds[1]);
			fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
			char line[256];
			size_t len = 0;
			bool open = true;
			// the FreeRTOS scheduler tick interrupts the reads; keep going
			// until esptool closes its end of the pipe
			while (open) {
				char buf[256];
				ssize_t n = read(fds[0], buf, sizeof(buf));
				if (n > 0) {
					print_output(line, &len, buf, (size_t) n);
				}
				else if (n == 0) {
					open = false;
				}
				else if ((errno == EAGAIN) || (errno == EINTR)) {
					uv_rtos_task_delay(ESPTOOL_POLL_MS);
				}
				else {
					open = false;
				}
			}
			print_output(line, &len, "\n", 1);
			close(fds[0]);

			int status = 0;
			pid_t rc;
			do {
				rc = waitpid(pid, &status, 0);
			} while ((rc == -1) && (errno == EINTR));
			ret = (rc == pid) && WIFEXITED(status) &&
					(WEXITSTATUS(status) == 0);
		}
		else {
			close(fds[0]);
			close(fds[1]);
			printf(PRINT_BOLDRED "ERROR: failed to start esptool.\n"
					PRINT_RESET);
		}
	}
	return ret;
}


#endif


// Flashes the images of the opened package *pkg* with esptool, to the port
// found with find_port() (or the one esptool finds itself)
static bool flash_images(const uvdev_st *pkg) {
	char portbuf[256];
	bool have_port = false;
	// the port may be mid re-enumeration from a reset just before
	for (uint32_t t = 0; !have_port && (t < ESP32_PORT_WAIT_MS); t += 100) {
		have_port = find_port(portbuf, sizeof(portbuf));
		if (!have_port) {
			uv_rtos_task_delay(100);
		}
	}

	if (have_port) {
		printf("Flashing the ESP32 firmware to '%s'\n", portbuf);
	}
	else if (strlen(port) != 0) {
		// the given port, even though it did not show up
		snprintf(portbuf, sizeof(portbuf), "%s", port);
		have_port = true;
		printf(PRINT_BOLDYELLOW "WARNING: the ESP32 serial port '%s' was not "
				"found. Trying it anyway.\n" PRINT_RESET, port);
	}
	else {
		printf("No Espressif USB port found, letting esptool look for the "
				"ESP32\n");
	}
	fflush(stdout);

	// esptool <options> write_flash <offset> <file> ...
	// The hyphenated spellings of esptool v5, the same as 'make esp' uses
	static char paths[UVDEV_ESP32_MAX_COUNT][1024 + 256 + 2];
	static char offsets[UVDEV_ESP32_MAX_COUNT][16];
	char *argv[16 + (2 * UVDEV_ESP32_MAX_COUNT)];
	uint16_t argc = 0;
	argv[argc++] = "esptool";
	argv[argc++] = "--chip";
	argv[argc++] = "auto";
	if (have_port) {
		argv[argc++] = "--port";
		argv[argc++] = portbuf;
	}
	argv[argc++] = "--baud";
	argv[argc++] = ESP32_BAUDRATE;
	argv[argc++] = "--before";
	argv[argc++] = "default-reset";
	argv[argc++] = "--after";
	argv[argc++] = "hard-reset";
	argv[argc++] = "write-flash";
	argv[argc++] = "-z";
	for (uint16_t i = 0; i < pkg->esp32_count; i++) {
		snprintf(offsets[i], sizeof(offsets[i]), "0x%x",
				(unsigned int) pkg->esp32[i].offset);
		snprintf(paths[i], sizeof(paths[i]), "%s/%s",
				pkg->dir, pkg->esp32[i].file);
		argv[argc++] = offsets[i];
		argv[argc++] = paths[i];
	}
	argv[argc] = NULL;

	return run_esptool(argv);
}


bool loadesp32_uvdev(const char *uvdev_path, uint8_t nodeid) {
	bool ret = false;
	uvdev_st pkg;
	if (!uvdev_open(&pkg, uvdev_path)) {
		printf(PRINT_BOLDRED "Failed to open the device package '%s' for ESP32 "
				"flashing.\n" PRINT_RESET, uvdev_path);
	}
	else {
		if (pkg.esp32_count == 0) {
			printf(PRINT_BOLDYELLOW "WARNING: device package '%s' bundles no ESP32 "
					"firmware to flash.\n" PRINT_RESET, uvdev_path);
		}
		else {
			esp32_comm(nodeid, false);
			ret = flash_images(&pkg);
			// resuming resets the module, so it boots the new firmware with
			// the device watching. Done after a failed flash too, so the device
			// is not left ignoring its module.
			esp32_comm(nodeid, true);
			if (ret) {
				printf("ESP32 firmware flashed.\n");
			}
			else {
				printf(PRINT_BOLDRED "ERROR: flashing the ESP32 firmware failed. "
						"Is the module's USB port connected?\n" PRINT_RESET);
			}
			fflush(stdout);
		}
		uvdev_close(&pkg);
	}
	return ret;
}


static void loadesp32_device_task(void *ptr) {
	(void) ptr;
	loadesp32_uvdev(async_filepath, async_nodeid);
	async_finished = true;
	uv_rtos_task_delete(NULL);
}


void loadesp32_load_device_async(const char *uvdev_path, uint8_t nodeid) {
	strncpy(async_filepath, (uvdev_path != NULL) ? uvdev_path : "",
			sizeof(async_filepath) - 1);
	async_filepath[sizeof(async_filepath) - 1] = '\0';
	async_nodeid = nodeid;
	// mark in-progress before the task starts so a caller can poll immediately
	async_finished = false;
	uv_rtos_task_create(&loadesp32_device_task, "loadesp32_task",
			UV_RTOS_MIN_STACK_SIZE * 5, NULL, UV_RTOS_IDLE_PRIORITY + 1, NULL);
}


bool loadesp32_load_device_is_finished(void) {
	return async_finished;
}


// Flashes the ESP32 firmware of the one system device in [start, end) whose
// package bundles it. There is one USB port to flash through, so more than one
// such device is refused.
static void loadesp32_devices(uint8_t start, uint8_t end) {
	// a node id forced on the command line wins over the device's own
	system_apply_forced_nodeid(&dev.system, start, end);
	device_st *target = NULL;
	uint8_t count = 0;
	for (uint8_t i = start; i < end; i++) {
		if (dev.system.devs[i].has_esp32) {
			target = &dev.system.devs[i];
			count++;
		}
	}
	if (count == 0) {
		printf(PRINT_BOLDRED "ERROR: none of the devices' packages bundles ESP32 "
				"firmware.\n" PRINT_RESET);
	}
	else if (count > 1) {
		printf(PRINT_BOLDRED "ERROR: %u devices bundle ESP32 firmware. Give the "
				"one to flash to *loadesp32* as its .uvdev package.\n"
				PRINT_RESET, (unsigned int) count);
	}
	else {
		printf("\n=== Flashing ESP32 firmware of device '%s' (node 0x%x) ===\n",
				target->name, (unsigned int) target->nodeid);
		loadesp32_uvdev(target->filepath, target->nodeid);
	}
}


// Task body for the --loadesp32 command
static void loadesp32_dispatch_step(void *ptr) {
	(void) ptr;
	// operate on the node ids selected where the command stood on the command
	// line (see loadmedia_dispatch_step())
	system_nodeids_st current;
	system_nodeids_save(&dev.system, &current);
	system_nodeids_restore(&dev.system, &cmd_nodeids);

	char argbuf[1024];
	const char *arg = NULL;
	if (!cmdline_load_arg_nodeid(cmd_file, argbuf, sizeof(argbuf), &arg)) {
		// the node id in the argument is out of range; already reported
	}
	else if (path_is_uvdev(arg)) {
		uint8_t prev = dev.system.dev_count;
		// use the node id forced with --nodeid when given (0 -> the package's own)
		if (!system_add_device(&dev.system, arg, db_get_nodeid(&dev.db))) {
			printf(PRINT_BOLDRED "ERROR: failed to add device package '%s'.\n"
					PRINT_RESET, arg);
		}
		else {
			loadesp32_devices(prev, dev.system.dev_count);
		}
	}
	else if (arg != NULL) {
		printf(PRINT_BOLDRED "ERROR: *loadesp32* takes a .uvdev device package, "
				"not '%s'.\n" PRINT_RESET, arg);
	}
	else if (dev.system.dev_count != 0) {
		loadesp32_devices(0, dev.system.dev_count);
	}
	else {
		printf(PRINT_BOLDRED "ERROR: no device package given. Provide a .uvdev "
				"package, or load devices with --dev / --sys first.\n"
				PRINT_RESET);
	}

	system_nodeids_restore(&dev.system, &current);
}


bool cmd_loadesp32(const char *arg) {
	strncpy(cmd_file, (arg != NULL) ? arg : "", sizeof(cmd_file) - 1);
	cmd_file[sizeof(cmd_file) - 1] = '\0';
	system_nodeids_save(&dev.system, &cmd_nodeids);
	add_task(loadesp32_dispatch_step);
	uv_can_set_up(false);
	return true;
}
