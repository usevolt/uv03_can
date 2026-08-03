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


#include "ui/fleettab.h"

#if CONFIG_UI

#include <stdio.h>
#include <string.h>
#include "mqtt.h"
#include "remotecan.h"
#include "ui/remoteui_win.h"
#include "uv_remote_proto.h"
// for the "Remove" button image, the same one the System tab's device tabs use
#include "ui/uvui.h"


// Margin in pixels around the tab content, height of a button / field row and of
// a plain text row. Matched to the system tab so the two tabs line up.
#define MARGIN			10
#define BUTTON_H		44
#define TITLE_H			30

// Colour of the "connected" status line, and of a failure reason. Same green and
// red the device tabs use for their status dots and warnings.
#define OK_COLOR		C(0xFF22B14C)
#define WARNING_COLOR	C(0xFFE02020)


// --- the fleet / device tab windows. The connection itself is opened from the
// System tab's Account panel: one Usevolt account serves both the file server
// and the fleet broker, so it is entered in one place.
static uv_uitabwindow_st fleet_tabs;
static uv_uiobject_st *fleet_tabs_buf[4];
static char fleet_name_buf[MQTT_MAX_FLEETS][MQTT_NAME_MAX];
static char *fleet_names[MQTT_MAX_FLEETS];
static uint8_t fleet_tab_count;

static uv_uitabwindow_st dev_tabs;
// the device tab holds its information label and the row of buttons below it;
// uv_uiwindow_add() does not bounds check, so this has to have room for all of
// them
static uv_uiobject_st *dev_tabs_buf[8];
static char dev_name_buf[MQTT_MAX_DEVS][MQTT_NAME_MAX];
static char *dev_names[MQTT_MAX_DEVS];
static uint8_t dev_tab_count;

// Selected fleet / device tab. Kept across the rebuilds a new device triggers, so
// the view does not jump around while the fleet fills in.
static int16_t selected_fleet;
static int16_t selected_dev;

// Content of a device tab, and the placeholder shown when there is nothing to
// show yet (not connected, no fleets, or a fleet with no devices).
static uv_uilabel_st dev_info;
static char dev_info_str[1024];
// "Open remote UI" / "Kill remote UI" for the active device tab
static uv_uibutton_st dev_ui_btn;
// "Remove Device": takes the active device tab out of the view
static uv_uimediabutton_st dev_remove_btn;
// "Remote CAN": bridges this device's CAN bus to a netdev of its own, and the
// two boxes that decide which kinds of frame are carried over it.
static uv_uitogglebutton_st dev_can_btn;
static uv_uicheckbox_st dev_std_cb;
static uv_uicheckbox_st dev_ext_cb;
// Which device the mirror window is showing, so the button on every other tab
// reads "Open" and closing it targets the right device.
static int16_t ui_fleet = -1;
static int16_t ui_dev = -1;
static uv_uilabel_st placeholder;
static char placeholder_str[320];

// True while the Fleet tab's widgets are built (see fleettab_set_shown()).
static bool shown;

static void build_dev_info_str(void);
static void refresh_can_btn(void);
static void build_fleet_tabs(uv_uitabwindow_st *tabwin, int16_t y, int16_t h);
static void show_active_dev_tab(void);
static uv_uiobject_ret_e fleet_tabs_step(void *me, const uint16_t step_ms);
static uv_uiobject_ret_e dev_tabs_step(void *me, const uint16_t step_ms);


void fleettab_set_shown(bool value) {
	shown = value;
}


/// @brief: (Re)builds the active device tab's information text from what the
/// client currently knows about that device.
static void build_dev_info_str(void) {
	uint8_t f = (uint8_t) selected_fleet;
	uint8_t d = (uint8_t) selected_dev;
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
				"\nRemote CAN: netdev %s, %s messages%s\n"
				"%s"
				"  to netdev %u   from netdev %u   filtered %u   failed %u\n",
				remotecan_get_ifname(),
				carried,
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

	snprintf(dev_info_str, sizeof(dev_info_str),
			"Name:       %s\n"
			"Fleet:      %s\n"
			"Client id:  %s\n"
			"State:      %u\n"
			"Uptime:     %u h %02u min\n"
			"Remote:     UI %s, CAN %s\n"
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
			size_str,
			(unsigned int) mqtt_get_dev_msg_count(f, d),
			(unsigned int) mqtt_get_dev_age_s(f, d),
			can_str);
}


