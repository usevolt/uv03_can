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


#include "ui/uvui.h"

#if CONFIG_UI

#include <uv_ui.h>
#include <uv_rtos.h>
#include <unistd.h>
#include <string.h>
#include <limits.h>
#include "main.h"
#include "system.h"
#include "find.h"
#include "logcap.h"
#include "ui/devicetab.h"


// Maximum number of tabs: "System" + one per device + "Add device".
#define UVUI_TAB_MAX			(1 + SYSTEM_DEV_MAX_COUNT + 1)
// Maximum number of objects the tab content area can hold at once.
#define TABWINDOW_OBJ_MAX		16
// UI step interval in milliseconds.
#define STEP_MS					20
// Height of the one-line log strip shown below the tab window.
#define LOG_LINE_H				26
// Horizontal margin for the log widgets.
#define MARGIN_X				8


typedef struct {
	bool terminate;

	uv_uidisplay_st display;
	uv_uiobject_st *display_buffer[8];

	uv_uitabwindow_st tabwindow;
	uv_uiobject_st *tabwindow_buffer[TABWINDOW_OBJ_MAX];

	// tab name strings, and the array of pointers handed to the tab window
	// (the tab window uses UITABWINDOW_LIST_OF_POINTERS, i.e. a char* array).
	char tab_name_buffer[UVUI_TAB_MAX][128];
	char *tab_names[UVUI_TAB_MAX];
	// per-tab names as actually drawn: the full names, or shortened with a
	// trailing "..." when the tabs would not all fit (see compute_tab_display())
	char tab_display_buffer[UVUI_TAB_MAX][128];
	uint16_t tab_count;

	// index of the "Add device" tab, or -1 when the system is full
	int16_t add_tab_index;

	// log view: an opaque window pinned to the bottom of the screen, always
	// visible. Minimized it shows a short titled frame with the newest log line;
	// expanded it animates up to fill the screen with the scrollable history. The
	// text buffer is large so a full screen of (colour-coded) lines fits at once.
	uv_uiwindow_st log_window;
	uv_uiobject_st *log_window_buf[4];
	uv_uiframewindow_st log_frame;
	uv_uiobject_st *log_frame_buf[4];
	char log_title_str[128];
	uv_uilabel_st log_full;
	char log_full_str[40960];
	uv_uibutton_st log_close;
	// height of the window in its minimized state (frame title + one log line)
	int16_t log_collapsed_h;
	// how many lines the log view is scrolled back from the newest line (0 = the
	// newest line is at the bottom). The wheel adjusts this to page through the
	// captured history; the view always shows one screenful, anchored here.
	int log_scroll_lines;

	// scalar transition animating the log window's top edge (and a flag for while
	// it is animating); log_expanded is the target state
	uv_uiscalartransition_st log_trans;
	int16_t log_anim_top;
	bool log_animating;
	bool log_expanded;
} uvui_st;


static uvui_st _uvui;
#define this (&_uvui)


// Status-dot colours for the device tabs, by CAN-bus state: grey when offline,
// light yellow on boot-up, strong yellow in pre-operational and green when
// operational. A hollow (ring) dot additionally marks a device with no
// configuration file assigned.
#define DOT_COLOR_OFFLINE		C(0xFF888888)
#define DOT_COLOR_BOOTUP		C(0xFFF7E98A)
#define DOT_COLOR_PREOP			C(0xFFF2C200)
#define DOT_COLOR_OP			C(0xFF22B14C)


// Status-dot geometry drawn before a device tab name: the gap from the tab's
// left edge to the dot, the dot diameter, and the total space reserved (so the
// tab name is shifted right to clear the dot).
#define TAB_DOT_PAD			6
#define TAB_DOT_DIAMETER	12
#define TAB_DOT_SPACE		(TAB_DOT_PAD + TAB_DOT_DIAMETER + 2)


