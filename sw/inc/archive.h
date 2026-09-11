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

#ifndef UVCAN_ARCHIVE_H_
#define UVCAN_ARCHIVE_H_

#include <stdbool.h>
#include <stddef.h>

/// @file: Cross-platform helpers for the temporary-directory + zip-extraction
/// dance used to read the .uvsys (system) and .uvdev (device) packages, which
/// are both plain zip archives. On Linux these shell out to mkdtemp/unzip/rm;
/// on Windows (mingw) they use the Win32 temp API plus the bundled tar.exe
/// (bsdtar, present on Windows 10 1803+ and later), which extracts zip archives
/// by content regardless of file extension.


/// @brief: Creates a fresh, uniquely-named temporary directory under the
/// system temp location and writes its absolute path into *dest*. *prefix* is a
/// short tag (e.g. "uvcan_uvsys") embedded in the name so leftovers are
/// identifiable, and so is the process id, which is what lets
/// archive_sweep_stale_tmpdirs() tell a leftover from a directory in use.
/// Returns true on success.
bool archive_mktempdir(const char *prefix, char *dest, size_t dest_len);

/// @brief: Extracts the zip archive *archive* into the existing directory
/// *destdir*, overwriting any existing entries. Returns true on success.
bool archive_extract(const char *archive, const char *destdir);

/// @brief: Recursively removes the directory tree at *dir* (best effort; failures
/// are ignored since the OS reaps the temp area eventually).
void archive_rmtree(const char *dir);

/// @brief: Best-effort removal of stale uvcan temporary directories left behind
/// by an earlier run that crashed or was killed before its cleanup could run.
/// A directory is swept once the uvcan whose process id its name carries is gone,
/// so a crashed run's leftovers (an extraction directory, or a package the server
/// files window downloaded) are reclaimed by the next run and not a day later,
/// while the directories of a uvcan which is still running are left alone. Only
/// directories whose name says nothing about their owner -- from a uvcan older
/// than that naming -- keep the age guard of a day. A no-op where no safe sweep
/// is available.
void archive_sweep_stale_tmpdirs(void);

#endif /* UVCAN_ARCHIVE_H_ */
