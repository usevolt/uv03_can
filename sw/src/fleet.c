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


#include "fleet.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <uv_rtos.h>
#include <uv_remote_proto.h>
#include "mqtt.h"
#include "remotecan.h"
#include "remotefiles.h"
#if CONFIG_UI
#include "ui/remoteui_win.h"
#endif


// Which device the remote UI session is held with, -1 when none. Kept from
// fleet_ui_start() until the session ends, also while the window is still
// waiting for the device's display size.
static int16_t ui_fleet = -1;
static int16_t ui_dev = -1;

// Set by what ends or starts a session behind the caller's back (the device's
// close request), and reported by the next fleet_step()
static bool changed;


void fleet_login(const char *files_url, const char *broker_url,
		const char *username, const char *password, fleet_login_st *result) {
	printf("File server: connecting to '%s' as '%s'...\n",
			files_url, username);
	fflush(stdout);
	result->err[0] = '\0';
	result->files_ok = remotefiles_login(files_url, username, password,
			result->err, sizeof(result->err));
	if (result->files_ok) {
		printf("File server: connected to '%s' as '%s', %u fleet(s):",
				files_url, username,
				(unsigned int) remotefiles_get_fleet_count());
		for (uint8_t i = 0; i < remotefiles_get_fleet_count(); i++) {
			printf(" %s", remotefiles_get_fleet(i));
		}
		printf("\n");
	}
	else {
		printf("File server: connecting to '%s' failed: %s\n",
				files_url, result->err);
	}
	fflush(stdout);

	// the same account opens the fleet broker, so one action does both
	result->broker_ok = mqtt_connect(broker_url, username, password);
}


void fleet_login_finish(fleet_login_st *result) {
	if (result->broker_ok) {
		// The file server already said which fleets this account holds, so
		// they can be listed before any device has published. Only the broker
		// knows whether they are alive; only the file server knows they exist
		// at all when they are quiet.
		for (uint8_t i = 0; i < remotefiles_get_fleet_count(); i++) {
			mqtt_add_fleet(remotefiles_get_fleet(i));
		}
	}
	else if (result->err[0] == '\0') {
		snprintf(result->err, sizeof(result->err), "%s", mqtt_get_error());
	}
	else {
	}
}


bool fleet_wait_connected(uint32_t timeout_ms, char *err, size_t err_len) {
	bool ret = false;
	bool done = false;
	uint32_t waited_ms = 0;
	const uint32_t step_ms = 20;
	while (!done) {
		mqtt_state_e state = mqtt_get_state();
		if (state == MQTT_STATE_CONNECTED) {
			ret = true;
			done = true;
		}
		else if ((state == MQTT_STATE_ERROR) ||
				(state == MQTT_STATE_DISCONNECTED)) {
			snprintf(err, err_len, "%s", (mqtt_get_error()[0] != '\0') ?
					mqtt_get_error() : "Not connected to the broker.");
			done = true;
		}
		else if (waited_ms >= timeout_ms) {
			snprintf(err, err_len, "Timed out connecting to %s.",
					mqtt_get_host());
			done = true;
		}
		else {
			uv_rtos_task_delay(step_ms);
			waited_ms += step_ms;
		}
	}
	return ret;
}


/// @brief: Case-insensitive string equality. strcasecmp is not on every
/// platform this builds for.
static bool str_ieq(const char *a, const char *b) {
	while ((*a != '\0') &&
			(tolower((unsigned char) *a) == tolower((unsigned char) *b))) {
		a++;
		b++;
	}
	return (*a == '\0') && (*b == '\0');
}


