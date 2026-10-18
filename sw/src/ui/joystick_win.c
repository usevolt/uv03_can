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


#include "ui/joystick_win.h"

#if CONFIG_UI

#include "ui/uv_uidialog.h"
#include "ui/uv_uicheckbox.h"
#include "ui/uv_uilistbutton.h"
#include "ui/uv_uidigitedit.h"
#include "ui/uv_uitextedit.h"
#include "joystick.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define JW_MARGIN		10
#define JW_TITLE_H		30
#define JW_INFO_H		26
#define JW_HEAD_H		26
#define JW_BTN_H		44
#define JW_BTN_W		150
#define JW_ROW_H		44
#define JW_GAP			6
// how far in from the list window's edge its content starts, which is where
// the column titles have to start to line up with the columns
#define JW_LIST_PAD		12
// a section title ("Axes", "Buttons") above its rows
#define JW_SECTION_H	34

// the columns of a row, from the left
#define JW_COL_SLOT_W	100
#define JW_COL_DEV_W	250
#define JW_COL_OBJ_W	340
#define JW_COL_MIN_W	120
#define JW_COL_MAX_W	120
#define JW_COL_INV_W	100

// the device list: "Not used", the system's devices, and the node ids the
// configuration names that are not in the system
#define JW_DEV_MAX		(1 + SYSTEM_DEV_MAX_COUNT + JOYSTICK_SLOT_COUNT)

// the most entries the input/output picker lists at once; the filter narrows
// a longer list down
#define JW_PICK_MAX		128

#define JW_DIM_COLOR	C(0xFF909090)


// ---- the device list ------------------------------------------------------

typedef struct {
	char str[96];
	uint8_t nodeid;
	// the system's device, NULL for "Not used" and for a node id the system
	// does not have
	device_st *device;
	// the device's TXPDO values, read from its package when first needed
	joystick_obj_st *objs;
	uint16_t obj_count;
	bool objs_tried;
} jw_dev_st;

static jw_dev_st devs[JW_DEV_MAX];
static char *dev_items[JW_DEV_MAX + 1];
static uint8_t dev_count;


// ---- the window -----------------------------------------------------------

static uv_uidialog_st dialog;
static uv_uiobject_st *dialog_buf[12];
static uv_uilabel_st title_label;
static uv_uilabel_st status_label;
static char status_str[200];
static uv_uilabel_st head_labels[7];
static uv_uibutton_st cancel_btn;
static uv_uibutton_st save_btn;

static uv_uiwindow_st list_win;
static uv_uiobject_st *list_buf[2 + 7 * JOYSTICK_SLOT_COUNT];
static uv_uilabel_st section_labels[2];

typedef struct {
	uv_uilabel_st name;
	char name_str[24];
	uv_uilistbutton_st dev;
	uv_uibutton_st obj;
	char obj_str[128];
	uv_uidigitedit_st min;
	uv_uidigitedit_st max;
	uv_uicheckbox_st invert;
	uv_uilabel_st live;
	char live_str[48];
	// which entry of the device list the row has
	uint8_t dev_index;
} jw_row_st;

static jw_row_st rows[JOYSTICK_SLOT_COUNT];

// the configuration being edited
static joystick_conf_st edit;
static bool saved;
static system_st *sys;
static const uv_uistyle_st *win_style;


// ---- the input/output picker ----------------------------------------------

static uv_uidialog_st pick_dialog;
static uv_uiobject_st *pick_dialog_buf[8];
static uv_uilabel_st pick_title;
static char pick_title_str[160];
static uv_uilabel_st pick_filter_label;
static uv_uitextedit_st pick_filter;
static char pick_filter_buf[64];
static char pick_filter_applied[sizeof(pick_filter_buf)];
static uv_uilabel_st pick_info;
static char pick_info_str[96];
static uv_uibutton_st pick_cancel;
static uv_uiwindow_st pick_win;
static uv_uiobject_st *pick_buf[JW_PICK_MAX];
static uv_uibutton_st pick_btns[JW_PICK_MAX];
static char pick_strs[JW_PICK_MAX][160];
// the catalog index of each listed entry
static uint16_t pick_index[JW_PICK_MAX];
static uint16_t pick_count;
static jw_dev_st *pick_dev;
static int32_t pick_result;


