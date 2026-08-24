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


#ifndef MAKEUVDEV_H_
#define MAKEUVDEV_H_


#include <stdbool.h>


/// @file: Assembles a .uvdev device distribution package, i.e. does what the
/// projects' 'make publish' targets used to do with a shell script of their own.
///
/// A .uvdev file is a plain zip archive bundling, for a single device project:
///   * the firmware binary flashable to the device        (key FIRMWARE)
///   * the CANopen object dictionary file and every file
///     it pulls in with a "content" reference             (key DATABASE)
///   * the Linux simulator executable, optional           (key LINUX_BIN)
///   * the bootloader binary, optional                    (key BOOTLOADER)
///   * a directory of display media assets, optional      (key MEDIA)
///   * a uvdev.json manifest naming which file is which, plus the firmware
///     version string                                     (key VERSION)
///
/// The firmware binary (--firmware) and the database (--db) are mandatory,
/// everything else is left out of the package when it is not given.
///
/// A typical invocation from a project's makefile:
///
///     uvcan --db uvcan/can_uv0d.json
///           --firmware uv0d_jhc_LPC4078.bin
///           --linuxbin uv0d_jhc
///           --bootloader bootloader_LPC4078.bin
///           --media media
///           --fwversion $(GIT_VERSION)
///           --makeuvdev ../prod/uv0d_jhc_uv0d1_$(GIT_VERSION).uvdev


/// @brief: --firmware: The firmware binary bundled into the package. Mandatory.
bool cmd_firmware(const char *arg);

/// @brief: --linuxbin: The Linux simulator executable. Optional.
bool cmd_linuxbin(const char *arg);

/// @brief: --bootloader: The bootloader binary. Optional.
bool cmd_bootloader(const char *arg);

/// @brief: --media: A media file or a directory of media files. Optional, can
/// be given more than once; all of them end up in the package's media directory.
bool cmd_media(const char *arg);

/// @brief: --fwversion: The firmware version string stored in the manifest.
/// Optional.
bool cmd_fwversion(const char *arg);

/// @brief: Returns the firmware binary given with --firmware, or an empty
/// string when it was not given. *loadbin* flashes it when it is given no file
/// of its own and no devices were loaded with --dev / --sys.
const char *makeuvdev_get_firmware(void);


/// @brief: --makeuvdev: Writes the .uvdev package to the path given as the
/// argument, out of the database loaded with --db and the files given with the
/// options above.
bool cmd_makeuvdev(const char *arg);


#endif /* MAKEUVDEV_H_ */
