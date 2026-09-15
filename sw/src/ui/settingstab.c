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


#include "ui/settingstab.h"

#if CONFIG_UI

#include <stdio.h>
#include <string.h>
#include "credentials.h"
#include "mqtt.h"
#include "remotefiles.h"
#include "ui/uv_uitextedit.h"


// Colour of a status line whose server reported a failure
#define WARNING_COLOR	C(0xFFE02020)
// Colour of a status line whose server is connected, the device tabs' "online"
// green
#define DOT_COLOR_OP	C(0xFF22B14C)
#define MARGIN			10
#define TITLE_H			30
#define BUTTON_H		44


// The "Account" panel: the two servers' URLs, and a username and a password
// field whose values are stored on this computer and shared by every uvcan
// install (see credentials.c). Edits are saved back in settingstab_step().
static struct {
	uv_uiframewindow_st account_frame;
	uv_uiobject_st *account_frame_buf[9];
	uv_uitextedit_st account_url;
	uv_uitextedit_st account_fleet_url;
	uv_uitextedit_st account_user;
	uv_uitextedit_st account_pass;
	// "Connect" button opening both sessions with the fields above, and the
	// status line beside it (green while a session is open)
	uv_uibutton_st account_connect_btn;
	// One label per server rather than one with two lines: each carries its own
	// colour, so a file server failure cannot paint a healthy broker red.
	uv_uilabel_st account_status;
	uv_uilabel_st account_status_fleet;
	char account_status_str[256];
	char account_status_fleet_str[256];
} content;


// Whether the widgets above are part of the display right now. The main tab
// window clears them whenever another main tab is picked.
static bool shown;

// Text buffers backing the "Account" panel's fields. They must outlive the tab
// rebuilds (the textedits read/write them in place), so they are file-scope
// rather than part of *content*. Seeded once from the stored credentials the
// first time the tab is built; edits are saved back to the shared file in
// settingstab_step().
static char account_url_buf[CREDENTIALS_MAX];
static char account_fleet_url_buf[CREDENTIALS_MAX];
static char account_user_buf[CREDENTIALS_MAX];
static char account_pass_buf[CREDENTIALS_MAX];
static bool account_seeded;

// Reason the last "Connect" attempt failed, shown in red under the fields until
// the next attempt. "" when there was none.
static char account_err[256];

// Last broker state the Account panel drew. The connection completes
// asynchronously, well after the button was pressed, so the panel has to notice
// the change itself or it keeps showing "connecting...".
static mqtt_state_e account_last_mqtt = MQTT_STATE_DISCONNECTED;

// Whether the stored credentials have been tried yet. Reconnecting is the
// normal case on start-up - the account was entered once and is meant to keep
// working - so it happens by itself rather than making the user press Connect
// every time.
static bool account_autoconnect_tried;


/// @brief: Opens both sessions with whatever is stored, if anything is.
static void account_connect(void) {
	account_err[0] = '\0';
	printf("File server: connecting to '%s' as '%s'...\n",
			credentials_get_url(), credentials_get_username());
	fflush(stdout);
	if (remotefiles_login(credentials_get_url(), credentials_get_username(),
			credentials_get_password(), account_err, sizeof(account_err))) {
		printf("File server: connected to '%s' as '%s', %u fleet(s):",
				credentials_get_url(), credentials_get_username(),
				(unsigned int) remotefiles_get_fleet_count());
		for (uint8_t i = 0; i < remotefiles_get_fleet_count(); i++) {
			printf(" %s", remotefiles_get_fleet(i));
		}
		printf("\n");
		fflush(stdout);
	}
	else {
		printf("File server: connecting to '%s' failed: %s\n",
				credentials_get_url(), account_err);
		fflush(stdout);
	}

	// the same account opens the fleet broker, so one action does both
	if (!mqtt_connect(credentials_fleet_get_url(), credentials_get_username(),
			credentials_get_password())) {
		if (account_err[0] == '\0') {
			strncpy(account_err, mqtt_get_error(), sizeof(account_err) - 1);
			account_err[sizeof(account_err) - 1] = '\0';
		}
	}
	else {
		// The file server already said which fleets this account holds, so
		// their tabs can exist before any device has published. Only the
		// broker knows whether they are alive; only the file server knows
		// they exist at all when they are quiet.
		for (uint8_t i = 0; i < remotefiles_get_fleet_count(); i++) {
			mqtt_add_fleet(remotefiles_get_fleet(i));
		}
	}
}


