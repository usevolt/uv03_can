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
#ifndef UVCAN_HTTP_H_
#define UVCAN_HTTP_H_

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/// @file: The curl plumbing shared by everything here that talks HTTP: the
/// server file panel (remotefiles.c) and the self-updater (selfupdate.c).
///
/// Every request is made by running curl with a CONFIG FILE rather than a
/// command line. The config file carries the URL, the method, the headers, the
/// credentials and the output paths, so nothing user- or server-supplied is
/// ever handed to a shell -- the only thing on curl's command line is one of
/// our own fixed temp paths. It is also what keeps credentials out of the
/// process arguments, where any other user on the machine could read them.
///
/// Redirects are deliberately not followed: a request may carry credentials,
/// and following a redirect would hand them to wherever the answer points.


/// @brief: Builds a per-process temp path "/tmp/uvcan_<tag>_<pid>_<suffix>"
/// into *out*. The paths are fixed (no user input), so the commands built from
/// them are injection-safe.
void uvhttp_tmp_path(char *out, size_t len, const char *tag, const char *suffix);


/// @brief: Reads the whole file at *path* into a freshly malloc'd,
/// null-terminated buffer (the caller frees). NULL on error.
char *uvhttp_read_file(const char *path);


/// @brief: Writes *content* to *path* with 0600 permissions.
bool uvhttp_write_file(const char *path, const char *content);


/// @brief: Copies *src* into *dst* dropping the characters that could break out
/// of a quoted value in a curl config file (double quote, CR, LF). Used for
/// everything that goes into one: URLs, credentials, paths.
void uvhttp_cfg_sanitize(char *dst, size_t dstlen, const char *src);


/// @brief: Emits the curl config lines every request shares: quiet operation,
/// the timeouts, and HTTPS-only with no redirects.
///
/// @param user, pass: HTTP Basic credentials, or NULL for an unauthenticated
/// request -- which is what the public part of the file server wants.
/// @return: the number of characters written, so the caller can append its own
/// lines after them.
int uvhttp_cfg_common(char *dst, size_t dstlen, int timeout_s,
		const char *user, const char *pass);


/// @brief: Runs curl with *cfg* as its config file and returns the HTTP status
/// code curl reports through its write-out, or 0 when curl could not be run at
/// all. The config file is removed afterwards, credentials and all.
long uvhttp_curl(const char *cfg);


/// @brief: As uvhttp_curl(), for a config file naming SEVERAL transfers.
///
/// One curl process for the lot, which is the whole point: curl keeps the
/// connection open between them, so N requests to one host cost one TCP
/// connection and one TLS handshake instead of N of each. Walking a directory
/// tree one curl process per directory spent most of its time shaking hands.
///
/// The config file has to carry `write-out = "%{http_code}\n"` in its common
/// section; curl then writes one status line per transfer, in order, and they
/// come back in *codes*.
///
/// @return: how many status codes were read, i.e. how many transfers curl
/// actually reported. Fewer than asked for means it stopped early.
int uvhttp_curl_multi(const char *cfg, long *codes, int max_codes);


/// @brief: As uvhttp_curl(), for a download whose progress is logged to stdout.
///
/// @param dest_path: where the config file tells curl to write the body. The
/// progress is measured from the size that file has reached, rather than from
/// curl's own meter, which redraws one line with carriage returns and would
/// reach the line-based log capture as one enormous line.
/// @param expected: the size in bytes when it is known, 0 when it is not.
long uvhttp_curl_logged(const char *cfg, const char *dest_path,
		uint64_t expected);


/// @brief: Fills *err* with *msg* when *err* is non-NULL.
void uvhttp_err(char *err, unsigned int err_len, const char *msg);


/// @brief: Turns an HTTP status into the reason a caller shows, so that every
/// call describes the same failure the same way. *what* names what was being
/// done ("on login", "listing files"), and is used only for a status this has
/// no words of its own for.
void uvhttp_status_err(long code, const char *what, char *err,
		unsigned int err_len);


/// @brief: The lower-case hex SHA-256 of the file at *path*, written into *out*
/// (which wants room for 65 characters).
///
/// @return: false when the file cannot be read or sha256sum is not there to run.
bool uvhttp_sha256_file(const char *path, char *out, size_t out_len);


#endif /* UVCAN_HTTP_H_ */