static bool is_axis_row(uint8_t i) {
	return joystick_slot_is_axis(i);
}


/// @brief: Case-insensitive substring match; an empty pattern matches all
static bool name_matches(const char *name, const char *pattern) {
	bool ret = (pattern[0] == '\0');
	size_t pl = strlen(pattern);
	for (const char *p = name; !ret && (*p != '\0'); p++) {
		ret = (strncasecmp(p, pattern, pl) == 0);
	}
	return ret;
}


/// @brief: Scrolls *win* with the mouse wheel while the pointer is over it
static void wheel_step(uv_uiwindow_st *win, int16_t notch_px) {
	int16_t x = 0;
	int16_t y = 0;
	uv_ui_get_touch(&x, &y);
	int16_t gx = uv_ui_get_xglobal(win);
	int16_t gy = uv_ui_get_yglobal(win);
	uv_bounding_box_st *bb = uv_uibb(win);
	if ((x >= gx) && (x < (gx + bb->width)) &&
			(y >= gy) && (y < (gy + bb->height))) {
		int16_t scroll = uv_ui_get_scroll();
		if (scroll != 0) {
			uv_uiwindow_content_move(win, 0, scroll * notch_px);
		}
	}
}


/// @brief: Lists the picker's entries which pass the filter
static void pick_build_list(void) {
	snprintf(pick_filter_applied, sizeof(pick_filter_applied), "%s",
			pick_filter_buf);
	uv_uiwindow_clear(&pick_win);
	uv_uiwindow_content_move_to(&pick_win, 0, 0);
	int16_t w = uv_uibb(&pick_win)->width - CONFIG_UI_WINDOW_SCROLLBAR_WIDTH -
			JW_GAP;
	pick_count = 0;
	uint16_t matches = 0;
	for (uint16_t i = 0; i < pick_dev->obj_count; i++) {
		joystick_obj_st *o = &pick_dev->objs[i];
		if (name_matches(o->name, pick_filter_applied)) {
			matches++;
			if (pick_count < JW_PICK_MAX) {
				snprintf(pick_strs[pick_count], sizeof(pick_strs[pick_count]),
						"%s   (0x%x:%u, TXPDO 0x%x byte %u)", o->name,
						(unsigned int) o->mindex, (unsigned int) o->sindex,
						(unsigned int) o->pdo_cobid, (unsigned int) o->pdo_offset);
				uv_uibutton_init(&pick_btns[pick_count], pick_strs[pick_count],
						win_style);
				uv_uiwindow_addxy(&pick_win, &pick_btns[pick_count], 0,
						pick_count * (JW_ROW_H + JW_GAP), w, JW_ROW_H);
				pick_index[pick_count] = i;
				pick_count++;
			}
		}
	}
	int16_t content_h = pick_count * (JW_ROW_H + JW_GAP);
	if (content_h < uv_uibb(&pick_win)->height) {
		content_h = uv_uibb(&pick_win)->height;
	}
	uv_uiwindow_set_contentbb(&pick_win, w, content_h);

	if (matches > pick_count) {
		snprintf(pick_info_str, sizeof(pick_info_str),
				"Showing %u of %u. Type in the filter to narrow the list down.",
				(unsigned int) pick_count, (unsigned int) matches);
	}
	else {
		snprintf(pick_info_str, sizeof(pick_info_str), "%u of %u values",
				(unsigned int) matches, (unsigned int) pick_dev->obj_count);
	}
	uv_ui_refresh(&pick_dialog);
}