/// @brief: True while the mirror window is showing the device whose tab is open.
static bool ui_shows_selected(void) {
	return remoteui_win_is_open() &&
			(ui_fleet == selected_fleet) &&
			(ui_dev == selected_dev);
}


// uv_uibutton_set_text takes a mutable string, so hand it a buffer of ours
static char ui_btn_buf[24];

static char *ui_btn_text(void) {
	strcpy(ui_btn_buf, ui_shows_selected() ?
			"Kill remote UI" : "Open remote UI");
	return ui_btn_buf;
}


/// @brief: Retitles the button and greys it out while the device cannot be
/// mirrored: that needs a live connection, and the device has to have answered
/// at least once so we know it is really there.
static void refresh_ui_btn(void) {
	uv_uibutton_set_text(&dev_ui_btn, ui_btn_text());
	bool can = mqtt_is_connected() &&
			(mqtt_get_dev_msg_count((uint8_t) selected_fleet,
					(uint8_t) selected_dev) > 0);
	uv_ui_set_enabled(&dev_ui_btn, can || ui_shows_selected());
	uv_ui_refresh(&dev_ui_btn);
}


/// @brief: Puts the CAN buttons back in step with what is actually happening.
///
/// The toggle follows the bridge rather than the click: starting one can fail
/// (no privileges for the interface) and a device can refuse the feature, and
/// in both cases the button has to come back off by itself.
static void refresh_can_btn(void) {
	uv_uitogglebutton_set_state(&dev_can_btn,
			remotecan_shows((uint8_t) selected_fleet,
					(uint8_t) selected_dev));
	bool can = mqtt_is_connected() &&
			(mqtt_get_dev_msg_count((uint8_t) selected_fleet,
					(uint8_t) selected_dev) > 0);
	uv_ui_set_enabled(&dev_can_btn, can || remotecan_is_active());
	uv_uicheckbox_set_state(&dev_std_cb, remotecan_get_allow_std());
	uv_uicheckbox_set_state(&dev_ext_cb, remotecan_get_allow_ext());
	uv_ui_refresh(&dev_can_btn);
	uv_ui_refresh(&dev_std_cb);
	uv_ui_refresh(&dev_ext_cb);
}


/// @brief: Stops mirroring whoever is being mirrored, closing the window and
/// telling the device to stop sending.
static void ui_stop(void) {
	if ((ui_fleet >= 0) && (ui_dev >= 0)) {
		(void) mqtt_dev_set_ui_active((uint8_t) ui_fleet, (uint8_t) ui_dev,
				false);
	}
	ui_fleet = -1;
	ui_dev = -1;
	remoteui_win_close();
}


/// @brief: The device has ended the session from its own screen — the machine
/// operator touched the notification bar that said remote access was on.
///
/// The window goes now rather than sitting there showing a screen that has
/// stopped moving, and the reason is logged: a view that vanishes on its own is
/// otherwise indistinguishable from one that broke.
static void ui_close_callb(uint8_t fleet_index, uint8_t dev_index, void *user) {
	(void) user;
	if (remotecan_shows(fleet_index, dev_index)) {
		// the same close ends the CAN bridge, and with it the netdev: the
		// device has switched every feature off at its end
		printf("remote CAN: closing the bridge - the device sent a close "
				"request\n");
		fflush(stdout);
		remotecan_stop();
	}
	else {
	}
	if ((fleet_index == ui_fleet) && (dev_index == ui_dev)) {
		printf("remote UI: closing the mirrored view - the device sent a close "
				"request\n");
		fflush(stdout);
		ui_stop();
		if (shown) {
			refresh_ui_btn();
		}
		else {
		}
	}
	else {
		// a device we are not mirroring; it has no view of ours to close
	}
}


/// @brief: Receives one mirrored frame. Frames for a device other than the one
/// on screen are ignored: the device may still be finishing a frame that was in
/// flight when mirroring was switched off.
static void ui_open_window(uint16_t w, uint16_t h);


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
			ui_stop();
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
}