static void populate_tab_names(void);
static void rebuild_tabs(void);
static void show_active_tab(void);
static uv_uiobject_ret_e tabwindow_step(void *me, const uint16_t step_ms);
static color_t tab_dot_color(void *me, uint16_t tab_i);
static void tabwindow_draw(void *me, const uv_bounding_box_st *pbb);
static void tabwindow_touch(void *me, uv_touch_st *touch);
static int16_t tab_dot_space(uint16_t tab_i);
static void log_window_touch(void *me, uv_touch_st *touch);


// Set for one cycle when the minimized log frame is tapped (see
// log_window_touch), requesting the full log view to open.
static bool log_line_clicked;


void uvui_set_log_title(const char *title) {
	if (title == NULL) {
		title = "Log";
	}
	strncpy(this->log_title_str, title, sizeof(this->log_title_str) - 1);
	this->log_title_str[sizeof(this->log_title_str) - 1] = '\0';
	// the title buffer is the one the frame already points at; refresh to redraw
	uv_ui_refresh(&this->log_frame);
}


void uvui_reset_log_title(void) {
	uvui_set_log_title("Log");
}


/// @brief: Starts the animation that expands the minimized log frame into the
/// full-screen log view. No-op if it is already expanded.
static void log_expand(void) {
	if (!this->log_expanded) {
		this->log_expanded = true;
		this->log_animating = true;
		// open anchored at the newest line (the live tail), and show the Close
		// button (hidden while minimized)
		this->log_scroll_lines = 0;
		uv_uiobject_set_visible(&this->log_close, true);
		uv_uitransition_play(&this->log_trans);
	}
}


/// @brief: Starts the animation that collapses the full-screen log view back
/// into the minimized bottom frame. No-op if it is already collapsed.
static void log_collapse(void) {
	if (this->log_expanded) {
		this->log_expanded = false;
		this->log_animating = true;
		// the Close button has no place in the minimized frame
		uv_uiobject_set_visible(&this->log_close, false);
		uv_uitransition_reverseplay(&this->log_trans);
	}
}