static uv_uiobject_ret_e pick_step(void *user_ptr, uint16_t step_ms) {
	(void) user_ptr;
	(void) step_ms;
	uv_uiobject_ret_e ret = UIOBJECT_RETURN_ALIVE;
	wheel_step(&pick_win, JW_ROW_H + JW_GAP);
	uv_uitextedit_set_focused(&pick_filter, true);
	if (uv_uibutton_clicked(&pick_cancel)) {
		ret = UIOBJECT_RETURN_KILLED;
	}
	else if (strcmp(pick_filter_buf, pick_filter_applied) != 0) {
		pick_build_list();
	}
	else {
		for (uint16_t i = 0; i < pick_count; i++) {
			if (uv_uibutton_clicked(&pick_btns[i])) {
				pick_result = pick_index[i];
				ret = UIOBJECT_RETURN_KILLED;
				break;
			}
		}
	}
	return ret;
}


/// @brief: Shows the values *d* sends in its TXPDOs and lets the user pick
/// one. Returns its catalog index, or -1 when the user cancelled.
static int32_t pick_exec(jw_dev_st *d, uint8_t slot) {
	pick_dev = d;
	pick_result = -1;
	pick_filter_buf[0] = '\0';
	pick_filter_applied[0] = '\0';

	uv_uidialog_init(&pick_dialog, pick_dialog_buf, win_style);
	uv_uidialog_set_stepcallback(&pick_dialog, &pick_step, NULL);
	int16_t w = uv_uibb(&pick_dialog)->width;
	int16_t h = uv_uibb(&pick_dialog)->height;

	snprintf(pick_title_str, sizeof(pick_title_str), "%s %s: select an input "
			"or output of %s", is_axis_row(slot) ? "Axis" : "Button",
			rows[slot].name_str, d->str);
	uv_uilabel_init(&pick_title, &UI_TITLE_FONT, ALIGN_CENTER_LEFT,
			win_style->text_color, pick_title_str);
	uv_uidialog_addxy(&pick_dialog, &pick_title, JW_MARGIN, JW_MARGIN,
			w - 2 * JW_MARGIN, JW_TITLE_H);

	int16_t y = JW_MARGIN + JW_TITLE_H + JW_MARGIN;
	uv_uilabel_init(&pick_filter_label, win_style->font, ALIGN_CENTER_LEFT,
			win_style->text_color, "Filter:");
	uv_uidialog_addxy(&pick_dialog, &pick_filter_label, JW_MARGIN, y,
			80, JW_BTN_H);
	uv_uitextedit_init(&pick_filter, pick_filter_buf, sizeof(pick_filter_buf),
			UITEXTEDIT_FLAG_ONELINE | UITEXTEDIT_FLAG_CMDLINE, win_style);
	uv_uitextedit_set_align(&pick_filter, ALIGN_CENTER_LEFT);
	uv_uidialog_addxy(&pick_dialog, &pick_filter, JW_MARGIN + 80, y,
			400, JW_BTN_H);
	uv_uitextedit_set_focused(&pick_filter, true);
	uv_uiwindow_set_focus(&pick_filter);
	uv_uilabel_init(&pick_info, win_style->font, ALIGN_CENTER_LEFT,
			JW_DIM_COLOR, pick_info_str);
	uv_uidialog_addxy(&pick_dialog, &pick_info, JW_MARGIN + 80 + 400 + JW_MARGIN,
			y, w - (2 * JW_MARGIN + 80 + 400 + JW_MARGIN), JW_BTN_H);
	y += JW_BTN_H + JW_MARGIN;

	int16_t btn_y = h - JW_BTN_H - JW_MARGIN;
	uv_uiwindow_init(&pick_win, pick_buf, win_style);
	uv_uiwindow_set_transparent(&pick_win, true);
	uv_uidialog_addxy(&pick_dialog, &pick_win, JW_MARGIN, y,
			w - 2 * JW_MARGIN, btn_y - JW_MARGIN - y);

	uv_uibutton_init(&pick_cancel, "Cancel", win_style);
	uv_uidialog_addxy(&pick_dialog, &pick_cancel, w - JW_BTN_W - JW_MARGIN,
			btn_y, JW_BTN_W, JW_BTN_H);

	pick_build_list();
	uv_uidialog_exec(&pick_dialog);
	return pick_result;
}


