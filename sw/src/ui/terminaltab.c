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


#include "ui/terminaltab.h"

#if CONFIG_UI

#include <string.h>
#include <uv_terminal.h>
#include <uv_canopen.h>
#include <uv_rtos.h>
#include <uv_ui.h>
#include "ui/uv_uitabwindow.h"
#include "ui/uv_uiwindow.h"
#include "ui/uv_uilabel.h"
#include "ui/uv_uitextedit.h"
#include "main.h"
#include "find.h"


// Layout margins (px) inside the terminal sub-tab content area.
#define MARGIN				8
// Height of the command-line input field (px).
#define INPUT_H				40
// Maximum number of received characters kept for display; older text is dropped
// a chunk at a time when this fills.
#define RX_TEXT_MAX			8192
// How much of the oldest text is discarded once RX_TEXT_MAX is reached.
#define RX_TEXT_DROP		(RX_TEXT_MAX / 4)
// Per-chunk acknowledge timeout for the SDO reply protocol (ms).
#define WAIT_FOR_RESPONSE_MS	500
// Size of the command-line input buffer.
#define INPUT_BUF_LEN		256


// --- protocol / transfer state (shared between the UI, transmit and CAN tasks)

// Node id of the device the terminal currently talks to. Read by the CAN sniffer
// (HAL task) and the transmit task; written by terminaltab_build (UI task).
static volatile uint8_t term_nodeid;

// Characters received from the device, pushed by the CAN sniffer and drained
// into rx_text by terminaltab_step.
static char rx_ringbuf[2048];
static uv_ring_buffer_st rx_rb;

// Characters queued to send to the device, pushed by terminaltab_step on Enter
// and drained by the transmit task.
static char tx_ringbuf[1024];
static uv_ring_buffer_st tx_rb;

static uv_mutex_st mutex;
// Set by the CAN sniffer when the device acknowledges a transmitted chunk; the
// transmit task waits on it so chunks are sent one acknowledged step at a time.
static bool responded = true;

// One-time init guard for the ring buffers, mutex, sniffer and transmit task.
static bool inited;


// --- receive view widgets (persist across rebuilds; re-added on each build)

// Accumulated received text shown in the scrollable window.
static char rx_text[RX_TEXT_MAX];
static uv_uiwindow_st term_win;
static uv_uiobject_st *term_win_buf[2];
static uv_uilabel_st term_label;
static uv_uitextedit_st input;
static char input_buf[INPUT_BUF_LEN];
// True while the widgets are built into a parent window.
static bool built;
// True while the view follows the newest text (the live tail). Cleared when the
// user scrolls back with the mouse wheel and re-set once they scroll to the
// bottom again, so incoming text does not yank the view away from history.
static bool pinned_bottom = true;
// Receive-history lines advanced per mouse-wheel notch.
#define SCROLL_LINES_PER_NOTCH	3


// --- receive path ----------------------------------------------------------

/// @brief: CAN receive sniffer (runs in the HAL task via the find monitor hook).
/// Accepts the device's Usevolt SDO reply frames (0x580 + nodeid, command 0x42,
/// object 0x5FFF sub 0) and pushes the carried characters into rx_rb, mirroring
/// terminal.c's can_callb. Setting *responded* both delivers the characters and
/// acknowledges the previous transmit chunk.
static void can_sniff(void *ptr, uv_can_message_st *msg) {
	(void) ptr;
	if ((msg->type == CAN_STD) &&
			(msg->id == (CANOPEN_SDO_RESPONSE_ID + term_nodeid)) &&
			(msg->data_8bit[0] == 0x42) &&
			(msg->data_length > 4) &&
			(msg->data_8bit[1] == UV_TERMINAL_CAN_INDEX % 256) &&
			(msg->data_8bit[2] == UV_TERMINAL_CAN_INDEX / 256) &&
			(msg->data_8bit[3] == UV_TERMINAL_CAN_SUBINDEX)) {
		uv_mutex_lock(&mutex);
		for (int i = 4; i < msg->data_length; i++) {
			uv_ring_buffer_push(&rx_rb, &msg->data_8bit[i]);
		}
		responded = true;
		uv_mutex_unlock(&mutex);
	}
}


/// @brief: Appends one received character to the display buffer, dropping the
/// oldest chunk when the buffer is full. Carriage returns are dropped so the text
/// wraps on '\n' only.
static void rx_text_append(char c) {
	if (c == '\r') {
		return;
	}
	size_t len = strlen(rx_text);
	if (len + 2 > RX_TEXT_MAX) {
		memmove(rx_text, rx_text + RX_TEXT_DROP, len - RX_TEXT_DROP + 1);
		len -= RX_TEXT_DROP;
	}
	rx_text[len] = c;
	rx_text[len + 1] = '\0';
}


