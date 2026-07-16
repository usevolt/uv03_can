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


#include "ui/devicetab.h"

#if CONFIG_UI

#include <string.h>
#include <uv_canopen.h>
#include "main.h"
// PNG image for the "Remove" button, compiled into the binary as a byte array
// (minus_hd_png / minus_hd_png_len). Generated from media/minus_hd.png by the
// makefile via xxd.
#include "media_minus_hd.h"
#include "find.h"
#include "saveparam.h"
#include "loadparam.h"
#include "savesys.h"
#include "load.h"
#include "loadmedia.h"
#include "uvui.h"
#include "simrun.h"
#include "terminaltab.h"
#include "credentials.h"
#include "ui/uv_uitextedit.h"
#include "ui/serverfiles_win.h"


// Color for the revision-mismatch warning and the mismatched revision numbers.
#define WARNING_COLOR		C(0xFFE02020)


// Selectable file types for the parameter save/load dialogs.
static const uv_uifileedit_filter_st PARAM_FILE_FILTERS[] = {
	{ "Parameter files", "*.json" },
	{ "All files", "*" },
};


// Margin in pixels around the tab content.
#define MARGIN			10
// Height of the title label (used by the simple title+info tabs).
#define TITLE_H			30
// Height of a file picker on the system tab.
#define FILEEDIT_H		56
// Height of the "Search devices" button on the system tab.
#define BUTTON_H		44

// Status-dot colours, by CAN-bus state: grey when offline, light yellow on
// boot-up, strong yellow in pre-operational and green when operational.
#define DOT_COLOR_OFFLINE	C(0xFF888888)
#define DOT_COLOR_BOOTUP	C(0xFFF7E98A)
#define DOT_COLOR_PREOP		C(0xFFF2C200)
#define DOT_COLOR_OP		C(0xFF22B14C)


/// @brief: Maps a device's CAN-bus state onto its status-dot colour.
static color_t dot_color_for_state(dev_state_e state) {
	color_t ret;
	switch (state) {
	case DEV_STATE_OP:
		ret = DOT_COLOR_OP;
		break;
	case DEV_STATE_PREOP:
		ret = DOT_COLOR_PREOP;
		break;
	case DEV_STATE_BOOTUP:
		ret = DOT_COLOR_BOOTUP;
		break;
	case DEV_STATE_OFFLINE:
	default:
		ret = DOT_COLOR_OFFLINE;
		break;
	}
	return ret;
}


/// @brief: Maps a device's CAN-bus state onto the label shown for it.
static const char *state_name_str(dev_state_e state) {
	const char *ret;
	switch (state) {
	case DEV_STATE_OP:
		ret = "OPERATIONAL";
		break;
	case DEV_STATE_PREOP:
		ret = "PRE-OPERATIONAL";
		break;
	case DEV_STATE_BOOTUP:
		ret = "BOOT-UP";
		break;
	case DEV_STATE_OFFLINE:
	default:
		ret = "OFFLINE";
		break;
	}
	return ret;
}



// A tiny custom widget that draws a status dot centered in its bounding box. Used
// on a device tab to indicate the device's CAN-bus state. When *hollow* is set
// (the device has no configuration file) a smaller dot in the background colour
// is painted on top, leaving only a ring.
typedef struct {
	uv_uiobject_st super;
	color_t color;
	color_t bg;
	bool hollow;
} statusdot_st;

static void statusdot_draw(void *me, const uv_bounding_box_st *pbb) {
	statusdot_st *dot = me;
	int16_t gx = uv_ui_get_xglobal(me);
	int16_t gy = uv_ui_get_yglobal(me);
	uv_bounding_box_st *bb = uv_uibb(me);
	int16_t diam = (bb->h < bb->w) ? bb->h : bb->w;
	if (diam > 20) {
		diam = 20;
	}
	int16_t cx = gx + bb->w / 2;
	int16_t cy = gy + bb->h / 2;
	uv_ui_draw_point(cx, cy, dot->color, diam);
	if (dot->hollow) {
		uv_ui_draw_point(cx, cy, dot->bg, diam / 2);
	}
}

static void statusdot_init(statusdot_st *me, color_t color, color_t bg,
		bool hollow) {
	uv_uiobject_init(me);
	me->color = color;
	me->bg = bg;
	me->hollow = hollow;
	uv_uiobject_set_draw_callb(me, &statusdot_draw);
}


// Selectable file types offered by the device-file picker. Device files are
// .uvdev packages (see uvdev.h / make_uvdev.sh).
static const uv_uifileedit_filter_st DEVICE_FILE_FILTERS[] = {
	{ "Device packages", "*.uvdev" },
	{ "All files", "*" },
};


// Selectable file types offered by the system-file picker. System files are
// .uvsys packages that bundle the system's device files.
static const uv_uifileedit_filter_st SYSTEM_FILE_FILTERS[] = {
	{ "System packages", "*.uvsys" },
	{ "All files", "*" },
};


// Persistent storage for the widgets and strings shown in the currently active
// tab. Only one tab is visible at a time, so a single shared set is reused: the
// tab window keeps pointers to these objects (and to the label strings), so
// they must outlive each show call.
static struct {
	// Nested tab window shown on a device tab, splitting it into a "Device" view
	// (the panels below) and a "Terminal" view (terminaltab). Fills the device
	// tab's content area.
	uv_uitabwindow_st subtabs;
	uv_uiobject_st *subtabs_buf[8];

	// On a device tab this is the big "Device configuration" header (right
	// column); on the system tab it is the system title.
	uv_uilabel_st title;
	char title_str[256];

	// the two framed panels of a device tab: "CAN-bus device" (left, the device's
	// bus identity) and "Device configuration" (right, the configuration file)
	uv_uiframewindow_st left_frame;
	uv_uiobject_st *left_buf[12];
	uv_uiframewindow_st right_frame;
	uv_uiobject_st *right_buf[12];

	// the framed panels of the system tab: "System configuration" (the load /
	// search / save / load-to-devices buttons) and, on Linux only, "Simulator"
	// (the run-simulator button and the running-simulator list)
	uv_uiframewindow_st sys_frame;
	uv_uiobject_st *sys_frame_buf[8];
	uv_uiframewindow_st sim_frame;
	uv_uiobject_st *sim_frame_buf[4];

	// right-panel subtitle naming the device being configured ("current topic")
	uv_uilabel_st curtopic;
	char curtopic_str[128];

	// left-panel device state label (e.g. "OPERATIONAL", optionally with a
	// "CONFIGURATION FILE NOT SELECTED" second line) and its colour dot
	uv_uilabel_st state;
	char state_str[80];
	statusdot_st statedot;

	uv_uilabel_st info;
	// large enough to hold a full device file path (1024) plus a label prefix
	char info_str[1152];

	// left-panel "Revision number" read from the device (own label so it can be
	// colored red on a mismatch)
	uv_uilabel_st dev_rev;
	char dev_rev_str[64];

	// device-file picker, shown only on a device tab
	uv_uifileedit_st fileedit;
	char fileedit_path[1024];
	// "Server files" button, to the right of the device-file picker: opens the
	// Usevolt file-server browser (needs the account credentials + URL set on the
	// system tab)
	uv_uibutton_st serverfiles_btn;

	// right-panel software version and revision read from the configuration file
	uv_uilabel_st conf_sw;
	char conf_sw_str[128];
	uv_uilabel_st conf_rev;
	char conf_rev_str[64];

	// right-panel "Default Node-ID" read from the configuration file (the node id
	// the file defines, shown even when the live device keeps its own)
	uv_uilabel_st conf_nodeid;
	char conf_nodeid_str[48];

	// red warning shown when the configuration file revision and the device
	// revision do not match
	uv_uilabel_st warning;
	char warning_str[192];

	// "Flash firmware" button (above the parameter buttons)
	uv_uibutton_st flash_btn;

	// "Load media files" button, shown under "Flash firmware" only when the
	// device's .uvdev package bundles a media directory
	uv_uibutton_st media_btn;

	// "Save parameters" / "Load parameters" buttons
	uv_uibutton_st save_btn;
	uv_uibutton_st load_btn;

	// node-id editor, shown on a device tab once a configuration file is set
	uv_uidigitedit_st nodeid_edit;

	// left-panel CANopen device-command buttons, under the node-id editor:
	// restore defaults (0x1011 "load"), store settings (0x1010 "save") and an
	// NMT reset of the device
	uv_uibutton_st revert_btn;
	uv_uibutton_st savesettings_btn;
	uv_uibutton_st reset_btn;

	// "Remove this device" button, shown on a device tab
	uv_uimediabutton_st remove_btn;

	// system-file path buffer, filled by the "Load system configuration" dialog
	char sys_fileedit_path[1024];

	// "or" separator between the load-configuration button and the search button
	uv_uilabel_st or_label;

	// "Load system configuration from file" button, shown on the system tab
	uv_uibutton_st sys_load_btn;

	// "Search devices" button, shown on the system tab, plus its mutable label
	// (it shows a live "Searching Xs..." countdown while a scan runs)
	uv_uibutton_st search_btn;
	char search_btn_str[64];

	// "Save system configuration" button, shown on the system tab
	uv_uibutton_st sys_save_btn;

	// "Load system configuration" button, shown on the system tab below the save
	// button: writes the loaded system's bundled parameters onto every device
	uv_uibutton_st sys_loadparams_btn;

	// "Run simulator" button, shown on the system tab: launches the Linux
	// simulator of every configured device. Its label gets a second line when
	// disabled because devices are online.
	uv_uibutton_st run_sim_btn;
	char run_sim_str[96];

