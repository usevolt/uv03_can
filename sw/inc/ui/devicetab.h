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


#ifndef UI_DEVICETAB_H_
#define UI_DEVICETAB_H_


#include <uv_hal_config.h>

#if CONFIG_UI

#include <uv_ui.h>
#include "system.h"


/// @brief: Populates the tab window's content area with the system overview:
/// the system name and the list of devices it contains. When no system
/// configuration was loaded a placeholder is shown.
///
/// The caller is responsible for clearing the tab window before calling this.
void devicetab_show_system(uv_uitabwindow_st *tabwin, system_st *system);

/// @brief: Populates the tab window's content area with a single device's view.
/// When *device* is NULL or has no configuration loaded an empty placeholder is
/// shown.
///
/// The caller is responsible for clearing the tab window before calling this.
void devicetab_show_device(uv_uitabwindow_st *tabwin, device_st *device);

/// @brief: Polls the device tab's file picker. Must be called every UI cycle
/// while a device tab is shown. Returns true when the user just chose a new
/// device-configuration file (the device was updated accordingly), so the
/// caller can refresh tab names and content.
bool devicetab_step(void);

/// @brief: Returns true while an asynchronous device operation started from a
/// device tab (firmware flash, or parameter save/load) is in progress. While
/// true the caller should not update device states or otherwise touch the SDO
/// client, which the running operation owns.
bool devicetab_is_busy(void);

/// @brief: Returns true when the in-progress busy operation is a CAN-bus device
/// search (as opposed to a flash / parameter save / load). The search shows its
/// own progress on the system tab, so the UI should not pop the log view open
/// for it the way it does for the other operations.
bool devicetab_busy_is_search(void);

/// @brief: Returns true once after the user removed the currently-shown device
/// via its "Remove" button (the flag is cleared by the read). Lets the caller
/// move the tab selection to the tab just before the removed one instead of
/// keeping the same index, which would otherwise land on whichever device
/// shifted into the freed slot.
bool devicetab_poll_device_removed(void);


#endif

#endif /* UI_DEVICETAB_H_ */
