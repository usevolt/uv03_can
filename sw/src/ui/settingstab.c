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
#include <ctype.h>
#include "credentials.h"
#include "loadparam.h"
#include "mqtt.h"
#include "remotefiles.h"
#include "ui/uvui.h"
#include "ui/uv_uifileedit.h"
#include "ui/uv_uitextedit.h"


// Colour of a status line whose server reported a failure
#define WARNING_COLOR	C(0xFFE02020)
// Colour of a status line whose server is connected, the device tabs' "online"
// green
#define DOT_COLOR_OP	C(0xFF22B14C)
#define MARGIN			10
#define TITLE_H			30
#define BUTTON_H		44
// A row of the parameter file list, and the gap between two rows
#define PARAM_ROW_H		BUTTON_H
#define PARAM_ROW_GAP	4


// Selectable file types for the "Add parameter files" chooser. Parameter files
// are written in JSON or in YAML, told apart by the extension.
static const uv_uifileedit_filter_st PARAM_FILE_FILTERS[] = {
	{ "Parameter files", "*.json *.yaml *.yml" },
	{ "All files", "*" },
};


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

	// The "Load parameters" panel: the file list on the left, the buttons on the
	// right
	uv_uiframewindow_st params_frame;
	uv_uiobject_st *params_frame_buf[5];
	uv_uiwindow_st params_list;
	uv_uiobject_st *params_list_buf[4 * LOADPARAM_FILES_MAX + 2];
	uv_uilabel_st params_empty;
	uv_uilabel_st param_labels[LOADPARAM_FILES_MAX];
	uv_uibutton_st param_up_btns[LOADPARAM_FILES_MAX];
	uv_uibutton_st param_down_btns[LOADPARAM_FILES_MAX];
	uv_uibutton_st param_remove_btns[LOADPARAM_FILES_MAX];
	uv_uibutton_st params_add_btn;
	uv_uibutton_st params_load_btn;
} content;


// The parameter files of the "Load parameters" panel, in the order they are
// loaded. File-scope so the list outlives the tab rebuilds.
static char param_files[LOADPARAM_FILES_MAX][LOADPARAM_FILE_LEN];
static uint8_t param_file_count;

// True while the panel's load runs (see settingstab_is_busy())
static bool params_loading;


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


// The file name part of *path*
static const char *path_basename(const char *path) {
	const char *ret = path;
	for (const char *c = path; *c != '\0'; c++) {
		if ((*c == '/') || (*c == '\\')) {
			ret = c + 1;
		}
	}
	return ret;
}


// Orders two parameter file paths alphabetically by their file names, case
// insensitively; files of the same name by their whole paths.
static int param_file_cmp(const char *a, const char *b) {
	const char *na = path_basename(a);
	const char *nb = path_basename(b);
	int ret = 0;
	while ((ret == 0) && ((*na != '\0') || (*nb != '\0'))) {
		ret = tolower((unsigned char) *na) - tolower((unsigned char) *nb);
		na += (*na != '\0') ? 1 : 0;
		nb += (*nb != '\0') ? 1 : 0;
	}
	if (ret == 0) {
		ret = strcmp(a, b);
	}
	return ret;
}