void uvui_exec(void) {
	// start capturing stdout/stderr so the log strip can show it (also keeps
	// echoing to the terminal). Done first so UI start-up messages are captured.
	logcap_init();

	// Detach stdin: several CLI flows (saveparam / loadparam / db_check_can_if_
	// version) prompt for terminal input with fgetc/fgets(stdin) inside a
	// preemption-disabled critical section. Run from the GUI there is no one at
	// the terminal, so they would block the UI thread and freeze the whole app.
	// Pointing stdin at /dev/null makes those reads return EOF immediately, so
	// each prompt falls through to its default ("continue").
	if (freopen("/dev/null", "r", stdin) == NULL) {
		// non-fatal: if this fails the prompts simply behave as before
	}

	uv_ui_init();

	this->terminate = false;
	uv_uidisplay_init(&this->display, this->display_buffer, &uv_uistyles[0]);
	uv_uidisplay_set_touch_indicator(&this->display, true);

	// build the tab names from the current system state and initialize the
	// tab window to fill the whole display
	populate_tab_names();
	uv_uitabwindow_init(&this->tabwindow, this->tab_count, &uv_uistyles[0],
			this->tabwindow_buffer, this->tab_names);
	// the tab window fills the whole display; keep it transparent so the
	// display background colour shows through instead of the window's opaque
	// (near-black) fill
	uv_uiwindow_set_transparent(&this->tabwindow, true);
	// override the tab window's draw with our own so a status dot (green = online,
	// amber = offline) is drawn before each device tab name. Kept in the app so
	// the HAL tab window stays generic. uv_uitabwindow_clear() preserves whatever
	// draw callback is currently set, so this survives tab rebuilds.
	uv_uiobject_set_draw_callb(&this->tabwindow, &tabwindow_draw);
	// matching custom touch handler: the HAL one measures tab widths without the
	// status-dot space, so its hit regions drift from the drawn tabs. This one
	// mirrors tabwindow_draw's geometry (string width + dot space) so touches land.
	uv_uiobject_set_touch_callb(&this->tabwindow, &tabwindow_touch);
	int16_t disp_w = uv_uibb(&this->display)->w;
	int16_t disp_h = uv_uibb(&this->display)->h;

	// minimized log height: enough for the frame title row plus one line of text.
	// Two text lines worth of height plus the frame/window margins is ample.
	int16_t line_h = uv_ui_get_string_height("A", uv_uistyles[0].font);
	this->log_collapsed_h = (int16_t) (line_h * 2 + 4 * MARGIN_X);

	// the tab window fills the display above the minimized log frame
	uv_uidisplay_addxy(&this->display, &this->tabwindow,
			0, 0, disp_w, disp_h - this->log_collapsed_h);

	// log view: an opaque window pinned to the bottom of the screen, always
	// visible. It is added last so it draws on top of the tab window. Minimized it
	// occupies log_collapsed_h at the bottom; expanded it animates up to the top.
	uv_uiwindow_init(&this->log_window, this->log_window_buf, &uv_uistyles[0]);
	uv_uiwindow_set_transparent(&this->log_window, false);
	uv_uiobject_set_touch_callb(&this->log_window, &log_window_touch);
	uv_uidisplay_addxy(&this->display, &this->log_window,
			0, disp_h - this->log_collapsed_h, disp_w, this->log_collapsed_h);

	// titled frame filling the window. Its title (default "Log") is updated to
	// describe ongoing activity, e.g. system saving progress. Its height is kept in
	// sync with the window as it animates (see the loop below).
	strcpy(this->log_title_str, "Log");
	uv_uiframewindow_init(&this->log_frame, this->log_frame_buf, &uv_uistyles[0]);
	uv_uiframewindow_set_title(&this->log_frame, this->log_title_str);
	uv_uiwindow_addxy(&this->log_window, &this->log_frame,
			MARGIN_X, MARGIN_X, disp_w - 2 * MARGIN_X,
			this->log_collapsed_h - 2 * MARGIN_X);
	uv_bounding_box_st lfc = uv_uiframewindow_get_content_bb(&this->log_frame);

	this->log_full_str[0] = '\0';
	uv_uilabel_init(&this->log_full, uv_uistyles[0].font, ALIGN_TOP_LEFT,
			uv_uistyles[0].text_color, this->log_full_str);
	// Size the label to the frame's content area (not the whole display): the log
	// pages its own text, so only the visible lines are ever in the string, and a
	// label is not clipped to its bounding box. Keeping the height at the frame
	// size stops the window's content box (and thus a scroll bar) from being grown
	// to the full display height while the log is collapsed.
	uv_uiframewindow_addxy(&this->log_frame, &this->log_full,
			0, 0, lfc.w, lfc.h);

	// Close button, shown only while expanded
	uv_uibutton_init(&this->log_close, "Close", &uv_uistyles[0]);
	uv_uiwindow_addxy(&this->log_window, &this->log_close,
			disp_w - 120 - 2 * MARGIN_X, 2 * MARGIN_X, 120, LOG_LINE_H + 8);
	uv_uiobject_set_visible(&this->log_close, false);

	// the transition animates the log window's top edge between the minimized
	// frame (collapsed) and the top of the screen (expanded)
	this->log_expanded = false;
	this->log_animating = false;
	this->log_anim_top = disp_h - this->log_collapsed_h;
	uv_uiscalartransition_init(&this->log_trans, UITRANSITION_EASING_INOUT_QUAD,
			125, disp_h - this->log_collapsed_h, 0, &this->log_anim_top);

	// start listening for OPERATIONAL heartbeats so devices are flagged online
	// live while the UI is open, and pick up devices already seen
	find_start_monitor();
	find_update_device_states(&dev.system);

	// populate the initially active "System" tab
	show_active_tab();

	// previous device count, used to rebuild the tabs as a CAN search discovers
	// devices so their tabs appear live
	uint8_t prev_dev_count = system_get_dev_count(&dev.system);

	// log-text refresh tracking: only re-fetch/redraw the log when something that
	// affects it changed (new lines captured, scrolled, or the frame resized).
	// New lines are detected via the monotonic capture sequence rather than the
	// stored line count, which saturates once the ring buffer fills.
	unsigned long prev_seq = ULONG_MAX;
	int prev_scroll = -1;
	int prev_visible_lines = -1;

	while (true) {
		uv_uidisplay_step(&this->display, STEP_MS);

		if (this->terminate) {
			break;
		}

		// reflect any device that just came online: redraw the tab dots and
		// refresh the active tab so its state label/dot update too. While an async
		// device operation is in progress the state is frozen (the operation owns
		// the SDO client, and a flash resets the device).
		if (!devicetab_is_busy() && find_update_device_states(&dev.system)) {
			uv_ui_refresh(&this->tabwindow);
			show_active_tab();
		}

		// rebuild the tab set when the device count changes (e.g. as an async CAN
		// search discovers devices, or one is removed), so the tabs track the
		// device list live
		uint8_t dev_count_now = system_get_dev_count(&dev.system);
		if (dev_count_now != prev_dev_count) {
			prev_dev_count = dev_count_now;
			rebuild_tabs();
			// a shrink can drop the active index out of range; keep it valid
			if (uv_uitabwindow_get_tab(&this->tabwindow) >= this->tab_count) {
				uv_uitabwindow_set_tab(&this->tabwindow, this->tab_count - 1);
			}
			show_active_tab();
		}

		// start the expand / collapse animation when the strip or Close is clicked
		if (log_line_clicked) {
			log_line_clicked = false;
			log_expand();
		}
		else if (this->log_expanded &&
				uv_uibutton_clicked(&this->log_close)) {
			log_collapse();
		}
		else {
			// nothing to toggle this cycle
		}

		// drive the animation: resize the window AND its frame to the animated top
		// edge so the frame border and title track the window as it grows/shrinks
		if (this->log_animating) {
			uv_uitransition_step(&this->log_trans, &this->display, STEP_MS);
			if (!uv_uitransition_is_playing(&this->log_trans)) {
				// snap to the exact end position
				this->log_animating = false;
				this->log_anim_top = this->log_expanded ?
						0 : (disp_h - this->log_collapsed_h);
			}
			int16_t win_h = disp_h - this->log_anim_top;
			uv_uibb(&this->log_window)->y = this->log_anim_top;
			uv_uibb(&this->log_window)->height = win_h;
			uv_uibb(&this->log_frame)->height = win_h - 2 * MARGIN_X;
			uv_ui_refresh(&this->display);
		}

		// mouse wheel pages the expanded log view through the captured history
		int16_t scroll = uv_ui_get_scroll();
		if ((scroll != 0) && this->log_expanded) {
			// wheel up (positive) scrolls back to older lines, ~3 lines per notch
			this->log_scroll_lines += scroll * 3;
		}
		if (!this->log_expanded) {
			// minimized: always pinned to the newest line (the live tail)
			this->log_scroll_lines = 0;
		}

		// how many log lines fit the frame's current content height
		int16_t line_h = uv_ui_get_string_height("A", uv_uistyles[0].font);
		uv_bounding_box_st fc =
				uv_uiframewindow_get_content_bb(&this->log_frame);
		int visible_lines = fc.height / line_h;
		if (visible_lines < 1) {
			visible_lines = 1;
		}
		// clamp the scroll-back to the available history
		int line_count = logcap_get_line_count();
		int max_scroll = line_count - visible_lines;
		if (max_scroll < 0) {
			max_scroll = 0;
		}
		if (this->log_scroll_lines > max_scroll) {
			this->log_scroll_lines = max_scroll;
		}
		if (this->log_scroll_lines < 0) {
			this->log_scroll_lines = 0;
		}

		// re-fetch and redraw the log text only when it could have changed: new
		// lines arrived, the scroll position moved, or the frame resized (which
		// changes how many lines fit, e.g. while animating or at its final size)
		unsigned long seq = logcap_get_seq();
		if ((seq != prev_seq) ||
				(this->log_scroll_lines != prev_scroll) ||
				(visible_lines != prev_visible_lines)) {
			logcap_get_range(this->log_full_str, sizeof(this->log_full_str),
					this->log_scroll_lines, visible_lines);
			uv_ui_refresh(&this->log_full);
			prev_seq = seq;
			prev_scroll = this->log_scroll_lines;
			prev_visible_lines = visible_lines;
		}

		uv_rtos_task_delay(STEP_MS);
	}

	uv_ui_destroy();
}


