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


#define REMOTEFILES_MAX_PRODUCTS	24
#define REMOTEFILES_MAX_VERSIONS	16
/// @brief: How many fleets one account can hold, and how long a fleet name may
/// be. Matches what the broker side tracks (MQTT_MAX_FLEETS / MQTT_NAME_MAX).
#define REMOTEFILES_MAX_FLEETS		16
#define REMOTEFILES_FLEET_MAX		64


/// @brief: One downloadable version of a product, with its display metadata.
typedef struct {
	// version label shown to the user (e.g. "13")
	char version[32];
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
typedef struct {
	char id[64];
	char name[128];
	remotefiles_version_st versions[REMOTEFILES_MAX_VERSIONS];
	uint8_t version_count;
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
uint8_t remotefiles_get_product_count(void);

/// @brief: Product at *index*, or NULL when out of range.
const remotefiles_product_st *remotefiles_get_product(uint8_t index);


/// @brief: Downloads the file at server-relative *path* to the local *dest_path*.
/// When the version carries a SHA-256 it is verified after the transfer. Returns
/// true on success; fills *err* on failure (including a checksum mismatch).
bool remotefiles_download(const char *path, const char *dest_path,
		char *err, unsigned int err_len);


#endif /* REMOTEFILES_H_ */
