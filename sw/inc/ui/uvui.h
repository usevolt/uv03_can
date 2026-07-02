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


#ifndef UI_UVUI_H_
#define UI_UVUI_H_


#include <uv_hal_config.h>

#if CONFIG_UI

/// @brief: Opens uvcan's main graphical display and blocks until the window is
/// closed.
///
/// The display is a tab window whose first tab is "System" and whose remaining
/// tabs are one per device in the current system (see system.h). When the
/// system holds fewer than SYSTEM_DEV_MAX_COUNT devices, a final "Add device"
/// tab is shown; selecting it appends a new, empty device and opens its tab.
///
/// This is intended to be run after the HAL configuration window
/// (uv_ui_confwindow_exec()) has closed.
void uvui_exec(void);


/// @brief: Sets the title shown on the full log view's frame. Pass NULL to reset
/// it to the default "Log". Used to describe an ongoing activity (e.g. system
/// configuration saving progress) so the log is easier to follow. Safe to call
/// from a worker task; the change is picked up by the UI loop.
void uvui_set_log_title(const char *title);

/// @brief: Resets the log view's frame title back to the default "Log".
void uvui_reset_log_title(void);


/// @brief: Returns true while the bottom log view is expanded to full screen.
/// Used by front-most views (e.g. the device terminal) to yield keyboard focus to
/// the log view's own command line while it covers them.
bool uvui_log_is_expanded(void);

#endif

#endif /* UI_UVUI_H_ */
