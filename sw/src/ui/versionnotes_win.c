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


#include "ui/versionnotes_win.h"

#if CONFIG_UI

#include "ui/uv_uidialog.h"
#include "selfupdate.h"
#include "versionnotes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define VNW_MARGIN		10
#define VNW_TITLE_H		30
#define VNW_INFO_H		26
#define VNW_BTN_H		44
#define VNW_BTN_W		150
// Width of the date column. One date in the style's font with room to spare, so
// the subjects beside it all start at the same place.
#define VNW_DATE_W		110
// The gap between two notes. One note is as tall as the lines it wraps onto, so
// without it the rows of a wrapped note and the next one would read as one
// block.
#define VNW_ROW_GAP		8
// One wheel notch scrolls the list by this much
#define VNW_SCROLL_STEP	40
// Colour of the date column: the dates are there to be glanced past, not read
#define VNW_DATE_COLOR	C(0xFF909090)
// Longest note text after wrapping, i.e. the subject plus the line breaks put
// into it. The generator caps a subject well below this.
#define VNW_TEXT_LEN	288


// The dialog and its permanent widgets. Kept file-scope (static) so they outlive
// the modal's own step loop; the window is one-at-a-time so a single set suffices.
static uv_uidialog_st dialog;
static uv_uiobject_st *dialog_buf[6];
static uv_uilabel_st title_label;
static char title_str[64];
static uv_uilabel_st info_label;
static char info_str[256];
static uv_uilabel_st empty_label;
static char empty_str[256];
static uv_uibutton_st close_btn;

// the notes live in their own window, so the list scrolls when there are more
// of them than fit on screen
static uv_uiwindow_st list_win;

// One note is two labels, the date and the subject. How many notes a build
// carries is not bounded by anything the UI knows - it is however many commits
// were made since the last release - so the rows are allocated here and freed
// once uv_uidialog_exec() has returned: the widgets are handed to the window by
// pointer and have to stay put until then.
static uv_uiobject_st **list_buf;
static uv_uilabel_st *date_labels;
static uv_uilabel_st *text_labels;
static char *text_strs;
// whether the list holds any rows: an empty build shows a single label instead,
// and the wheel then has nothing to move
static bool list_shown;


/// @brief: Breaks *src* into lines that fit *width* pixels, writing them into
/// *dst* separated by newlines, which a label draws as lines of its own.
///
/// Words are kept whole; one that is wider than the column all by itself is left
/// to overflow rather than cut in half, which no commit subject does.
///
/// @return: the number of lines written.
static uint16_t vnw_wrap(const char *src, ui_font_st *font, int16_t width,
		char *dst, uint16_t dst_len) {
	uint16_t lines = 1;
	uint16_t len = 0;
	// where the line being filled starts in *dst*
	uint16_t line_start = 0;
	uint16_t i = 0;

	dst[0] = '\0';
	while ((src[i] != '\0') && ((len + 1) < dst_len)) {
		if ((src[i] == ' ') || (src[i] == '\t')) {
			// a run of blanks between two words becomes the single space written
			// below, or the line break that replaces it
			i++;
		}
		else {
			// the word, and the space joining it to what is already on the line
			uint16_t word_start = len;
			if (len > line_start) {
				dst[len] = ' ';
				len++;
			}
			else {
			}
			while ((src[i] != '\0') && (src[i] != ' ') && (src[i] != '\t') &&
					((len + 1) < dst_len)) {
				dst[len] = src[i];
				len++;
				i++;
			}
			dst[len] = '\0';
			// measured only once the whole word is there: a word that no longer
			// fits moves down as a whole, and the space in front of it is what
			// becomes the break
			if ((word_start > line_start) &&
					(uv_ui_get_string_width(&dst[line_start], font) > width)) {
				dst[word_start] = '\n';
				line_start = word_start + 1;
				lines++;
			}
			else {
			}
		}
	}
	dst[len] = '\0';

	return lines;
}


/// @brief: Releases what the rows were built from. Called only after
/// uv_uidialog_exec() has returned - the window holds pointers to all of it.
static void vnw_free(void) {
	free(list_buf);
	free(date_labels);
	free(text_labels);
	free(text_strs);
	list_buf = NULL;
	date_labels = NULL;
	text_labels = NULL;
	text_strs = NULL;
	list_shown = false;
}


