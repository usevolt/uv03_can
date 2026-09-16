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

#include "selfupdate.h"
#include "http.h"
#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv_rtos.h>

// The background check's result, and the flags the UI polls. Written by the
// check task and read by the UI thread: both are single words, the info is
// published only after `su_done` is set, and the worst a torn read could do is
// show the notice one frame late.
static selfupdate_info_st su_async_info;
static volatile bool su_async_running;
static volatile bool su_async_done;
static volatile bool su_async_newer;
// whether the server answered at all, why it did not, and whether the caller
// has yet been told how the check went (see selfupdate_check_poll())
static volatile bool su_async_ok;
static volatile bool su_async_reported;
static char su_async_err[256];


uint32_t selfupdate_this_version(void) {
	return (uint32_t) __UV_PROGRAM_VERSION;
}


const char *selfupdate_this_name(void) {
	return __UV_APP_VERSION;
}


// The curl config lines every request here shares: no credentials at all (the
// public shelf asks for none, and sending them would hand them to a path that
// never wanted them), and https pinned, since the URL is compiled in and is
// always https.
static int su_cfg_common(char *dst, size_t dstlen, int timeout_s) {
	int n = uvhttp_cfg_common(dst, dstlen, timeout_s, NULL, NULL);
	if ((size_t) n < dstlen) {
		n += snprintf(&dst[n], dstlen - (size_t) n,
				"proto = \"=https\"\ntlsv1.2\n");
	}
	return n;
}


// Reads the manifest's fields out of *body*. Returns false when it is not the
// JSON object this expects, which is what a proxy's error page or a truncated
// answer looks like.
static bool su_parse_manifest(const char *body, selfupdate_info_st *info) {
	bool ret = false;
	parser_node_st root = parser_read_buffer((char*) body, strlen(body),
			PARSER_FORMAT_JSON);
	parser_node_st c;
	if (parser_node_is_valid(root) &&
			parser_node_is_valid(c = parser_find_child(root, "version"))) {
		memset(info, 0, sizeof(*info));
		info->version = (uint32_t) parser_get_int(c);
		if (parser_node_is_valid(c = parser_find_child(root, "name"))) {
			parser_get_string(c, info->name, sizeof(info->name));
		}
		if (parser_node_is_valid(c = parser_find_child(root, "file"))) {
			parser_get_string(c, info->file, sizeof(info->file));
		}
		if (parser_node_is_valid(c = parser_find_child(root, "sha256"))) {
			parser_get_string(c, info->sha256, sizeof(info->sha256));
		}
		if (parser_node_is_valid(c = parser_find_child(root, "size"))) {
			info->size = (uint64_t) parser_get_int(c);
		}
		if (parser_node_is_valid(c = parser_find_child(root, "released"))) {
			parser_get_string(c, info->released, sizeof(info->released));
		}
		if (parser_node_is_valid(c = parser_find_child(root, "notes"))) {
			parser_get_string(c, info->notes, sizeof(info->notes));
		}
		// A manifest naming no file is no use: there would be nothing to
		// download when the user asked for the update it advertises.
		ret = (info->file[0] != '\0');
	}
	else {
	}
	return ret;
}


bool selfupdate_check(selfupdate_info_st *info, bool *newer,
		char *err, unsigned int err_len) {
	bool ret = false;
	selfupdate_info_st local;
	memset(&local, 0, sizeof(local));

	char resp_path[256];
	uvhttp_tmp_path(resp_path, sizeof(resp_path), "su", "manifest");

	char cfg[1024];
	int n = su_cfg_common(cfg, sizeof(cfg), 20);
	snprintf(&cfg[n], sizeof(cfg) - (size_t) n,
			"url = \"%s/%s\"\n"
			"output = \"%s\"\n"
			"write-out = \"%%{http_code}\"\n",
			SELFUPDATE_URL, SELFUPDATE_MANIFEST, resp_path);
	long code = uvhttp_curl(cfg);
	char *body = (code == 200) ? uvhttp_read_file(resp_path) : NULL;
	remove(resp_path);

	if (code != 200) {
		// Not uvhttp_status_err(): its 401/403/404 wording is about fleets and
		// accounts, and none of that applies to a path that asks for no
		// credentials. Here a 404 means the shelf is not there.
		if (code == 0) {
			uvhttp_err(err, err_len,
					"Could not reach the update server (is curl installed?).");
		}
		else if (code == 404) {
			uvhttp_err(err, err_len,
					"No update information is published at " SELFUPDATE_URL ".");
		}
		else {
			char m[128];
			snprintf(m, sizeof(m),
					"The update server returned HTTP %ld.", code);
			uvhttp_err(err, err_len, m);
		}
	}
	else if ((body == NULL) || !su_parse_manifest(body, &local)) {
		uvhttp_err(err, err_len,
				"The update server did not answer with version information.");
	}
	else {
		ret = true;
		if (info != NULL) {
			*info = local;
		}
		if (newer != NULL) {
			*newer = (local.version > selfupdate_this_version());
		}
	}
	free(body);
	return ret;
}


