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


#include "ui/devsel_win.h"

#if CONFIG_UI

#include "ui/uv_uidialog.h"
#include "ui/uv_uicheckbox.h"
#include <stdio.h>
#include <string.h>


#define DSW_MARGIN		10
#define DSW_TITLE_H		30
#define DSW_INFO_H		26
#define DSW_BTN_H		44
#define DSW_BTN_W		150
// height of one device row
#define DSW_ROW_H		44

// colour of the greyed-out reason text of a device that cannot be selected
#define DSW_DIM_COLOR	C(0xFF909090)


// The dialog and its persistent widgets. Kept file-scope (static) so they outlive
// the modal's own step loop; the window is one-at-a-time so a single set suffices.
static uv_uidialog_st dialog;
static uv_uiobject_st *dialog_buf[8];
static uv_uilabel_st title_label;
static char title_str[160];
static uv_uilabel_st info_label;
static uv_uibutton_st all_btn;
static uv_uibutton_st none_btn;
static uv_uibutton_st cancel_btn;
static uv_uibutton_st accept_btn;
static char accept_str[64];

// the device rows live in their own window so the list scrolls when the system
// holds more devices than fit on screen
static uv_uiwindow_st list_win;
static uv_uiobject_st *list_buf[2 * SYSTEM_DEV_MAX_COUNT];
static uv_uicheckbox_st dev_boxes[SYSTEM_DEV_MAX_COUNT];
static char dev_strs[SYSTEM_DEV_MAX_COUNT][160];
static uv_uilabel_st dev_reasons[SYSTEM_DEV_MAX_COUNT];
static char dev_reason_strs[SYSTEM_DEV_MAX_COUNT][96];

// the listed devices and whether each of them may be selected at all
static device_st *listed[SYSTEM_DEV_MAX_COUNT];
static bool selectable[SYSTEM_DEV_MAX_COUNT];
static uint8_t listed_count;

// set true when the user closed the window with the accepting button (rather
// than Cancel), i.e. the checkbox states should be read back
static bool accepted;


/// @brief: Checks or unchecks every selectable device's checkbox at once.
static void set_all(bool state) {
	for (uint8_t i = 0; i < listed_count; i++) {
		if (selectable[i]) {
			uv_uicheckbox_set_state(&dev_boxes[i], state);
			uv_ui_refresh(&dev_boxes[i]);
		}
	}
}


static uv_uiobject_ret_e dsw_step(void *user_ptr, uint16_t step_ms) {
	(void) user_ptr;
	(void) step_ms;
	uv_uiobject_ret_e ret = UIOBJECT_RETURN_ALIVE;

	if (uv_uibutton_clicked(&accept_btn)) {
		accepted = true;
		ret = UIOBJECT_RETURN_KILLED;
	}
	else if (uv_uibutton_clicked(&cancel_btn)) {
		ret = UIOBJECT_RETURN_KILLED;
	}
	else if (uv_uibutton_clicked(&all_btn)) {
		set_all(true);
	}
	else if (uv_uibutton_clicked(&none_btn)) {
		set_all(false);
	}
	else {
		// no button clicked; the checkboxes track their own state
	}
	return ret;
}


