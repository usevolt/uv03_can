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


#ifndef REMOTEFILES_H_
#define REMOTEFILES_H_


#include <stdbool.h>
#include <stdint.h>


/// @brief: Client for the Usevolt file server (see doc/remote_files_spec.md). Logs
/// in with the account credentials, lists the files available to that account
/// (grouped as products -> versions with metadata) and downloads a selected file.
///
/// The transport is the `curl` command-line tool (present on Linux and built into
/// Windows 10+), invoked over HTTPS so TLS and certificate verification are handled
/// by curl - uvcan links no HTTP library. All calls block; run them off the UI
/// thread (from a task or a modal dialog's own loop).


/// @brief: Products and the files inside them are held in grown-on-demand
/// arrays, so neither is capped: a fleet with more directories than any fixed
/// bound would silently lose the rest, and a dropped firmware package is the
/// kind of missing thing nobody notices until it is needed. Only the walk's
/// depth is bounded (see RF_MAX_DEPTH in remotefiles.c), and that is a
/// termination guard rather than a limit.


/// @brief: How many fleets one account can hold, and how long a fleet name may
/// be. Matches what the broker side tracks (MQTT_MAX_FLEETS / MQTT_NAME_MAX).
#define REMOTEFILES_MAX_FLEETS		16
#define REMOTEFILES_FLEET_MAX		64


/// @brief: One downloadable version of a product, with its display metadata.
typedef struct {
	// The version label shown to the user, which for a package on this server
	// is its whole file name -- "uv0d_jhc_uv0d1_noremote_1042-gcf8c.uvdev" is
	// 40 characters, and at 32 the panel showed it cut off mid-word. Sized for
	// the longest name the makefiles produce, with room to spare.
	char version[96];
	// server-relative path used to download it (the download allowlist key)
	char path[512];
	// release date (ISO "YYYY-MM-DD"), free-text release notes
	char released[32];
	char notes[256];
	// lower-case hex SHA-256 of the file, for post-download verification ("" if the
	// server did not supply one)
	char sha256[72];
	// file size in bytes and last-modified timestamp, as reported by the server
	uint64_t size;
	char modified[40];
} remotefiles_version_st;


/// @brief: A product: a named group of versions.
///
/// One product is one directory on the server. The tree is walked recursively,
/// so a nested directory becomes a product of its own, named by its path
/// relative to the fleet (e.g. "uv0d/rev2"). Files sitting directly in the
/// fleet's own folder are grouped under the fleet name.
typedef struct {
	// Doubles as the server-relative path this product's downloads are built
	// from ("<fleet>/<nested/path>"), so it has to hold a whole nested path
	// rather than just one name.
	char id[256];
	char name[128];
	// Which of this account's fleets the product came from, as an index into
	// remotefiles_get_fleet(). The name does not carry the fleet - the panel
	// shows one fleet per tab, and prefixing it there would only say twice what
	// the tab already says.
	uint8_t fleet;
	// Grown as files are found; owned by remotefiles.c and valid until the next
	// remotefiles_list() or remotefiles_logout(). Callers may read it (and hold
	// pointers into `name`) only for that long.
	remotefiles_version_st *versions;
	uint16_t version_count;
	uint16_t version_cap;
} remotefiles_product_st;


/// @brief: Opens a session on *url* (the server base address) as
/// *username* / *password*.
///
/// The server authenticates every request with HTTP Basic and serves each
/// fleet's files under its own path, which is what lets one account see one set
/// of files and another account a different one. **Which fleets those are is the
/// server's decision**: this asks it, with GET /fleets.json, rather than being
/// told a fleet name by the user. That request doubles as the login, there being
/// no login endpoint to call - it is the smallest request that proves the
/// credentials work. The credentials are then kept for the subsequent calls,
/// since every one of them carries them again.
///
/// Returns true on success. On failure *err* (if non-NULL, size *err_len*) is
/// filled with a short human-readable reason.
bool remotefiles_login(const char *url, const char *username,
		const char *password, char *err, unsigned int err_len);


/// @brief: The fleets this account may read, as reported at login. Zero of them
/// is a perfectly valid answer: the account exists but has not been granted
/// anything yet.
uint8_t remotefiles_get_fleet_count(void);
const char *remotefiles_get_fleet(uint8_t index);


/// @brief: True while a session opened by remotefiles_login() is
/// held, i.e. while the tool is logged in to the file server. Used by the system
/// tab's Account panel to show the connection status.
bool remotefiles_is_logged_in(void);


/// @brief: Drops the session token, so the next server access needs a fresh
/// remotefiles_login(). Called when the user edits any of the account fields:
/// the token belongs to the credentials that were in them at login time.
void remotefiles_logout(void);


/// @brief: Fetches the file list for the logged-in account into the internal store
/// (accessed with remotefiles_get_*). Must be called after remotefiles_login().
/// Returns true on success; fills *err* on failure.
bool remotefiles_list(char *err, unsigned int err_len);


/// @brief: Number of products in the last successful remotefiles_list().
uint16_t remotefiles_get_product_count(void);

/// @brief: Product at *index*, or NULL when out of range.
///
/// The returned pointer, and everything it points at, belongs to remotefiles.c
/// and stays valid until the next remotefiles_list() or remotefiles_logout().
const remotefiles_product_st *remotefiles_get_product(uint16_t index);


/// @brief: Downloads the file at server-relative *path* to the local *dest_path*.
/// Returns true on success; fills *err* on failure.
///
/// Blocks until the transfer is done, logging what it is doing and how far it
/// has got to stdout, which is both the terminal and the UI's log view. *size*
/// is the file's size as the listing reported it, which is what the percentage
/// is measured against; pass 0 when it is not known.
bool remotefiles_download(const char *path, const char *dest_path,
		uint64_t size, char *err, unsigned int err_len);


/// @brief: Starts remotefiles_download() on a task of its own and returns at
/// once, so the caller's UI keeps drawing (and showing the transfer's log) while
/// the file comes down. Poll remotefiles_download_is_finished(), then take the
/// outcome from remotefiles_download_result().
///
/// One download at a time: starting another while one runs is the caller's
/// mistake, and is refused (the running one is left alone).
void remotefiles_download_async(const char *path, const char *dest_path,
		uint64_t size);


/// @brief: True when no asynchronous download is running, i.e. the one started
/// with remotefiles_download_async() is done (or none was ever started).
bool remotefiles_download_is_finished(void);


/// @brief: How the last asynchronous download ended. Returns true when it
/// succeeded, with *dest* holding the local path of the file it wrote; on
/// failure *err* holds the reason, as a sentence fit for an error dialog.
bool remotefiles_download_result(char *dest, unsigned int dest_len,
		char *err, unsigned int err_len);


#endif /* REMOTEFILES_H_ */