#if !CONFIG_TARGET_WIN

#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>


bool selfupdate_can_apply(void) {
	return true;
}


// Where the running binary lives.
//
// /proc/self/exe rather than argv[0]: argv[0] may be a relative path (the
// working directory has changed by then), a bare name found on PATH, or the
// name of a wrapper. The link is what the kernel knows, and it is also what
// resolves to the real file when uvcan was started through a symlink.
static bool su_exe_path(char *out, size_t len, char *err, unsigned int err_len) {
	bool ret = false;
	ssize_t n = readlink("/proc/self/exe", out, len - 1);
	if (n > 0) {
		out[n] = '\0';
		ret = true;
	}
	else {
		uvhttp_err(err, err_len,
				"Could not work out where this uvcan is installed.");
	}
	return ret;
}


bool selfupdate_apply(const selfupdate_info_st *info,
		char *err, unsigned int err_len) {
	bool ret = false;
	char exe[1024];
	if (!su_exe_path(exe, sizeof(exe), err, err_len)) {
		return false;
	}

	// The download lands next to the binary it will replace, not in /tmp: the
	// install is finished with rename(), which only works within one file
	// system, and /tmp is very often a different one.
	char tmp_path[1100];
	snprintf(tmp_path, sizeof(tmp_path), "%s.new", exe);
	char old_path[1100];
	snprintf(old_path, sizeof(old_path), "%s.old", exe);

	// Checked before the download rather than after it: a system install is
	// root-owned, and there is no point pulling ten megabytes over the network
	// to find that out. Writing the new file is the permission that matters --
	// the rename needs the same directory.
	char dir[1024];
	snprintf(dir, sizeof(dir), "%s", exe);
	char *slash = strrchr(dir, '/');
	if (slash != NULL) {
		*slash = '\0';
	}
	else {
		snprintf(dir, sizeof(dir), ".");
	}
	if (access(dir, W_OK) != 0) {
		char m[1200];
		snprintf(m, sizeof(m),
				"No permission to replace %s. Run 'sudo uvcan --update', or "
				"reinstall with install.sh.", exe);
		uvhttp_err(err, err_len, m);
		return false;
	}

	printf("Downloading uvcan %s (%llu KB)...\n",
			(info->name[0] != '\0') ? info->name : "update",
			(unsigned long long) (info->size / 1024u));
	fflush(stdout);

	remove(tmp_path);
	char cfg[2048];
	int n = su_cfg_common(cfg, sizeof(cfg), 600);
	snprintf(&cfg[n], sizeof(cfg) - (size_t) n,
			"url = \"%s/%s\"\n"
			"output = \"%s\"\n"
			"write-out = \"%%{http_code}\"\n",
			SELFUPDATE_URL, info->file, tmp_path);
	long code = uvhttp_curl_logged(cfg, tmp_path, info->size);

	if (code != 200) {
		char m[160];
		snprintf(m, sizeof(m),
				"Downloading the update failed (HTTP %ld).", code);
		uvhttp_err(err, err_len, m);
		remove(tmp_path);
	}
	else {
		// Verified before anything is replaced. Both the size and the hash
		// come from the same manifest as the file itself, so this catches a
		// truncated or corrupted transfer rather than a hostile one -- which
		// is the failure that actually happens, and the one that would
		// otherwise leave an unrunnable uvcan behind.
		struct stat st;
		char got[72] = { '\0' };
		if (stat(tmp_path, &st) != 0) {
			uvhttp_err(err, err_len, "The downloaded update disappeared.");
			remove(tmp_path);
		}
		else if ((info->size > 0) && ((uint64_t) st.st_size != info->size)) {
			char m[160];
			snprintf(m, sizeof(m),
					"The update is %llu bytes, not the %llu it should be.",
					(unsigned long long) st.st_size,
					(unsigned long long) info->size);
			uvhttp_err(err, err_len, m);
			remove(tmp_path);
		}
		else if ((info->sha256[0] != '\0') &&
				!uvhttp_sha256_file(tmp_path, got, sizeof(got))) {
			// Said apart from a real mismatch on purpose: one means the file is
			// wrong and one means we could not tell, and reporting the second
			// as the first sends whoever reads it looking at the server.
			uvhttp_err(err, err_len,
					"Could not check the downloaded update against its "
					"checksum; nothing was replaced.");
			remove(tmp_path);
		}
		else if ((info->sha256[0] != '\0') &&
				(strcasecmp(got, info->sha256) != 0)) {
			uvhttp_err(err, err_len,
					"The downloaded update does not match its checksum; "
					"nothing was replaced.");
			remove(tmp_path);
		}
		else if (chmod(tmp_path, 0755) != 0) {
			uvhttp_err(err, err_len,
					"Could not make the downloaded update executable.");
			remove(tmp_path);
		}
		else {
			// Keep the binary being replaced, so a bad build is one 'mv' from
			// being undone. link() rather than a copy: it is the same inode,
			// so it costs nothing and cannot half-succeed.
			remove(old_path);
			if (link(exe, old_path) != 0) {
				// not fatal -- an install on a file system with no hard links
				// still updates, it just cannot be rolled back this way
			}
			else {
			}
			// rename() rather than writing over the file in place: a running
			// executable cannot be written to (ETXTBSY), but it can be renamed
			// away from, because the running process holds the inode and not
			// the name.
			if (rename(tmp_path, exe) != 0) {
				char m[200];
				snprintf(m, sizeof(m), "Could not put the update in place: %s.",
						strerror(errno));
				uvhttp_err(err, err_len, m);
				remove(tmp_path);
			}
			else {
				ret = true;
			}
		}
	}
	return ret;
}


