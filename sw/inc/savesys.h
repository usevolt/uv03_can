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


#ifndef SAVESYS_H_
#define SAVESYS_H_


#include <stdbool.h>


/// @brief: Writes the current system to a .uvsys package at *file*.
///
/// A .uvsys file is a plain zip archive bundling, for every device of the
/// system:
///   * the device's .uvdev configuration package, renamed <name>_0x<nodeid>.uvdev
///   * a parameter file read from every OPERATIONAL device over the bus, named
///     <devname>_0x<nodeid>.json (like the --saveparam command)
///   * a uvsys.json manifest whose "DEVS" array lists one object per device with
///     its "UVDEV" file, "PARAM" file and "NODEID" (a hexadecimal string).
///
/// Blocks while parameters are read over SDO from each operational device, so it
/// must be called from a task/UI context where blocking is acceptable.
///
/// @return: true when the package was written successfully.
bool savesys_save(const char *file);


/// @brief: Starts savesys_save() on its own task so the caller (the UI) is not
/// blocked while parameters are read over SDO. Poll savesys_is_finished() for
/// completion. The log view's frame title is updated with the save progress
/// while the task runs, and reset when it finishes.
void savesys_save_async(const char *file);


/// @brief: Returns true when no asynchronous system save is running (i.e. the
/// last savesys_save_async() has completed). Returns true before any save starts.
bool savesys_is_finished(void);


#endif /* SAVESYS_H_ */