uint8_t devsel_win_exec(system_st *system, const char *title,
		const char *accept_text, devsel_reason_t reason_callb,
		device_st **targets, uint8_t max_count, const uv_uistyle_st *style) {
	uint8_t ret = 0;
	accepted = false;
	listed_count = 0;

	uv_uidialog_init(&dialog, dialog_buf, style);
	uv_uidialog_set_stepcallback(&dialog, &dsw_step, NULL);
	int16_t w = uv_uibb(&dialog)->width;
	int16_t h = uv_uibb(&dialog)->height;

	snprintf(title_str, sizeof(title_str), "%s", title);
	uv_uilabel_init(&title_label, &UI_TITLE_FONT, ALIGN_CENTER_LEFT,
			style->text_color, title_str);
	uv_uidialog_addxy(&dialog, &title_label,
			DSW_MARGIN, DSW_MARGIN, w - 2 * DSW_MARGIN, DSW_TITLE_H);

	// string literal, so it lives for the program's lifetime
	uv_uilabel_init(&info_label, style->font, ALIGN_CENTER_LEFT,
			style->text_color,
			"Select the devices to include. Devices that cannot take part are "
			"greyed out.");
	int16_t info_y = DSW_MARGIN + DSW_TITLE_H;
	uv_uidialog_addxy(&dialog, &info_label,
			DSW_MARGIN, info_y, w - 2 * DSW_MARGIN, DSW_INFO_H);

	// the device list fills everything between the info line and the button row
	int16_t list_y = info_y + DSW_INFO_H + DSW_MARGIN;
	int16_t list_h = h - list_y - (DSW_BTN_H + 2 * DSW_MARGIN);
	int16_t list_w = w - 2 * DSW_MARGIN;
	uv_uiwindow_init(&list_win, list_buf, style);
	uv_uiwindow_set_transparent(&list_win, true);
	uv_uidialog_addxy(&dialog, &list_win, DSW_MARGIN, list_y, list_w, list_h);

	// leave room for the scroll bar, and split each row between the checkbox
	// (with the device name) and the right-aligned status text
	int16_t inner_w = list_w - CONFIG_UI_WINDOW_SCROLLBAR_WIDTH;
	int16_t reason_w = inner_w / 3;
	int16_t box_w = inner_w - reason_w - DSW_MARGIN;

	for (uint8_t i = 0; (i < system_get_dev_count(system)) &&
			(listed_count < SYSTEM_DEV_MAX_COUNT); i++) {
		device_st *d = system_get_dev(system, i);
		uint8_t r = listed_count;
		listed[r] = d;
		const char *reason = (reason_callb != NULL) ? reason_callb(d) : NULL;
		selectable[r] = (reason == NULL);

		const char *dname = (strlen(d->devname) > 0) ? d->devname : d->name;
		snprintf(dev_strs[r], sizeof(dev_strs[r]), "%s  (node 0x%x)",
				(strlen(dname) > 0) ? dname : "Unnamed device",
				(unsigned int) d->nodeid);
		// every selectable device starts checked: the user deselects the ones to
		// leave out
		int16_t row_y = (int16_t) r * DSW_ROW_H;
		uv_uicheckbox_init(&dev_boxes[r], selectable[r], dev_strs[r], style);
		uv_uiwindow_addxy(&list_win, &dev_boxes[r], 0, row_y, box_w, DSW_ROW_H);
		if (!selectable[r]) {
			uv_uiobject_disable(&dev_boxes[r]);
		}

		snprintf(dev_reason_strs[r], sizeof(dev_reason_strs[r]), "%s",
				(reason != NULL) ? reason : "");
		uv_uilabel_init(&dev_reasons[r], style->font, ALIGN_CENTER_RIGHT,
				DSW_DIM_COLOR, dev_reason_strs[r]);
		uv_uiwindow_addxy(&list_win, &dev_reasons[r],
				box_w + DSW_MARGIN, row_y, reason_w, DSW_ROW_H);

		listed_count++;
	}

	// scroll only when the rows do not fit
	int16_t content_h = (int16_t) listed_count * DSW_ROW_H;
	if (content_h < list_h) {
		content_h = list_h;
	}
	uv_uiwindow_set_contentbb(&list_win, inner_w, content_h);

	// button row: the bulk selection buttons on the left, cancel / accept on the
	// right
	int16_t btn_y = h - DSW_BTN_H - DSW_MARGIN;
	uv_uibutton_init(&all_btn, "Select all", style);
	uv_uidialog_addxy(&dialog, &all_btn,
			DSW_MARGIN, btn_y, DSW_BTN_W, DSW_BTN_H);
	uv_uibutton_init(&none_btn, "Select none", style);
	uv_uidialog_addxy(&dialog, &none_btn,
			DSW_MARGIN + DSW_BTN_W + DSW_MARGIN, btn_y, DSW_BTN_W, DSW_BTN_H);

	snprintf(accept_str, sizeof(accept_str), "%s", accept_text);
	uv_uibutton_init(&cancel_btn, "Cancel", style);
	uv_uidialog_addxy(&dialog, &cancel_btn,
			w - 2 * DSW_BTN_W - DSW_MARGIN - DSW_MARGIN, btn_y,
			DSW_BTN_W, DSW_BTN_H);
	uv_uibutton_init(&accept_btn, accept_str, style);
	uv_uidialog_addxy(&dialog, &accept_btn,
			w - DSW_BTN_W - DSW_MARGIN, btn_y, DSW_BTN_W, DSW_BTN_H);

	uv_uidialog_exec(&dialog);

	if (accepted) {
		for (uint8_t i = 0; (i < listed_count) && (ret < max_count); i++) {
			if (selectable[i] && uv_uicheckbox_get_state(&dev_boxes[i])) {
				targets[ret] = listed[i];
				ret++;
			}
		}
	}
	return ret;
}


#endif
