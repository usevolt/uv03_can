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


/// @brief: Pumps the window. Must be called every UI cycle; it is what notices
/// the user closing the window from its title bar.
void remoteui_win_step(void);


#endif

#endif /* UI_REMOTEUI_WIN_H_ */