/// @brief: Allocates room for *count* notes, plus the one extra row the omitted
/// count is written on. Returns false when there is no memory for them, which
/// leaves the window showing the empty label instead of nothing at all.
static bool vnw_alloc(uint32_t count) {
	uint32_t rows = count + 1;
	list_buf = calloc(2 * rows, sizeof(uv_uiobject_st*));
	date_labels = calloc(rows, sizeof(uv_uilabel_st));
	text_labels = calloc(rows, sizeof(uv_uilabel_st));
	text_strs = calloc(rows, VNW_TEXT_LEN);
	bool ret = (list_buf != NULL) && (date_labels != NULL) &&
			(text_labels != NULL) && (text_strs != NULL);
	if (!ret) {
		vnw_free();
	}
	else {
	}
	return ret;
}


// The mouse wheel scrolls the list while the pointer is over it. A window
// scrolls itself by dragging only; see the Settings tab's parameter file list.
static void vnw_wheel_step(void) {
	if (list_shown) {
		int16_t x = 0;
		int16_t y = 0;
		// the position is reported whether or not a button is held down
		uv_ui_get_touch(&x, &y);
		int16_t gx = uv_ui_get_xglobal(&list_win);
		int16_t gy = uv_ui_get_yglobal(&list_win);
		uv_bounding_box_st *bb = uv_uibb(&list_win);
		if ((x >= gx) && (x < (gx + bb->width)) &&
				(y >= gy) && (y < (gy + bb->height))) {
			int16_t scroll = uv_ui_get_scroll();
			if (scroll != 0) {
				// positive (wheel up) moves the content down. content_move clamps
				// to the content box, so a list that fits stays put
				uv_uiwindow_content_move(&list_win, 0, scroll * VNW_SCROLL_STEP);
				uv_ui_refresh(&list_win);
			}
			else {
			}
		}
		else {
		}
	}
	else {
	}
}


static uv_uiobject_ret_e vnw_step(void *user_ptr, uint16_t step_ms) {
	(void) user_ptr;
	(void) step_ms;
	uv_uiobject_ret_e ret = UIOBJECT_RETURN_ALIVE;

	vnw_wheel_step();
	if (uv_uibutton_clicked(&close_btn)) {
		ret = UIOBJECT_RETURN_KILLED;
	}
	else {
	}
	return ret;
}


/// @brief: Builds the rows of the note list into *list_win*, and returns the
/// height they need.
static int16_t vnw_build_rows(int16_t inner_w, const uv_uistyle_st *style) {
	int16_t font_h = uv_ui_get_font_height(style->font);
	int16_t text_x = VNW_DATE_W + VNW_MARGIN;
	int16_t text_w = inner_w - text_x;
	int16_t y = 0;

	for (uint32_t i = 0; i < versionnotes_count(); i++) {
		const versionnote_st *note = versionnotes_get(i);
		char *text = &text_strs[i * VNW_TEXT_LEN];
		uint16_t lines = vnw_wrap(note->text, style->font, text_w,
				text, VNW_TEXT_LEN);
		int16_t row_h = (int16_t) lines * font_h;

		// the date is cast const away only to be drawn: a label never writes to
		// the string it was given
		uv_uilabel_init(&date_labels[i], style->font, ALIGN_TOP_LEFT,
				VNW_DATE_COLOR, (char*) note->date);
		uv_uiwindow_addxy(&list_win, &date_labels[i], 0, y, VNW_DATE_W, row_h);

		uv_uilabel_init(&text_labels[i], style->font, ALIGN_TOP_LEFT,
				style->text_color, text);
		uv_uiwindow_addxy(&list_win, &text_labels[i], text_x, y, text_w, row_h);

		y += row_h + VNW_ROW_GAP;
	}

	// the commits the generator left off the end of the list, said on a row of
	// its own so the list does not look as if it reached the published build
	if (versionnotes_omitted() != 0) {
		char *text = &text_strs[versionnotes_count() * VNW_TEXT_LEN];
		snprintf(text, VNW_TEXT_LEN, "...and %u earlier commits.",
				(unsigned int) versionnotes_omitted());
		uv_uilabel_init(&text_labels[versionnotes_count()], style->font,
				ALIGN_TOP_LEFT, VNW_DATE_COLOR, text);
		uv_uiwindow_addxy(&list_win, &text_labels[versionnotes_count()],
				text_x, y, text_w, font_h);
		y += font_h + VNW_ROW_GAP;
	}
	else {
	}

	return y;
}


