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


#include "fleetcmd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv_rtos.h>
#include "main.h"
#include "fleet.h"
#include "mqtt.h"
#include "remotecan.h"
#include "credentials.h"
#if CONFIG_UI
#include "ui/remoteui_win.h"
#endif


// How long the broker connect may take before giving up on it
#define CONNECT_TIMEOUT_MS		15000
// How long --remotelist listens by default. Devices are only known once they
// publish, and an idle one does so with its heartbeat every 5 seconds, so this
// is one heartbeat and some to spare.
#define LIST_DEFAULT_S			7
// How long a device named by --remoteui / --remotecan is waited for to show
// up, and how long --remoteui waits for it to report its display size
#define DEV_WAIT_MS				15000
#define UI_WAIT_MS				15000
// How often --remotecan prints how the bridge is doing
#define CAN_REPORT_MS			10000
#define STEP_MS					20


// The command callbacks run while the command line is parsed and the work
// later on the tasks, so the arguments are kept until then
static uint32_t list_s = LIST_DEFAULT_S;
static char ui_spec[2 * MQTT_NAME_MAX];
static char can_spec[2 * MQTT_NAME_MAX];


/// @brief: Opens the broker session with the stored account, unless one is
/// open already. Blocks until it is connected or has failed.
static bool login(void) {
	bool ret = false;
	if (!mqtt_is_supported()) {
		printf("The remote fleet needs an MQTT client, which this build "
				"does not have.\n");
	}
	else if (mqtt_is_connected()) {
		ret = true;
	}
	else if ((credentials_get_username()[0] == '\0') ||
			(credentials_get_password()[0] == '\0')) {
		printf("No account stored on this computer. Give it with --user and "
				"--pwd.\n");
	}
	else {
		mqtt_start_pump();
		fleet_login_st res;
		fleet_login(credentials_get_url(), credentials_fleet_get_url(),
				credentials_get_username(), credentials_get_password(), &res);
		fleet_login_finish(&res);
		char err[256] = "";
		if (!res.broker_ok) {
			printf("Fleet: connecting to '%s' failed: %s\n",
					credentials_fleet_get_url(), res.err);
		}
		else if (!fleet_wait_connected(CONNECT_TIMEOUT_MS, err, sizeof(err))) {
			printf("Fleet: connecting to '%s' failed: %s\n",
					credentials_fleet_get_url(), err);
		}
		else {
			printf("Fleet: connected to '%s' as '%s'\n",
					credentials_fleet_get_url(), credentials_get_username());
			ret = true;
		}
	}
	fflush(stdout);
	return ret;
}


/// @brief: Waits for the device *spec* to show up in the fleet view: a device
/// is only known once it has published something.
static bool wait_dev(const char *spec, uint8_t *f, uint8_t *d) {
	bool ret = false;
	char err[512] = "";
	uint32_t waited_ms = 0;
	printf("Looking for '%s'...\n", spec);
	fflush(stdout);
	while (!ret && (waited_ms < DEV_WAIT_MS)) {
		fleet_step();
		ret = fleet_find_dev(spec, f, d, err, sizeof(err));
		if (!ret) {
			uv_rtos_task_delay(STEP_MS);
			waited_ms += STEP_MS;
		}
		else {
		}
	}
	if (!ret) {
		printf("%s\n", err);
	}
	else {
		printf("Found %s/%s\n", mqtt_get_fleet_name(*f),
				mqtt_get_dev_name(*f, *d));
	}
	fflush(stdout);
	return ret;
}


static void login_task(void *ptr) {
	(void) ptr;
	(void) login();
}


bool cmd_login(const char *arg) {
	(void) arg;
	add_task(&login_task);
	return true;
}


static void list_task(void *ptr) {
	(void) ptr;
	if (login()) {
		printf("Listening for devices for %u s...\n", (unsigned int) list_s);
		fflush(stdout);
		for (uint32_t ms = 0; ms < (list_s * 1000u); ms += STEP_MS) {
			fleet_step();
			uv_rtos_task_delay(STEP_MS);
		}
		printf("\n%s\n", fleet_dev_summary_header());
		uint16_t total = 0;
		for (uint8_t f = 0; f < mqtt_get_fleet_count(); f++) {
			uint8_t count = mqtt_get_dev_count(f);
			if (count == 0) {
				printf("%-36s (no devices heard from)\n",
						mqtt_get_fleet_name(f));
			}
			else {
			}
			for (uint8_t d = 0; d < count; d++) {
				char line[256];
				fleet_dev_summary(f, d, line, sizeof(line));
				printf("%s\n", line);
			}
			total = (uint16_t) (total + count);
		}
		printf("\n%u device(s) in %u fleet(s)\n", (unsigned int) total,
				(unsigned int) mqtt_get_fleet_count());
		fflush(stdout);
	}
	else {
	}
}


