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
#include "ui/remoteui_win.h"
#include "uv_remote_proto.h"


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
static uv_uiobject_st *dev_tabs_buf[4];
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
static char dev_info_str[512];
// "Open remote UI" / "Kill remote UI" for the active device tab
static uv_uibutton_st dev_ui_btn;
// Which device the mirror window is showing, so the button on every other tab
// reads "Open" and closing it targets the right device.
static int16_t ui_fleet = -1;
static int16_t ui_dev = -1;
static uv_uilabel_st placeholder;
static char placeholder_str[320];

// True while the Fleet tab's widgets are built (see fleettab_set_shown()).
static bool shown;

static void build_dev_info_str(void);
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

	snprintf(dev_info_str, sizeof(dev_info_str),
			"Name:       %s\n"
			"Fleet:      %s\n"
			"Client id:  %s\n"
			"State:      %u\n"
			"Uptime:     %u h %02u min\n"
			"Remote:     UI %s, CAN %s\n"
			"Display:    %s\n"
			"Messages:   %u\n"
			"Last seen:  %u s ago\n",
			(devname[0] != '\0') ? devname : "(not heard from yet)",
			mqtt_get_fleet_name(f),
			mqtt_get_dev_name(f, d),
			(unsigned int) mqtt_get_dev_state(f, d),
			(unsigned int) (up / 3600u), (unsigned int) ((up / 60u) % 60u),
			((feat & REMOTE_IOT_FEATURE_UI) != 0) ? "on" : "off",
			((feat & REMOTE_IOT_FEATURE_CAN) != 0) ? "on" : "off",
			size_str,
			(unsigned int) mqtt_get_dev_msg_count(f, d),
			(unsigned int) mqtt_get_dev_age_s(f, d));
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


/// @brief: Receives one mirrored frame. Frames for a device other than the one
/// on screen are ignored: the device may still be finishing a frame that was in
/// flight when mirroring was switched off.
static void ui_frame_callb(uint8_t fleet_index, uint8_t dev_index,
		const uint8_t *cmds, uint32_t len, void *user) {
	(void) user;
	if (remoteui_win_is_open() &&
			(fleet_index == ui_fleet) &&
			(dev_index == ui_dev)) {
		remoteui_win_draw_frame(cmds, len);
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
	remoteui_win_set_asset_request_callb(&ui_asset_req_callb, NULL);
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

		uv_uibutton_init(&dev_ui_btn, ui_btn_text(), style);
		uv_uitabwindow_addxy(&dev_tabs, &dev_ui_btn, MARGIN,
				MARGIN + info_h + MARGIN, 5 * BUTTON_H, BUTTON_H);
		refresh_ui_btn();

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
			char title[160];
			const char *devname = mqtt_get_dev_devname((uint8_t) ui_fleet,
					(uint8_t) ui_dev);
			snprintf(title, sizeof(title), "%s - %s",
					(devname[0] != '\0') ? devname : "remote device",
					mqtt_get_dev_name((uint8_t) ui_fleet, (uint8_t) ui_dev));
			if (!remoteui_win_open(title, w, h)) {
				ui_stop();
			}
			if (shown) {
				refresh_ui_btn();
			}
		}
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