/// @brief: Rebuilds the tab name array (and add_tab_index) from the current
/// system state. The tab window keeps a pointer to this->tab_names, so only the
/// contents and the tab count are refreshed here.
static void populate_tab_names(void) {
	system_st *sys = &dev.system;
	uint16_t i = 0;

	strcpy(this->tab_name_buffer[i], "System");
	i++;

	for (uint8_t d = 0; d < system_get_dev_count(sys); d++) {
		device_st *device = system_get_dev(sys, d);
		char *buf = this->tab_name_buffer[i];
		size_t bufsz = sizeof(this->tab_name_buffer[i]);
		if (device->nodeid != 0) {
			// tab name is "<name>_0x<nodeid>", but the device name may already end
			// with that suffix (devices loaded from a .uvsys keep the renamed
			// package name); strip any such trailing suffix so it is not doubled
			char suffix[16];
			snprintf(suffix, sizeof(suffix), "_0x%x",
					(unsigned int) device->nodeid);
			// base kept short enough that base + suffix always fits *buf*
			char base[96];
			strncpy(base, device->name, sizeof(base) - 1);
			base[sizeof(base) - 1] = '\0';
			size_t slen = strlen(suffix);
			size_t blen = strlen(base);
			while ((blen >= slen) && (strcmp(base + blen - slen, suffix) == 0)) {
				base[blen - slen] = '\0';
				blen -= slen;
			}
			snprintf(buf, bufsz, "%s%s", base, suffix);
		}
		else {
			strncpy(buf, device->name, bufsz - 1);
			buf[bufsz - 1] = '\0';
		}
		i++;
	}

	if (!system_is_full(sys)) {
		strcpy(this->tab_name_buffer[i], "New device (+)");
		this->add_tab_index = i;
		i++;
	}
	else {
		this->add_tab_index = -1;
	}

	for (uint16_t j = 0; j < i; j++) {
		this->tab_names[j] = this->tab_name_buffer[j];
	}
	this->tab_count = i;
}