	// Running-simulator list on the system tab: its own window holding one row per
	// running simulator. Each row has a name/PID label, the node id, a "Log"
	// button (opens a terminal on the simulator's output) and a "Kill" button.
	// The rows are scaled to fit so every simulator is visible.
	uv_uiwindow_st sim_window;
	uv_uiobject_st *sim_window_buf[4 * SYSTEM_DEV_MAX_COUNT];
	uv_uilabel_st sim_labels[SYSTEM_DEV_MAX_COUNT];
	char sim_label_strs[SYSTEM_DEV_MAX_COUNT][96];
	uv_uilabel_st sim_node_labels[SYSTEM_DEV_MAX_COUNT];
	char sim_node_strs[SYSTEM_DEV_MAX_COUNT][16];
	uv_uibutton_st sim_log_btns[SYSTEM_DEV_MAX_COUNT];
	uv_uibutton_st sim_kill_btns[SYSTEM_DEV_MAX_COUNT];

	// "Account" panel on the system tab: a username and a password field whose
	// values are stored on this computer and shared by every uvcan install (see
	// credentials.c). Edits are saved back in devicetab_step().
	uv_uiframewindow_st account_frame;
	uv_uiobject_st *account_frame_buf[6];
	uv_uitextedit_st account_url;
	uv_uitextedit_st account_user;
	uv_uitextedit_st account_pass;
} content;


// Decoded image shown on the Remove button. Loaded once from the embedded PNG
// (the renderer caches it by name, so a single load serves every device tab).
static uv_uimedia_st remove_media;
static bool remove_media_loaded;


// The device whose tab is currently shown, or NULL when the active tab is the
// system overview. Polled by devicetab_step().
static device_st *current_device;

// True while the system overview tab is the one currently shown. Used by
// devicetab_step() to poll the right file picker.
static bool showing_system;

// Text buffers backing the "Account" panel's username / password fields. They must
// outlive the frequent tab rebuilds (the textedits read/write them in place), so
// they are file-scope rather than part of *content*. Seeded once from the stored
// credentials the first time the system tab is built; edits are saved back to the
// shared file in devicetab_step().
static char account_url_buf[CREDENTIALS_MAX];
static char account_user_buf[CREDENTIALS_MAX];
static char account_pass_buf[CREDENTIALS_MAX];
static bool account_seeded;

// The two sub-tabs of a device tab.
typedef enum {
	SUBTAB_DEVICE = 0,
	SUBTAB_TERMINAL,
	SUBTAB_COUNT,
} subtab_e;

// Names of the device sub-tabs (handed to the nested tab window as a pointer
// array).
static char *subtab_names[SUBTAB_COUNT] = { "Device", "Terminal" };

// Which sub-tab is selected. Kept across the frequent device-tab rebuilds so
// switching to the terminal is not undone by an unrelated state refresh. Reset to
// SUBTAB_DEVICE when a different device's tab is opened.
static subtab_e selected_subtab;

// Builds the "Device" view (the panels) into *tabwin*, which is either the outer
// tab window (empty-device placeholder) or the nested sub-tab window.
static void build_device_view(uv_uitabwindow_st *tabwin, device_st *device);
// Fills the nested sub-tab window with the currently selected sub-tab's content.
static void show_active_subtab(void);
static uv_uiobject_ret_e subtab_step(void *me, const uint16_t step_ms);

// A long-running, asynchronous device operation started from the device tab.
typedef enum {
	OP_NONE = 0,
	OP_FLASH,
	OP_MEDIA,
	OP_SAVE,
	OP_LOAD,
	OP_SEARCH,
	OP_SAVESYS,
	OP_SYSPARAM,
	OP_SYSLOAD,
	OP_SIMPARAM,
} busy_op_e;

// While an operation is in progress the device frames are disabled and the
// device state is not updated (see devicetab_is_busy()).
static bool busy;
static busy_op_e busy_op;


bool devicetab_is_busy(void) {
	return busy;
}


bool devicetab_busy_is_search(void) {
	return busy && (busy_op == OP_SEARCH);
}


/// @brief: Returns true when the in-progress asynchronous operation has finished.
static bool busy_finished(void) {
	bool ret = true;
	switch (busy_op) {
	case OP_FLASH:
		ret = loadbin_is_finished(&dev.load);
		break;
	case OP_MEDIA:
		ret = loadmedia_load_device_is_finished();
		break;
	case OP_SAVE:
		ret = saveparam_is_finished(&dev.saveparam);
		break;
	case OP_LOAD:
		ret = loadparam_is_finished(&dev.loadparam);
		break;
	case OP_SEARCH:
		ret = find_search_is_finished();
		break;
	case OP_SAVESYS:
		ret = savesys_is_finished();
		break;
	case OP_SYSPARAM:
		ret = loadparam_load_params_is_finished();
		break;
	case OP_SYSLOAD:
		ret = loadparam_load_system_is_finished();
		break;
	case OP_SIMPARAM:
		ret = simrun_load_params_is_finished();
		break;
	default:
		break;
	}
	return ret;
}


// Tracks the last-shown flash phase so the cancel button label is only redrawn
// when it changes (idle -> "waiting for device" -> downloading).
static bool flash_btn_waiting;


/// @brief: While a flash runs, the whole device panel is disabled except the flash
/// button, which becomes a cancel button. This keeps that button (and its frame,
/// so touches reach it) enabled and labels it for the current flash phase.
static void flash_btn_set_cancel_mode(void) {
	char *txt = loadbin_is_waiting(&dev.load) ?
			"Cancel (waiting for device)" : "Cancel flashing";
	uv_uibutton_set_text(&content.flash_btn, txt);
	uv_ui_set_enabled(&content.right_frame, true);
	uv_ui_set_enabled(&content.flash_btn, true);
	uv_ui_refresh(&content.right_frame);
	uv_ui_refresh(&content.flash_btn);
}


/// @brief: Marks an asynchronous operation as started and disables both device
/// frames (and all their content) so nothing is touched while it runs.
static void start_busy(busy_op_e op) {
	busy = true;
	busy_op = op;
	uv_uiwindow_set_enabled(&content.left_frame, false);
	uv_uiwindow_set_enabled(&content.right_frame, false);
	uv_ui_refresh(&content.left_frame);
	uv_ui_refresh(&content.right_frame);
	// a firmware flash keeps its button live as a cancel button
	if (op == OP_FLASH) {
		flash_btn_waiting = loadbin_is_waiting(&dev.load);
		flash_btn_set_cancel_mode();
	}
	// a system save/load or simulator parameter load is started from the system
	// tab; grey out all of its buttons for the duration so they read as inactive
	if ((op == OP_SAVESYS) || (op == OP_SYSLOAD) || (op == OP_SIMPARAM)) {
		uv_uiobject_disable(&content.sys_load_btn);
		uv_uiobject_disable(&content.search_btn);
		uv_uiobject_disable(&content.sys_save_btn);
		uv_uiobject_disable(&content.sys_loadparams_btn);
		uv_ui_refresh(&content.sys_load_btn);
		uv_ui_refresh(&content.search_btn);
		uv_ui_refresh(&content.sys_save_btn);
		uv_ui_refresh(&content.sys_loadparams_btn);
	}
}


/// @brief: Lays out a simple "title on top, info filling the rest" tab using the
/// shared title/info labels (both label strings are filled in by the caller).
static void show_title_and_info(uv_uitabwindow_st *tabwin) {
	const uv_uistyle_st *style = &uv_uistyles[0];
	uv_bounding_box_st cbb = uv_uitabwindow_get_contentbb(tabwin);
	int16_t w = cbb.w - 2 * MARGIN;

	uv_uilabel_init(&content.title, style->font, ALIGN_CENTER_LEFT,
			style->text_color, content.title_str);
	uv_uitabwindow_addxy(tabwin, &content.title, MARGIN, MARGIN, w, TITLE_H);

	uv_uilabel_init(&content.info, style->font, ALIGN_TOP_LEFT,
			style->text_color, content.info_str);
	uv_uitabwindow_addxy(tabwin, &content.info, MARGIN, MARGIN + TITLE_H + MARGIN,
			w, cbb.h - (MARGIN + TITLE_H + MARGIN) - MARGIN);
}