void versionnotes_win_exec(const uv_uistyle_st *style) {
	list_shown = false;

	uv_uidialog_init(&dialog, dialog_buf, style);
	uv_uidialog_set_stepcallback(&dialog, &vnw_step, NULL);
	int16_t w = uv_uibb(&dialog)->width;
	int16_t h = uv_uibb(&dialog)->height;

	snprintf(title_str, sizeof(title_str), "Version notes");
	uv_uilabel_init(&title_label, &UI_TITLE_FONT, ALIGN_CENTER_LEFT,
			style->text_color, title_str);
	uv_uidialog_addxy(&dialog, &title_label,
			VNW_MARGIN, VNW_MARGIN, w - 2 * VNW_MARGIN, VNW_TITLE_H);

	// The line under the title says which uvcan this is and where the notes
	// start. A build that does not know what was last published (see
	// versionnotes.h) cannot name that build, so it does not pretend to, and one
	// with nothing to list leaves the whole explanation to the label below -
	// saying "what has changed since 304" over "nothing has changed since 304"
	// only made the window read twice.
	uint32_t count = versionnotes_count();
	if (count == 0) {
		snprintf(info_str, sizeof(info_str), "uvcan %s (build %u)",
				selfupdate_this_name(),
				(unsigned int) selfupdate_this_version());
	}
	else if (versionnotes_since_build() != 0) {
		snprintf(info_str, sizeof(info_str),
				"uvcan %s (build %u) - what has changed since %s (build %u), "
				"the build published before it.",
				selfupdate_this_name(),
				(unsigned int) selfupdate_this_version(),
				versionnotes_since_name(),
				(unsigned int) versionnotes_since_build());
	}
	else {
		snprintf(info_str, sizeof(info_str),
				"uvcan %s (build %u) - the newest changes it was built with.",
				selfupdate_this_name(),
				(unsigned int) selfupdate_this_version());
	}
	uv_uilabel_init(&info_label, style->font, ALIGN_CENTER_LEFT,
			style->text_color, info_str);
	int16_t info_y = VNW_MARGIN + VNW_TITLE_H;
	uv_uidialog_addxy(&dialog, &info_label,
			VNW_MARGIN, info_y, w - 2 * VNW_MARGIN, VNW_INFO_H);

	// the list fills everything between the info line and the button row
	int16_t list_y = info_y + VNW_INFO_H + VNW_MARGIN;
	int16_t list_h = h - list_y - (VNW_BTN_H + 2 * VNW_MARGIN);
	int16_t list_w = w - 2 * VNW_MARGIN;

	// the window is initialised only once its child array exists: it is handed
	// over by pointer and kept for as long as the window lives
	if ((count != 0) && vnw_alloc(count)) {
		uv_uiwindow_init(&list_win, list_buf, style);
		uv_uiwindow_set_transparent(&list_win, true);
		uv_uidialog_addxy(&dialog, &list_win,
				VNW_MARGIN, list_y, list_w, list_h);

		// leave room for the scroll bar, whether or not the rows need one: the
		// notes are laid out against the width they are wrapped to, so the bar
		// must not appear over the end of a line afterwards
		int16_t inner_w = list_w - CONFIG_UI_WINDOW_SCROLLBAR_WIDTH;
		int16_t content_h = vnw_build_rows(inner_w, style);
		if (content_h < list_h) {
			content_h = list_h;
		}
		else {
		}
		uv_uiwindow_set_contentbb(&list_win, inner_w, content_h);
		list_shown = true;
	}
	else {
		// Nothing to list: this is the published build itself, or there was no
		// memory for the rows. Either way the window says so rather than opening
		// empty.
		if (count == 0) {
			if (versionnotes_since_build() != 0) {
				snprintf(empty_str, sizeof(empty_str),
						"Nothing has changed since %s: this is the build that "
						"was published.", versionnotes_since_name());
			}
			else {
				snprintf(empty_str, sizeof(empty_str),
						"This build carries no version notes.");
			}
		}
		else {
			snprintf(empty_str, sizeof(empty_str),
					"The version notes could not be shown: out of memory.");
		}
		uv_uilabel_init(&empty_label, style->font, ALIGN_CENTER,
				style->text_color, empty_str);
		uv_uidialog_addxy(&dialog, &empty_label,
				VNW_MARGIN, list_y, list_w, list_h);
	}

	uv_uibutton_init(&close_btn, "Close", style);
	uv_uidialog_addxy(&dialog, &close_btn, w - VNW_BTN_W - VNW_MARGIN,
			h - VNW_BTN_H - VNW_MARGIN, VNW_BTN_W, VNW_BTN_H);

	uv_uidialog_exec(&dialog);

	// only now: everything above was handed to the window by pointer
	vnw_free();
}


#endif