bool cmd_remotelist(const char *arg) {
	bool ret = true;
	if (arg != NULL) {
		char *end = NULL;
		long s = strtol(arg, &end, 0);
		if ((end == arg) || (*end != '\0') || (s <= 0)) {
			printf("--remotelist: '%s' is not a number of seconds\n", arg);
			ret = false;
		}
		else {
			list_s = (uint32_t) s;
		}
	}
	else {
	}
	if (ret) {
		add_task(&list_task);
	}
	else {
	}
	return ret;
}


static void ui_task(void *ptr) {
	(void) ptr;
#if CONFIG_UI
	uint8_t f;
	uint8_t d;
	if (!login() || !wait_dev(ui_spec, &f, &d)) {
	}
	else if (!fleet_ui_start(f, d)) {
		printf("remote UI: could not ask the device to start mirroring\n");
	}
	else {
		printf("remote UI: waiting for the device to answer...\n");
		fflush(stdout);
		uint32_t waited_ms = 0;
		while (fleet_ui_is_active()) {
			remoteui_win_poll_events();
			fleet_step();
			if (!fleet_ui_shows(f, d) && fleet_ui_is_active()) {
				// no window yet: the device has not told its display size
				waited_ms += STEP_MS;
				if (waited_ms >= UI_WAIT_MS) {
					printf("remote UI: the device did not answer. Is remote UI "
							"allowed on it?\n");
					fleet_ui_stop();
				}
				else {
				}
			}
			else {
			}
			uv_rtos_task_delay(STEP_MS);
		}
		// let the request to stop mirroring reach the broker
		uv_rtos_task_delay(500);
	}
#else
	printf("This build has no UI to mirror a device's display in.\n");
#endif
	fflush(stdout);
}


bool cmd_remoteui(const char *arg) {
	snprintf(ui_spec, sizeof(ui_spec), "%s", arg);
	add_task(&ui_task);
	return true;
}


static void can_task(void *ptr) {
	(void) ptr;
	uint8_t f;
	uint8_t d;
	if (!login() || !wait_dev(can_spec, &f, &d)) {
	}
	else if (!remotecan_start(f, d)) {
		// remotecan_start() already said why
	}
	else {
		printf("remote CAN: the bus of %s/%s is on %s until Ctrl-C. Reach it "
				"with e.g. 'uvcan -c %s ...' or 'candump %s'.\n",
				mqtt_get_fleet_name(f), mqtt_get_dev_name(f, d),
				remotecan_get_ifname(), remotecan_get_ifname(),
				remotecan_get_ifname());
		fflush(stdout);
		uint32_t report_ms = 0;
		while (remotecan_is_active()) {
			fleet_step();
			report_ms += STEP_MS;
			if (report_ms >= CAN_REPORT_MS) {
				report_ms = 0;
				printf("remote CAN: to netdev %u, from netdev %u, filtered %u, "
						"failed %u%s\n",
						(unsigned int) remotecan_get_rx_count(),
						(unsigned int) remotecan_get_tx_count(),
						(unsigned int) remotecan_get_filtered_count(),
						(unsigned int) remotecan_get_error_count(),
						mqtt_dev_get_can_active(f, d) ? "" :
								" (device reports CAN off)");
				// and what the device says it did with them: a frame it threw
				// away looks from here exactly like one that was never sent
				remote_can_stats_st st;
				if (mqtt_get_dev_can_stats(f, d, &st)) {
					printf("remote CAN: device injected %u",
							(unsigned int) st.injected);
					for (uint8_t i = 0; i < REMOTE_CAN_CLASS_COUNT; i++) {
						printf(", %s %u fwd %u drop",
								remote_can_class_to_str((remote_can_class_e) i),
								(unsigned int) st.forwarded[i],
								(unsigned int) st.dropped[i]);
					}
					printf("\n");
					fflush(stdout);
				}
				else {
				}
			}
			else {
			}
			uv_rtos_task_delay(STEP_MS);
		}
	}
	fflush(stdout);
}


bool cmd_remotecan(const char *arg) {
	snprintf(can_spec, sizeof(can_spec), "%s", arg);
	add_task(&can_task);
	return true;
}