// Adds the newline-separated paths of *list* (modified in place) to the end of
// the parameter file list, in alphabetical order among themselves. A file which
// already is on the list is not added again.
static void params_add(char *list) {
	char *added[LOADPARAM_FILES_MAX];
	uint8_t n = 0;
	char *tok = list;
	while ((tok != NULL) && (*tok != '\0')) {
		char *nl = strchr(tok, '\n');
		if (nl != NULL) {
			*nl = '\0';
		}
		bool dup = false;
		for (uint8_t i = 0; (i < param_file_count) && !dup; i++) {
			dup = (strcmp(param_files[i], tok) == 0);
		}
		for (uint8_t i = 0; (i < n) && !dup; i++) {
			dup = (strcmp(added[i], tok) == 0);
		}
		if (dup) {
			// already listed
		}
		else if (strlen(tok) >= LOADPARAM_FILE_LEN) {
			printf("The path of the parameter file '%s' is too long to be "
					"loaded; it is not added.\n", tok);
		}
		else if ((param_file_count + n) >= LOADPARAM_FILES_MAX) {
			printf("The list holds at most %u parameter files; '%s' is not "
					"added.\n", (unsigned int) LOADPARAM_FILES_MAX, tok);
		}
		else {
			// insertion sort: find the place of the new file among the added
			uint8_t pos = n;
			while ((pos > 0) && (param_file_cmp(added[pos - 1], tok) > 0)) {
				added[pos] = added[pos - 1];
				pos--;
			}
			added[pos] = tok;
			n++;
		}
		tok = (nl != NULL) ? (nl + 1) : NULL;
	}
	fflush(stdout);
	for (uint8_t i = 0; i < n; i++) {
		strcpy(param_files[param_file_count++], added[i]);
	}
}


// Draws an arrow button: the button itself and a triangle pointing up or down
// on it. The UI font has no arrow glyphs.
static void arrow_btn_draw(void *me, bool up) {
	uv_uibutton_draw(me, NULL);
	int16_t x = uv_ui_get_xglobal(me);
	int16_t y = uv_ui_get_yglobal(me);
	int16_t w = uv_uibb(me)->width;
	int16_t h = uv_uibb(me)->height;
	// half the arrow's width, and its height
	int16_t s = ((w < h) ? w : h) / 4;
	int16_t cx = x + w / 2;
	int16_t cy = y + h / 2;
	int16_t tip = up ? (cy - s / 2) : (cy + s / 2);
	int16_t base = up ? (cy + s / 2) : (cy - s / 2);
	uv_ui_linestrip_point_st p[3] = {
			{ cx - s, base },
			{ cx + s, base },
			{ cx, tip }
	};
	uv_uibutton_st *btn = me;
	// a disabled arrow (the first row cannot move up) is drawn faint
	color_t c = ((uv_uiobject_st*) me)->enabled ?
			btn->text_c : uv_uic_brighten(btn->main_c, 40);
	uv_ui_draw_polygon(p, 3, c);
}

static void up_btn_draw(void *me, const uv_bounding_box_st *pbb) {
	(void) pbb;
	arrow_btn_draw(me, true);
}

static void down_btn_draw(void *me, const uv_bounding_box_st *pbb) {
	(void) pbb;
	arrow_btn_draw(me, false);
}


// Enables the panel's buttons according to the list and whether a load runs
static void params_refresh_buttons(void) {
	if (params_loading || (param_file_count >= LOADPARAM_FILES_MAX)) {
		uv_uiobject_disable(&content.params_add_btn);
	}
	else {
		uv_uiobject_enable(&content.params_add_btn);
	}
	if (params_loading || (param_file_count == 0)) {
		uv_uiobject_disable(&content.params_load_btn);
	}
	else {
		uv_uiobject_enable(&content.params_load_btn);
	}
	uv_ui_refresh(&content.params_add_btn);
	uv_ui_refresh(&content.params_load_btn);
}