bool fleet_find_dev(const char *spec, uint8_t *fleet_index,
		uint8_t *dev_index, char *err, size_t err_len) {
	uint8_t matches = 0;
	char names[256];
	names[0] = '\0';
	const char *slash = strchr(spec, '/');

	for (uint8_t f = 0; f < mqtt_get_fleet_count(); f++) {
		const char *fleet = mqtt_get_fleet_name(f);
		for (uint8_t d = 0; d < mqtt_get_dev_count(f); d++) {
			const char *client = mqtt_get_dev_name(f, d);
			bool match;
			if (slash != NULL) {
				size_t flen = (size_t) (slash - spec);
				match = (strlen(fleet) == flen) &&
						(strncmp(fleet, spec, flen) == 0) &&
						(strcmp(client, slash + 1) == 0);
			}
			else {
				match = str_ieq(client, spec) ||
						str_ieq(mqtt_get_dev_devname(f, d), spec);
			}
			if (match) {
				if (matches == 0) {
					*fleet_index = f;
					*dev_index = d;
				}
				else {
				}
				matches++;
				char one[MQTT_NAME_MAX * 2 + 4];
				snprintf(one, sizeof(one), " %s/%s", fleet, client);
				strncat(names, one, sizeof(names) - strlen(names) - 1);
			}
			else {
			}
		}
	}

	if (matches == 0) {
		snprintf(err, err_len, "No device '%s' in the fleet view.", spec);
	}
	else if (matches > 1) {
		snprintf(err, err_len, "'%s' names %u devices:%s. Give it as "
				"<fleet>/<client id>.", spec, (unsigned int) matches, names);
	}
	else {
	}
	return (matches == 1);
}


void fleet_dev_info(uint8_t fleet_index, uint8_t dev_index,
		char *buf, size_t len) {
	uint8_t f = fleet_index;
	uint8_t d = dev_index;
	const char *devname = mqtt_get_dev_devname(f, d);
	uint8_t feat = mqtt_get_dev_features(f, d);
	uint16_t w = 0;
	uint16_t h = 0;
	char size_str[32];
	if (mqtt_get_dev_ui_size(f, d, &w, &h)) {
		snprintf(size_str, sizeof(size_str), "%u x %u", w, h);
	}
	else {
		strcpy(size_str, "unknown until mirrored");
	}
	uint32_t up = mqtt_get_dev_uptime_s(f, d);

	// What the device says about the CAN traffic it is forwarding. The dropped
	// counts are the reason this is shown at all: from here a frame the device
	// threw away is indistinguishable from one that was never sent, so without
	// them a bridge that is quietly losing half the bus looks perfectly healthy.
	char can_str[512];
	can_str[0] = '\0';
	if (remotecan_shows(f, d)) {
		remote_can_stats_st st;
		char classes[320];
		classes[0] = '\0';
		if (mqtt_get_dev_can_stats(f, d, &st)) {
			for (uint8_t i = 0; i < REMOTE_CAN_CLASS_COUNT; i++) {
				char line[80];
				snprintf(line, sizeof(line),
						"  %-5s %8u fwd %8u drop  q %u\n",
						remote_can_class_to_str((remote_can_class_e) i),
						(unsigned int) st.forwarded[i],
						(unsigned int) st.dropped[i],
						(unsigned int) st.queued[i]);
				strncat(classes, line, sizeof(classes) - strlen(classes) - 1);
			}
			char line[96];
			snprintf(line, sizeof(line), "  injected %u   filters %u%s\n",
					(unsigned int) st.injected,
					(unsigned int) st.rxconf_count,
					((st.flags & REMOTE_CAN_STATS_FLAG_RX_ALL) != 0) ?
							" (whole bus)" : "");
			strncat(classes, line, sizeof(classes) - strlen(classes) - 1);
		}
		else {
			strcpy(classes, "  (waiting for the device's first report)\n");
		}
		const char *carried;
		if (remotecan_get_allow_std() && remotecan_get_allow_ext()) {
			carried = "STD and EXT";
		}
		else if (remotecan_get_allow_std()) {
			carried = "STD";
		}
		else if (remotecan_get_allow_ext()) {
			carried = "EXT";
		}
		else {
			carried = "no";
		}
		snprintf(can_str, sizeof(can_str),
				"\nRemote CAN: netdev %s, %s messages%s%s\n"
				"%s"
				"  to netdev %u   from netdev %u   filtered %u   failed %u\n",
				remotecan_get_ifname(),
				carried,
				// the bulk class is where the saving shows: with this on its
				// forwarded count stops rising while the other two carry on
				remotecan_get_sdo_only() ? ", SDO only" : "",
				// the device is the one that decides; while it says no, the
				// interface is there but nothing is coming through it
				mqtt_dev_get_can_active(f, d) ? "" :
						"   (device reports CAN off)",
				classes,
				(unsigned int) remotecan_get_rx_count(),
				(unsigned int) remotecan_get_tx_count(),
				(unsigned int) remotecan_get_filtered_count(),
				(unsigned int) remotecan_get_error_count());
	}
	else {
	}

	snprintf(buf, len,
			"Name:       %s\n"
			"Fleet:      %s\n"
			"Client id:  %s\n"
			"State:      %u\n"
			"Uptime:     %u h %02u min\n"
			"Remote:     UI %s, CAN %s%s\n"
			"Display:    %s\n"
			"Messages:   %u\n"
			"Last seen:  %u s ago\n"
			"%s",
			(devname[0] != '\0') ? devname : "(not heard from yet)",
			mqtt_get_fleet_name(f),
			mqtt_get_dev_name(f, d),
			(unsigned int) mqtt_get_dev_state(f, d),
			(unsigned int) (up / 3600u), (unsigned int) ((up / 60u) % 60u),
			((feat & REMOTE_IOT_FEATURE_UI) != 0) ? "on" : "off",
			((feat & REMOTE_IOT_FEATURE_CAN) != 0) ? "on" : "off",
			// what the device says it is doing, which is the only account of
			// it that counts: the device may have been told one thing and
			// applied another
			((feat & REMOTE_IOT_FEATURE_CAN_SDO) != 0) ? " (SDO only)" : "",
			size_str,
			(unsigned int) mqtt_get_dev_msg_count(f, d),
			(unsigned int) mqtt_get_dev_age_s(f, d),
			can_str);
}


