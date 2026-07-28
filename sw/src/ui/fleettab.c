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
#include "credentials.h"
#include "mqtt.h"
#include "ui/uv_uitextedit.h"
#include "ui/remoteui_win.h"
#include "uv_remote_proto.h"


// Margin in pixels around the tab content, height of a button / field row and of
// a plain text row. Matched to the system tab so the two Account panels line up.
#define MARGIN			10
#define BUTTON_H		44
#define TITLE_H			30

// Colour of the "connected" status line, and of a failure reason. Same green and
// red the device tabs use for their status dots and warnings.
#define OK_COLOR		C(0xFF22B14C)
#define WARNING_COLOR	C(0xFFE02020)


// --- "Account" panel: the broker address, the fleet admin credentials and the
// fleet name, all persisted with credentials_fleet_set_*(). The text buffers must
// outlive the frequent tab rebuilds (the textedits read and write them in place),
// so they are file-scope; they are seeded once from the stored values.
static char acc_url_buf[CREDENTIALS_MAX];
static char acc_user_buf[CREDENTIALS_MAX];
static char acc_pass_buf[CREDENTIALS_MAX];
static char acc_fleet_buf[CREDENTIALS_MAX];
static bool acc_seeded;

static uv_uiframewindow_st acc_frame;
static uv_uiobject_st *acc_frame_buf[8];
static uv_uitextedit_st acc_url;
static uv_uitextedit_st acc_user;
static uv_uitextedit_st acc_pass;
static uv_uitextedit_st acc_fleet;
static uv_uibutton_st acc_connect_btn;
static uv_uilabel_st acc_status;
static char acc_status_str[320];

// --- the fleet / device tab windows, shown below the Account panel once the
// broker connection is up.
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

// Reason the last connection attempt failed, shown in red until the next one.
static char acc_err[256];


static void acc_refresh_status(void);
static void build_dev_info_str(void);
static void build_fleet_tabs(uv_uitabwindow_st *tabwin, int16_t y, int16_t h);
static void show_active_dev_tab(void);
static uv_uiobject_ret_e fleet_tabs_step(void *me, const uint16_t step_ms);
static uv_uiobject_ret_e dev_tabs_step(void *me, const uint16_t step_ms);


void fleettab_set_shown(bool value) {
	shown = value;
}


/// @brief: Starts connecting with whatever is in the Account fields.
static void fleet_connect(void) {
	acc_err[0] = '\0';
	if (!mqtt_connect(credentials_fleet_get_url(),
			credentials_fleet_get_username(), credentials_fleet_get_password(),
			credentials_fleet_get_fleet())) {
		strncpy(acc_err, mqtt_get_error(), sizeof(acc_err) - 1);
		acc_err[sizeof(acc_err) - 1] = '\0';
		printf("Connecting to the broker failed: %s\n", acc_err);
		fflush(stdout);
	}
}


/// @brief: Rewrites the Account panel's status line from the current connection
/// state and greys the "Connect" button out while the connection is up (or being
/// established). Called on every state change, so the panel does not have to be
/// rebuilt just to reflect one.
static void acc_refresh_status(void) {
	color_t c = uv_uistyles[0].text_color;
	bool btn_enabled = true;

	switch (mqtt_get_state()) {
	case MQTT_STATE_CONNECTED:
		snprintf(acc_status_str, sizeof(acc_status_str),
				"Connected to %s as '%s'", mqtt_get_host(),
				credentials_fleet_get_username());
		c = OK_COLOR;
		btn_enabled = false;
		break;
	case MQTT_STATE_CONNECTING:
		snprintf(acc_status_str, sizeof(acc_status_str), "Connecting to %s...",
				mqtt_get_host());
		btn_enabled = false;
		break;
	case MQTT_STATE_ERROR:
		snprintf(acc_status_str, sizeof(acc_status_str), "%s", mqtt_get_error());
		c = WARNING_COLOR;
		break;
	case MQTT_STATE_DISCONNECTED:
	default:
		if (acc_err[0] != '\0') {
			snprintf(acc_status_str, sizeof(acc_status_str), "%s", acc_err);
			c = WARNING_COLOR;
		}
		else {
			strcpy(acc_status_str, "Not connected");
		}
		break;
	}

	uv_uilabel_set_color(&acc_status, c);
	uv_ui_set_enabled(&acc_connect_btn, btn_enabled);
	uv_ui_refresh(&acc_status);
	uv_ui_refresh(&acc_connect_btn);
}