// (Re)builds the rows of the parameter file list: the file name, the up and
// down arrows moving the file in the load order and a button removing it
static void params_build_list(void) {
	const uv_uistyle_st *style = &uv_uistyles[0];
	uv_uiwindow_clear(&content.params_list);
	int16_t w = uv_uibb(&content.params_list)->width;
	int16_t h = uv_uibb(&content.params_list)->height;

	if (param_file_count == 0) {
		uv_uilabel_init(&content.params_empty, style->font, ALIGN_CENTER,
				style->text_color, "No parameter files. Add them with "
				"\"Add parameter files\".");
		uv_uiwindow_addxy(&content.params_list, &content.params_empty,
				0, 0, w, h);
		uv_uiwindow_set_contentbb(&content.params_list, w, h);
	}
	else {
		int16_t rows_h = param_file_count * PARAM_ROW_H +
				(param_file_count - 1) * PARAM_ROW_GAP;
		// leave room for the scroll bar when the rows do not fit
		int16_t list_w = w;
		if (rows_h > h) {
			list_w -= CONFIG_UI_WINDOW_SCROLLBAR_WIDTH + PARAM_ROW_GAP;
		}
		int16_t arrow_w = 3 * PARAM_ROW_H / 2;
		int16_t remove_w = 100;
		int16_t remove_x = list_w - remove_w;
		int16_t down_x = remove_x - PARAM_ROW_GAP - arrow_w;
		int16_t up_x = down_x - PARAM_ROW_GAP - arrow_w;
		int16_t name_w = up_x - PARAM_ROW_GAP - MARGIN;

		for (uint8_t i = 0; i < param_file_count; i++) {
			int16_t row_y = i * (PARAM_ROW_H + PARAM_ROW_GAP);
			uv_uilabel_init(&content.param_labels[i], style->font,
					ALIGN_CENTER_LEFT, style->text_color,
					(char*) path_basename(param_files[i]));
			uv_uiwindow_addxy(&content.params_list, &content.param_labels[i],
					MARGIN, row_y, name_w, PARAM_ROW_H);

			uv_uibutton_init(&content.param_up_btns[i], "", style);
			uv_uiobject_set_draw_callb(&content.param_up_btns[i], &up_btn_draw);
			uv_uiwindow_addxy(&content.params_list, &content.param_up_btns[i],
					up_x, row_y, arrow_w, PARAM_ROW_H);

			uv_uibutton_init(&content.param_down_btns[i], "", style);
			uv_uiobject_set_draw_callb(&content.param_down_btns[i],
					&down_btn_draw);
			uv_uiwindow_addxy(&content.params_list, &content.param_down_btns[i],
					down_x, row_y, arrow_w, PARAM_ROW_H);

			uv_uibutton_init(&content.param_remove_btns[i], "Remove", style);
			uv_uiwindow_addxy(&content.params_list, &content.param_remove_btns[i],
					remove_x, row_y, remove_w, PARAM_ROW_H);

			if (params_loading || (i == 0)) {
				uv_uiobject_disable(&content.param_up_btns[i]);
			}
			if (params_loading || (i == (param_file_count - 1))) {
				uv_uiobject_disable(&content.param_down_btns[i]);
			}
			if (params_loading) {
				uv_uiobject_disable(&content.param_remove_btns[i]);
			}
		}
		uv_uiwindow_set_contentbb(&content.params_list, w,
				(rows_h > h) ? rows_h : h);
	}
	uv_ui_refresh(&content.params_list);
}


// The mouse wheel scrolls the parameter file list while the pointer is over it.
// A window scrolls itself by dragging only; see the device tab's simulator list.
static void params_list_wheel_step(void) {
	if ((param_file_count != 0) && !uvui_log_is_expanded()) {
		int16_t x = 0;
		int16_t y = 0;
		uv_ui_get_touch(&x, &y);
		int16_t gx = uv_ui_get_xglobal(&content.params_list);
		int16_t gy = uv_ui_get_yglobal(&content.params_list);
		uv_bounding_box_st *bb = uv_uibb(&content.params_list);
		if ((x >= gx) && (x < (gx + bb->width)) &&
				(y >= gy) && (y < (gy + bb->height))) {
			int16_t scroll = uv_ui_get_scroll();
			if (scroll != 0) {
				uv_uiwindow_content_move(&content.params_list, 0,
						scroll * (PARAM_ROW_H + PARAM_ROW_GAP));
			}
		}
	}
}


