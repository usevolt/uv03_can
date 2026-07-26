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


#ifndef UI_FLEETTAB_H_
#define UI_FLEETTAB_H_


#include <uv_hal_config.h>

#if CONFIG_UI

#include <uv_ui.h>


/// @brief: Content of the main window's "Fleet" tab: an "Account" panel holding
/// the MQTT broker address and the fleet admin credentials (stored on this
/// computer, entirely separate from the system tab's file-server account), and -
/// once connected - a tab window with one tab per fleet, each holding a tab window
/// with one tab per device of that fleet. See mqtt.h for how the two are
/// discovered.


/// @brief: Populates the tab window's content area with the Fleet view.
///
/// The caller is responsible for clearing the tab window before calling this.
void fleettab_show(uv_uitabwindow_st *tabwin);


/// @brief: Pumps the MQTT client and polls the Fleet tab's own widgets. Must be
/// called every UI cycle, also while another main tab is shown, so the connection
/// stays alive and the fleet list keeps filling in. Returns true when the tab's
/// content has to be rebuilt (the fleet or device list changed), which the caller
/// should act on only while the Fleet tab is actually visible.
bool fleettab_step(void);


/// @brief: Tells the Fleet tab whether its widgets are currently built. While
/// they are not, fleettab_step() leaves them alone and only keeps the connection
/// running.
void fleettab_set_shown(bool value);


#endif

#endif /* UI_FLEETTAB_H_ */