/// @brief: Builds the "Account" panel at the top of the tab and returns its
/// height, so the caller knows where the fleet tabs start.
static int16_t build_account_panel(uv_uitabwindow_st *tabwin,
		uv_bounding_box_st cbb) {
	const uv_uistyle_st *style = &uv_uistyles[0];

	// seed the fields once from the stored values; later rebuilds keep whatever is
	// in the buffers, including edits the user has not committed yet
	if (!acc_seeded) {
		strncpy(acc_url_buf, credentials_fleet_get_url(),
				sizeof(acc_url_buf) - 1);
		strncpy(acc_user_buf, credentials_fleet_get_username(),
				sizeof(acc_user_buf) - 1);
		strncpy(acc_pass_buf, credentials_fleet_get_password(),
				sizeof(acc_pass_buf) - 1);
		strncpy(acc_fleet_buf, credentials_fleet_get_fleet(),
				sizeof(acc_fleet_buf) - 1);
		acc_seeded = true;
	}

	// laid out to match the system tab's Account panel: the fields share the top
	// row with the "Connect" button, and a shorter status row (a plain label) sits
	// below. The status row is only a label, so it gets a label's height (TITLE_H)
	// rather than a full button's, keeping the panel compact.
	int16_t frame_h = 2 * BUTTON_H + MARGIN + TITLE_H + MARGIN + TITLE_H;
	uv_uiframewindow_init(&acc_frame, acc_frame_buf, style);
	uv_uiframewindow_set_title(&acc_frame, "Account");
	uv_uitabwindow_addxy(tabwin, &acc_frame, MARGIN, MARGIN,
			cbb.w - 2 * MARGIN, frame_h);
	uv_bounding_box_st ac = uv_uiframewindow_get_content_bb(&acc_frame);

	int16_t status_h = TITLE_H;
	int16_t status_row_y = ac.h - status_h;
	int16_t field_h = status_row_y - MARGIN;

	// top row: URL, Username, Password, Fleet and the "Connect" button side by
	// side. The URL is the longest value so it takes two shares, the other three
	// fields one each; the button is kept narrow (four fields here leave less room
	// than the system tab's three). Each field draws its title below itself.
	int16_t gap = MARGIN;
	int16_t conn_w = 2 * BUTTON_H;
	int16_t unit_w = (ac.w - conn_w - 5 * gap) / 5;
	int16_t url_w = 2 * unit_w;
	int16_t x = 0;

	uv_uitextedit_init(&acc_url, acc_url_buf, sizeof(acc_url_buf),
			UITEXTEDIT_FLAG_ONELINE, style);
	uv_uitextedit_set_title(&acc_url, "URL");
	uv_uitextedit_set_align(&acc_url, ALIGN_CENTER_LEFT);
	uv_uiframewindow_addxy(&acc_frame, &acc_url, x, 0, url_w, field_h);
	x += url_w + gap;

	uv_uitextedit_init(&acc_user, acc_user_buf, sizeof(acc_user_buf),
			UITEXTEDIT_FLAG_ONELINE, style);
	uv_uitextedit_set_title(&acc_user, "Username");
	uv_uitextedit_set_align(&acc_user, ALIGN_CENTER_LEFT);
	uv_uiframewindow_addxy(&acc_frame, &acc_user, x, 0, unit_w, field_h);
	x += unit_w + gap;

	uv_uitextedit_init(&acc_pass, acc_pass_buf, sizeof(acc_pass_buf),
			UITEXTEDIT_FLAG_ONELINE | UITEXTEDIT_FLAG_PASSWORD, style);
	uv_uitextedit_set_title(&acc_pass, "Password");
	uv_uitextedit_set_align(&acc_pass, ALIGN_CENTER_LEFT);
	uv_uiframewindow_addxy(&acc_frame, &acc_pass, x, 0, unit_w, field_h);
	x += unit_w + gap;

	uv_uitextedit_init(&acc_fleet, acc_fleet_buf, sizeof(acc_fleet_buf),
			UITEXTEDIT_FLAG_ONELINE, style);
	uv_uitextedit_set_title(&acc_fleet, "Fleet");
	uv_uitextedit_set_align(&acc_fleet, ALIGN_CENTER_LEFT);
	uv_uiframewindow_addxy(&acc_frame, &acc_fleet, x, 0, unit_w, field_h);
	x += unit_w + gap;

	// the "Connect" button sits at the top of its column so it lines up with the
	// fields' entry boxes, not with the titles drawn below them
	uv_uibutton_init(&acc_connect_btn, "Connect", style);
	uv_uiframewindow_addxy(&acc_frame, &acc_connect_btn,
			x, 0, ac.w - x, BUTTON_H);

	// bottom row: the connection status, centered and filling the whole width
	uv_uilabel_init(&acc_status, style->font, ALIGN_CENTER,
			style->text_color, acc_status_str);
	uv_uiframewindow_addxy(&acc_frame, &acc_status,
			0, status_row_y, ac.w, status_h);

	acc_refresh_status();

	return frame_h;
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


/// @brief: Starts mirroring the selected device. The window can only be sized
/// once the device has said how big its display is, so this asks first and the
/// window opens from fleettab_step() when the answer arrives.
static void ui_start(void) {
	ui_stop();
	ui_fleet = selected_fleet;
	ui_dev = selected_dev;
	mqtt_set_ui_frame_callb(&ui_frame_callb, NULL);
	if (!mqtt_dev_set_ui_active((uint8_t) ui_fleet, (uint8_t) ui_dev, true)) {
		ui_fleet = -1;
		ui_dev = -1;
	}
}


/// @brief: Shows *text* in the area below the Account panel, for the states in
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

	int16_t acc_h = build_account_panel(tabwin, cbb);
	int16_t rest_y = MARGIN + acc_h + MARGIN;
	int16_t rest_h = cbb.h - rest_y - MARGIN;

	if (!mqtt_is_supported()) {
		build_placeholder(tabwin, "The fleet view needs an MQTT connection, "
				"which is not available in the Windows build.",
				rest_y, rest_h, cbb);
	}
	else if (!mqtt_is_connected()) {
		build_placeholder(tabwin, "Connect to the broker to see the fleets and "
				"their devices.", rest_y, rest_h, cbb);
	}
	else if (mqtt_get_fleet_count() == 0) {
		build_placeholder(tabwin, "Connected. No fleets seen yet - a fleet "
				"appears once one of its devices publishes something. Name a "
				"fleet in the Account panel to list it right away.",
				rest_y, rest_h, cbb);
	}
	else {
		build_fleet_tabs(tabwin, rest_y, rest_h);
	}

	shown = true;
}