/// @brief: Shows a one-button message over the window
static void message(const char *msg) {
	uv_uiacceptdialog_st ad = { };
	char buf[512];
	snprintf(buf, sizeof(buf), "%s", msg);
	uv_uiacceptdialog_exec(&ad, buf, "OK", "OK", win_style);
}


// ---- the rows -------------------------------------------------------------


/// @brief: The device list entry for node id *nodeid*, added when the system
/// has no such device so that the binding is kept
static uint8_t dev_index_for(uint8_t nodeid) {
	uint8_t ret = 0;
	if (nodeid != 0) {
		for (uint8_t i = 1; i < dev_count; i++) {
			if (devs[i].nodeid == nodeid) {
				ret = i;
				break;
			}
		}
		if ((ret == 0) && (dev_count < JW_DEV_MAX)) {
			jw_dev_st *d = &devs[dev_count];
			memset(d, 0, sizeof(*d));
			d->nodeid = nodeid;
			snprintf(d->str, sizeof(d->str), "Node 0x%x (not in system)",
					(unsigned int) nodeid);
			dev_items[dev_count] = d->str;
			ret = dev_count;
			dev_count++;
			dev_items[dev_count] = NULL;
		}
	}
	return ret;
}


static void build_dev_list(void) {
	dev_count = 0;
	memset(devs, 0, sizeof(devs));
	snprintf(devs[0].str, sizeof(devs[0].str), "Not used");
	dev_items[0] = devs[0].str;
	dev_count = 1;
	for (uint8_t i = 0; i < system_get_dev_count(sys); i++) {
		device_st *d = system_get_dev(sys, i);
		// a device needs its package: that is where its TXPDOs are told
		if ((d->nodeid != 0) && (strlen(d->filepath) != 0) && !d->thirdparty) {
			jw_dev_st *e = &devs[dev_count];
			e->nodeid = d->nodeid;
			e->device = d;
			const char *dname = (strlen(d->devname) != 0) ? d->devname : d->name;
			snprintf(e->str, sizeof(e->str), "%.80s (0x%x)", dname,
					(unsigned int) d->nodeid);
			dev_items[dev_count] = e->str;
			dev_count++;
		}
	}
	dev_items[dev_count] = NULL;
	for (uint8_t i = 0; i < JOYSTICK_SLOT_COUNT; i++) {
		rows[i].dev_index = dev_index_for(edit.slots[i].nodeid);
	}
}


/// @brief: Shows slot *i* of the edited configuration in its row's widgets
static void row_show(uint8_t i) {
	jw_row_st *r = &rows[i];
	joystick_slot_st *s = &edit.slots[i];
	bool used = joystick_slot_is_used(s);
	if (used) {
		snprintf(r->obj_str, sizeof(r->obj_str), "%s", s->object);
	}
	else if (r->dev_index != 0) {
		snprintf(r->obj_str, sizeof(r->obj_str), "Select input / output...");
	}
	else {
		r->obj_str[0] = '\0';
	}
	uv_uibutton_set_text(&r->obj, r->obj_str);
	uv_ui_set_enabled(&r->obj, r->dev_index != 0);
	uv_uidigitedit_set_value(&r->min, is_axis_row(i) ? s->min : s->threshold);
	uv_ui_set_enabled(&r->min, used);
	if (is_axis_row(i)) {
		uv_uidigitedit_set_value(&r->max, s->max);
		uv_ui_set_enabled(&r->max, used);
	}
	uv_uicheckbox_set_state(&r->invert, s->invert);
	uv_ui_set_enabled(&r->invert, used);
	uv_ui_refresh(&list_win);
}