/// @brief: Resizes the receive label and window content to the current text and
/// scrolls to the newest line, so the window shows a live tail with a scroll bar
/// for the history.
static void rx_relayout(void) {
	if (!built) {
		return;
	}
	uv_font_st *font = &UI_MONO_FONT;
	int16_t win_w = uv_uibb(&term_win)->w;
	int16_t win_h = uv_uibb(&term_win)->h;
	int16_t inner_w = win_w - CONFIG_UI_WINDOW_SCROLLBAR_WIDTH;
	int16_t text_h = uv_ui_get_string_height(rx_text, font);
	int16_t content_h = (text_h > win_h) ? text_h : win_h;

	uv_uibb(&term_label)->w = inner_w;
	uv_uibb(&term_label)->h = content_h;
	uv_uiwindow_set_contentbb(&term_win, inner_w, content_h);

	int16_t maxy = content_h - win_h;
	if (maxy < 0) {
		maxy = 0;
	}
	if (pinned_bottom) {
		// follow the newest text
		uv_uiwindow_content_move_to(&term_win, 0, maxy);
	}
	else {
		// the user scrolled back: keep their position, only re-clamping it to the
		// (possibly grown) content height so it stays in range
		int16_t cur = -uv_uiwindow_get_contentbb(&term_win).y;
		if (cur > maxy) {
			cur = maxy;
		}
		if (cur < 0) {
			cur = 0;
		}
		uv_uiwindow_content_move_to(&term_win, 0, cur);
	}
	uv_ui_refresh(&term_win);
}


/// @brief: Scrolls the receive view by *notches* mouse-wheel steps (positive =
/// wheel up = back towards older text) and updates the pinned-to-bottom state so
/// the live tail resumes only once the user scrolls all the way down again.
static void rx_wheel_scroll(int16_t notches) {
	if (!built || (notches == 0)) {
		return;
	}
	int16_t line_h = uv_ui_get_string_height("A", &UI_MONO_FONT);
	uv_uiwindow_content_move(&term_win, 0, notches * line_h * SCROLL_LINES_PER_NOTCH);

	uv_bounding_box_st cbb = uv_uiwindow_get_contentbb(&term_win);
	int16_t maxy = cbb.h - uv_uibb(&term_win)->h;
	if (maxy < 0) {
		maxy = 0;
	}
	// re-pin to the live tail only when scrolled back down to the bottom
	pinned_bottom = (-cbb.y >= maxy);
	uv_ui_refresh(&term_win);
}


// --- transmit path ---------------------------------------------------------

/// @brief: Waits for the device to acknowledge the previous chunk (mirrors
/// terminal.c). Returns true once acknowledged, false on timeout (after which the
/// chunk is skipped, matching the CLI behaviour).
static bool wait_for_response(void) {
	bool ret = false;
	uint32_t step_ms = 20;
	for (uint32_t i = 0; i < WAIT_FOR_RESPONSE_MS; i += step_ms) {
		if (responded) {
			responded = false;
			ret = true;
			break;
		}
		uv_rtos_task_delay(step_ms);
	}
	if (!ret) {
		uv_mutex_lock(&mutex);
		responded = true;
		const char *no = "\n*** NO RESPONSE ***\n";
		for (const char *p = no; *p != '\0'; p++) {
			char c = *p;
			uv_ring_buffer_push(&rx_rb, &c);
		}
		uv_mutex_unlock(&mutex);
	}
	return ret;
}


/// @brief: Background task that drains the transmit queue and writes it to the
/// device with SDO downloads to object 0x5FFF, four characters per chunk, each
/// chunk sent only after the previous one is acknowledged.
static void tx_task(void *ptr) {
	(void) ptr;
	while (true) {
		uint8_t chunk[4];
		int n = 0;
		uv_mutex_lock(&mutex);
		while (n < 4) {
			char c;
			if (uv_ring_buffer_pop(&tx_rb, &c) != ERR_NONE) {
				break;
			}
			chunk[n++] = (uint8_t) c;
		}
		uv_mutex_unlock(&mutex);

		if (n > 0) {
			uv_can_msg_st msg;
			msg.type = CAN_STD;
			msg.id = UV_TERMINAL_CAN_RX_ID + term_nodeid;
			msg.data_8bit[0] = 0x22;
			msg.data_8bit[1] = UV_TERMINAL_CAN_INDEX % 256;
			msg.data_8bit[2] = UV_TERMINAL_CAN_INDEX / 256;
			msg.data_8bit[3] = UV_TERMINAL_CAN_SUBINDEX;
			for (int i = 0; i < n; i++) {
				msg.data_8bit[4 + i] = chunk[i];
			}
			msg.data_length = 4 + n;
			if (wait_for_response()) {
				uv_can_send(dev.can_channel, &msg);
			}
			uv_rtos_task_delay(5);
		}
		else {
			// nothing to send: idle
			uv_rtos_task_delay(50);
		}
	}
}