/// @brief: Starts mirroring the selected device. The window can only be sized
/// once the device has said how big its display is, so this asks first and the
/// window opens from fleettab_step() when the answer arrives.
static void ui_start(void) {
	ui_stop();
	ui_fleet = selected_fleet;
	ui_dev = selected_dev;
	mqtt_set_ui_frame_callb(&ui_frame_callb, NULL);
	mqtt_set_asset_callb(&ui_asset_callb, NULL);
	mqtt_set_close_callb(&ui_close_callb, NULL);
	remoteui_win_set_asset_request_callb(&ui_asset_req_callb, NULL);
	remoteui_win_set_input_callb(&ui_input_callb, NULL);
	if (!mqtt_dev_set_ui_active((uint8_t) ui_fleet, (uint8_t) ui_dev, true)) {
		ui_fleet = -1;
		ui_dev = -1;
	}
}


/// @brief: Shows *text* in the tab's content area, for the states in
/// which there is no fleet tree to show.
static void build_placeholder(uv_uitabwindow_st *tabwin, const char *text,
		int16_t y, int16_t h, uv_bounding_box_st cbb) {
	strncpy(placeholder_str, text, sizeof(placeholder_str) - 1);
	placeholder_str[sizeof(placeholder_str) - 1] = '\0';
	uv_uilabel_init(&placeholder, uv_uistyles[0].font, ALIGN_TOP_LEFT,
			uv_uistyles[0].text_color, placeholder_str);
	uv_uitabwindow_addxy(tabwin, &placeholder, MARGIN, y,
			cbb.w - 2 * MARGIN, h);
}


void fleettab_show(uv_uitabwindow_st *tabwin) {
	uv_bounding_box_st cbb = uv_uitabwindow_get_contentbb(tabwin);

	int16_t rest_y = MARGIN;
	int16_t rest_h = cbb.h - rest_y - MARGIN;

	if (!mqtt_is_supported()) {
		build_placeholder(tabwin, "The fleet view needs an MQTT connection, "
				"which is not available in the Windows build.",
				rest_y, rest_h, cbb);
	}
	else if (!mqtt_is_connected()) {
		build_placeholder(tabwin, "Not connected to the fleet broker. Connect "
				"on the System tab, under Account.", rest_y, rest_h, cbb);
	}
	else if (mqtt_get_fleet_count() == 0) {
		build_placeholder(tabwin, "Connected to broker. No fleets seen yet - a fleet "
				"appears once one of its devices publishes something.",
				rest_y, rest_h, cbb);
	}
	else {
		build_fleet_tabs(tabwin, rest_y, rest_h);
	}

	shown = true;
}


/// @brief: Builds the fleet tab window (one tab per discovered fleet) into the
/// tab's content area, and fills it with the active fleet's devices.
static void build_fleet_tabs(uv_uitabwindow_st *tabwin, int16_t y, int16_t h) {
	uv_bounding_box_st cbb = uv_uitabwindow_get_contentbb(tabwin);

	fleet_tab_count = mqtt_get_fleet_count();
	if (fleet_tab_count > MQTT_MAX_FLEETS) {
		fleet_tab_count = MQTT_MAX_FLEETS;
	}
	for (uint8_t i = 0; i < fleet_tab_count; i++) {
		strncpy(fleet_name_buf[i], mqtt_get_fleet_name(i),
				sizeof(fleet_name_buf[i]) - 1);
		fleet_name_buf[i][sizeof(fleet_name_buf[i]) - 1] = '\0';
		fleet_names[i] = fleet_name_buf[i];
	}

	uv_uitabwindow_init(&fleet_tabs, fleet_tab_count, &uv_uistyles[0],
			fleet_tabs_buf, fleet_names);
	uv_uiwindow_set_transparent(&fleet_tabs, true);
	uv_uitabwindow_addxy(tabwin, &fleet_tabs, MARGIN, y,
			cbb.w - 2 * MARGIN, h);

	// a fleet can have disappeared with a reconnect; keep the selection in range
	if (selected_fleet >= fleet_tab_count) {
		selected_fleet = fleet_tab_count - 1;
	}
	if (selected_fleet < 0) {
		selected_fleet = 0;
	}
	uv_uitabwindow_set_tab(&fleet_tabs, selected_fleet);

	show_active_dev_tab();
}