/// @brief: Recomputes the tab set after the device count changed and pushes the
/// new tab count to the tab window.
static void rebuild_tabs(void) {
	populate_tab_names();
	uv_uitabwindow_set_tab_count(&this->tabwindow, this->tab_count);
	uv_ui_refresh(&this->tabwindow);
}


/// @brief: Clears the tab content area and fills it with the widgets for the
/// currently active tab.
static void show_active_tab(void) {
	uv_uitabwindow_clear(&this->tabwindow);

	int16_t tab = uv_uitabwindow_get_tab(&this->tabwindow);
	if (tab == 0) {
		devicetab_show_system(&this->tabwindow, &dev.system);
	}
	else if (tab == this->add_tab_index) {
		// transient: the "Add device" tab never shows content, selecting it
		// immediately creates a new device (handled in tabwindow_step)
	}
	else {
		devicetab_show_device(&this->tabwindow,
				system_get_dev(&dev.system, tab - 1));
	}

	// uv_uitabwindow_clear() drops the window step callback, so re-register it
	uv_uitabwindow_set_stepcallb(&this->tabwindow, &tabwindow_step, NULL);
	uv_ui_refresh(&this->tabwindow);
}


static uv_uiobject_ret_e tabwindow_step(void *me, const uint16_t step_ms) {
	if (uv_uitabwindow_tab_changed(&this->tabwindow)) {
		if (uv_uitabwindow_get_tab(&this->tabwindow) == this->add_tab_index &&
				this->add_tab_index >= 0) {
			// the new device takes the slot the "Add device" tab occupied
			int16_t new_tab = this->add_tab_index;
			if (system_add_empty_device(&dev.system) != NULL) {
				rebuild_tabs();
				uv_uitabwindow_set_tab(&this->tabwindow, new_tab);
			}
			show_active_tab();
		}
		else {
			show_active_tab();
		}
	}
	else if (devicetab_step()) {
		// the user picked a new device file or removed a device: refresh tab
		// names and content
		rebuild_tabs();
		// a removal can shrink the tab list below the active index; keep the
		// selection in range before re-showing
		if (uv_uitabwindow_get_tab(&this->tabwindow) >= this->tab_count) {
			uv_uitabwindow_set_tab(&this->tabwindow, this->tab_count - 1);
		}
		show_active_tab();
	}
	else {
		// nothing changed this cycle
	}

	return UIOBJECT_RETURN_ALIVE;
}


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


