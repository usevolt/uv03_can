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


#ifndef DEVSEL_WIN_H_
#define DEVSEL_WIN_H_


#include <uv_ui.h>
#include "system.h"


/// @brief: Per-device eligibility callback. Returns NULL when *device* can take
/// part in the operation, or a short reason string ("Offline", "No saved
/// parameters", ...) when it cannot. Devices with a reason are listed greyed out
/// and can never be selected. Pass NULL to make every device selectable.
typedef const char *(*devsel_reason_t)(device_st *device);


/// @brief: Opens the modal device-selection window: lists every device of
/// *system* on its own row with a checkbox, each selectable device checked by
/// default, so the user can deselect the devices to leave out of the operation.
/// Blocks until the user accepts or cancels.
///
/// @param title: Window title, naming the operation the devices are picked for.
/// @param accept_text: Label of the accepting button (e.g. "Load").
/// @param reason_callb: Eligibility callback, see devsel_reason_t. May be NULL.
/// @param targets: Output array receiving the selected devices, in system order.
/// @param max_count: Capacity of *targets*.
///
/// @return: The number of devices written into *targets*. 0 when the user
/// cancelled or deselected everything.
uint8_t devsel_win_exec(system_st *system, const char *title,
		const char *accept_text, devsel_reason_t reason_callb,
		device_st **targets, uint8_t max_count, const uv_uistyle_st *style);


#endif /* DEVSEL_WIN_H_ */