/// @brief: Builds the fleet tab window (one tab per discovered fleet) into the
/// area below the Account panel, and fills it with the active fleet's devices.
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
		uv_uilabel_init(&dev_info, &UI_MONO_FONT, ALIGN_TOP_LEFT,
				style->text_color, dev_info_str);
		uv_uitabwindow_addxy(&dev_tabs, &dev_info, MARGIN, MARGIN,
				dc.w - 2 * MARGIN, dc.h - 2 * MARGIN);

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
		// persist every committed field edit (Enter or click away). Editing any of
		// them invalidates the current session - it was made with the previous
		// values - so the connection is dropped and "Connect" becomes available
		// again for reconnecting.
		bool edited = false;
		if (uv_uitextedit_value_changed(&acc_url)) {
			credentials_fleet_set_url(uv_uitextedit_get_text(&acc_url));
			edited = true;
		}
		if (uv_uitextedit_value_changed(&acc_user)) {
			credentials_fleet_set_username(uv_uitextedit_get_text(&acc_user));
			edited = true;
		}
		if (uv_uitextedit_value_changed(&acc_pass)) {
			credentials_fleet_set_password(uv_uitextedit_get_text(&acc_pass));
			edited = true;
		}
		if (uv_uitextedit_value_changed(&acc_fleet)) {
			credentials_fleet_set_fleet(uv_uitextedit_get_text(&acc_fleet));
			edited = true;
		}
		if (edited) {
			mqtt_disconnect();
			acc_err[0] = '\0';
			acc_refresh_status();
			// the fleet tree went away with the connection
			ret = true;
		}

		if (uv_uibutton_clicked(&acc_connect_btn)) {
			fleet_connect();
			acc_refresh_status();
		}

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

	// a state change or a newly discovered fleet / device means the tabs below the
	// Account panel no longer match what the client knows
	if (mqtt_poll_changed()) {
		if (shown) {
			acc_refresh_status();
		}
		ret = true;
	}

	return ret;
}


#endif