/// @brief: Returns true when *device* has no configuration file assigned, so its
/// status dot should be drawn hollow (a ring) rather than filled.
static bool device_dot_hollow(const device_st *device) {
	return device->filepath[0] == '\0';
}


/// @brief: Returns the status-dot colour for tab *tab_i*. Device tabs (indices
/// 1..dev_count) get a colour reflecting the device's CAN-bus state; the
/// "System" and "New device" tabs get no dot (returns 0).
static color_t tab_dot_color(void *me, uint16_t tab_i) {
	color_t ret = 0;
	if ((tab_i >= 1) && (tab_i <= system_get_dev_count(&dev.system))) {
		device_st *device = system_get_dev(&dev.system, tab_i - 1);
		ret = dot_color_for_state(device->state);
	}
	return ret;
}


/// @brief: Touch callback for the (always-visible) log window. While minimized, a
/// tap anywhere on the frame opens the full log view; when expanded, touches fall
/// through to the children (the Close button) and the window base class.
static void log_window_touch(void *me, uv_touch_st *touch) {
	if (!this->log_expanded && (touch->action == TOUCH_CLICKED)) {
		log_line_clicked = true;
		touch->action = TOUCH_NONE;
	}
	// propagate to children (Close button) and window scroll handling
	_uv_uiwindow_touch(me, touch);
}


/// @brief: Horizontal space reserved at the left of a tab for its status dot,
/// or 0 when the tab has none.
static int16_t tab_dot_space(uint16_t tab_i) {
	return (tab_dot_color(NULL, tab_i) != 0) ? TAB_DOT_SPACE : 0;
}


/// @brief: Draws tab *tab_i*'s status dot (if any) at the given tab origin. When
/// the device has no configuration file the dot is drawn hollow: a smaller dot in
/// the tab background colour *bg* is painted on top to leave only a ring.
static void draw_tab_dot(uint16_t tab_i, int16_t tab_x, int16_t tab_y, color_t bg) {
	color_t c = tab_dot_color(NULL, tab_i);
	if (c != 0) {
		int16_t cx = tab_x + TAB_DOT_PAD + TAB_DOT_DIAMETER / 2;
		int16_t cy = tab_y + CONFIG_UI_TABWINDOW_HEADER_HEIGHT / 2;
		uv_ui_draw_point(cx, cy, c, TAB_DOT_DIAMETER);
		if (device_dot_hollow(system_get_dev(&dev.system, tab_i - 1))) {
			uv_ui_draw_point(cx, cy, bg, TAB_DOT_DIAMETER / 2);
		}
	}
}


/// @brief: Full drawn width of tab *i* (its name plus padding and status-dot
/// space), clamped to the minimum header width. Uses *name* for measurement.
static int16_t tab_width_of(uv_uitabwindow_st *tabwin, int16_t i, const char *name) {
	int16_t dot_w = tab_dot_space((uint16_t) i);
	int16_t w = uv_ui_get_string_width((char*) name, tabwin->font) + 10 + dot_w;
	if (w < CONFIG_UI_TABWINDOW_HEADER_MIN_WIDTH) {
		w = CONFIG_UI_TABWINDOW_HEADER_MIN_WIDTH;
	}
	return w;
}