void devicetab_show_system(uv_uitabwindow_st *tabwin, system_st *system) {
	const uv_uistyle_st *style = &uv_uistyles[0];
	uv_bounding_box_st cbb = uv_uitabwindow_get_contentbb(tabwin);

	current_device = NULL;
	showing_system = true;

	// The system tab is laid out as framed panels stacked top to bottom: a "System
	// configuration" panel on top, a "Simulator" panel in the middle (Linux only -
	// simulators are unavailable on other targets) and an "Account" panel pinned to
	// the bottom on every target.
	int16_t frame_x = MARGIN;
	int16_t frame_w = cbb.w - 2 * MARGIN;
	// the Account panel: its title bar plus a single row (URL, Username and
	// Password side by side), the row being a field with its title below it
	int16_t account_frame_h = 2 * BUTTON_H + MARGIN + TITLE_H;
	int16_t account_frame_y = cbb.h - MARGIN - account_frame_h;
#if !CONFIG_TARGET_WIN
	// the configuration panel needs room for the double-height source row plus the
	// save/load row; the simulator panel takes whatever is left between it and the
	// account panel
	int16_t sys_frame_h = 4 * BUTTON_H + 2 * MARGIN;
#else
	// no simulator panel on Windows: the configuration panel fills everything above
	// the account panel
	int16_t sys_frame_h = account_frame_y - 2 * MARGIN;
#endif

	// --- "System configuration" panel: load / search / save / load-to-devices ---
	uv_uiframewindow_init(&content.sys_frame, content.sys_frame_buf, style);
	uv_uiframewindow_set_title(&content.sys_frame, "System configuration");
	uv_uitabwindow_addxy(tabwin, &content.sys_frame, frame_x, MARGIN,
			frame_w, sys_frame_h);
	uv_bounding_box_st sc = uv_uiframewindow_get_content_bb(&content.sys_frame);

	// Source row: load a system configuration file, OR search the CAN bus. Two
	// equally-wide, double-height buttons sit side by side with an "or" between.
	int16_t or_w = 36;
	int16_t src_h = 2 * BUTTON_H;
	int16_t src_btn_w = (sc.w - or_w - 2 * MARGIN) / 2;
	int16_t y = 0;
	int16_t x = 0;

	// "Load system configuration from file": opens a .uvsys file chooser when
	// clicked (handled in devicetab_step).
	uv_uibutton_init(&content.sys_load_btn,
			"Open system configuration file", style);
	uv_uiframewindow_addxy(&content.sys_frame, &content.sys_load_btn,
			x, y, src_btn_w, src_h);
	x += src_btn_w + MARGIN;

	// "or" separator (string literal lives for the program's lifetime, so the
	// label can point at it directly)
	uv_uilabel_init(&content.or_label, &UI_TITLE_FONT, ALIGN_CENTER,
			style->text_color, "or");
	uv_uiframewindow_addxy(&content.sys_frame, &content.or_label, x, y, or_w, src_h);
	x += or_w + MARGIN;

	// "Search CAN-bus for devices": scans the bus and adds any newly found
	// devices alongside the existing ones, on its own task (handled in
	// devicetab_step()). While a scan runs, devicetab_step() rewrites the label
	// to a live "Searching Xs..." countdown; reset it to the default here when
	// no scan is in progress.
	if (find_search_is_finished()) {
		strcpy(content.search_btn_str, "Search CAN-bus for devices");
	}
	uv_uibutton_init(&content.search_btn, content.search_btn_str, style);
	uv_uiframewindow_addxy(&content.sys_frame, &content.search_btn,
			x, y, src_btn_w, src_h);
	y += src_h + MARGIN;

	// Survey device states once: whether any device is operational (used to enable
	// save / load to devices).
	bool any_online = false;
	for (uint8_t i = 0; i < system_get_dev_count(system); i++) {
		device_st *d = system_get_dev(system, i);
		if (d->state == DEV_STATE_OP) {
			any_online = true;
		}
	}

	// "Save system configuration" and "Load system configuration" share one row,
	// each half the width. Save writes a .uvsys package bundling every device's
	// .uvdev and the parameters read from each operational device; Load writes the
	// bundled parameters back onto the online devices. Both run on their own task
	// (handled in devicetab_step). Both need an operational device.
	int16_t half_w = (sc.w - MARGIN) / 2;
	uv_uibutton_init(&content.sys_save_btn, "Save system configuration to file",
			style);
	uv_uiframewindow_addxy(&content.sys_frame, &content.sys_save_btn,
			0, y, half_w, BUTTON_H);
	if (!any_online) {
		uv_uiobject_disable(&content.sys_save_btn);
	}
	uv_uibutton_init(&content.sys_loadparams_btn,
			"Load system configuration to devices", style);
	uv_uiframewindow_addxy(&content.sys_frame, &content.sys_loadparams_btn,
			half_w + MARGIN, y, half_w, BUTTON_H);
	if ((system_get_dev_count(system) == 0) || !any_online) {
		uv_uiobject_disable(&content.sys_loadparams_btn);
	}

	// while a system save/load or simulator parameter load is running, keep every
	// configuration-panel button greyed out
	bool sys_busy = busy && ((busy_op == OP_SAVESYS) || (busy_op == OP_SYSLOAD) ||
			(busy_op == OP_SIMPARAM));
	if (sys_busy) {
		uv_uiobject_disable(&content.sys_load_btn);
		uv_uiobject_disable(&content.search_btn);
		uv_uiobject_disable(&content.sys_save_btn);
		uv_uiobject_disable(&content.sys_loadparams_btn);
	}

#if !CONFIG_TARGET_WIN
	// --- "Simulator" panel (Linux only): run-simulator button + running list ---
	int16_t sim_frame_y = MARGIN + sys_frame_h + MARGIN;
	// leave room for the account panel below (and a margin between the two)
	int16_t sim_frame_h = account_frame_y - MARGIN - sim_frame_y;
	uv_uiframewindow_init(&content.sim_frame, content.sim_frame_buf, style);
	uv_uiframewindow_set_title(&content.sim_frame, "Simulator");
	uv_uitabwindow_addxy(tabwin, &content.sim_frame, frame_x, sim_frame_y,
			frame_w, sim_frame_h);
	uv_bounding_box_st mc = uv_uiframewindow_get_content_bb(&content.sim_frame);

	// the panel is split into two columns: the "Run simulator" button on the left
	// (a third of the width) and the running-simulator list on the right (the
	// remaining two thirds).
	int16_t left_w = (mc.w - MARGIN) / 3;
	int16_t right_x = left_w + MARGIN;
	int16_t right_w = mc.w - left_w - MARGIN;

	// "Run simulator": launches the Linux simulator of every configured device.
	// While the simulators are starting up / loading parameters (OP_SIMPARAM) the
	// same button turns into a "Force stop simulator" that kills them all (handled
	// in the busy branch of devicetab_step). Otherwise it is disabled only when no
	// system file is loaded, when the simulators are already running, or while
	// another system operation is busy. Devices already online no longer block it:
	// pressing it restores those devices to defaults and loads the system
	// parameters onto them instead of simulating them (see the click handler).
	// Fills the left column.
	bool sims_running = simrun_any_running();
	bool simparam_busy = busy && (busy_op == OP_SIMPARAM);
	strcpy(content.run_sim_str,
			simparam_busy ? "Force stop simulator" : "Run simulator");
	uv_uibutton_init(&content.run_sim_btn, content.run_sim_str, style);
	uv_uiframewindow_addxy(&content.sim_frame, &content.run_sim_btn,
			0, 0, left_w, mc.h);
	if (!simparam_busy && (!system->loaded || sims_running || sys_busy)) {
		uv_uiobject_disable(&content.run_sim_btn);
	}

	// Running-simulator list in the right column: one row per running simulator,
	// showing its device name and process id with a "Kill" button. Polled in
	// devicetab_step().
	uint8_t simcount = simrun_get_count();
	if (simcount == 0) {
		snprintf(content.info_str, sizeof(content.info_str),
				"No simulators running.");
		uv_uilabel_init(&content.info, style->font, ALIGN_CENTER_LEFT,
				style->text_color, content.info_str);
		uv_uiframewindow_addxy(&content.sim_frame, &content.info,
				right_x, 0, right_w, mc.h);
	}
	else {
		// the rows live in their own window so they are not limited by the frame's
		// object budget. Scale the row height so that every simulator's row (with
		// its Kill button) fits inside the window's height, capped at the normal
		// button height when there is room to spare.
		uv_uiwindow_init(&content.sim_window, content.sim_window_buf, style);
		uv_uiframewindow_addxy(&content.sim_frame, &content.sim_window,
				right_x, 0, right_w, mc.h);

		int16_t gap = 4;
		int16_t row_h = (mc.h - (simcount - 1) * gap) / simcount;
		if (row_h > BUTTON_H) {
			row_h = BUTTON_H;
		}
		// columns laid out from the right: Kill, Log, node id, then the name fills
		// whatever is left
		int16_t kill_w = 80;
		int16_t log_w = 80;
		int16_t node_w = 70;
		int16_t kill_x = right_w - kill_w;
		int16_t log_x = kill_x - gap - log_w;
		int16_t node_x = log_x - gap - node_w;
		int16_t name_w = node_x - gap;
		for (uint8_t i = 0; i < simcount; i++) {
			int16_t row_y = i * (row_h + gap);
			snprintf(content.sim_label_strs[i], sizeof(content.sim_label_strs[i]),
					"%s  (pid %d)", simrun_get_name(i), simrun_get_pid(i));
			uv_uilabel_init(&content.sim_labels[i], style->font,
					ALIGN_CENTER_LEFT, style->text_color,
					content.sim_label_strs[i]);
			uv_uiwindow_addxy(&content.sim_window, &content.sim_labels[i], 0, row_y,
					name_w, row_h);

			// node id, with the simulator's state ("Running" / "Killed" /
			// "Stopped") on a second line
			snprintf(content.sim_node_strs[i], sizeof(content.sim_node_strs[i]),
					"0x%x\n%s", (unsigned int) simrun_get_nodeid(i),
					simrun_get_state_str(i));
			uv_uilabel_init(&content.sim_node_labels[i], style->font,
					ALIGN_CENTER_LEFT, style->text_color,
					content.sim_node_strs[i]);
			uv_uiwindow_addxy(&content.sim_window, &content.sim_node_labels[i],
					node_x, row_y, node_w, row_h);

			uv_uibutton_init(&content.sim_log_btns[i], "Log", style);
			uv_uiwindow_addxy(&content.sim_window, &content.sim_log_btns[i],
					log_x, row_y, log_w, row_h);

			uv_uibutton_init(&content.sim_kill_btns[i], "Kill", style);
			uv_uiwindow_addxy(&content.sim_window, &content.sim_kill_btns[i],
					kill_x, row_y, kill_w, row_h);
			// a simulator that is no longer running cannot be killed again
			if (simrun_get_state(i) != SIMRUN_RUNNING) {
				uv_uiobject_disable(&content.sim_kill_btns[i]);
			}
		}
	}
#endif

	// --- "Account" panel (all targets): server URL + username + password stored on
	// this computer and shared by every uvcan install. Seed the fields once from the
	// stored values (later rebuilds keep whatever is in the buffers, including
	// unsaved edits); edits are saved back to the shared file in devicetab_step().
	if (!account_seeded) {
		strncpy(account_url_buf, credentials_get_url(),
				sizeof(account_url_buf) - 1);
		account_url_buf[sizeof(account_url_buf) - 1] = '\0';
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
	uv_uitabwindow_addxy(tabwin, &content.account_frame, frame_x, account_frame_y,
			frame_w, account_frame_h);
	uv_bounding_box_st ac = uv_uiframewindow_get_content_bb(&content.account_frame);

	// one row holding all three fields: the URL takes half the width (it is by far
	// the longest value), Username and Password a quarter each. Each field draws
	// its title below itself.
	int16_t acc_gap = MARGIN;
	int16_t acc_url_w = (ac.w - 2 * acc_gap) / 2;
	int16_t acc_field_w = (ac.w - 2 * acc_gap - acc_url_w) / 2;
	int16_t acc_x = 0;

	uv_uitextedit_init(&content.account_url, account_url_buf,
			sizeof(account_url_buf), UITEXTEDIT_FLAG_ONELINE, style);
	uv_uitextedit_set_title(&content.account_url, "URL");
	uv_uitextedit_set_align(&content.account_url, ALIGN_CENTER_LEFT);
	uv_uiframewindow_addxy(&content.account_frame, &content.account_url,
			acc_x, 0, acc_url_w, ac.h);
	acc_x += acc_url_w + acc_gap;

	uv_uitextedit_init(&content.account_user, account_user_buf,
			sizeof(account_user_buf), UITEXTEDIT_FLAG_ONELINE, style);
	uv_uitextedit_set_title(&content.account_user, "Username");
	uv_uitextedit_set_align(&content.account_user, ALIGN_CENTER_LEFT);
	uv_uiframewindow_addxy(&content.account_frame, &content.account_user,
			acc_x, 0, acc_field_w, ac.h);
	acc_x += acc_field_w + acc_gap;

	uv_uitextedit_init(&content.account_pass, account_pass_buf,
			sizeof(account_pass_buf),
			UITEXTEDIT_FLAG_ONELINE | UITEXTEDIT_FLAG_PASSWORD, style);
	uv_uitextedit_set_title(&content.account_pass, "Password");
	uv_uitextedit_set_align(&content.account_pass, ALIGN_CENTER_LEFT);
	uv_uiframewindow_addxy(&content.account_frame, &content.account_pass,
			acc_x, 0, acc_field_w, ac.h);
}


static void build_device_view(uv_uitabwindow_st *tabwin, device_st *device) {
	const uv_uistyle_st *style = &uv_uistyles[0];

	if (device == NULL) {
		// defensive: a device tab with no backing device
		snprintf(content.title_str, sizeof(content.title_str), "Device");
		snprintf(content.info_str, sizeof(content.info_str),
				"This device tab is empty.");
		show_title_and_info(tabwin);
	}
	else {
		// The device tab is split into two framed panels. The right panel
		// ("Device configuration") holds the device name, the configuration-file
		// picker, the configuration's version/revision and the parameter
		// save/load buttons; the left panel ("CAN-bus device") shows the device's
		// bus identity: online/offline state, the identification read from it, its
		// revision read from the bus, and the node-id editor.
		uv_bounding_box_st cbb = uv_uitabwindow_get_contentbb(tabwin);
		int16_t half_w = (cbb.w - 3 * MARGIN) / 2;
		int16_t frame_y = MARGIN;
		// leave room below the frames for the centered "Remove" button row
		int16_t frame_h = cbb.h - BUTTON_H - 3 * MARGIN;

		// third-party (non-Usevolt) devices have no configuration package, so the
		// right "Device configuration" panel is hidden and the left "CAN-bus
		// device" panel is widened to fill the whole content area.
		bool thirdparty = device->thirdparty;
		int16_t left_w = thirdparty ? (cbb.w - 2 * MARGIN) : half_w;

		// read the device's own revision over the bus once (blocks briefly); it
		// was reset to 0 when a file was assigned, so this re-reads after changes.
		// Skipped while an async operation owns the SDO client (busy), for
		// third-party devices (the CAN IF VERSION object is Usevolt-specific),
		// and while the device is in BOOT-UP: a bootloading device does not
		// serve the normal object dictionary, so the read would only stall.
		if (device->loaded && !thirdparty && (device->dev_revision == 0) &&
				!busy && (device->state != DEV_STATE_BOOTUP)) {
			device->dev_revision = find_read_device_revision(device);
		}
		// the configuration file revision and the device revision are compared:
		// a mismatch is flagged in red because parameters may not transfer cleanly
		bool mismatch = device->loaded && (device->dev_revision != 0) &&
				(device->revision != 0) &&
				(device->dev_revision != device->revision);
		color_t rev_color = mismatch ? WARNING_COLOR : style->text_color;

		// --- left panel: CAN-bus device identity ---
		uv_uiframewindow_init(&content.left_frame, content.left_buf, style);
		uv_uiframewindow_set_title(&content.left_frame, "CAN-bus device");
		uv_uitabwindow_addxy(tabwin, &content.left_frame,
				MARGIN, frame_y, left_w, frame_h);
		uv_bounding_box_st lc = uv_uiframewindow_get_content_bb(&content.left_frame);

		// device state: a colour dot followed by a state label. The dot is drawn
		// hollow when the device has no configuration file assigned; in that case
		// the label also gets a second line spelling that out, so the row is taller.
		bool hollow = (device->filepath[0] == '\0');
		int16_t state_h = hollow ? 64 : 40;
		int16_t dot_w = 40;
		// the frame and tab window are transparent, so the colour behind the dot
		// is the display background; use it for the hollow centre
		statusdot_init(&content.statedot, dot_color_for_state(device->state),
				style->display_c, hollow);
		uv_uiframewindow_addxy(&content.left_frame, &content.statedot,
				0, 0, dot_w, state_h);
		const char *state_name = state_name_str(device->state);
		if (hollow) {
			// no .uvdev configuration file assigned to this device yet
			snprintf(content.state_str, sizeof(content.state_str),
					"%s\nCONFIGURATION FILE NOT SELECTED", state_name);
		}
		else {
			snprintf(content.state_str, sizeof(content.state_str), "%s", state_name);
		}
		uv_uilabel_init(&content.state, style->font, ALIGN_CENTER_LEFT,
				style->text_color, content.state_str);
		uv_uiframewindow_addxy(&content.left_frame, &content.state,
				dot_w, 0, lc.w - dot_w, state_h);

		// identification and node-id editor: shown once the device has an
		// identity (read from a configuration file or discovered on the bus)
		if (device->loaded) {
			int16_t nodeid_h = 64;
			int16_t info_y = state_h + MARGIN;
			int16_t info_h = 70;
			int16_t devrev_y = info_y + info_h;
			// the node-id editor and the three CANopen device-command buttons are
			// stacked at the bottom of the frame: "Reset device" on the lowest row,
			// "Revert to defaults" / "Save settings" above it, and the node-id
			// editor above those
			int16_t cmdbtn_w = (lc.w - MARGIN) / 2;
			int16_t reset_y = lc.h - BUTTON_H;
			int16_t cfgrow_y = reset_y - BUTTON_H - MARGIN;
			int16_t nodeid_y = cfgrow_y - MARGIN - nodeid_h;

			// identification read from the device / its database: vendor id,
			// product code and the device's own software version
			char swbuf[32];
			if (device->sw_version != 0) {
				snprintf(swbuf, sizeof(swbuf), "%u",
						(unsigned int) device->sw_version);
			}
			else {
				strcpy(swbuf, "-");
			}
			// name the manufacturer when the vendor id is Usevolt's
			const char *vendor_name =
					(device->vendor_id == CANOPEN_USEVOLT_VENDOR_ID) ?
							" (Usevolt Oy)" : "";
			snprintf(content.info_str, sizeof(content.info_str),
					"Vendor ID: 0x%X%s\n"
					"Product code: 0x%X\n"
					"Software version: %s",
					device->vendor_id, vendor_name, device->product_code, swbuf);
			uv_uilabel_init(&content.info, style->font, ALIGN_TOP_LEFT,
					style->text_color, content.info_str);
			uv_uiframewindow_addxy(&content.left_frame, &content.info,
					0, info_y, lc.w, info_h);

			// the device's own revision (CAN IF VERSION) read over the bus,
			// colored red when it does not match the configuration file. This is a
			// Usevolt-specific object, so it is hidden for third-party devices.
			if (!thirdparty) {
				char devrevbuf[32];
				if (device->dev_revision != 0) {
					snprintf(devrevbuf, sizeof(devrevbuf), "%u",
							(unsigned int) device->dev_revision);
				}
				else {
					strcpy(devrevbuf, "-");
				}
				snprintf(content.dev_rev_str, sizeof(content.dev_rev_str),
						"Revision number: %s", devrevbuf);
				uv_uilabel_init(&content.dev_rev, style->font, ALIGN_TOP_LEFT,
						rev_color, content.dev_rev_str);
				uv_uiframewindow_addxy(&content.left_frame, &content.dev_rev,
						0, devrev_y, lc.w, 26);
			}

			uv_uidigitedit_init(&content.nodeid_edit, device->nodeid, style);
			uv_uidigitedit_set_mode(&content.nodeid_edit, UIDIGITEDIT_MODE_NORMAL);
			uv_uidigitedit_set_hex(&content.nodeid_edit, true);
			// valid CANopen node id range
			uv_uidigitedit_set_limits(&content.nodeid_edit, 1, 127);
			uv_uidigitedit_set_title(&content.nodeid_edit, "Node ID");
			uv_uiframewindow_addxy(&content.left_frame, &content.nodeid_edit,
					0, nodeid_y, lc.w, nodeid_h);
			// the node id of a third-party device is not ours to reassign
			if (thirdparty) {
				uv_uiobject_disable(&content.nodeid_edit);
			}

			// CANopen device-command buttons. They act on the device directly over
			// the bus (no configuration file needed), logging the outcome to the
			// log strip. Disabled while the device is offline.
			uv_uibutton_init(&content.revert_btn, "Revert to defaults", style);
			uv_uiframewindow_addxy(&content.left_frame, &content.revert_btn,
					0, cfgrow_y, cmdbtn_w, BUTTON_H);
			uv_uibutton_init(&content.savesettings_btn, "Save settings", style);
			uv_uiframewindow_addxy(&content.left_frame, &content.savesettings_btn,
					cmdbtn_w + MARGIN, cfgrow_y, cmdbtn_w, BUTTON_H);
			uv_uibutton_init(&content.reset_btn, "Reset device", style);
			uv_uiframewindow_addxy(&content.left_frame, &content.reset_btn,
					0, reset_y, lc.w, BUTTON_H);
			if (device->state == DEV_STATE_OFFLINE) {
				uv_uiobject_disable(&content.revert_btn);
				uv_uiobject_disable(&content.savesettings_btn);
				uv_uiobject_disable(&content.reset_btn);
			}
		}

		// --- right panel: device configuration (Usevolt devices only) ---
		if (!thirdparty) {
		uv_uiframewindow_init(&content.right_frame, content.right_buf, style);
		uv_uiframewindow_set_title(&content.right_frame, "Device configuration");
		uv_uitabwindow_addxy(tabwin, &content.right_frame,
				MARGIN + half_w + MARGIN, frame_y, half_w, frame_h);
		uv_bounding_box_st rc =
				uv_uiframewindow_get_content_bb(&content.right_frame);

		// current topic: the friendly DEV_NAME from the database, or the
		// file-based name when the database provides none
		int16_t curtopic_h = 30;
		snprintf(content.curtopic_str, sizeof(content.curtopic_str), "%s",
				(strlen(device->devname) > 0) ? device->devname : device->name);
		uv_uilabel_init(&content.curtopic, &UI_TITLE_FONT, ALIGN_CENTER_LEFT,
				style->text_color, content.curtopic_str);
		uv_uiframewindow_addxy(&content.right_frame, &content.curtopic,
				0, 0, rc.w, curtopic_h);

		// configuration-file picker: shows the current file and lets the user
		// pick a new .uvdev package
		int16_t ry = curtopic_h + MARGIN;
		strncpy(content.fileedit_path, device->filepath,
				sizeof(content.fileedit_path) - 1);
		content.fileedit_path[sizeof(content.fileedit_path) - 1] = '\0';
		uv_uifileedit_init(&content.fileedit, content.fileedit_path,
				sizeof(content.fileedit_path), style);
		uv_uifileedit_set_title(&content.fileedit, "Device configuration file");
		uv_uifileedit_set_filters(&content.fileedit, DEVICE_FILE_FILTERS,
				sizeof(DEVICE_FILE_FILTERS) / sizeof(DEVICE_FILE_FILTERS[0]));
		// the file picker takes 3/4 of the width; the "Server files" button takes
		// the remaining 1/4 on the right
		int16_t sf_btn_w = rc.w / 4;
		int16_t fe_w = rc.w - sf_btn_w - MARGIN;
		uv_uiframewindow_addxy(&content.right_frame, &content.fileedit,
				0, ry, fe_w, FILEEDIT_H);
		uv_uibutton_init(&content.serverfiles_btn, "Server files", style);
		uv_uiframewindow_addxy(&content.right_frame, &content.serverfiles_btn,
				fe_w + MARGIN, ry, sf_btn_w, FILEEDIT_H);
		// the per-device picker stays enabled even when the system was loaded from
		// a .uvsys file: picking a new .uvdev simply overrides the file for this
		// device (handled in devicetab_step()).
		ry += FILEEDIT_H + MARGIN;

		if (device->loaded) {
			// configuration software version (from the .uvdev manifest) and
			// revision (from its database), so they can be compared with the
			// device's own values in the left panel
			snprintf(content.conf_sw_str, sizeof(content.conf_sw_str),
					"Software version: %s",
					(strlen(device->conf_version) != 0) ?
							device->conf_version : "-");
			uv_uilabel_init(&content.conf_sw, style->font, ALIGN_TOP_LEFT,
					style->text_color, content.conf_sw_str);
			uv_uiframewindow_addxy(&content.right_frame, &content.conf_sw,
					0, ry, rc.w, 26);
			ry += 26;

			// the node id the configuration file defines. Shown for reference; the
			// device's actual node id (which may differ for a device that was
			// already live on the bus when the file was assigned) is the editable
			// "Node ID" in the left panel.
			if (device->default_nodeid != 0) {
				snprintf(content.conf_nodeid_str, sizeof(content.conf_nodeid_str),
						"Default Node-ID: 0x%x",
						(unsigned int) device->default_nodeid);
				uv_uilabel_init(&content.conf_nodeid, style->font, ALIGN_TOP_LEFT,
						style->text_color, content.conf_nodeid_str);
				uv_uiframewindow_addxy(&content.right_frame, &content.conf_nodeid,
						0, ry, rc.w, 26);
				ry += 26;
			}

			char confrevbuf[32];
			if (device->revision != 0) {
				snprintf(confrevbuf, sizeof(confrevbuf), "%u",
						(unsigned int) device->revision);
			}
			else {
				strcpy(confrevbuf, "-");
			}
			snprintf(content.conf_rev_str, sizeof(content.conf_rev_str),
					"Revision number: %s", confrevbuf);
			uv_uilabel_init(&content.conf_rev, style->font, ALIGN_TOP_LEFT,
					rev_color, content.conf_rev_str);
			uv_uiframewindow_addxy(&content.right_frame, &content.conf_rev,
					0, ry, rc.w, 26);
			ry += 26 + MARGIN;

			// red warning when the configuration and device revisions differ
			if (mismatch) {
				snprintf(content.warning_str, sizeof(content.warning_str),
						"Configuration file revision does not match device.\n"
						"All parameters might not save / load correctly");
				uv_uilabel_init(&content.warning, style->font, ALIGN_TOP_LEFT,
						WARNING_COLOR, content.warning_str);
				uv_uiframewindow_addxy(&content.right_frame, &content.warning,
						0, ry, rc.w, 44);
			}
		}

		// Button stack at the bottom of the panel, from the bottom up: the
		// "Save parameters" / "Load parameters" row, then "Flash firmware", and
		// (only when the package bundles media) "Load media files" between them.
		// Polled in devicetab_step().
		bool has_media = device->has_media;
		int16_t btn_w = (rc.w - MARGIN) / 2;
		int16_t btn_y = rc.h - BUTTON_H;
		int16_t media_y = btn_y - BUTTON_H - MARGIN;
		int16_t flash_y = has_media ? (media_y - BUTTON_H - MARGIN) : media_y;
		uv_uibutton_init(&content.flash_btn, "Flash firmware", style);
		uv_uiframewindow_addxy(&content.right_frame, &content.flash_btn,
				0, flash_y, rc.w, BUTTON_H);
		if (has_media) {
			uv_uibutton_init(&content.media_btn, "Load media files", style);
			uv_uiframewindow_addxy(&content.right_frame, &content.media_btn,
					0, media_y, rc.w, BUTTON_H);
		}
		uv_uibutton_init(&content.save_btn, "Save parameters", style);
		uv_uiframewindow_addxy(&content.right_frame, &content.save_btn,
				0, btn_y, btn_w, BUTTON_H);
		uv_uibutton_init(&content.load_btn, "Load parameters", style);
		uv_uiframewindow_addxy(&content.right_frame, &content.load_btn,
				btn_w + MARGIN, btn_y, btn_w, BUTTON_H);
		// media loading and saving/loading need the device's object dictionary /
		// bundled files; disable when there is no configuration package. Flashing is
		// left enabled even without a package: the user can pick a .uvdev to flash
		// (see the flash handler in devicetab_step).
		if (strlen(device->filepath) == 0) {
			if (has_media) {
				uv_uiobject_disable(&content.media_btn);
			}
			uv_uiobject_disable(&content.save_btn);
			uv_uiobject_disable(&content.load_btn);
		}

		} // end of !thirdparty right panel

		// while an async operation is in progress keep the frame(s) (and all their
		// content) disabled so nothing is touched while it runs
		if (busy) {
			uv_uiwindow_set_enabled(&content.left_frame, false);
			if (!thirdparty) {
				uv_uiwindow_set_enabled(&content.right_frame, false);
			}
			// a flash keeps its button live (as a cancel button) so it survives a
			// rebuild that happens mid-flash (e.g. switching sub-tabs)
			if (!thirdparty && (busy_op == OP_FLASH)) {
				flash_btn_set_cancel_mode();
			}
		}

		// "Remove" button: drops this device from the system. Polled in
		// devicetab_step(). The image is loaded from the embedded PNG once. It
		// sits below both frames, centered with 25% margins on each side, and is
		// added straight to the tab window (not either frame).
		if (!remove_media_loaded) {
			uv_uimedia_newbitmapexmem_mem(&remove_media, "minus_hd",
					minus_hd_png, minus_hd_png_len);
			remove_media_loaded = true;
		}
		uv_uimediabutton_init(&content.remove_btn, "Remove Device", &remove_media, style);
		uv_uitabwindow_addxy(tabwin, &content.remove_btn,
				cbb.w / 4, frame_y + frame_h + MARGIN,
				cbb.w / 2, BUTTON_H);
	}
}


/// @brief: Fills the nested sub-tab window with the content of the currently
/// selected sub-tab. Mirrors uvui's show_active_tab() but one level down.
static void show_active_subtab(void) {
	uv_uitabwindow_clear(&content.subtabs);
	if ((selected_subtab == SUBTAB_TERMINAL) && (current_device != NULL)) {
		terminaltab_build(&content.subtabs, current_device->nodeid);
	}
	else {
		build_device_view(&content.subtabs, current_device);
	}
	// uv_uitabwindow_clear() drops the step callback, so re-register it
	uv_uitabwindow_set_stepcallb(&content.subtabs, &subtab_step, NULL);
	uv_ui_refresh(&content.subtabs);
}


/// @brief: Step callback of the nested sub-tab window. Swaps content when the
/// user switches sub-tabs and drives the terminal while it is the active view.
static uv_uiobject_ret_e subtab_step(void *me, const uint16_t step_ms) {
	(void) me;
	(void) step_ms;
	if (uv_uitabwindow_tab_changed(&content.subtabs)) {
		selected_subtab = uv_uitabwindow_get_tab(&content.subtabs);
		show_active_subtab();
	}
	if (uv_uitabwindow_get_tab(&content.subtabs) == SUBTAB_TERMINAL) {
		// the terminal owns the keyboard unless the log view is expanded over it
		terminaltab_step(!uvui_log_is_expanded());
	}
	return UIOBJECT_RETURN_ALIVE;
}


void devicetab_show_device(uv_uitabwindow_st *tabwin, device_st *device) {
	const uv_uistyle_st *style = &uv_uistyles[0];
	// opening a different device's tab resets the sub-tab selection to "Device"
	if (device != current_device) {
		selected_subtab = SUBTAB_DEVICE;
	}
	current_device = device;
	showing_system = false;

	if (device == NULL) {
		// empty device tab: no sub-tabs, just the placeholder
		build_device_view(tabwin, NULL);
	}
	else {
		// wrap the device view and the terminal in a nested tab window filling the
		// device tab's content area
		uv_uitabwindow_init(&content.subtabs, SUBTAB_COUNT, style,
				content.subtabs_buf, subtab_names);
		uv_uiwindow_set_transparent(&content.subtabs, true);
		uv_bounding_box_st cbb = uv_uitabwindow_get_contentbb(tabwin);
		uv_uitabwindow_addxy(tabwin, &content.subtabs, 0, 0, cbb.w, cbb.h);
		uv_uitabwindow_set_tab(&content.subtabs, selected_subtab);
		show_active_subtab();
	}
}



/// @brief: Rewrites the system-tab search button label to a live countdown of
/// the remaining listen time ("Searching Xs...") while a scan is running.
static void refresh_search_btn_text(void) {
	unsigned int rem_s = (find_search_remaining_ms() + 999u) / 1000u;
	snprintf(content.search_btn_str, sizeof(content.search_btn_str),
			"Searching %us...", rem_s);
	uv_uibutton_set_text(&content.search_btn, content.search_btn_str);
	uv_ui_refresh(&content.search_btn);
}


bool devicetab_step(void) {
	bool ret = false;
	// reap any device simulators that have exited, regardless of the current tab,
	// so their processes are cleaned up and the running list stays accurate
	simrun_step();
	bool sims_changed = simrun_poll_changed();

	// persist the Account fields whenever the user commits an edit (Enter or click
	// away). Polled here - before the busy early-return - so it works on every tab
	// state; the fields exist only while the system tab is built. Editing them is
	// equivalent to running with --user / --pwd.
	if (showing_system) {
		if (uv_uitextedit_value_changed(&content.account_url)) {
			credentials_set_url(uv_uitextedit_get_text(&content.account_url));
		}
		if (uv_uitextedit_value_changed(&content.account_user)) {
			credentials_set_username(
					uv_uitextedit_get_text(&content.account_user));
		}
		if (uv_uitextedit_value_changed(&content.account_pass)) {
			credentials_set_password(
					uv_uitextedit_get_text(&content.account_pass));
		}
	}

	if (busy) {
		// while an async operation runs all input is ignored and the frames stay
		// disabled; rebuild once it finishes
		if (busy_op == OP_SEARCH) {
			// keep the search button's countdown ticking while the scan runs
			refresh_search_btn_text();
		}
		// a firmware flash keeps its button live as a cancel button: update the
		// label as the flash moves between waiting for the device and downloading,
		// and cancel the flash when it is clicked again
		if (busy_op == OP_FLASH) {
			bool waiting = loadbin_is_waiting(&dev.load);
			if (waiting != flash_btn_waiting) {
				flash_btn_waiting = waiting;
				flash_btn_set_cancel_mode();
			}
			if (uv_uibutton_clicked(&content.flash_btn)) {
				printf("Cancelling flashing of node 0x%x...\n",
						(unsigned int) dev.load.nodeid);
				fflush(stdout);
				load_cancel();
			}
		}
		// during a simulator parameter load the system tab stays visible; rebuild
		// it as the simulators move through Started -> Loading params -> Running.
		// The "Run simulator" button is live in this state as "Force stop
		// simulator": clicking it kills every simulator and cancels the in-progress
		// parameter load (simrun_kill_all sets the load's cancel flag).
		if ((busy_op == OP_SIMPARAM) && showing_system) {
			if (uv_uibutton_clicked(&content.run_sim_btn)) {
				printf("Force-stopping the simulators...\n");
				fflush(stdout);
				simrun_kill_all();
				ret = true;
			}
			else if (sims_changed) {
				ret = true;
			}
			else {
				// no state change and no click: nothing to rebuild
			}
		}
		if (busy_finished()) {
			// the single-device save/load set the log title themselves; restore it
			// to the default now they are done (the system save/load reset it from
			// within their own task)
			if ((busy_op == OP_SAVE) || (busy_op == OP_LOAD)) {
				uvui_reset_log_title();
			}
			// the flash (wfr / uv paths) took the CAN callback over to watch for the
			// device's boot-up; restore the heartbeat monitor and terminal sniffer
			if (busy_op == OP_FLASH) {
				find_reinstall_monitor();
			}
			busy = false;
			busy_op = OP_NONE;
			// rebuild: show the final device list and reset the search label
			ret = true;
		}
		return ret;
	}
	if (showing_system) {
		// a simulator exited on its own: rebuild so its row disappears
		if (sims_changed) {
			ret = true;
		}
		// "Search devices" clicked: scan the CAN bus and add any newly found
		// devices alongside the existing ones (the existing devices are kept).
		if (uv_uibutton_clicked(&content.search_btn)) {
			// a manual search clears the auto-discovery blacklist, so devices the
			// user previously removed can be found again
			find_clear_blacklist();
			// run the scan on its own task so the UI stays live and new device
			// tabs appear as soon as each device is found (the uvui loop
			// rebuilds the tabs as the device count grows)
			find_search_async();
			start_busy(OP_SEARCH);
		}
		// "Load system configuration from file": pick a .uvsys file and load it.
		// system_set_file() appends a device per its uvsys.json DEVS entry (see
		// system.c). If any of those devices are online and carry a saved
		// parameter file, offer to load those parameters onto the devices.
		else if (uv_uibutton_clicked(&content.sys_load_btn)) {
			char path[1024] = "";
			if (uv_uifiledialog_exec("Load system configuration from file",
					SYSTEM_FILE_FILTERS,
					sizeof(SYSTEM_FILE_FILTERS) / sizeof(SYSTEM_FILE_FILTERS[0]),
					false, path, sizeof(path))) {
				system_set_file(&dev.system, path);
				// the system (and its device tabs) changed: rebuild and re-show
				ret = true;

				// refresh device states so freshly added devices reflect whether
				// they are actually on the bus right now
				find_update_device_states(&dev.system);

				// collect online devices that carry a saved parameter file, and
				// build the confirmation message listing each device and its file
				device_st *targets[SYSTEM_DEV_MAX_COUNT];
				uint8_t tcount = 0;
				char msg[1024];
				int off = snprintf(msg, sizeof(msg),
						"Some online devices in this system configuration have "
						"saved parameters. Load these parameters onto the "
						"devices?\n\n");
				for (uint8_t i = 0; i < system_get_dev_count(&dev.system); i++) {
					device_st *d = system_get_dev(&dev.system, i);
					if ((strlen(d->param_file) != 0) &&
							(d->state != DEV_STATE_OFFLINE)) {
						if (tcount < SYSTEM_DEV_MAX_COUNT) {
							targets[tcount] = d;
							tcount++;
						}
						const char *dname = (strlen(d->devname) > 0) ?
								d->devname : d->name;
						// show just the file name, not the temp-dir path
						const char *base = strrchr(d->param_file, '/');
						base = (base != NULL) ? base + 1 : d->param_file;
						if (off < (int) sizeof(msg)) {
							off += snprintf(msg + off, sizeof(msg) - off,
									"  %s (node 0x%x)  <-  %s\n", dname,
									(unsigned int) d->nodeid, base);
						}
					}
				}

				if (tcount > 0) {
					uv_uiacceptdialog_st dialog = { };
					if (uv_uiacceptdialog_exec(&dialog, msg, "Yes", "No",
							&uv_uistyles[0]) == UIACCEPTDIALOG_RET_YES) {
						loadparam_load_params_async(targets, tcount);
						start_busy(OP_SYSPARAM);
					}
				}
			}
		}
		// "Save system configuration": pick a destination .uvsys file, then write
		// the whole system (every device's .uvdev plus parameters from each
		// operational device) to it on its own task so the UI stays live.
		else if (uv_uibutton_clicked(&content.sys_save_btn)) {
			char path[1024] = "";
			if (uv_uifiledialog_exec("Save system configuration as",
					SYSTEM_FILE_FILTERS,
					sizeof(SYSTEM_FILE_FILTERS) / sizeof(SYSTEM_FILE_FILTERS[0]),
					true, path, sizeof(path))) {
				savesys_save_async(path);
				start_busy(OP_SAVESYS);
			}
		}
		// "Load system configuration": write the loaded system's bundled
		// parameters onto every online device. First read the CAN interface
		// version from each device and compare it against the version stored in
		// its param file; if any differ, warn (the saved parameters may not load
		// correctly). Once confirmed, the load runs on its own task with the
		// EMCY/store/reset sequencing handled by loadparam.
		else if (uv_uibutton_clicked(&content.sys_loadparams_btn)) {
			// collect online devices that carry a saved parameter file
			device_st *targets[SYSTEM_DEV_MAX_COUNT];
			uint8_t tcount = 0;
			for (uint8_t i = 0; i < system_get_dev_count(&dev.system); i++) {
				device_st *d = system_get_dev(&dev.system, i);
				if ((strlen(d->param_file) != 0) &&
						(d->state == DEV_STATE_OP) &&
						(tcount < SYSTEM_DEV_MAX_COUNT)) {
					targets[tcount] = d;
					tcount++;
				}
			}

			if (tcount == 0) {
				uv_uiacceptdialog_st dialog = { };
				uv_uiacceptdialog_exec(&dialog,
						"None of the online devices have saved parameters in this "
						"system configuration.", "OK", "OK", &uv_uistyles[0]);
			}
			else {
				// compare each device's CAN interface version against the one
				// stored in its param file, listing any that differ
				char msg[1024];
				int off = snprintf(msg, sizeof(msg),
						"The CAN interface version of these devices differs from "
						"the one stored in the system configuration. The saved "
						"parameters might not load correctly:\n\n");
				uint8_t mismatches = 0;
				for (uint8_t i = 0; i < tcount; i++) {
					device_st *d = targets[i];
					uint32_t file_if = 0;
					uint32_t dev_if = 0;
					if (loadparam_can_if_mismatch(d, &file_if, &dev_if)) {
						mismatches++;
						const char *dname = (strlen(d->devname) > 0) ?
								d->devname : d->name;
						if (off < (int) sizeof(msg)) {
							off += snprintf(msg + off, sizeof(msg) - off,
									"  %s (node 0x%x): file %u, device %u\n",
									dname, (unsigned int) d->nodeid,
									(unsigned int) file_if,
									(unsigned int) dev_if);
						}
					}
				}

				bool proceed = true;
				if (mismatches > 0) {
					if (off < (int) sizeof(msg)) {
						snprintf(msg + off, sizeof(msg) - off,
								"\nLoad the parameters anyway?");
					}
					uv_uiacceptdialog_st dialog = { };
					proceed = (uv_uiacceptdialog_exec(&dialog, msg, "Yes", "No",
							&uv_uistyles[0]) == UIACCEPTDIALOG_RET_YES);
				}

				if (proceed) {
					loadparam_load_system_async(targets, tcount);
					start_busy(OP_SYSLOAD);
				}
			}
		}
#if !CONFIG_TARGET_WIN
		// "Run simulator": launch the Linux simulator of every configured device,
		// each as its own process connected to uvcan's CAN channel. Rebuild so the
		// running-simulator list appears. Simulators are a Linux-only feature, so
		// the button and its list exist only there.
		else if (uv_uibutton_clicked(&content.run_sim_btn)) {
			// use the CAN device actually selected in the HAL (the config window
			// sets it there, not in dev.can_channel) so the simulators connect to
			// the same bus uvcan is monitoring
			const char *chn = uv_can_get_dev();

			// refresh the device states so we know which configured devices are
			// actually present on the bus right now: those are not simulated (a
			// simulator would clash with the real hardware). Instead they are
			// restored to defaults and have the system parameters loaded onto them.
			find_update_device_states(&dev.system);
			uint8_t online_nodeids[SYSTEM_DEV_MAX_COUNT];
			uint8_t online_count = 0;
			char online_list[512];
			int oo = 0;
			for (uint8_t i = 0; i < system_get_dev_count(&dev.system); i++) {
				device_st *d = system_get_dev(&dev.system, i);
				if ((strlen(d->filepath) != 0) &&
						(d->state != DEV_STATE_OFFLINE) &&
						(online_count < SYSTEM_DEV_MAX_COUNT)) {
					online_nodeids[online_count] = d->nodeid;
					online_count++;
					const char *dname = (strlen(d->devname) > 0) ?
							d->devname : d->name;
					if (oo < (int) sizeof(online_list)) {
						oo += snprintf(online_list + oo, sizeof(online_list) - oo,
								"    %s (node 0x%x)\n", dname,
								(unsigned int) d->nodeid);
					}
				}
			}

			bool proceed = true;
			// Some configured devices are already online: warn that they will not
			// be simulated but restored + reloaded instead, and let the user back
			// out.
			if (online_count > 0) {
				char msg[1024];
				snprintf(msg, sizeof(msg),
						"These configured devices are already online:\n\n%s\n"
						"The simulator will NOT be started for them. Instead each "
						"is restored to its system defaults and the parameters from "
						"the system configuration file are loaded onto it. The "
						"remaining (offline) devices are simulated as usual.\n\n"
						"Continue?", online_list);
				uv_uiacceptdialog_st dialog = { };
				proceed = (uv_uiacceptdialog_exec(&dialog, msg, "Yes", "No",
						&uv_uistyles[0]) == UIACCEPTDIALOG_RET_YES);
			}

			// On a real CAN device the simulators' frames need another node on the
			// bus to acknowledge them; with no real device present they never reach
			// uvcan, so the devices never come online and no parameters load. When
			// some configured device is already online it provides that
			// acknowledgement, so this warning is only shown otherwise.
			if (proceed && (online_count == 0) && !simrun_can_is_virtual(chn)) {
				char msg[512];
				snprintf(msg, sizeof(msg),
						"The simulators will run on the real CAN device '%s'.\n\n"
						"For them to communicate, at least one real device must be "
						"present on the CAN network to acknowledge the bus messages. "
						"Without a real device on the bus the simulators cannot "
						"exchange data and their parameters will not load.\n\n"
						"Run the simulators anyway?", chn);
				uv_uiacceptdialog_st dialog = { };
				proceed = (uv_uiacceptdialog_exec(&dialog, msg, "Yes", "No",
						&uv_uistyles[0]) == UIACCEPTDIALOG_RET_YES);
			}

			if (proceed) {
				// starts a simulator only for the offline configured devices (online
				// ones are skipped inside simrun_start_system)
				uint8_t started = simrun_start_system(&dev.system, chn);
				if ((started == 0) && (online_count == 0)) {
					uv_uiacceptdialog_st dialog = { };
					uv_uiacceptdialog_exec(&dialog,
							"No device simulators could be started. The devices need "
							"a configuration package (.uvdev) bundling a Linux "
							"simulator.", "OK", "OK", &uv_uistyles[0]);
				}
				else {
					// load the system's bundled parameters: the simulators (once
					// online) and, for the online real devices, after restoring them
					// to defaults and resetting them. The UI goes busy so it does not
					// touch the SDO client while the load task owns it.
					simrun_load_params_async(&dev.system,
							online_nodeids, online_count);
					start_busy(OP_SIMPARAM);
				}
				ret = true;
			}
		}
		else {
			// no system-level button clicked; check the per-simulator "Log" and
			// "Kill" buttons
			for (uint8_t i = 0; i < simrun_get_count(); i++) {
				if (uv_uibutton_clicked(&content.sim_log_btns[i])) {
					simrun_open_log(i);
					break;
				}
				if (uv_uibutton_clicked(&content.sim_kill_btns[i])) {
					simrun_kill(i);
					ret = true;
					break;
				}
			}
		}
#endif
	}
	else if (current_device != NULL) {
		// "Server files": open the Usevolt file-server browser. Needs the account
		// username, password and server URL set in the Account panel on the system
		// tab; if any is missing, tell the user to set them there.
		if (uv_uibutton_clicked(&content.serverfiles_btn)) {
			if ((strlen(credentials_get_username()) == 0) ||
					(strlen(credentials_get_password()) == 0) ||
					(strlen(credentials_get_url()) == 0)) {
				uv_uiacceptdialog_st dialog = { };
				uv_uiacceptdialog_exec(&dialog,
						"Set the server URL, username and password in the Account "
						"panel on the System tab first.", "OK", "OK",
						&uv_uistyles[0]);
			}
			else {
				serverfiles_win_exec(&uv_uistyles[0]);
			}
		}
		// "Flash firmware": confirm, then flash the package's firmware to the
		// device. The flash runs asynchronously; the frames are disabled until it
		// finishes (handled above and in devicetab_show_device).
		else if (uv_uibutton_clicked(&content.flash_btn)) {
			// A device with no configuration package flashes a .uvdev the user
			// picks here (that package is then kept as the device's config file); a
			// configured device flashes its own package. An offline device is
			// flashed with the wait-for-boot-up logic (wfr): the flash waits until
			// the device is powered on, then downloads.
			bool configless = (strlen(current_device->filepath) == 0);
			bool offline = (current_device->state == DEV_STATE_OFFLINE);
			bool bootup = (current_device->state == DEV_STATE_BOOTUP);
			char picked[1024] = "";
			bool proceed = true;
			if (configless) {
				// ask the user which firmware package to flash
				if (!uv_uifiledialog_exec(
						"Select firmware package (.uvdev) to flash",
						DEVICE_FILE_FILTERS,
						sizeof(DEVICE_FILE_FILTERS) / sizeof(DEVICE_FILE_FILTERS[0]),
						false, picked, sizeof(picked))) {
					// cancelled: nothing to flash
					proceed = false;
				}
			}

			// A device stuck in BOOTUP sits in its bootloader with no running
			// firmware, so it answers at whatever node id the bootloader uses. Once
			// the flash finishes it boots the new firmware, which takes its node id
			// from the package's DATABASE default. Follow that node id here so the
			// tab keeps addressing the device after it comes back up.
			uint8_t boot_nodeid = 0;
			if (proceed && bootup) {
				boot_nodeid = configless ? system_read_file_nodeid(picked) :
						current_device->default_nodeid;
				if (boot_nodeid == current_device->nodeid) {
					// it boots at the node id the tab already uses: nothing to change
					// and nothing worth telling the user about
					boot_nodeid = 0;
				}
			}

			if (proceed) {
				uv_uiacceptdialog_st dialog = { };
				char msgbuf[2048];
				// note explaining the wait-for-boot-up behaviour for an offline
				// device (it also tells the user how to cancel)
				const char *offline_note = offline ?
						" The device is offline: flashing will begin once it is "
						"powered on. Click the button again to cancel." : "";
				// note about the node id change a bootloader-state device makes when
				// it boots the flashed firmware
				char boot_note[256] = "";
				if (boot_nodeid != 0) {
					snprintf(boot_note, sizeof(boot_note),
							"\n\nThe device is in its bootloader: after flashing it "
							"boots the new firmware at the package's default node id "
							"0x%x. This device tab switches to node 0x%x once the "
							"flash finishes.",
							(unsigned int) boot_nodeid, (unsigned int) boot_nodeid);
				}
				if (configless) {
					// show the node id and chosen package, and make clear uvcan
					// cannot vouch for the package matching this device
					const char *base = strrchr(picked, '/');
					base = (base != NULL) ? base + 1 : picked;
					snprintf(msgbuf, sizeof(msgbuf),
							"Flash firmware package '%s' to the device at node "
							"0x%x?\n\n"
							"uvcan cannot verify that this package is suitable for "
							"this device \342\200\224 you are responsible for "
							"choosing the correct firmware. The device will be reset "
							"and must not be powered off during flashing.%s%s",
							base, (unsigned int) current_device->nodeid,
							offline_note, boot_note);
				}
				else {
					snprintf(msgbuf, sizeof(msgbuf),
							"Flash the firmware to the device at node 0x%x? The "
							"device will be reset and must not be powered off during "
							"flashing.%s%s",
							(unsigned int) current_device->nodeid, offline_note,
							boot_note);
				}
				if (uv_uiacceptdialog_exec(&dialog, msgbuf, "Yes", "No",
						&uv_uistyles[0]) == UIACCEPTDIALOG_RET_YES) {
					// an offline device waits for its boot-up message before flashing
					bool wfr = offline;
					bool started = configless ?
							load_flash_uvdev_to_node(picked,
									current_device->nodeid, wfr) :
							load_flash_device(current_device, wfr);
					if (started) {
						// the firmware changes, so re-read the device's revision and
						// software version once it is back online after the flash
						current_device->dev_revision = 0;
						current_device->sw_version = 0;
						// keep the picked package as this device's configuration file
						// so it is no longer config-less. Takes visual effect when the
						// flash finishes and the tab rebuilds. Preserve the node id we
						// are flashing (assigning a file to an offline device would
						// otherwise adopt the file's default node id).
						if (configless) {
							uint8_t flashed_nodeid = current_device->nodeid;
							system_set_device_file(current_device, picked);
							current_device->nodeid = flashed_nodeid;
						}
						// the device was flashed in its bootloader: it boots the new
						// firmware at the package's default node id, so address it
						// there from now on. Takes visual effect when the flash
						// finishes and the tab rebuilds.
						if (boot_nodeid != 0) {
							current_device->nodeid = boot_nodeid;
						}
						start_busy(OP_FLASH);
					}
				}
			}
		}
		// "Load media files": confirm, then load the media bundled in the device's
		// .uvdev package onto the device (same transfer as --loadmedia). Runs
		// asynchronously; the frames stay disabled until it finishes. The button is
		// only laid out when the package bundles media, so guard on has_media too.
		else if (current_device->has_media &&
				uv_uibutton_clicked(&content.media_btn)) {
			uv_uiacceptdialog_st dialog = { };
			if (uv_uiacceptdialog_exec(&dialog,
					"Load the media files bundled in the configuration package "
					"onto this device?\n\n"
					"The device is reset once the loading finishes, so that it "
					"starts using the new media.",
					"Yes", "No", &uv_uistyles[0]) == UIACCEPTDIALOG_RET_YES) {
				loadmedia_load_device_async(current_device->filepath,
						current_device->nodeid);
				start_busy(OP_MEDIA);
			}
		}
		// the user clicked "Remove": drop this device from the system and ask the
		// caller to rebuild the tabs (this device's tab disappears)
		else if (uv_uimediabutton_clicked(&content.remove_btn)) {
			// blacklist the node so live auto-discovery does not immediately re-add
			// the device the user just removed (a manual search clears the list)
			find_blacklist_node(current_device->nodeid);
			system_remove_device(&dev.system, current_device);
			ret = true;
		}
		// "Revert to defaults": CANopen restore-default-parameters request
		// (write "load" to 0x1011 sub 1). Logs the outcome; no full log window.
		else if (uv_uibutton_clicked(&content.revert_btn)) {
			uint32_t sig = 0x64616F6Cu; // "load"
			printf("Reverting node 0x%x to default settings "
					"(CANopen restore 0x1011)...\n",
					(unsigned int) current_device->nodeid);
			fflush(stdout);
			uv_errors_e e = uv_canopen_sdo_write(current_device->nodeid,
					CONFIG_CANOPEN_RESTORE_PARAMS_INDEX,
					CONFIG_CANOPEN_RESTORE_ALL_PARAMS_SUBINDEX, sizeof(sig), &sig);
			if (e == ERR_NONE) {
				printf("Node 0x%x restored to defaults. Reset the device to "
						"apply.\n", (unsigned int) current_device->nodeid);
			}
			else {
				printf("ERROR: restore-defaults request to node 0x%x failed.\n",
						(unsigned int) current_device->nodeid);
			}
			fflush(stdout);
		}
		// "Save settings": CANopen store-parameters request (write "save" to
		// 0x1010 sub 1). Logs the outcome; no full log window.
		else if (uv_uibutton_clicked(&content.savesettings_btn)) {
			uint32_t sig = 0x65766173u; // "save"
			printf("Saving settings on node 0x%x (CANopen store 0x1010)...\n",
					(unsigned int) current_device->nodeid);
			fflush(stdout);
			uv_errors_e e = uv_canopen_sdo_write(current_device->nodeid,
					CONFIG_CANOPEN_STORE_PARAMS_INDEX,
					CONFIG_CANOPEN_STORE_ALL_PARAMS_SUBINDEX, sizeof(sig), &sig);
			if (e == ERR_NONE) {
				printf("Settings stored to non-volatile memory on node 0x%x.\n",
						(unsigned int) current_device->nodeid);
			}
			else {
				printf("ERROR: save-settings request to node 0x%x failed.\n",
						(unsigned int) current_device->nodeid);
			}
			fflush(stdout);
		}
		// "Reset device": NMT reset-node command. Logs the action; no full log
		// window. The device drops offline and reboots.
		else if (uv_uibutton_clicked(&content.reset_btn)) {
			printf("Resetting node 0x%x (NMT reset node)...\n",
					(unsigned int) current_device->nodeid);
			fflush(stdout);
			uv_canopen_nmt_master_send_cmd(current_device->nodeid,
					CANOPEN_NMT_CMD_RESET_NODE);
			printf("Reset command sent to node 0x%x.\n",
					(unsigned int) current_device->nodeid);
			fflush(stdout);
		}
		// "Save parameters": pick a destination file (save dialog) and write the
		// device's parameters to it, like --saveparam for this single device. The
		// save runs on its own task so the UI stays live (and the log updates).
		else if (uv_uibutton_clicked(&content.save_btn)) {
			char path[1024] = "";
			if (uv_uifiledialog_exec("Save parameters as", PARAM_FILE_FILTERS,
					sizeof(PARAM_FILE_FILTERS) / sizeof(PARAM_FILE_FILTERS[0]),
					true, path, sizeof(path))) {
				char title[160];
				const char *dname = (strlen(current_device->devname) > 0) ?
						current_device->devname : current_device->name;
				snprintf(title, sizeof(title), "Saving parameters from %s...",
						dname);
				uvui_set_log_title(title);
				saveparam_save_device_async(current_device, path);
				start_busy(OP_SAVE);
			}
		}
		// "Load parameters": pick an existing file (open dialog) and write its
		// parameters to the device, like --loadparam for this single device, on
		// its own task so the UI stays live.
		else if (uv_uibutton_clicked(&content.load_btn)) {
			char path[1024] = "";
			if (uv_uifiledialog_exec("Load parameters from", PARAM_FILE_FILTERS,
					sizeof(PARAM_FILE_FILTERS) / sizeof(PARAM_FILE_FILTERS[0]),
					false, path, sizeof(path))) {
				char title[160];
				const char *dname = (strlen(current_device->devname) > 0) ?
						current_device->devname : current_device->name;
				snprintf(title, sizeof(title), "Loading parameters to %s...",
						dname);
				uvui_set_log_title(title);
				loadparam_load_device_async(current_device, path);
				start_busy(OP_LOAD);
			}
		}
		// The picker writes the chosen path straight into content.fileedit_path;
		// a mismatch against the device's stored path means the user picked a new
		// file. Comparing paths is robust regardless of widget step ordering.
		else if (strcmp(content.fileedit_path, current_device->filepath) != 0) {
			system_set_device_file(current_device, content.fileedit_path);
			// the device name (and node id / revision) changed: ask the caller
			// to rebuild the tabs and re-show this tab's content
			ret = true;
		}
		else if (current_device->loaded &&
				uv_uidigitedit_value_changed(&content.nodeid_edit)) {
			// the user adjusted the node id; store it back on the device. No
			// rebuild is needed, the editor already shows the new value.
			current_device->nodeid = uv_uidigitedit_get_value(&content.nodeid_edit);
		}
		else {
			// nothing changed this cycle
		}
	}
	else {
		// no pickable content on this tab
	}
	return ret;
}


#endif
