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


#ifndef UI_SETTINGSTAB_H_
#define UI_SETTINGSTAB_H_


#include <uv_hal_config.h>

#if CONFIG_UI

#include <uv_ui.h>


/// @brief: Content of the main window's "Settings" tab: the "Account" panel
/// holding the file server URL, the fleet broker URL and the Usevolt account's
/// username and password. The values are stored on this computer and shared by
/// every uvcan install (see credentials.h); one account opens both the file
/// server and the fleet broker.
///
/// Under it the "Load parameters" panel: a list of parameter files, ordered by
/// the user, which are loaded onto the devices one after another.


/// @brief: True while the "Load parameters" panel's load is running. It owns
/// the SDO client for the duration, so the other tabs leave the devices alone.
bool settingstab_is_busy(void);


/// @brief: Populates the tab window's content area with the Settings view.
///
/// The caller is responsible for clearing the tab window before calling this.
void settingstab_show(uv_uitabwindow_st *tabwin);


/// @brief: Connects with the stored account once at start-up, and while the
/// tab is shown saves the fields the user edits and handles "Connect". Must be
/// called every UI cycle, also while another main tab is shown, so the
/// start-up connection is made whichever tab the window opens on.
void settingstab_step(void);


/// @brief: Tells the Settings tab whether its widgets are currently built. While
/// they are not, settingstab_step() leaves them alone.
void settingstab_set_shown(bool value);


#endif

#endif /* UI_SETTINGSTAB_H_ */