/// @brief: The live value of row *i*: what the running driver gets for the
/// slot, when the row still binds the same value the driver runs with
static void row_live(uint8_t i) {
	jw_row_st *r = &rows[i];
	const joystick_slot_st *s = &edit.slots[i];
	const joystick_slot_st *run = &joystick_get_conf()->slots[i];
	char str[sizeof(r->live_str)];
	int32_t raw = 0;
	int32_t out = 0;
	if (!joystick_slot_is_used(s)) {
		str[0] = '\0';
	}
	else if ((run->nodeid != s->nodeid) || (run->cobid != s->cobid) ||
			(run->offset != s->offset) || (run->size != s->size) ||
			!joystick_get_live(i, &raw, &out)) {
		snprintf(str, sizeof(str), "-");
	}
	else if (is_axis_row(i)) {
		snprintf(str, sizeof(str), "%d  ->  %d %%", (int) raw,
				(int) ((out * 100) / JOYSTICK_AXIS_MAX));
	}
	else {
		snprintf(str, sizeof(str), "%d  ->  %s", (int) raw, out ? "ON" : "off");
	}
	if (strcmp(str, r->live_str) != 0) {
		snprintf(r->live_str, sizeof(r->live_str), "%s", str);
		uv_ui_refresh(&r->live);
	}
}


/// @brief: The user picked device list entry *index* for row *i*: the bound
/// value belonged to the previous device, so the row starts over
static void row_set_dev(uint8_t i, uint8_t index) {
	rows[i].dev_index = index;
	memset(&edit.slots[i], 0, sizeof(edit.slots[i]));
	row_show(i);
}


static void row_pick(uint8_t i) {
	jw_dev_st *d = &devs[rows[i].dev_index];
	if (!d->objs_tried && (d->device != NULL)) {
		d->objs_tried = true;
		d->objs = joystick_catalog_build(d->device, &d->obj_count);
	}
	if (d->device == NULL) {
		message("This node is not one of the system's devices, so what it "
				"sends cannot be listed. Load the system file holding it, or "
				"pick another device.");
	}
	else if (d->obj_count == 0) {
		message("This device sends no values in its TXPDOs, or its "
				"configuration package could not be read.");
	}
	else {
		int32_t k = pick_exec(d, i);
		if (k >= 0) {
			joystick_slot_set_obj(&edit.slots[i], i, d->device, &d->objs[k]);
		}
	}
	row_show(i);
	uv_ui_refresh(&dialog);
}


static uv_uiobject_ret_e jw_step(void *user_ptr, uint16_t step_ms) {
	(void) user_ptr;
	(void) step_ms;
	uv_uiobject_ret_e ret = UIOBJECT_RETURN_ALIVE;

	wheel_step(&list_win, JW_ROW_H + JW_GAP);

	if (uv_uibutton_clicked(&cancel_btn)) {
		ret = UIOBJECT_RETURN_KILLED;
	}
	else if (uv_uibutton_clicked(&save_btn)) {
		saved = true;
		ret = UIOBJECT_RETURN_KILLED;
	}
	else {
		for (uint8_t i = 0; i < JOYSTICK_SLOT_COUNT; i++) {
			jw_row_st *r = &rows[i];
			joystick_slot_st *s = &edit.slots[i];
			if (uv_uilistbutton_clicked(&r->dev)) {
				row_set_dev(i, uv_uilistbutton_get_current_index(&r->dev));
			}
			else if (uv_uibutton_clicked(&r->obj)) {
				row_pick(i);
			}
			else if (uv_uidigitedit_value_changed(&r->min)) {
				if (is_axis_row(i)) {
					s->min = (int32_t) uv_uidigitedit_get_value(&r->min);
				}
				else {
					s->threshold = (int32_t) uv_uidigitedit_get_value(&r->min);
				}
			}
			else if (is_axis_row(i) && uv_uidigitedit_value_changed(&r->max)) {
				s->max = (int32_t) uv_uidigitedit_get_value(&r->max);
			}
			else if (uv_uicheckbox_clicked(&r->invert)) {
				s->invert = uv_uicheckbox_get_state(&r->invert);
			}
			else {
			}
			row_live(i);
		}
		snprintf(status_str, sizeof(status_str), "%s", joystick_get_status());
	}
	return ret;
}