/// @brief: Copies *name* into *out*, shortened with a trailing "..." so it fits
/// within *max_w* pixels. An empty/zero budget yields just "...".
static void shorten_to_width(const char *name, int16_t max_w, uv_font_st *font,
		char *out, size_t outlen) {
	if (uv_ui_get_string_width((char*) name, font) <= max_w) {
		strncpy(out, name, outlen - 1);
		out[outlen - 1] = '\0';
		return;
	}
	int16_t ell_w = uv_ui_get_string_width("...", font);
	char tmp[128] = { };
	size_t fit = 0;
	for (size_t k = 0; (k < strlen(name)) && (k < sizeof(tmp) - 1); k++) {
		tmp[k] = name[k];
		tmp[k + 1] = '\0';
		if ((uv_ui_get_string_width(tmp, font) + ell_w) > max_w) {
			break;
		}
		fit = k + 1;
	}
	if (fit > outlen - 4) {
		fit = outlen - 4;
	}
	memcpy(out, name, fit);
	out[fit] = '\0';
	strncat(out, "...", outlen - strlen(out) - 1);
}


/// @brief: Fills tab_display_buffer with the names to draw: the full names when
/// they all fit, otherwise the full names for the System tab (0), the active tab
/// and the "Add device" tab, and shortened "start..." names for the rest, so the
/// whole row fits the window width. Called by both the draw and touch handlers so
/// their tab geometry stays identical.
static void compute_tab_display(uv_uitabwindow_st *tabwin, int16_t active) {
	uint16_t count = uv_uitabwindow_get_tab_count(tabwin);
	int16_t avail = uv_uibb(tabwin)->width;

	int16_t total = 0;
	for (int16_t i = 0; i < count; i++) {
		total += tab_width_of(tabwin, i, this->tab_names[i]);
	}

	if (total <= avail) {
		for (int16_t i = 0; i < count; i++) {
			strncpy(this->tab_display_buffer[i], this->tab_names[i],
					sizeof(this->tab_display_buffer[i]) - 1);
			this->tab_display_buffer[i][sizeof(this->tab_display_buffer[i]) - 1] =
					'\0';
		}
		return;
	}

	// overflow: keep System, the active tab and the add tab full; shrink the rest
	int16_t fixed = 0;
	int16_t truncatable = 0;
	for (int16_t i = 0; i < count; i++) {
		if ((i == 0) || (i == active) || (i == this->add_tab_index)) {
			fixed += tab_width_of(tabwin, i, this->tab_names[i]);
		}
		else {
			truncatable++;
		}
	}
	int16_t per = (truncatable > 0) ? ((avail - fixed) / truncatable) : 0;
	if (per < CONFIG_UI_TABWINDOW_HEADER_MIN_WIDTH) {
		per = CONFIG_UI_TABWINDOW_HEADER_MIN_WIDTH;
	}

	for (int16_t i = 0; i < count; i++) {
		if ((i == 0) || (i == active) || (i == this->add_tab_index)) {
			strncpy(this->tab_display_buffer[i], this->tab_names[i],
					sizeof(this->tab_display_buffer[i]) - 1);
			this->tab_display_buffer[i][sizeof(this->tab_display_buffer[i]) - 1] =
					'\0';
		}
		else {
			int16_t dot_w = tab_dot_space((uint16_t) i);
			int16_t text_budget = per - 10 - dot_w;
			shorten_to_width(this->tab_names[i], text_budget, tabwin->font,
					this->tab_display_buffer[i],
					sizeof(this->tab_display_buffer[i]));
		}
	}
}