#else /* CONFIG_TARGET_WIN: a folder of files, not a binary to replace */

bool selfupdate_can_apply(void) {
	return false;
}

bool selfupdate_apply(const selfupdate_info_st *info,
		char *err, unsigned int err_len) {
	(void) info;
	uvhttp_err(err, err_len,
			"The Windows uvcan is a folder of files rather than one binary, so "
			"it cannot replace itself. Install the new one with get-uvcan.ps1.");
	return false;
}

#endif


// The background check. One shot: it runs once per uvcan session, so opening
// the UI costs one request and no more.
static void su_check_task(void *ptr) {
	(void) ptr;
	bool newer = false;
	selfupdate_info_st info;
	char err[sizeof(su_async_err)] = "";
	if (selfupdate_check(&info, &newer, err, sizeof(err))) {
		su_async_info = info;
		su_async_newer = newer;
		su_async_ok = true;
	}
	else {
		// Kept rather than printed here: a machine on a CAN bus in a field has
		// no network, and a tool that interrupts with that is worse than one
		// that mentions it where the user is already looking. The UI logs one
		// line for it, whichever way the check went.
		snprintf(su_async_err, sizeof(su_async_err), "%s", err);
	}
	su_async_done = true;
	su_async_running = false;
	uv_rtos_task_delete(NULL);
}


void selfupdate_check_async(void) {
	if (su_async_running || su_async_done) {
		// already running, or already answered this session
	}
	else {
		su_async_running = true;
		uv_rtos_task_create(&su_check_task, "su_check",
				UV_RTOS_MIN_STACK_SIZE * 5, NULL,
				UV_RTOS_IDLE_PRIORITY + 1, NULL);
	}
}


bool selfupdate_available(selfupdate_info_st *info) {
	bool ret = (su_async_done && su_async_newer);
	if (ret && (info != NULL)) {
		*info = su_async_info;
	}
	else {
	}
	return ret;
}


bool selfupdate_check_poll(selfupdate_info_st *info, bool *ok, bool *newer,
		char *err, unsigned int err_len) {
	bool ret = (su_async_done && !su_async_reported);
	if (ret) {
		su_async_reported = true;
		if (info != NULL) {
			*info = su_async_info;
		}
		if (ok != NULL) {
			*ok = su_async_ok;
		}
		if (newer != NULL) {
			*newer = su_async_newer;
		}
		if ((err != NULL) && (err_len > 0)) {
			snprintf(err, err_len, "%s", su_async_err);
		}
	}
	else {
	}
	return ret;
}


#if !CONFIG_TARGET_WIN


// The background install. Written by the install task and read by the UI
// thread, published the same way as the check's result: the flags are single
// words and the error is complete before `su_apply_done` is set.
static volatile bool su_apply_running;
static volatile bool su_apply_done;
static volatile bool su_apply_ok;
static char su_apply_err[256];


static void su_apply_task(void *ptr) {
	(void) ptr;
	su_apply_ok = selfupdate_apply(&su_async_info, su_apply_err,
			sizeof(su_apply_err));
	su_apply_done = true;
	su_apply_running = false;
	uv_rtos_task_delete(NULL);
}


bool selfupdate_apply_async(void) {
	bool ret = false;
	if (su_async_done && su_async_newer && !su_apply_running) {
		su_apply_err[0] = '\0';
		su_apply_done = false;
		su_apply_running = true;
		uv_rtos_task_create(&su_apply_task, "su_apply",
				UV_RTOS_MIN_STACK_SIZE * 5, NULL,
				UV_RTOS_IDLE_PRIORITY + 1, NULL);
		ret = true;
	}
	else {
		// nothing found to install, or an install is already on its way
	}
	return ret;
}


bool selfupdate_apply_poll(bool *ok, char *err, unsigned int err_len) {
	bool ret = su_apply_done;
	if (ret) {
		su_apply_done = false;
		if (ok != NULL) {
			*ok = su_apply_ok;
		}
		if ((err != NULL) && (err_len > 0)) {
			snprintf(err, err_len, "%s", su_apply_err);
		}
	}
	else {
	}
	return ret;
}

#else /* CONFIG_TARGET_WIN: nothing here to install over */

bool selfupdate_apply_async(void) {
	return false;
}

bool selfupdate_apply_poll(bool *ok, char *err, unsigned int err_len) {
	(void) ok;
	(void) err;
	(void) err_len;
	return false;
}

#endif
