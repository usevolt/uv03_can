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


#ifndef UI_JOYSTICK_WIN_H_
#define UI_JOYSTICK_WIN_H_


#include <uv_hal_config.h>

#if CONFIG_UI

#include <uv_ui.h>
#include "system.h"


/// @brief: Opens the joystick driver's configuration window and blocks until
/// it is closed. Every axis and button of the virtual controller has a row of
/// its own with a list button selecting the CAN-bus device and a button which
/// opens the list of that device's inputs and outputs (the values it sends in
/// its TXPDOs), plus the axis range or the button threshold and an invert box.
///
/// It starts out with the configuration in use. Saving takes the edited one
/// into use (the driver restarts with it) and writes it into the system file
/// loaded last; without one, it goes into the next system file saved. Returns
/// true when the user saved.
bool joystick_win_exec(system_st *system, const uv_uistyle_st *style);


#endif

#endif /* UI_JOYSTICK_WIN_H_ */