/// @brief: Rewrites the Account panel's status line from the current file-server
/// session state and greys the "Connect" button out while that session is open.
/// Called after building the panel and whenever the state changes, so the panel
/// never has to be rebuilt just to reflect a connect / disconnect.
static void account_refresh_status(void) {
	bool files = remotefiles_is_logged_in();
	bool fleet = mqtt_is_connected();
	const char *user = credentials_get_username();

	// One line per server. They are two different protocols against two
	// different hosts and either can be up without the other, so a single
	// combined line could only ever be vague about which one had failed.
	char files_line[192];
	if (files) {
		snprintf(files_line, sizeof(files_line),
				"Files: connected to %.100s as '%.60s'",
				credentials_get_url(), user);
	}
	else if (account_err[0] != '\0') {
		snprintf(files_line, sizeof(files_line), "Files: %.180s", account_err);
	}
	else {
		strcpy(files_line, "Files: not connected");
	}

	char fleet_line[192];
	switch (mqtt_get_state()) {
	case MQTT_STATE_CONNECTED:
		snprintf(fleet_line, sizeof(fleet_line), "Fleet: connected to %s as '%s'",
				credentials_fleet_get_url(), user);
		break;
	case MQTT_STATE_CONNECTING:
		snprintf(fleet_line, sizeof(fleet_line), "Fleet: connecting to %s...",
				credentials_fleet_get_url());
		break;
	case MQTT_STATE_ERROR:
		snprintf(fleet_line, sizeof(fleet_line), "Fleet: %s", mqtt_get_error());
		break;
	default:
		strcpy(fleet_line, "Fleet: not connected");
		break;
	}

	snprintf(content.account_status_str, sizeof(content.account_status_str),
			"%s", files_line);
	snprintf(content.account_status_fleet_str,
			sizeof(content.account_status_fleet_str), "%s", fleet_line);

	// each line is coloured by its own server, so one failing does not paint
	// the other red
	color_t files_c = files ? DOT_COLOR_OP :
			((account_err[0] != '\0') ? WARNING_COLOR :
					uv_uistyles[0].text_color);
	color_t fleet_c = fleet ? DOT_COLOR_OP :
			((mqtt_get_state() == MQTT_STATE_ERROR) ? WARNING_COLOR :
					uv_uistyles[0].text_color);

	// the button reconnects whatever is still down
	if (files && fleet) {
		uv_uiobject_disable(&content.account_connect_btn);
	}
	else {
		uv_uiobject_enable(&content.account_connect_btn);
	}
	uv_uilabel_set_color(&content.account_status, files_c);
	uv_uilabel_set_color(&content.account_status_fleet, fleet_c);
	uv_ui_refresh(&content.account_status);
	uv_ui_refresh(&content.account_status_fleet);
	uv_ui_refresh(&content.account_connect_btn);
}