/// @brief: Fills the active fleet's tab with a device tab window (one tab per
/// device of that fleet) and the active device's information.
static void show_active_dev_tab(void) {
	const uv_uistyle_st *style = &uv_uistyles[0];
	uv_uitabwindow_clear(&fleet_tabs);
	uv_bounding_box_st fc = uv_uitabwindow_get_contentbb(&fleet_tabs);

	dev_tab_count = mqtt_get_dev_count((uint8_t) selected_fleet);
	if (dev_tab_count > MQTT_MAX_DEVS) {
		dev_tab_count = MQTT_MAX_DEVS;
	}

	if (dev_tab_count == 0) {
		// the fleet is known but none of its devices has published yet
		strcpy(placeholder_str, "No devices seen in this fleet yet.");
		uv_uilabel_init(&placeholder, style->font, ALIGN_TOP_LEFT,
				style->text_color, placeholder_str);
		uv_uitabwindow_addxy(&fleet_tabs, &placeholder, MARGIN, MARGIN,
				fc.w - 2 * MARGIN, fc.h - 2 * MARGIN);
	}
	else {
		for (uint8_t i = 0; i < dev_tab_count; i++) {
			strncpy(dev_name_buf[i],
					mqtt_get_dev_name((uint8_t) selected_fleet, i),
					sizeof(dev_name_buf[i]) - 1);
			dev_name_buf[i][sizeof(dev_name_buf[i]) - 1] = '\0';
			dev_names[i] = dev_name_buf[i];
		}

		uv_uitabwindow_init(&dev_tabs, dev_tab_count, style, dev_tabs_buf,
				dev_names);
		uv_uiwindow_set_transparent(&dev_tabs, true);
		uv_uitabwindow_addxy(&fleet_tabs, &dev_tabs, 0, 0, fc.w, fc.h);

		if (selected_dev >= dev_tab_count) {
			selected_dev = dev_tab_count - 1;
		}
		if (selected_dev < 0) {
			selected_dev = 0;
		}
		uv_uitabwindow_set_tab(&dev_tabs, selected_dev);

		// the device tab itself: what the broker has told us about this client
		uv_bounding_box_st dc = uv_uitabwindow_get_contentbb(&dev_tabs);
		build_dev_info_str();
		// the information fills the tab except for the button row at its foot
		int16_t info_h = dc.h - 2 * MARGIN - BUTTON_H - MARGIN;
		if (info_h < BUTTON_H) {
			info_h = BUTTON_H;
		}
		uv_uilabel_init(&dev_info, &UI_MONO_FONT, ALIGN_TOP_LEFT,
				style->text_color, dev_info_str);
		uv_uitabwindow_addxy(&dev_tabs, &dev_info, MARGIN, MARGIN,
				dc.w - 2 * MARGIN, info_h);

		int16_t btn_y = MARGIN + info_h + MARGIN;
		uv_uibutton_init(&dev_ui_btn, ui_btn_text(), style);
		uv_uitabwindow_addxy(&dev_tabs, &dev_ui_btn, MARGIN,
				btn_y, 5 * BUTTON_H, BUTTON_H);
		refresh_ui_btn();

		// "Remove Device": drops this device from the view, the same way the
		// System tab's device tabs drop a CAN device. Polled in fleettab_step().
		uv_uimediabutton_init(&dev_remove_btn, "Remove Device",
				uvui_get_remove_media(), style);
		uv_uitabwindow_addxy(&dev_tabs, &dev_remove_btn,
				MARGIN + 5 * BUTTON_H + MARGIN, btn_y, 5 * BUTTON_H, BUTTON_H);

		// "Remote CAN": the device's bus on a netdev of this machine's own.
		// The toggle shows what is actually in effect, which is what the device
		// last reported — a device configured to refuse remote CAN
		// (`remote allowcan 0`) leaves it springing straight back off.
		uv_uitogglebutton_init(&dev_can_btn,
				remotecan_shows((uint8_t) selected_fleet,
						(uint8_t) selected_dev),
				"Remote CAN", style);
		uv_uitabwindow_addxy(&dev_tabs, &dev_can_btn,
				MARGIN + 2 * (5 * BUTTON_H + MARGIN), btn_y,
				5 * BUTTON_H, BUTTON_H);

		// which frames the bridge carries, both ways. Standard on, extended
		// off is what a CANopen machine wants; the boxes are here rather than
		// behind a settings page because they are the difference between a
		// usable link and one drowning in J1939 broadcasts.
		uv_uicheckbox_init(&dev_std_cb, remotecan_get_allow_std(),
				"STD messages", style);
		uv_uitabwindow_addxy(&dev_tabs, &dev_std_cb,
				MARGIN + 3 * (5 * BUTTON_H + MARGIN), btn_y,
				4 * BUTTON_H, BUTTON_H);

		uv_uicheckbox_init(&dev_ext_cb, remotecan_get_allow_ext(),
				"EXT messages", style);
		uv_uitabwindow_addxy(&dev_tabs, &dev_ext_cb,
				MARGIN + 3 * (5 * BUTTON_H + MARGIN) + 4 * BUTTON_H + MARGIN,
				btn_y, 4 * BUTTON_H, BUTTON_H);
		refresh_can_btn();

		uv_uitabwindow_set_stepcallb(&dev_tabs, &dev_tabs_step, NULL);
	}

	// uv_uitabwindow_clear() drops the window step callback, so re-register it
	uv_uitabwindow_set_stepcallb(&fleet_tabs, &fleet_tabs_step, NULL);
	uv_ui_refresh(&fleet_tabs);
}