/// @brief: Queues a line (already including its trailing newline) for transmit.
static void tx_enqueue(const char *line) {
	uv_mutex_lock(&mutex);
	for (const char *p = line; *p != '\0'; p++) {
		char c = *p;
		uv_ring_buffer_push(&tx_rb, &c);
	}
	uv_mutex_unlock(&mutex);
}


// --- public API ------------------------------------------------------------

/// @brief: One-time setup of the transfer state, sniffer and transmit task.
static void terminaltab_init(void) {
	if (inited) {
		return;
	}
	inited = true;

	uv_mutex_init(&mutex);
	uv_mutex_unlock(&mutex);
	uv_ring_buffer_init(&rx_rb, rx_ringbuf,
			sizeof(rx_ringbuf) / sizeof(rx_ringbuf[0]), sizeof(rx_ringbuf[0]));
	uv_ring_buffer_init(&tx_rb, tx_ringbuf,
			sizeof(tx_ringbuf) / sizeof(tx_ringbuf[0]), sizeof(tx_ringbuf[0]));
	responded = true;
	rx_text[0] = '\0';

	// a left-aligned, application-focused command line, monospace to match the
	// terminal output above it
	uv_uitextedit_init(&input, input_buf, sizeof(input_buf),
			UITEXTEDIT_FLAG_ONELINE | UITEXTEDIT_FLAG_CMDLINE, &uv_uistyles[0]);
	uv_uitextedit_set_font(&input, &UI_MONO_FONT);
	uv_uitextedit_set_align(&input, ALIGN_CENTER_LEFT);

	// observe device replies alongside the heartbeat monitor
	find_set_extra_can_callback(&can_sniff);

	uv_rtos_task_create(&tx_task, "term_tx", UV_RTOS_MIN_STACK_SIZE, NULL,
			UV_RTOS_IDLE_PRIORITY, NULL);
}


void terminaltab_build(void *parent, uint8_t nodeid) {
	const uv_uistyle_st *style = &uv_uistyles[0];
	terminaltab_init();

	term_nodeid = nodeid;

	uv_bounding_box_st cbb = uv_uitabwindow_get_contentbb(parent);
	int16_t win_h = cbb.h - INPUT_H - 3 * MARGIN;
	if (win_h < INPUT_H) {
		win_h = INPUT_H;
	}

	// scrollable receive window
	uv_uiwindow_init(&term_win, term_win_buf, style);
	uv_uiwindow_set_transparent(&term_win, false);
	uv_uitabwindow_addxy(parent, &term_win,
			MARGIN, MARGIN, cbb.w - 2 * MARGIN, win_h);

	// receive text label, re-bound to the persistent history buffer; monospace so
	// device output (tables, hex, aligned columns) lines up
	uv_uilabel_init(&term_label, &UI_MONO_FONT, ALIGN_TOP_LEFT,
			style->text_color, rx_text);
	uv_uiwindow_addxy(&term_win, &term_label,
			0, 0, cbb.w - 2 * MARGIN - CONFIG_UI_WINDOW_SCROLLBAR_WIDTH, win_h);

	// command line under the receive window
	uv_uitabwindow_addxy(parent, &input,
			MARGIN, MARGIN + win_h + MARGIN, cbb.w - 2 * MARGIN, INPUT_H);

	built = true;
	// a freshly opened terminal shows the live tail
	pinned_bottom = true;
	rx_relayout();
}


void terminaltab_step(bool focused) {
	if (!built) {
		return;
	}

	// pump received characters into the display
	bool got = false;
	uv_mutex_lock(&mutex);
	char c;
	while (uv_ring_buffer_pop(&rx_rb, &c) == ERR_NONE) {
		rx_text_append(c);
		got = true;
	}
	uv_mutex_unlock(&mutex);
	if (got) {
		uv_ui_refresh(&term_label);
		rx_relayout();
	}

	// mouse wheel scrolls the receive history. Only drained while the terminal
	// owns input (log view collapsed); otherwise the wheel notches are left for
	// the expanded log view, which reads them in uvui.c.
	if (focused) {
		int16_t scroll = uv_ui_get_scroll();
		if (scroll != 0) {
			rx_wheel_scroll(scroll);
		}
	}

	// command line: send on Enter, keeping focus
	uv_uitextedit_set_focused(&input, focused);
	if (uv_uitextedit_submitted(&input)) {
		char line[INPUT_BUF_LEN + 2];
		snprintf(line, sizeof(line), "%s\n", uv_uitextedit_get_text(&input));
		tx_enqueue(line);
		uv_uitextedit_set_text(&input, "");
	}
}


#endif