void settingstab_show(uv_uitabwindow_st *tabwin) {
	const uv_uistyle_st *style = &uv_uistyles[0];
	uv_bounding_box_st cbb = uv_uitabwindow_get_contentbb(tabwin);

	int16_t frame_x = MARGIN;
	int16_t frame_w = cbb.w - 2 * MARGIN;
	// the Account panel: the fields row (URL / Username / Password, each a field
	// with its title below it, sharing the row with the "Connect" button), plus a
	// second row holding the fleet URL and the status lines.
	//
	// Sized to what a row actually draws rather than to button heights: a text
	// field is one line of text with a little padding, and its title is another
	// line under it (see uv_uitextedit_draw). Rows the height of a button were
	// half empty, and the panel wore a band of nothing between the fields and
	// the status lines below them.
	int16_t acc_row_h = 2 * uv_ui_get_font_height(style->font) + 4 * MARGIN / 2;
	// plus the frame's own title bar, which get_content_bb takes off the top
	int16_t account_frame_h = 2 * acc_row_h + MARGIN + TITLE_H;

	// Seed the fields once from the stored values (later rebuilds keep whatever
	// is in the buffers, including unsaved edits)
	if (!account_seeded) {
		strncpy(account_url_buf, credentials_get_url(),
				sizeof(account_url_buf) - 1);
		account_url_buf[sizeof(account_url_buf) - 1] = '\0';
		strncpy(account_fleet_url_buf, credentials_fleet_get_url(),
				sizeof(account_fleet_url_buf) - 1);
		account_fleet_url_buf[sizeof(account_fleet_url_buf) - 1] = '\0';
		strncpy(account_user_buf, credentials_get_username(),
				sizeof(account_user_buf) - 1);
		account_user_buf[sizeof(account_user_buf) - 1] = '\0';
		strncpy(account_pass_buf, credentials_get_password(),
				sizeof(account_pass_buf) - 1);
		account_pass_buf[sizeof(account_pass_buf) - 1] = '\0';
		account_seeded = true;
	}

	uv_uiframewindow_init(&content.account_frame, content.account_frame_buf, style);
	uv_uiframewindow_set_title(&content.account_frame, "Account");
	uv_uitabwindow_addxy(tabwin, &content.account_frame, frame_x, MARGIN,
			frame_w, account_frame_h);
	uv_bounding_box_st ac = uv_uiframewindow_get_content_bb(&content.account_frame);

	// Two field rows on the left and the "Connect" button filling the panel's
	// full height on the right. The status shares the second row with the fleet
	// URL rather than taking one of its own, which keeps the panel a row
	// shorter.
	// the frame was sized from acc_row_h above; share out whatever rounding is
	// left rather than letting the two drift apart
	acc_row_h = (ac.h - MARGIN) / 2;

	int16_t acc_gap = MARGIN;
	int16_t acc_conn_w = 3 * 2 * BUTTON_H;
	int16_t acc_fields_w = ac.w - acc_conn_w - 4 * acc_gap;
	int16_t acc_url_w = acc_fields_w / 2;
	int16_t acc_field_w = (acc_fields_w - acc_url_w) / 2;
	int16_t acc_x = 0;

	// top row: the two servers' credentials. The user name and the password are
	// shared - one Usevolt account opens both the file server and the fleet
	// broker - so they are entered once here.
	uv_uitextedit_init(&content.account_url, account_url_buf,
			sizeof(account_url_buf), UITEXTEDIT_FLAG_ONELINE, style);
	uv_uitextedit_set_title(&content.account_url, "File server URL");
	uv_uitextedit_set_align(&content.account_url, ALIGN_CENTER_LEFT);
	uv_uiframewindow_addxy(&content.account_frame, &content.account_url,
			acc_x, 0, acc_url_w, acc_row_h);

	// second row, directly under it: the broker the Fleet tab talks to
	uv_uitextedit_init(&content.account_fleet_url, account_fleet_url_buf,
			sizeof(account_fleet_url_buf), UITEXTEDIT_FLAG_ONELINE, style);
	uv_uitextedit_set_title(&content.account_fleet_url, "Fleet URL");
	uv_uitextedit_set_align(&content.account_fleet_url, ALIGN_CENTER_LEFT);
	uv_uiframewindow_addxy(&content.account_frame, &content.account_fleet_url,
			acc_x, acc_row_h + MARGIN, acc_url_w, acc_row_h);
	acc_x += acc_url_w + acc_gap;

	uv_uitextedit_init(&content.account_user, account_user_buf,
			sizeof(account_user_buf), UITEXTEDIT_FLAG_ONELINE, style);
	uv_uitextedit_set_title(&content.account_user, "Username");
	uv_uitextedit_set_align(&content.account_user, ALIGN_CENTER_LEFT);
	uv_uiframewindow_addxy(&content.account_frame, &content.account_user,
			acc_x, 0, acc_field_w, acc_row_h);
	acc_x += acc_field_w + acc_gap;

	uv_uitextedit_init(&content.account_pass, account_pass_buf,
			sizeof(account_pass_buf),
			UITEXTEDIT_FLAG_ONELINE | UITEXTEDIT_FLAG_PASSWORD, style);
	uv_uitextedit_set_title(&content.account_pass, "Password");
	uv_uitextedit_set_align(&content.account_pass, ALIGN_CENTER_LEFT);
	uv_uiframewindow_addxy(&content.account_frame, &content.account_pass,
			acc_x, 0, acc_field_w, acc_row_h);
	acc_x += acc_field_w + acc_gap;

	// the "Connect" button fills the panel top to bottom, so it is a large,
	// easy target and reads as acting on everything to its left
	uv_uibutton_init(&content.account_connect_btn, "Connect", style);
	uv_uiframewindow_addxy(&content.account_frame, &content.account_connect_btn,
			acc_x, 0, ac.w - acc_x, ac.h);

	// the two status lines sit beside the fleet URL, filling the width the user
	// name and password fields occupy on the row above
	int16_t acc_status_x = acc_url_w + acc_gap;
	int16_t acc_status_w = acc_x - acc_gap - acc_status_x;
	int16_t acc_status_line_h = acc_row_h / 2;
	uv_uilabel_init(&content.account_status, style->font, ALIGN_CENTER,
			style->text_color, content.account_status_str);
	uv_uiframewindow_addxy(&content.account_frame, &content.account_status,
			acc_status_x, acc_row_h + MARGIN, acc_status_w, acc_status_line_h);

	uv_uilabel_init(&content.account_status_fleet, style->font, ALIGN_CENTER,
			style->text_color, content.account_status_fleet_str);
	uv_uiframewindow_addxy(&content.account_frame, &content.account_status_fleet,
			acc_status_x, acc_row_h + MARGIN + acc_status_line_h,
			acc_status_w, acc_status_line_h);

	shown = true;
	account_last_mqtt = mqtt_get_state();
	account_refresh_status();
}