static uv_uiobject_ret_e fleet_tabs_step(void *me, const uint16_t step_ms) {
	(void) me;
	(void) step_ms;
	if (uv_uitabwindow_tab_changed(&fleet_tabs)) {
		selected_fleet = uv_uitabwindow_get_tab(&fleet_tabs);
		// a different fleet has its own device list; start at its first device
		selected_dev = 0;
		show_active_dev_tab();
	}
	return UIOBJECT_RETURN_ALIVE;
}


static uv_uiobject_ret_e dev_tabs_step(void *me, const uint16_t step_ms) {
	(void) me;
	(void) step_ms;
	if (uv_uitabwindow_tab_changed(&dev_tabs)) {
		selected_dev = uv_uitabwindow_get_tab(&dev_tabs);
		show_active_dev_tab();
	}
	return UIOBJECT_RETURN_ALIVE;
}


bool fleettab_step(void) {
	bool ret = false;

	// keep the client running whichever main tab is shown, so the connection
	// survives a visit to the System tab and the fleet list keeps filling in
	mqtt_step();
	// and with it the bridge: what is written to the netdev has to reach the
	// device whether or not anybody is looking at this tab
	remotecan_step();

	// A bridge cannot outlive the connection that carries it. The device stops
	// forwarding on its own when the broker session drops, and an interface
	// with nothing behind it is worse than no interface at all.
	if (remotecan_is_active() && !mqtt_is_connected()) {
		printf("remote CAN: closing the bridge - the broker connection is "
				"gone\n");
		fflush(stdout);
		remotecan_stop();
		if (shown) {
			refresh_can_btn();
		}
		else {
		}
	}
	else {
	}

	if (shown) {
		if ((dev_tab_count > 0) && uv_uibutton_clicked(&dev_ui_btn)) {
			if (ui_shows_selected()) {
				ui_stop();
			}
			else {
				ui_start();
			}
			refresh_ui_btn();
		}

		// "Remote CAN": put this device's bus on a netdev, or take it off
		// again. One bus at a time — there is one interface.
		//
		// What the toggle shows is compared against what is actually happening
		// rather than watching for a click: a togglebutton clears its clicked
		// flag in its own step, which has already run by the time this main
		// loop gets here, and only a window step callback would ever see it.
		// Comparing states is also self-correcting — a bridge that stops on its
		// own puts the button back by itself.
		bool can_wanted = uv_uitogglebutton_get_state(&dev_can_btn);
		bool can_is = remotecan_shows((uint8_t) selected_fleet,
				(uint8_t) selected_dev);
		if ((dev_tab_count > 0) && (can_wanted != can_is)) {
			if (can_is) {
				remotecan_stop();
			}
			else if (!remotecan_start((uint8_t) selected_fleet,
					(uint8_t) selected_dev)) {
				// the reason is already in the log; the toggle goes back off
				// below, which is what says it did not take
			}
			else {
			}
			refresh_can_btn();
			ret = true;
		}
		else {
		}

		// the frame kinds to carry. Compared rather than watched for a click,
		// for the same reason as the toggle above: a checkbox is a
		// togglebutton, and it clears its clicked flag in its own step.
		if ((dev_tab_count > 0) &&
				(uv_uicheckbox_get_state(&dev_std_cb) !=
						remotecan_get_allow_std())) {
			// re-negotiates with the device on its own if a bridge is up
			remotecan_set_allow_std(uv_uicheckbox_get_state(&dev_std_cb));
			refresh_can_btn();
			ret = true;
		}
		else {
		}

		if ((dev_tab_count > 0) &&
				(uv_uicheckbox_get_state(&dev_ext_cb) !=
						remotecan_get_allow_ext())) {
			remotecan_set_allow_ext(uv_uicheckbox_get_state(&dev_ext_cb));
			refresh_can_btn();
			ret = true;
		}
		else {
		}

		// "Remove Device": take this device out of the view. The tabs are
		// rebuilt from the client's device list, so the removal shows up as the
		// tab disappearing.
		if ((dev_tab_count > 0) && uv_uimediabutton_clicked(&dev_remove_btn)) {
			if (remotecan_shows((uint8_t) selected_fleet,
					(uint8_t) selected_dev)) {
				// a bridge to a device that is no longer in the view would have
				// nothing to show it through
				remotecan_stop();
			}
			else {
			}
			if (ui_shows_selected()) {
				// mirroring a device that is being taken out of the view cannot
				// outlive it
				ui_stop();
			}
			else if ((ui_fleet == selected_fleet) &&
					(ui_dev > selected_dev)) {
				// the mirrored device is addressed by index, and everything
				// after the removed one moves down a slot
				ui_dev--;
			}
			else {
			}
			if (mqtt_remove_dev((uint8_t) selected_fleet,
					(uint8_t) selected_dev)) {
				// land on the tab before the removed one rather than on
				// whichever device shifted into its place
				if (selected_dev > 0) {
					selected_dev--;
				}
				else {
				}
				ret = true;
			}
			else {
			}
		}
		else {
		}

		// the message count and the "last seen" age tick on with every message
		// from the device, which is far too often to rebuild the tabs for. Redraw
		// just the label, and only when its text actually changed.
		if (dev_tab_count > 0) {
			char prev[sizeof(dev_info_str)];
			strcpy(prev, dev_info_str);
			build_dev_info_str();
			if (strcmp(prev, dev_info_str) != 0) {
				uv_ui_refresh(&dev_info);
			}
		}
	}

	// The window is opened here rather than from the button, because it can only
	// be sized once the device has answered with its display geometry - the
	// button only asks it to start mirroring.
	if ((ui_fleet >= 0) && (ui_dev >= 0) && !remoteui_win_is_open()) {
		uint16_t w;
		uint16_t h;
		if (mqtt_get_dev_ui_size((uint8_t) ui_fleet, (uint8_t) ui_dev,
				&w, &h)) {
			ui_open_window(w, h);
			if (shown) {
				refresh_ui_btn();
			}
		}
	}

	// The mirrored view cannot tell a still screen from a dead link by itself -
	// the device only sends a frame when something changes - so it is told how
	// long ago the device was last heard from at all. The heartbeat counts, so
	// this keeps ticking over even while the display sits still.
	if (remoteui_win_is_open() && (ui_fleet >= 0) && (ui_dev >= 0)) {
		remoteui_win_set_link_age(mqtt_get_dev_age_s((uint8_t) ui_fleet,
				(uint8_t) ui_dev));
	}
	else {
	}

	// the user can close the mirror window from its own title bar, which leaves
	// the device still sending until we notice and tell it to stop
	remoteui_win_step();
	if ((ui_fleet >= 0) && (ui_dev >= 0) && !remoteui_win_is_open()) {
		uint16_t w;
		uint16_t h;
		if (mqtt_get_dev_ui_size((uint8_t) ui_fleet, (uint8_t) ui_dev,
				&w, &h)) {
			// it was open and the user closed it
			ui_stop();
			if (shown) {
				refresh_ui_btn();
			}
		}
	}

	// a state change or a newly discovered fleet / device means the tabs no
	// longer match what the client knows
	if (mqtt_poll_changed()) {
		ret = true;
	}

	return ret;
}


#endif