/// @brief: Custom tab-window draw callback. Mirrors the HAL tab window's header
/// drawing (so the look is unchanged) but reserves room for and draws a status
/// dot before each device tab name. Uses only public HAL drawing primitives, so
/// the HAL tab window itself stays generic.
static void tabwindow_draw(void *me, const uv_bounding_box_st *pbb) {
	uv_uitabwindow_st *tabwin = me;
	color_t bg = ((uv_uiwindow_st*) me)->bg_c;
	uv_font_st *font = tabwin->font;
	color_t text_c = tabwin->text_c;
	uint16_t count = uv_uitabwindow_get_tab_count(me);
	uint16_t active = uv_uitabwindow_get_tab(me);

	// super draw function: window background
	uv_uiwindow_draw(me, pbb);

	// compute the (possibly shortened) names that actually fit the header row
	compute_tab_display(me, active);

	int16_t thisx = uv_ui_get_xglobal(me);
	int16_t x = thisx;
	int16_t y = uv_ui_get_yglobal(me);
	int16_t active_tab_x = 0;
	int16_t active_tab_w = 0;

	for (int16_t i = 0; i < count; i++) {
		char *name = this->tab_display_buffer[i];
		int16_t dot_w = tab_dot_space((uint16_t) i);
		int16_t tab_w = tab_width_of(me, i, name);
		if (active != i) {
			uv_ui_draw_shadowrrect(x, y, tab_w, CONFIG_UI_TABWINDOW_HEADER_HEIGHT,
					CONFIG_UI_RADIUS, bg,
					uv_uic_brighten(bg, 80), uv_uic_brighten(bg, -80));
			draw_tab_dot((uint16_t) i, x, y, bg);
			uv_ui_draw_string(name, font, x + 4 + dot_w,
					y + CONFIG_UI_TABWINDOW_HEADER_HEIGHT / 2, ALIGN_CENTER_LEFT,
					text_c);
		}
		else {
			active_tab_x = x;
			active_tab_w = tab_w;
		}
		x += tab_w;
	}
	// horizontal separator line under the tabs
	uv_ui_draw_line(thisx, y + CONFIG_UI_TABWINDOW_HEADER_HEIGHT - 1,
			thisx + uv_uibb(me)->width, y + CONFIG_UI_TABWINDOW_HEADER_HEIGHT - 1, 1,
			C(0xFFFFFFFF));
	// active tab, drawn on top
	uv_ui_draw_shadowrrect(active_tab_x, y, active_tab_w,
			CONFIG_UI_TABWINDOW_HEADER_HEIGHT, CONFIG_UI_RADIUS,
			uv_uic_brighten(bg, 20), C(0xFFFFFFFF), C(0xFFFFFFFF));
	// the active tab background is brightened, so the hollow inner must match it
	draw_tab_dot(active, active_tab_x, y, uv_uic_brighten(bg, 20));
	uv_ui_draw_string(this->tab_display_buffer[active], font,
			active_tab_x + 5 + tab_dot_space(active),
			y + CONFIG_UI_TABWINDOW_HEADER_HEIGHT / 2, ALIGN_CENTER_LEFT, text_c);

	_uv_uiwindow_draw_children(me, pbb);

	// scroll bars on top of the children, when the tab content overflows
	uv_uiwindow_draw_scrollbars(me, pbb);
}


/// @brief: Custom tab-window touch handler. The HAL's own handler measures each
/// tab as string-width + 10, but tabwindow_draw also reserves the status-dot
/// space, so the HAL hit regions drift left of the drawn tabs (more so for each
/// following tab). This mirrors the draw geometry exactly so a tap selects the
/// tab under the finger, then defers child/scroll handling to the window base.
static void tabwindow_touch(void *me, uv_touch_st *touch) {
	uv_uitabwindow_st *tabwin = me;
	if ((touch->action == TOUCH_CLICKED) &&
			(touch->y <= CONFIG_UI_TABWINDOW_HEADER_HEIGHT)) {
		// match the draw geometry exactly, including any name shortening
		compute_tab_display(tabwin, uv_uitabwindow_get_tab(tabwin));
		int16_t total_w = 0;
		for (int16_t i = 0; i < tabwin->tab_count; i++) {
			int16_t tab_w = tab_width_of(tabwin, i, this->tab_display_buffer[i]);
			if (touch->x < total_w + tab_w) {
				tabwin->active_tab = i;
				tabwin->tab_changed = true;
				uv_ui_refresh(tabwin);
				touch->action = TOUCH_NONE;
				break;
			}
			total_w += tab_w;
		}
	}
	// defer child propagation and scroll handling to the window base class
	_uv_uiwindow_touch(tabwin, touch);
}


#endif
