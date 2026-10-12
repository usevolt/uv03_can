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
#include "fleet.h"
#include "remotecan.h"
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
// Settings tab's Account panel: one Usevolt account serves both the file server
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
// ...and the one that narrows what the device forwards to a parameter transfer
static uv_uicheckbox_st dev_sdo_cb;
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
	fleet_dev_info((uint8_t) selected_fleet, (uint8_t) selected_dev,
			dev_info_str, sizeof(dev_info_str));
}


/// @brief: True while the mirror window is showing the device whose tab is open.
static bool ui_shows_selected(void) {
	return fleet_ui_shows((uint8_t) selected_fleet, (uint8_t) selected_dev);
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
	uv_uicheckbox_set_state(&dev_sdo_cb, remotecan_get_sdo_only());
	uv_ui_refresh(&dev_can_btn);
	uv_ui_refresh(&dev_std_cb);
	uv_ui_refresh(&dev_ext_cb);
	uv_ui_refresh(&dev_sdo_cb);
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

		// What the bridge carries. Standard on, extended off is what a CANopen
		// machine wants, and "SDO only" narrows it further to the parameter
		// transfer itself. The boxes are here rather than behind a settings
		// page because they are the difference between a usable link and one
		// drowning in broadcasts.
		//
		// Laid out from the end of the button row, three to a row of their own
		// width: three boxes at the width the two used to have would run off
		// the right hand edge.
		int16_t cb_x = MARGIN + 3 * (5 * BUTTON_H + MARGIN);
		int16_t cb_w = 3 * BUTTON_H;
		uv_uicheckbox_init(&dev_std_cb, remotecan_get_allow_std(),
				"STD", style);
		uv_uitabwindow_addxy(&dev_tabs, &dev_std_cb, cb_x, btn_y,
				cb_w, BUTTON_H);

		cb_x += cb_w + MARGIN;
		uv_uicheckbox_init(&dev_ext_cb, remotecan_get_allow_ext(),
				"EXT", style);
		uv_uitabwindow_addxy(&dev_tabs, &dev_ext_cb, cb_x, btn_y,
				cb_w, BUTTON_H);

		cb_x += cb_w + MARGIN;
		uv_uicheckbox_init(&dev_sdo_cb, remotecan_get_sdo_only(),
				"SDO only", style);
		uv_uitabwindow_addxy(&dev_tabs, &dev_sdo_cb, cb_x, btn_y,
				cb_w, BUTTON_H);
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

	// The sessions are pumped whichever tab is shown: what arrived for the
	// mirror window, a device ending the session from its own screen, a bridge
	// whose broker connection is gone. When one of them started or ended the
	// buttons are put back in step before anything below compares against
	// them: the CAN toggle left on after the device closed the bridge would
	// otherwise start it straight back up.
	if (fleet_step() && shown && (dev_tab_count > 0)) {
		refresh_ui_btn();
		refresh_can_btn();
	}
	else {
	}

	if (shown) {
		if ((dev_tab_count > 0) && uv_uibutton_clicked(&dev_ui_btn)) {
			if (ui_shows_selected()) {
				fleet_ui_stop();
			}
			else {
				(void) fleet_ui_start((uint8_t) selected_fleet,
						(uint8_t) selected_dev);
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

		if ((dev_tab_count > 0) &&
				(uv_uicheckbox_get_state(&dev_sdo_cb) !=
						remotecan_get_sdo_only())) {
			// re-asks the device on its own if a bridge is up; this one does
			// not go through the filter table, it is a feature bit
			remotecan_set_sdo_only(uv_uicheckbox_get_state(&dev_sdo_cb));
			refresh_can_btn();
			ret = true;
		}
		else {
		}

		// "Remove Device": take this device out of the view. The tabs are
		// rebuilt from the client's device list, so the removal shows up as the
		// tab disappearing.
		if ((dev_tab_count > 0) && uv_uimediabutton_clicked(&dev_remove_btn)) {
			// the sessions held with the device end with it
			if (fleet_remove_dev((uint8_t) selected_fleet,
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

	// a state change or a newly discovered fleet / device means the tabs no
	// longer match what the client knows
	if (mqtt_poll_changed()) {
		ret = true;
	}

	return ret;
}


#endif
