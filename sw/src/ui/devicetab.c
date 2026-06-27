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
#include "uvui.h"
#include "simrun.h"


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

	// right-panel software version and revision read from the configuration file
	uv_uilabel_st conf_sw;
	char conf_sw_str[128];
	uv_uilabel_st conf_rev;
	char conf_rev_str[64];

	// red warning shown when the configuration file revision and the device
	// revision do not match
	uv_uilabel_st warning;
	char warning_str[192];

	// "Flash firmware" button (above the parameter buttons)
	uv_uibutton_st flash_btn;

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

// A long-running, asynchronous device operation started from the device tab.
typedef enum {
	OP_NONE = 0,
	OP_FLASH,
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


/// @brief: Marks an asynchronous operation as started and disables both device
/// frames (and all their content) so nothing is touched while it runs.
static void start_busy(busy_op_e op) {
	busy = true;
	busy_op = op;
	uv_uiwindow_set_enabled(&content.left_frame, false);
	uv_uiwindow_set_enabled(&content.right_frame, false);
	uv_ui_refresh(&content.left_frame);
	uv_ui_refresh(&content.right_frame);
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

	// The system tab is laid out as framed panels. On Linux it splits into a
	// "System configuration" panel on top and a "Simulator" panel below; on other
	// targets the simulators are unavailable, so the configuration panel fills the
	// whole tab.
	int16_t frame_x = MARGIN;
	int16_t frame_w = cbb.w - 2 * MARGIN;
#if !CONFIG_TARGET_WIN
	// the configuration panel needs room for the double-height source row plus the
	// save/load row; the simulator panel takes whatever is left below it
	int16_t sys_frame_h = 4 * BUTTON_H + 2 * MARGIN;
#else
	int16_t sys_frame_h = cbb.h - 2 * MARGIN;
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

	// "Search CAN-bus for devices": scans the bus and replaces the device list
	// with the Usevolt devices found, on its own task (handled in
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
	int16_t sim_frame_h = cbb.h - sim_frame_y - MARGIN;
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

	// whether any device that has a configuration package is live on the bus
	// (which blocks running the simulators, as a simulator would clash with the
	// real hardware)
	bool any_conf_online = false;
	for (uint8_t i = 0; i < system_get_dev_count(system); i++) {
		device_st *d = system_get_dev(system, i);
		if ((strlen(d->filepath) != 0) && (d->state != DEV_STATE_OFFLINE)) {
			any_conf_online = true;
		}
	}

	// "Run simulator": launches the Linux simulator of every configured device.
	// Disabled when no system file is loaded, when any configured device is live
	// on the bus, or while simulators are already running. When real devices block
	// it, a second line explains why. Fills the left column.
	bool sims_running = simrun_any_running();
	if (any_conf_online && !sims_running) {
		strcpy(content.run_sim_str,
				"Run simulator\nDisconnect online devices to enable");
	}
	else {
		strcpy(content.run_sim_str, "Run simulator");
	}
	uv_uibutton_init(&content.run_sim_btn, content.run_sim_str, style);
	uv_uiframewindow_addxy(&content.sim_frame, &content.run_sim_btn,
			0, 0, left_w, mc.h);
	if (!system->loaded || any_conf_online || sims_running || sys_busy) {
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
}


void devicetab_show_device(uv_uitabwindow_st *tabwin, device_st *device) {
	const uv_uistyle_st *style = &uv_uistyles[0];
	current_device = device;
	showing_system = false;

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
		const char *state_name;
		switch (device->state) {
		case DEV_STATE_OP:
			state_name = "OPERATIONAL";
			break;
		case DEV_STATE_PREOP:
			state_name = "PRE-OPERATIONAL";
			break;
		case DEV_STATE_BOOTUP:
			state_name = "BOOT-UP";
			break;
		case DEV_STATE_OFFLINE:
		default:
			state_name = "OFFLINE";
			break;
		}
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
		uv_uiframewindow_addxy(&content.right_frame, &content.fileedit,
				0, ry, rc.w, FILEEDIT_H);
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

		// "Flash firmware" button, and below it the "Save parameters" /
		// "Load parameters" buttons (side by side) at the bottom of the panel.
		// Polled in devicetab_step().
		int16_t btn_w = (rc.w - MARGIN) / 2;
		int16_t btn_y = rc.h - BUTTON_H;
		int16_t flash_y = btn_y - BUTTON_H - MARGIN;
		uv_uibutton_init(&content.flash_btn, "Flash firmware", style);
		uv_uiframewindow_addxy(&content.right_frame, &content.flash_btn,
				0, flash_y, rc.w, BUTTON_H);
		uv_uibutton_init(&content.save_btn, "Save parameters", style);
		uv_uiframewindow_addxy(&content.right_frame, &content.save_btn,
				0, btn_y, btn_w, BUTTON_H);
		uv_uibutton_init(&content.load_btn, "Load parameters", style);
		uv_uiframewindow_addxy(&content.right_frame, &content.load_btn,
				btn_w + MARGIN, btn_y, btn_w, BUTTON_H);
		// flashing and saving/loading need the device's object dictionary /
		// firmware; disable when there is no configuration package attached
		if (strlen(device->filepath) == 0) {
			uv_uiobject_disable(&content.flash_btn);
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
	if (busy) {
		// while an async operation runs all input is ignored and the frames stay
		// disabled; rebuild once it finishes
		if (busy_op == OP_SEARCH) {
			// keep the search button's countdown ticking while the scan runs
			refresh_search_btn_text();
		}
		// during a simulator parameter load the system tab stays visible; rebuild
		// it as the simulators move through Started -> Loading params -> Running
		if ((busy_op == OP_SIMPARAM) && showing_system && sims_changed) {
			ret = true;
		}
		if (busy_finished()) {
			// the single-device save/load set the log title themselves; restore it
			// to the default now they are done (the system save/load reset it from
			// within their own task)
			if ((busy_op == OP_SAVE) || (busy_op == OP_LOAD)) {
				uvui_reset_log_title();
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
		// "Search devices" clicked: warn if it would discard existing devices,
		// then scan the CAN bus and replace the device list with what is found
		if (uv_uibutton_clicked(&content.search_btn)) {
			bool proceed = true;
			if (system_get_dev_count(&dev.system) > 0) {
				uv_uiacceptdialog_st dialog = { };
				proceed = (uv_uiacceptdialog_exec(&dialog,
						"The existing devices will be removed and replaced by the "
						"devices found on the CAN bus. Continue?",
						"Yes", "No", &uv_uistyles[0]) == UIACCEPTDIALOG_RET_YES);
			}
			if (proceed) {
				// run the scan on its own task so the UI stays live and new device
				// tabs appear as soon as each device is found (the uvui loop
				// rebuilds the tabs as the device count grows)
				find_search_async();
				start_busy(OP_SEARCH);
			}
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
			// On a real CAN device the simulators' frames need another node on the
			// bus to acknowledge them; with no real device present they never reach
			// uvcan, so the devices never come online and no parameters load. Warn
			// (and let the user back out) before running on a non-virtual device.
			bool proceed = true;
			if (!simrun_can_is_virtual(chn)) {
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
				uint8_t started = simrun_start_system(&dev.system, chn);
				if (started == 0) {
					uv_uiacceptdialog_st dialog = { };
					uv_uiacceptdialog_exec(&dialog,
							"No device simulators could be started. The devices need "
							"a configuration package (.uvdev) bundling a Linux "
							"simulator.", "OK", "OK", &uv_uistyles[0]);
				}
				else {
					// once started, load the system's bundled parameters onto the
					// simulators (wait for them online, then suppress EMCY / write /
					// store / reset). The UI goes busy so it does not touch the SDO
					// client while the load task owns it.
					simrun_load_params_async(&dev.system);
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
		// "Flash firmware": confirm, then flash the package's firmware to the
		// device. The flash runs asynchronously; the frames are disabled until it
		// finishes (handled above and in devicetab_show_device).
		if (uv_uibutton_clicked(&content.flash_btn)) {
			uv_uiacceptdialog_st dialog = { };
			if (uv_uiacceptdialog_exec(&dialog,
					"Flash the firmware to this device? The device will be reset "
					"and must not be powered off during flashing.",
					"Yes", "No", &uv_uistyles[0]) == UIACCEPTDIALOG_RET_YES) {
				if (load_flash_device(current_device)) {
					// the firmware changes, so re-read the device's revision and
					// software version once it is back online after the flash
					current_device->dev_revision = 0;
					current_device->sw_version = 0;
					start_busy(OP_FLASH);
				}
			}
		}
		// the user clicked "Remove": drop this device from the system and ask the
		// caller to rebuild the tabs (this device's tab disappears)
		else if (uv_uimediabutton_clicked(&content.remove_btn)) {
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