const char *fleet_dev_summary_header(void) {
	return "FLEET/CLIENT ID                      NAME                  "
			"UPTIME    UI  CAN  SEEN";
}


void fleet_dev_summary(uint8_t fleet_index, uint8_t dev_index,
		char *buf, size_t len) {
	uint8_t f = fleet_index;
	uint8_t d = dev_index;
	char id[MQTT_NAME_MAX * 2 + 2];
	snprintf(id, sizeof(id), "%s/%s", mqtt_get_fleet_name(f),
			mqtt_get_dev_name(f, d));
	const char *devname = mqtt_get_dev_devname(f, d);
	uint8_t feat = mqtt_get_dev_features(f, d);
	uint32_t up = mqtt_get_dev_uptime_s(f, d);
	char uptime[16];
	snprintf(uptime, sizeof(uptime), "%uh%02um",
			(unsigned int) (up / 3600u), (unsigned int) ((up / 60u) % 60u));
	snprintf(buf, len, "%-36s %-21s %-9s %-3s %-4s %us ago",
			id,
			(devname[0] != '\0') ? devname : "-",
			uptime,
			((feat & REMOTE_IOT_FEATURE_UI) != 0) ? "on" : "off",
			((feat & REMOTE_IOT_FEATURE_CAN) != 0) ? "on" : "off",
			(unsigned int) mqtt_get_dev_age_s(f, d));
}


bool fleet_remove_dev(uint8_t fleet_index, uint8_t dev_index) {
	if (remotecan_shows(fleet_index, dev_index)) {
		// a bridge to a device that is no longer in the view would have
		// nothing to show it through
		remotecan_stop();
	}
	else {
	}
	if ((ui_fleet == (int16_t) fleet_index) &&
			(ui_dev == (int16_t) dev_index)) {
		// mirroring a device that is being taken out of the view cannot
		// outlive it
		fleet_ui_stop();
	}
	else if ((ui_fleet == (int16_t) fleet_index) &&
			(ui_dev > (int16_t) dev_index)) {
		// the mirrored device is addressed by index, and everything after the
		// removed one moves down a slot
		ui_dev--;
	}
	else {
	}
	return mqtt_remove_dev(fleet_index, dev_index);
}


#if CONFIG_UI

/// @brief: Opens the mirror window for the device being mirrored, sized and
/// titled from what it has told us. Safe to call when one is already open.
static void ui_open_window(uint16_t w, uint16_t h) {
	if (!remoteui_win_is_open() && (ui_fleet >= 0) && (ui_dev >= 0)) {
		char title[160];
		const char *devname = mqtt_get_dev_devname((uint8_t) ui_fleet,
				(uint8_t) ui_dev);
		snprintf(title, sizeof(title), "%s - %s",
				(devname[0] != '\0') ? devname : "remote device",
				mqtt_get_dev_name((uint8_t) ui_fleet, (uint8_t) ui_dev));
		if (!remoteui_win_open(title, w, h)) {
			fleet_ui_stop();
		}
		else {
		}
		changed = true;
	}
	else {
	}
}