/// @brief: Adds row *i* to the list window at *y*
static void add_row(uint8_t i, int16_t y, int16_t w) {
	jw_row_st *r = &rows[i];
	const uv_uistyle_st *style = win_style;
	int16_t x = 0;

	if (is_axis_row(i)) {
		snprintf(r->name_str, sizeof(r->name_str), "%s", JOYSTICK_AXIS_NAMES[i]);
	}
	else {
		snprintf(r->name_str, sizeof(r->name_str), "%u",
				(unsigned int) (i - JOYSTICK_AXIS_COUNT + 1));
	}
	uv_uilabel_init(&r->name, style->font, ALIGN_CENTER_LEFT, style->text_color,
			r->name_str);
	uv_uiwindow_addxy(&list_win, &r->name, x, y, JW_COL_SLOT_W, JW_ROW_H);
	x += JW_COL_SLOT_W + JW_GAP;

	uv_uilistbutton_init(&r->dev, dev_items, dev_count, r->dev_index, style);
	uv_uiwindow_addxy(&list_win, &r->dev, x, y, JW_COL_DEV_W, JW_ROW_H);
	x += JW_COL_DEV_W + JW_GAP;

	uv_uibutton_init(&r->obj, r->obj_str, style);
	uv_uiwindow_addxy(&list_win, &r->obj, x, y, JW_COL_OBJ_W, JW_ROW_H);
	x += JW_COL_OBJ_W + JW_GAP;

	uv_uidigitedit_init(&r->min, 0, style);
	uv_uidigitedit_set_limits(&r->min, INT32_MIN + 1, INT32_MAX);
	uv_uidigitedit_set_numpad_title(&r->min, is_axis_row(i) ?
			"The device value at the axis' minimum" :
			"The device value from which on the button is pressed");
	uv_uiwindow_addxy(&list_win, &r->min, x, y, JW_COL_MIN_W, JW_ROW_H);
	x += JW_COL_MIN_W + JW_GAP;

	if (is_axis_row(i)) {
		uv_uidigitedit_init(&r->max, 0, style);
		uv_uidigitedit_set_limits(&r->max, INT32_MIN + 1, INT32_MAX);
		uv_uidigitedit_set_numpad_title(&r->max,
				"The device value at the axis' maximum");
		uv_uiwindow_addxy(&list_win, &r->max, x, y, JW_COL_MAX_W, JW_ROW_H);
	}
	x += JW_COL_MAX_W + JW_GAP;

	uv_uicheckbox_init(&r->invert, false, "Invert", style);
	uv_uiwindow_addxy(&list_win, &r->invert, x, y, JW_COL_INV_W, JW_ROW_H);
	x += JW_COL_INV_W + JW_GAP;

	r->live_str[0] = '\0';
	uv_uilabel_init(&r->live, style->font, ALIGN_CENTER_LEFT, style->text_color,
			r->live_str);
	uv_uiwindow_addxy(&list_win, &r->live, x, y, w - x, JW_ROW_H);

	row_show(i);
}


