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


#ifndef UVDEV_H_
#define UVDEV_H_


#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


/// @brief: An opened .uvdev device package.
///
/// A .uvdev file is a plain zip archive bundling, for a single device project,
/// the firmware binary, the simulator executable, and the CANopen object
/// dictionary database (plus its json includes), together with a uvdev.json
/// manifest naming each. See make_uvdev.sh in the uvcan submodule for how the
/// package is assembled.
typedef struct {
	/// @brief: Temporary directory the package was extracted into.
	char dir[1024];
	/// @brief: DATABASE manifest entry: the object dictionary json path,
	/// relative to the package root (*dir*). Empty if the manifest has none.
	char database[512];
	/// @brief: FIRMWARE manifest entry: the firmware binary path, relative to
	/// the package root. Empty if the manifest has none.
	char firmware[512];
	/// @brief: LINUX_BIN manifest entry: the desktop (Linux) simulator
	/// executable path, relative to the package root. Empty if the manifest has
	/// none. Used to run the device as a simulator.
	char linux_bin[512];
	/// @brief: VERSION manifest entry: the firmware software version string
	/// (e.g. a git-describe value). Empty if the manifest has none.
	char version[64];
} uvdev_st;


/// @brief: Opens a .uvdev package: extracts it into a fresh temporary directory
/// and reads its uvdev.json manifest into *this*.
///
/// Returns false on failure (file missing, unzip failed, or no valid
/// uvdev.json). On success the caller must release the temporary directory with
/// uvdev_close() once done.
bool uvdev_open(uvdev_st *this, const char *uvdev_path);

/// @brief: Removes the temporary extraction directory created by uvdev_open().
void uvdev_close(uvdev_st *this);


#endif /* UVDEV_H_ */
