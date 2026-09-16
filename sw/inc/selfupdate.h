/*
 * This file is part of the uvcan distribution (www.usevolt.fi).
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
#ifndef UVCAN_SELFUPDATE_H_
#define UVCAN_SELFUPDATE_H_

#include <stdbool.h>
#include <stdint.h>


/// @file: Checking for, and installing, a newer uvcan.
///
/// uvcan is published on the PUBLIC shelf of the Usevolt file server, which is
/// served to anyone with no credentials at all -- uvcan is free software and
/// the people who need it are not all account holders. That is the whole reason
/// this does not go through remotefiles.c, which exists to read the per-fleet
/// areas and authenticates every request.
///
/// What is published there is the bare Linux binary. That is enough, because
/// the installed uvcan is one file: install.sh copies the binary and a handful
/// of desktop-integration assets, and the fonts are compiled into the binary
/// with the files next to it only ever an override. An update that changes the
/// desktop assets needs install.sh run again, which is what the manifest's
/// notes are for.


/// @brief: Where uvcan is published, and the manifest naming the newest build.
/// Compiled in rather than configurable: this is not the per-account file
/// server the Account panel points at, it is where uvcan itself comes from.
#define SELFUPDATE_URL			"https://files.usevolt.fi/pub/uvcan"
#define SELFUPDATE_MANIFEST		"latest.json"


/// @brief: One published build, as latest.json describes it.
typedef struct {
	/// @brief: The published build's __UV_PROGRAM_VERSION, i.e. the number of
	/// commits in its history.
	///
	/// This is the field versions are compared by, and the reason it is this
	/// one: it grows with every commit for the life of the project. The
	/// git-describe name below reads better but counts commits since the
	/// nearest tag, so it restarts at zero at every release and cannot be
	/// compared at all.
	uint32_t version;
	/// @brief: The git describe name, e.g. "1.1.1-204-g5746". Shown, not compared.
	char name[64];
	/// @brief: The binary's name on the server, below SELFUPDATE_URL.
	char file[128];
	/// @brief: Lower-case hex SHA-256 of the binary, and its size in bytes.
	char sha256[72];
	uint64_t size;
	char released[32];
	char notes[256];
} selfupdate_info_st;


/// @brief: This build's version number and name, i.e. what an update is
/// compared against.
uint32_t selfupdate_this_version(void);
const char *selfupdate_this_name(void);


/// @brief: Fetches the manifest and says whether it names something newer than
/// this build.
///
/// Blocks for as long as the request takes. *info* and *newer* may be NULL.
///
/// @return: false when the manifest could not be read at all, with the reason
/// in *err*. A successful check that finds nothing newer returns true with
/// *newer false.
bool selfupdate_check(selfupdate_info_st *info, bool *newer,
		char *err, unsigned int err_len);


/// @brief: Downloads the build *info* describes and puts it in the place of the
/// running binary.
///
/// The download is verified against the size and the SHA-256 in the manifest
/// before anything is replaced, and the binary it replaces is kept next to it
/// as "<name>.old", so a bad build is one `mv` away from being undone.
///
/// @return: false with the reason in *err*. On success the caller has to say
/// that uvcan must be restarted: the running process keeps running from the
/// file it started with.
bool selfupdate_apply(const selfupdate_info_st *info,
		char *err, unsigned int err_len);


/// @brief: Starts a check in the background, for a caller that must not block
/// (the UI). Does nothing if a check is already running or has already been
/// made in this session.
void selfupdate_check_async(void);


/// @brief: Whether the background check has finished and found something newer.
/// Never blocks. *info* may be NULL.
bool selfupdate_available(selfupdate_info_st *info);


/// @brief: True once, when the background check started by
/// selfupdate_check_async() has finished, so the caller can say how it went
/// whichever way it went: *ok* says whether the update server answered at all,
/// *newer* whether what it named is newer than this build, *info* what it
/// named, and *err* (size *err_len*) why a failed check failed. Never blocks;
/// every argument may be NULL.
bool selfupdate_check_poll(selfupdate_info_st *info, bool *ok, bool *newer,
		char *err, unsigned int err_len);


/// @brief: Whether this build can install an update over itself, i.e. whether
/// selfupdate_apply() does anything here.
///
/// The Linux uvcan is a single binary and replaces it. The Windows one is a
/// folder of files - the exe, a DLL, the fonts, the launchers - so it is
/// updated by unpacking the published package over it, which is what
/// get-uvcan.ps1 does; there is nothing for this to replace.
bool selfupdate_can_apply(void);


/// @brief: Installs the build the background check found (see
/// selfupdate_available()) with selfupdate_apply(), on a task of its own, so
/// the caller (the UI) keeps running while it downloads. The download logs its
/// progress.
///
/// @return: false when there is nothing to install or an install is already
/// running.
bool selfupdate_apply_async(void);


/// @brief: True once, when the install selfupdate_apply_async() started has
/// finished: *ok* then says whether it succeeded, and *err* (size *err_len*)
/// why it did not. Never blocks. *ok* and *err* may be NULL.
bool selfupdate_apply_poll(bool *ok, char *err, unsigned int err_len);


#endif /* UVCAN_SELFUPDATE_H_ */