// Polls the "Load parameters" panel's buttons while the tab is shown
static void params_step(void) {
	bool rebuild = false;
	if (params_loading) {
		// nothing is clickable while the load runs
	}
	else if (uv_uibutton_clicked(&content.params_add_btn)) {
		// static: room for every file the list can hold, too much for the stack
		static char picked[LOADPARAM_FILES_MAX * LOADPARAM_FILE_LEN];
		if (uv_uifiledialog_exec_multi("Add parameter files", PARAM_FILE_FILTERS,
				sizeof(PARAM_FILE_FILTERS) / sizeof(PARAM_FILE_FILTERS[0]),
				picked, sizeof(picked))) {
			params_add(picked);
			rebuild = true;
		}
	}
	else if (uv_uibutton_clicked(&content.params_load_btn)) {
		const char *files[LOADPARAM_FILES_MAX];
		for (uint8_t i = 0; i < param_file_count; i++) {
			files[i] = param_files[i];
		}
		uvui_set_log_title("Loading parameter files...");
		loadparam_load_files_async(files, param_file_count);
		params_loading = true;
		rebuild = true;
	}
	else {
		for (uint8_t i = 0; (i < param_file_count) && !rebuild; i++) {
			if (uv_uibutton_clicked(&content.param_up_btns[i]) && (i > 0)) {
				char tmp[LOADPARAM_FILE_LEN];
				strcpy(tmp, param_files[i - 1]);
				strcpy(param_files[i - 1], param_files[i]);
				strcpy(param_files[i], tmp);
				rebuild = true;
			}
			else if (uv_uibutton_clicked(&content.param_down_btns[i]) &&
					(i < (param_file_count - 1))) {
				char tmp[LOADPARAM_FILE_LEN];
				strcpy(tmp, param_files[i + 1]);
				strcpy(param_files[i + 1], param_files[i]);
				strcpy(param_files[i], tmp);
				rebuild = true;
			}
			else if (uv_uibutton_clicked(&content.param_remove_btns[i])) {
				memmove(param_files[i], param_files[i + 1],
						(param_file_count - i - 1) * sizeof(param_files[0]));
				param_file_count--;
				rebuild = true;
			}
			else {
			}
		}
	}
	if (rebuild) {
		params_build_list();
		params_refresh_buttons();
	}
}


bool settingstab_is_busy(void) {
	return params_loading;
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

	// The "Load parameters" panel fills the rest of the tab: the file list on the
	// left, and on the right the buttons, as wide as the "Connect" button above
	int16_t params_y = MARGIN + account_frame_h + MARGIN;
	uv_uiframewindow_init(&content.params_frame, content.params_frame_buf, style);
	uv_uiframewindow_set_title(&content.params_frame, "Load parameters");
	uv_uitabwindow_addxy(tabwin, &content.params_frame, frame_x, params_y,
			frame_w, cbb.h - params_y - MARGIN);
	uv_bounding_box_st pc = uv_uiframewindow_get_content_bb(&content.params_frame);
	int16_t params_btn_w = acc_conn_w;
	int16_t params_list_w = pc.w - params_btn_w - MARGIN;

	uv_uiwindow_init(&content.params_list, content.params_list_buf, style);
	uv_uiframewindow_addxy(&content.params_frame, &content.params_list,
			0, 0, params_list_w, pc.h);

	uv_uibutton_init(&content.params_add_btn, "Add parameter files", style);
	uv_uiframewindow_addxy(&content.params_frame, &content.params_add_btn,
			params_list_w + MARGIN, 0, params_btn_w, BUTTON_H);

	uv_uibutton_init(&content.params_load_btn, "Load parameters", style);
	uv_uiframewindow_addxy(&content.params_frame, &content.params_load_btn,
			params_list_w + MARGIN, pc.h - BUTTON_H, params_btn_w, BUTTON_H);

	params_build_list();
	params_refresh_buttons();

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

	// the parameter file load finished: give the panel its buttons back. Watched
	// whichever tab is shown, though the load keeps the user on this one.
	if (params_loading && loadparam_load_files_is_finished()) {
		params_loading = false;
		uvui_reset_log_title();
		if (shown) {
			params_build_list();
			params_refresh_buttons();
		}
	}
	else {
	}

	if (shown) {
		params_list_wheel_step();
		params_step();
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