bool joystick_win_exec(system_st *system, const uv_uistyle_st *style) {
	sys = system;
	win_style = style;
	saved = false;
	edit = *joystick_get_conf();
	build_dev_list();

	uv_uidialog_init(&dialog, dialog_buf, style);
	uv_uidialog_set_stepcallback(&dialog, &jw_step, NULL);
	int16_t w = uv_uibb(&dialog)->width;
	int16_t h = uv_uibb(&dialog)->height;

	uv_uilabel_init(&title_label, &UI_TITLE_FONT, ALIGN_CENTER_LEFT,
			style->text_color, "Joystick driver");
	uv_uidialog_addxy(&dialog, &title_label, JW_MARGIN, JW_MARGIN,
			300, JW_TITLE_H);
	snprintf(status_str, sizeof(status_str), "%s", joystick_get_status());
	uv_uilabel_init(&status_label, style->font, ALIGN_CENTER_RIGHT,
			JW_DIM_COLOR, status_str);
	uv_uidialog_addxy(&dialog, &status_label, 300 + JW_MARGIN, JW_MARGIN,
			w - 300 - 2 * JW_MARGIN, JW_TITLE_H);

	// column titles over the rows
	static const char *heads[7] = { "Axis", "CAN-bus device",
			"Input / output (TXPDO)", "Min / threshold", "Max", "", "Live value" };
	static const int16_t head_w[7] = { JW_COL_SLOT_W, JW_COL_DEV_W,
			JW_COL_OBJ_W, JW_COL_MIN_W, JW_COL_MAX_W, JW_COL_INV_W, 200 };
	int16_t y = JW_MARGIN + JW_TITLE_H + JW_MARGIN;
	int16_t x = JW_MARGIN + JW_LIST_PAD;
	for (uint8_t i = 0; i < 7; i++) {
		uv_uilabel_init(&head_labels[i], style->font, ALIGN_CENTER_LEFT,
				JW_DIM_COLOR, (char*) heads[i]);
		uv_uidialog_addxy(&dialog, &head_labels[i], x, y, head_w[i], JW_HEAD_H);
		x += head_w[i] + JW_GAP;
	}
	y += JW_HEAD_H + JW_GAP;

	int16_t btn_y = h - JW_BTN_H - JW_MARGIN;
	int16_t list_w = w - 2 * JW_MARGIN;
	uv_uiwindow_init(&list_win, list_buf, style);
	uv_uiwindow_set_transparent(&list_win, true);
	uv_uidialog_addxy(&dialog, &list_win, JW_MARGIN, y, list_w,
			btn_y - JW_MARGIN - y);

	// the axes first, then the buttons, each under a title of its own
	int16_t inner_w = list_w - CONFIG_UI_WINDOW_SCROLLBAR_WIDTH - JW_GAP;
	int16_t ry = 0;
	uv_uilabel_init(&section_labels[0], &UI_TITLE_FONT, ALIGN_CENTER_LEFT,
			style->text_color, "Axes");
	uv_uiwindow_addxy(&list_win, &section_labels[0], 0, ry, inner_w,
			JW_SECTION_H);
	ry += JW_SECTION_H;
	for (uint8_t i = 0; i < JOYSTICK_SLOT_COUNT; i++) {
		if (i == JOYSTICK_AXIS_COUNT) {
			ry += JW_MARGIN;
			uv_uilabel_init(&section_labels[1], &UI_TITLE_FONT, ALIGN_CENTER_LEFT,
					style->text_color, "Buttons");
			uv_uiwindow_addxy(&list_win, &section_labels[1], 0, ry, inner_w,
					JW_SECTION_H);
			ry += JW_SECTION_H;
		}
		add_row(i, ry, inner_w);
		ry += JW_ROW_H + JW_GAP;
	}
	uv_uiwindow_set_contentbb(&list_win, inner_w, ry);

	uv_uibutton_init(&cancel_btn, "Cancel", style);
	uv_uidialog_addxy(&dialog, &cancel_btn,
			w - 2 * JW_BTN_W - 2 * JW_MARGIN, btn_y, JW_BTN_W, JW_BTN_H);
	uv_uibutton_init(&save_btn, "Save", style);
	uv_uidialog_addxy(&dialog, &save_btn, w - JW_BTN_W - JW_MARGIN, btn_y,
			JW_BTN_W, JW_BTN_H);

	uv_uidialog_exec(&dialog);

	for (uint8_t i = 0; i < dev_count; i++) {
		free(devs[i].objs);
		devs[i].objs = NULL;
	}

	if (saved) {
		joystick_set_conf(&edit);
		if (!system_is_sysfile_loaded(sys)) {
			message("No system file is loaded, so the joystick configuration "
					"is not saved in one yet. The driver uses it now, and it goes "
					"into the system file the next time one is saved with \"Save "
					"system configuration to file\".");
		}
		else if (!joystick_save_to_system(sys)) {
			message("The joystick configuration is in use, but it could not be "
					"saved into the system file. The log tells why.");
		}
		else {
		}
	}
	return saved;
}


#endif