void settingstab_set_shown(bool value) {
	shown = value;
}


void settingstab_step(void) {
	// Connect with the stored account once, whichever main tab the window opened
	// on: the file server and the fleet broker are used from the other tabs.
	if (!account_autoconnect_tried) {
		account_autoconnect_tried = true;
		// only when there is something to try with; an empty account would just
		// produce a failure message nobody asked for
		if ((credentials_get_username()[0] != '\0') &&
				(credentials_get_password()[0] != '\0')) {
			account_connect();
		}
		else {
		}
	}
	else {
	}

	if (shown &&
			(mqtt_get_state() != account_last_mqtt)) {
		account_last_mqtt = mqtt_get_state();
		account_refresh_status();
	}
	else {
	}

	// persist the Account fields whenever the user commits an edit (Enter or click
	// away). Editing them is equivalent to running with --user / --pwd.
	if (shown) {
		bool account_edited = false;
		if (uv_uitextedit_value_changed(&content.account_url)) {
			credentials_set_url(uv_uitextedit_get_text(&content.account_url));
			account_edited = true;
		}
		if (uv_uitextedit_value_changed(&content.account_fleet_url)) {
			credentials_fleet_set_url(
					uv_uitextedit_get_text(&content.account_fleet_url));
			// the open fleet session was made against the previous broker
			mqtt_disconnect();
			account_edited = true;
		}
		if (uv_uitextedit_value_changed(&content.account_user)) {
			credentials_set_username(
					uv_uitextedit_get_text(&content.account_user));
			account_edited = true;
		}
		if (uv_uitextedit_value_changed(&content.account_pass)) {
			credentials_set_password(
					uv_uitextedit_get_text(&content.account_pass));
			account_edited = true;
		}
		// the session token belongs to the credentials that were in the fields when
		// it was issued: editing any of them drops it, which puts the status back to
		// "Not connected" and re-enables the button for reconnecting
		if (account_edited) {
			if (remotefiles_is_logged_in()) {
				printf("File server: disconnected (account settings changed)\n");
				fflush(stdout);
			}
			remotefiles_logout();
			// the user name and the password are shared, so the fleet session
			// was made with the old ones too
			mqtt_disconnect();
			account_err[0] = '\0';
			account_refresh_status();
		}

		// "Connect": log in to the file server with the current fields. This blocks
		// for the round trip (curl, like the "Server files" browser does), which is
		// short enough not to warrant its own task.
		if (uv_uibutton_clicked(&content.account_connect_btn)) {
			strcpy(content.account_status_str, "Connecting...");
			uv_uilabel_set_color(&content.account_status,
					uv_uistyles[0].text_color);
			uv_ui_refresh(&content.account_status);
			account_connect();
			account_refresh_status();
		}
	}
	else {
	}
}


#endif