/// @brief: Receives one mirrored frame. Frames for a device other than the one
/// on screen are ignored: the device may still be finishing a frame that was in
/// flight when mirroring was switched off.
static void ui_frame_callb(uint8_t fleet_index, uint8_t dev_index,
		const uint8_t *cmds, uint32_t len, void *user) {
	(void) user;
	if ((fleet_index == ui_fleet) && (dev_index == ui_dev)) {
		if (!remoteui_win_is_open()) {
			// The first frame can arrive in the same batch as the size that
			// the window is opened from, before the step that opens it has
			// run. Opening here rather than dropping it matters because the
			// device sends a frame when it has something new to show, and a
			// display sitting still may not have anything new for a long time.
			uint16_t w = 0;
			uint16_t h = 0;
			if (mqtt_get_dev_ui_size((uint8_t) ui_fleet, (uint8_t) ui_dev,
					&w, &h)) {
				ui_open_window(w, h);
			}
			else {
			}
		}
		else {
		}
		if (remoteui_win_is_open()) {
			remoteui_win_draw_frame(cmds, len);
		}
		else {
		}
	}
	else {
	}
}


/// @brief: Carries what the user does on the mirrored view to the device, which
/// treats it as its own screen being touched.
static void ui_input_callb(uint8_t action, int16_t x, int16_t y,
		int16_t scroll, char key, void *user) {
	(void) user;
	if ((ui_fleet >= 0) && (ui_dev >= 0)) {
		(void) mqtt_dev_send_input((uint8_t) ui_fleet, (uint8_t) ui_dev,
				action, x, y, scroll, key);
	}
	else {
	}
}


/// @brief: Carries the mirrored view's request for an asset to the device it is
/// mirroring. Only that device can answer: the id means nothing anywhere else.
static void ui_asset_req_callb(uint8_t kind, uint32_t id, void *user) {
	(void) user;
	if ((ui_fleet >= 0) && (ui_dev >= 0)) {
		(void) mqtt_dev_request_asset((uint8_t) ui_fleet, (uint8_t) ui_dev,
				kind, id);
	}
	else {
	}
}


/// @brief: Hands an asset the device sent to the mirrored view, provided it is
/// still the device being mirrored.
static void ui_asset_callb(uint8_t fleet_index, uint8_t dev_index,
		uint8_t kind, uint32_t id, const uint8_t *data, uint32_t len,
		void *user) {
	(void) user;
	if (remoteui_win_is_open() &&
			(fleet_index == ui_fleet) &&
			(dev_index == ui_dev)) {
		remoteui_win_asset_received(kind, id, data, len);
	}
	else {
	}
}


bool fleet_ui_start(uint8_t fleet_index, uint8_t dev_index) {
	fleet_ui_stop();
	ui_fleet = (int16_t) fleet_index;
	ui_dev = (int16_t) dev_index;
	mqtt_set_ui_frame_callb(&ui_frame_callb, NULL);
	mqtt_set_asset_callb(&ui_asset_callb, NULL);
	remoteui_win_set_asset_request_callb(&ui_asset_req_callb, NULL);
	remoteui_win_set_input_callb(&ui_input_callb, NULL);
	bool ret = mqtt_dev_set_ui_active(fleet_index, dev_index, true);
	if (!ret) {
		ui_fleet = -1;
		ui_dev = -1;
	}
	else {
	}
	changed = true;
	return ret;
}


void fleet_ui_stop(void) {
	if ((ui_fleet >= 0) && (ui_dev >= 0)) {
		(void) mqtt_dev_set_ui_active((uint8_t) ui_fleet, (uint8_t) ui_dev,
				false);
		changed = true;
	}
	else {
	}
	ui_fleet = -1;
	ui_dev = -1;
	remoteui_win_close();
}


bool fleet_ui_is_active(void) {
	return (ui_fleet >= 0) && (ui_dev >= 0);
}


bool fleet_ui_shows(uint8_t fleet_index, uint8_t dev_index) {
	return remoteui_win_is_open() &&
			(ui_fleet == (int16_t) fleet_index) &&
			(ui_dev == (int16_t) dev_index);
}

