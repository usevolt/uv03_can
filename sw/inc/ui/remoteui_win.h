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


#ifndef UI_REMOTEUI_WIN_H_
#define UI_REMOTEUI_WIN_H_


#include <uv_hal_config.h>

#if CONFIG_UI

#include <stdint.h>
#include <stdbool.h>


/// @file: A separate native window mirroring one remote device's display.
///
/// It is its own OS window rather than a panel inside the Fleet tab, because a
/// device's display has its own dimensions and its own refresh: squeezing it
/// into a tab would either distort it or force the rest of the view around it.
/// The window is a child of this process — closing uvcan closes it — and the
/// user can also close it from its title bar, which the Fleet tab notices via
/// remoteui_win_is_open().
///
/// Frames arrive as the compact UI command stream described in uv_ui_remote.h.
/// The device sends only what changed on screen, so the window repaints when a
/// frame arrives and holds the last one otherwise.


/// @brief: Opens the window at the device's own size, titled after it. Closes
/// any window already open — only one device is mirrored at a time.
///
/// @return: false if the window could not be created.
bool remoteui_win_open(const char *title, uint16_t width, uint16_t height);


/// @brief: Closes the window. Safe to call when none is open.
void remoteui_win_close(void);


/// @brief: True while the window exists.
bool remoteui_win_is_open(void);


/// @brief: Renders one complete mirrored frame, i.e. the command stream from
/// FRAME_BEGIN up to FRAME_END.
void remoteui_win_draw_frame(const uint8_t *cmds, uint32_t len);


/// @brief: Called for every press, release, drag and scroll the user makes on
/// the mirrored view, to be carried to the device as if its own screen had been
/// touched. *action* is a uv_ui_remote_input_action_e and the coordinates are on
/// the device's display, whatever size this window has been dragged to.
///
/// The device is sent raw presses and releases and derives clicks and drags
/// from them itself, the same way it does for its own touch screen.
typedef void (*remoteui_input_t)(uint8_t action, int16_t x, int16_t y,
		int16_t scroll, char key, void *user);

/// @brief: Registers who carries input back to the device. Without one, the
/// mirrored view is only something to look at.
void remoteui_win_set_input_callb(remoteui_input_t callb, void *user);


/// @brief: Called when the mirrored view needs an asset - so far only a font -
/// that it does not have. The device is drawing with it at that moment, so it
/// can answer; the reply comes back through remoteui_win_asset_received().
typedef void (*remoteui_asset_req_t)(uint8_t kind, uint32_t id, void *user);

/// @brief: Registers who carries an asset request to the device. Without one,
/// the mirrored view falls back to the fonts compiled into uvcan.
void remoteui_win_set_asset_request_callb(remoteui_asset_req_t callb,
		void *user);

/// @brief: Hands over an asset the device sent. A *len* of zero is the device
/// saying it has no such asset, which stops it being asked for again.
void remoteui_win_asset_received(uint8_t kind, uint32_t id,
		const uint8_t *data, uint32_t len);


/// @brief: Pumps the window. Must be called every UI cycle; it is what notices
/// the user closing the window from its title bar.
void remoteui_win_step(void);


#endif

#endif /* UI_REMOTEUI_WIN_H_ */