#else

bool fleet_ui_start(uint8_t fleet_index, uint8_t dev_index) {
	(void) fleet_index;
	(void) dev_index;
	return false;
}

void fleet_ui_stop(void) {
}

bool fleet_ui_is_active(void) {
	return false;
}

bool fleet_ui_shows(uint8_t fleet_index, uint8_t dev_index) {
	(void) fleet_index;
	(void) dev_index;
	return false;
}

#endif


/// @brief: The device has ended the session from its own screen — the machine
/// operator touched the notification bar that said remote access was on.
///
/// Both sessions go now rather than sitting there showing a bus or a screen
/// that has stopped moving, and the reason is logged: a view that vanishes on
/// its own is otherwise indistinguishable from one that broke.
static void close_callb(uint8_t fleet_index, uint8_t dev_index, void *user) {
	(void) user;
	if (remotecan_shows(fleet_index, dev_index)) {
		// the same close ends the CAN bridge, and with it the netdev: the
		// device has switched every feature off at its end
		printf("remote CAN: closing the bridge - the device sent a close "
				"request\n");
		fflush(stdout);
		remotecan_stop();
		changed = true;
	}
	else {
	}
	if ((fleet_index == ui_fleet) && (dev_index == ui_dev)) {
		printf("remote UI: closing the mirrored view - the device sent a close "
				"request\n");
		fflush(stdout);
		fleet_ui_stop();
	}
	else {
		// a device we are not mirroring; it has no view of ours to close
	}
}


bool fleet_step(void) {
	// A device ending the session from its own screen has to be heard whatever
	// is open at this end, so this is registered once and for good rather than
	// when a session is started. It used not to be, and the CAN bridge was the
	// casualty: nothing was listening for the close, the bridge kept re-asking
	// for the feature the operator had just refused, and the takeover bar they
	// had touched to be rid of came straight back.
	static bool close_callb_set;
	if (!close_callb_set) {
		close_callb_set = true;
		mqtt_set_close_callb(&close_callb, NULL);
	}
	else {
	}

	// The client and the bridge each run on a task of their own. What the pump
	// task must not do itself - draw a mirrored frame, decode an asset, answer
	// a close - it leaves here, for the thread that owns the windows.
	mqtt_ui_step();

	// A bridge cannot outlive the connection that carries it. The device stops
	// forwarding on its own when the broker session drops, and an interface
	// with nothing behind it is worse than no interface at all.
	if (remotecan_is_active() && !mqtt_is_connected()) {
		printf("remote CAN: closing the bridge - the broker connection is "
				"gone\n");
		fflush(stdout);
		remotecan_stop();
		changed = true;
	}
	else {
	}

#if CONFIG_UI
	// The window is opened here rather than when the session is started,
	// because it can only be sized once the device has answered with its
	// display geometry - starting only asks it to start mirroring.
	if (fleet_ui_is_active() && !remoteui_win_is_open()) {
		uint16_t w;
		uint16_t h;
		if (mqtt_get_dev_ui_size((uint8_t) ui_fleet, (uint8_t) ui_dev,
				&w, &h)) {
			ui_open_window(w, h);
		}
		else {
		}
	}
	else {
	}

	// The mirrored view cannot tell a still screen from a dead link by itself -
	// the device only sends a frame when something changes - so it is told how
	// long ago the device was last heard from at all. The heartbeat counts, so
	// this keeps ticking over even while the display sits still.
	if (remoteui_win_is_open() && fleet_ui_is_active()) {
		remoteui_win_set_link_age(mqtt_get_dev_age_s((uint8_t) ui_fleet,
				(uint8_t) ui_dev));
	}
	else {
	}

	// the user can close the mirror window from its own title bar, which leaves
	// the device still sending until we notice and tell it to stop
	remoteui_win_step();
	if (fleet_ui_is_active() && !remoteui_win_is_open()) {
		uint16_t w;
		uint16_t h;
		if (mqtt_get_dev_ui_size((uint8_t) ui_fleet, (uint8_t) ui_dev,
				&w, &h)) {
			// it was open and the user closed it
			fleet_ui_stop();
		}
		else {
		}
	}
	else {
	}
#endif

	bool ret = changed;
	changed = false;
	return ret;
}
