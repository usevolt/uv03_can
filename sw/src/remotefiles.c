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


#include "remotefiles.h"
#include "credentials.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#if !CONFIG_TARGET_WIN

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <strings.h>
#include "parser.h"


// Session state for the current login. The token is kept in RAM only (never
// written to the credentials file) and passed to curl through a config file so it
// does not appear in the process arguments.
static char rf_url[CREDENTIALS_MAX];
// The file server authenticates every request with HTTP Basic, so the
// credentials are kept rather than a session token: there is nothing to
// exchange them for. They live only in memory, and only for as long as the
// session lasts.
static char rf_user[CREDENTIALS_MAX];
static char rf_pass[CREDENTIALS_MAX];
// The fleets this account may read, as the server reported them at login. The
// client does not choose these - it is told them - so there is no fleet setting
// to keep in step with the server's idea of who may see what.
static char rf_fleets[REMOTEFILES_MAX_FLEETS][REMOTEFILES_FLEET_MAX];
static uint8_t rf_fleet_count;
static bool rf_logged_in;
// The product store: grown on demand rather than a fixed array, so no fleet
// layout can silently lose entries. Owned here and freed by rf_free_products(),
// which every fresh listing and every logout calls.
static remotefiles_product_st *rf_products;
static uint16_t rf_product_count;
static uint16_t rf_product_cap;


// Builds a per-process temp path "/tmp/uvcan_rf_<pid>_<suffix>" into *out*. The
// paths are fixed (no user input), so the shell commands built from them are
// injection-safe; everything user- or server-supplied goes through the curl config
// or a data file instead.
static void rf_tmp_path(char *out, size_t len, const char *suffix) {
	snprintf(out, len, "/tmp/uvcan_rf_%d_%s", (int) getpid(), suffix);
}


// Writes *content* to *path* with 0600 permissions. Returns true on success.
static bool rf_write_file(const char *path, const char *content) {
	bool ret = false;
	FILE *f = fopen(path, "w");
	if (f != NULL) {
		fputs(content, f);
		fclose(f);
		chmod(path, 0600);
		ret = true;
	}
	return ret;
}


// Reads the whole file at *path* into a freshly malloc'd, null-terminated buffer
// (caller frees). Returns NULL on error.
static char *rf_read_file(const char *path) {
	char *ret = NULL;
	FILE *f = fopen(path, "rb");
	if (f != NULL) {
		fseek(f, 0, SEEK_END);
		long size = ftell(f);
		rewind(f);
		if (size >= 0) {
			ret = malloc((size_t) size + 1);
			if (ret != NULL) {
				size_t rd = fread(ret, 1, (size_t) size, f);
				ret[rd] = '\0';
			}
		}
		fclose(f);
	}
	return ret;
}


// (The JSON string escaper that used to live here built the login request body.
// Basic auth has no body, so nothing needs escaping any more.)


// Copies *src* into *dst* (size *dstlen*) dropping characters that could break out
// of a curl config-file quoted value (double quote, CR, LF). Used for the URL and
// bearer token, which curl - not the shell - parses.
static void rf_cfg_sanitize(char *dst, size_t dstlen, const char *src) {
	size_t d = 0;
	for (size_t i = 0; (src[i] != '\0') && (d + 1 < dstlen); i++) {
		char c = src[i];
		if ((c != '"') && (c != '\r') && (c != '\n')) {
			dst[d++] = c;
		}
	}
	dst[d] = '\0';
}


// Runs `curl -K <cfg_path>` (the config file carries the URL, method, headers,
// data and output paths) and returns the HTTP status code curl reports via its
// write-out, or 0 when curl could not be run. The command line contains only our
// fixed temp paths, so nothing user- or server-supplied reaches the shell.
static long rf_run_curl(const char *cfg_path) {
	long code = 0;
	char code_path[256];
	rf_tmp_path(code_path, sizeof(code_path), "code");

	char cmd[1024];
	snprintf(cmd, sizeof(cmd), "curl -K '%s' > '%s' 2>/dev/null",
			cfg_path, code_path);
	int rc = system(cmd);
	if (rc != -1) {
		char *body = rf_read_file(code_path);
		if (body != NULL) {
			code = strtol(body, NULL, 10);
			free(body);
		}
	}
	remove(code_path);
	return code;
}


// Common helper: writes *cfg* to a temp config file, runs curl, removes the config
// file (it may hold the bearer token) and returns the HTTP status code.
static long rf_curl_with_cfg(const char *cfg) {
	char cfg_path[256];
	rf_tmp_path(cfg_path, sizeof(cfg_path), "cfg");
	long code = 0;
	if (rf_write_file(cfg_path, cfg)) {
		code = rf_run_curl(cfg_path);
	}
	remove(cfg_path);
	return code;
}


// Fills *err* with *msg* when *err* is non-NULL.
static void rf_err(char *err, unsigned int err_len, const char *msg) {
	if ((err != NULL) && (err_len > 0)) {
		strncpy(err, msg, err_len - 1);
		err[err_len - 1] = '\0';
	}
}


/// @brief: Builds the base URL of one of this account's fleet directories.
static void rf_fleet_url(char *dst, size_t dstlen, const char *fleet) {
	char f[REMOTEFILES_FLEET_MAX];
	rf_cfg_sanitize(f, sizeof(f), (fleet != NULL) ? fleet : "");
	snprintf(dst, dstlen, "%s/%s", rf_url, f);
}


/// @brief: Emits the curl config lines every request shares: the timeouts and
/// the Basic credentials.
static int rf_cfg_common(char *dst, size_t dstlen, int timeout_s) {
	char user[CREDENTIALS_MAX];
	char pass[CREDENTIALS_MAX];
	rf_cfg_sanitize(user, sizeof(user), rf_user);
	rf_cfg_sanitize(pass, sizeof(pass), rf_pass);
	return snprintf(dst, dstlen,
			"silent\nshow-error\n"
			"connect-timeout = 15\nmax-time = %d\n"
			"user = \"%s:%s\"\n",
			timeout_s, user, pass);
}


/// @brief: Turns an HTTP status into the reason the caller shows. Shared so the
/// three calls describe the same failure the same way.
static void rf_http_err(long code, const char *what, char *err,
		unsigned int err_len) {
	if (code == 0) {
		rf_err(err, err_len,
				"Could not reach the server (is curl installed and the URL "
				"correct?).");
	}
	else if (code == 401) {
		rf_err(err, err_len, "Invalid username or password.");
	}
	else if (code == 403) {
		rf_err(err, err_len, "This account may not read that fleet's files.");
	}
	else if (code == 404) {
		rf_err(err, err_len,
				"No such fleet on the file server, or it has no files area yet.");
	}
	else {
		char m[128];
		snprintf(m, sizeof(m), "Server returned HTTP %ld %s.", code, what);
		rf_err(err, err_len, m);
	}
}


/// @brief: Reads the fleet list the server answered with into rf_fleets.
///
/// The body is {"user":…,"super":…,"fleets":[{"name":"a"},{"name":"b"}]}. The
/// fleets are objects rather than bare strings because this project's JSON
/// reader finds a value by scanning forward for its key's ':' — a string
/// sitting directly in an array has no key, and reads back as whatever follows
/// it in the document.
///
/// An account with no fleets at all is a valid answer - somebody has an account
/// but has not been granted anything yet - and is reported as such rather than
/// as a failure to log in, because the credentials plainly worked.
static bool rf_parse_fleets(char *body) {
	bool ret = false;
	rf_fleet_count = 0;
	parser_node_st root = parser_read_buffer(body, strlen(body),
			PARSER_FORMAT_JSON);
	if (parser_node_is_valid(root)) {
		parser_node_st arr = parser_find_child(root, "fleets");
		if (parser_node_is_valid(arr) &&
				(parser_get_type(arr) == PARSER_ARRAY)) {
			unsigned int n = parser_array_get_size(arr);
			for (unsigned int i = 0;
					(i < n) && (rf_fleet_count < REMOTEFILES_MAX_FLEETS); i++) {
				parser_node_st e = parser_array_at(arr, i);
				parser_node_st nn = parser_find_child(e, "name");
				if (parser_node_is_valid(nn)) {
					char name[REMOTEFILES_FLEET_MAX] = { '\0' };
					parser_get_string(nn, name, sizeof(name));
					if (name[0] != '\0') {
						strncpy(rf_fleets[rf_fleet_count], name,
								REMOTEFILES_FLEET_MAX - 1);
						rf_fleets[rf_fleet_count][REMOTEFILES_FLEET_MAX - 1] =
								'\0';
						rf_fleet_count++;
					}
				}
			}
			ret = true;
		}
	}
	return ret;
}


uint8_t remotefiles_get_fleet_count(void) {
	return rf_fleet_count;
}


const char *remotefiles_get_fleet(uint8_t index) {
	return (index < rf_fleet_count) ? rf_fleets[index] : "";
}


bool remotefiles_login(const char *url, const char *username,
		const char *password, char *err, unsigned int err_len) {
	bool ret = false;
	rf_logged_in = false;
	rf_fleet_count = 0;
	rf_cfg_sanitize(rf_url, sizeof(rf_url), (url != NULL) ? url : "");
	// strip a trailing '/' so "<url>/<fleet>" never doubles the slash
	size_t ul = strlen(rf_url);
	if ((ul > 0) && (rf_url[ul - 1] == '/')) {
		rf_url[ul - 1] = '\0';
	}

	// Default to https when the address carries no scheme. curl would otherwise
	// try http, and the server permanently redirects that to https - a redirect
	// which by definition preserves the method and the headers, so the
	// credentials would go out in the clear before being told to use TLS.
	if ((strlen(rf_url) > 0) &&
			(strstr(rf_url, "://") == NULL)) {
		char scheme_url[sizeof(rf_url) + 16];
		snprintf(scheme_url, sizeof(scheme_url), "https://%s", rf_url);
		strncpy(rf_url, scheme_url, sizeof(rf_url) - 1);
		rf_url[sizeof(rf_url) - 1] = '\0';
	}
	else {
	}

	strncpy(rf_user, (username != NULL) ? username : "", sizeof(rf_user) - 1);
	rf_user[sizeof(rf_user) - 1] = '\0';
	strncpy(rf_pass, (password != NULL) ? password : "", sizeof(rf_pass) - 1);
	rf_pass[sizeof(rf_pass) - 1] = '\0';

	if (strlen(rf_url) == 0) {
		rf_err(err, err_len, "No server URL set.");
	}
	else {
		// There is no login endpoint to call: Basic auth is checked on every
		// request. Asking for the fleet list is both the smallest request that
		// proves the credentials work and the one that says what this account
		// may read - the server decides that, so there is nothing to configure
		// here and no fleet name to get wrong.
		char resp_path[256];
		rf_tmp_path(resp_path, sizeof(resp_path), "resp");

		char cfg[2048];
		int n = rf_cfg_common(cfg, sizeof(cfg), 30);
		snprintf(&cfg[n], sizeof(cfg) - n,
				"url = \"%s/fleets.json\"\n"
				"output = \"%s\"\n"
				"write-out = \"%%{http_code}\"\n",
				rf_url, resp_path);
		long code = rf_curl_with_cfg(cfg);
		char *body = (code == 200) ? rf_read_file(resp_path) : NULL;
		remove(resp_path);

		if (code != 200) {
			rf_http_err(code, "on login", err, err_len);
		}
		else if ((body == NULL) || !rf_parse_fleets(body)) {
			rf_err(err, err_len,
					"The server did not answer with a fleet list.");
		}
		else {
			rf_logged_in = true;
			ret = true;
		}
		free(body);
	}
	return ret;
}


/// @brief: Reads one entry of a directory listing into *ver*. The server
/// reports only what a file system knows, so the release notes and the checksum
/// the structure can carry stay empty.
static void rf_parse_entry(parser_node_st obj, const char *dir,
		remotefiles_version_st *ver) {
	memset(ver, 0, sizeof(*ver));
	parser_node_st c;
	char name[256] = { '\0' };
	if (parser_node_is_valid(c = parser_find_child(obj, "name"))) {
		parser_get_string(c, name, sizeof(name));
	}
	strncpy(ver->version, name, sizeof(ver->version) - 1);
	if (dir[0] != '\0') {
		snprintf(ver->path, sizeof(ver->path), "%s/%s", dir, name);
	}
	else {
		strncpy(ver->path, name, sizeof(ver->path) - 1);
	}
	if (parser_node_is_valid(c = parser_find_child(obj, "size"))) {
		ver->size = (uint64_t) parser_get_int(c);
	}
	if (parser_node_is_valid(c = parser_find_child(obj, "mod_time"))) {
		parser_get_string(c, ver->modified, sizeof(ver->modified));
		// the date alone is what the list shows
		strncpy(ver->released, ver->modified, 10);
		ver->released[10] = '\0';
	}
}


/// @brief: Returns true when a listing entry is a directory.
static bool rf_entry_is_dir(parser_node_st obj) {
	bool ret = false;
	parser_node_st c = parser_find_child(obj, "is_dir");
	if (parser_node_is_valid(c)) {
		ret = (parser_get_bool(c) != 0);
	}
	return ret;
}


/// @brief: Fetches one directory listing as JSON.
/// @return: the HTTP status; *out is the response body to free, or NULL.
static long rf_fetch_dir(const char *fleet, const char *dir, char **out) {
	*out = NULL;
	char base[1024];
	rf_fleet_url(base, sizeof(base), fleet);
	char edir[512];
	rf_cfg_sanitize(edir, sizeof(edir), dir);

	char resp_path[256];
	rf_tmp_path(resp_path, sizeof(resp_path), "resp");
	char cfg[2560];
	int n = rf_cfg_common(cfg, sizeof(cfg), 30);
	// A directory has to be asked for WITH its trailing slash. The server
	// answers a directory addressed without one with a 308 redirect to the
	// slashed form, and this client deliberately does not follow redirects --
	// there is no `location` in the curl config, because every request carries
	// Basic credentials -- so the fetch would come back 308, the caller would
	// see no listing at all, and the directory would look empty rather than
	// broken. The fleet's own folder passes dir="" and the "%s/" already ends
	// it in a slash.
	snprintf(&cfg[n], sizeof(cfg) - n,
			"url = \"%s/%s%s\"\n"
			"header = \"Accept: application/json\"\n"
			"output = \"%s\"\n"
			"write-out = \"%%{http_code}\"\n",
			base, edir, (edir[0] != '\0') ? "/" : "", resp_path);
	long code = rf_curl_with_cfg(cfg);
	if (code == 200) {
		*out = rf_read_file(resp_path);
	}
	remove(resp_path);
	return code;
}


/// @brief: Parses a directory listing into an array node.
///
/// The server answers with a bare top-level array, which this project's JSON
/// reader does not accept - it wants an object at the root - so the body is
/// wrapped in one first. The nodes point into that wrapper, so it is handed
/// back for the caller to free once it has finished reading them.
static parser_node_st rf_parse_listing(const char *body, char **wrapper) {
	parser_node_st ret = { 0 };
	*wrapper = NULL;
	size_t len = strlen(body);
	char *w = malloc(len + 16);
	if (w != NULL) {
		snprintf(w, len + 16, "{\"e\":%s}", body);
		parser_node_st root = parser_read_buffer(w, strlen(w),
				PARSER_FORMAT_JSON);
		ret = parser_find_child(root, "e");
		*wrapper = w;
	}
	return ret;
}


/// @brief: How deep the directory walk goes.
///
/// A guard, not a limit of the layout: the walk is recursive, and a symlinked
/// directory loop on the server would otherwise never terminate.
#define RF_MAX_DEPTH		8


/// @brief: Releases the whole product store, versions included.
///
/// Callers may hold pointers into it (the UI passes product->name straight to
/// the treeview), so this must not run while a listing is on screen - it is
/// called at the start of a new listing and on logout, both of which mean the
/// old list is already gone.
static void rf_free_products(void) {
	if (rf_products != NULL) {
		for (uint16_t i = 0; i < rf_product_count; i++) {
			free(rf_products[i].versions);
			rf_products[i].versions = NULL;
		}
		free(rf_products);
		rf_products = NULL;
	}
	rf_product_count = 0;
	rf_product_cap = 0;
}


/// @brief: Makes room for one more product. Doubling growth, so a listing costs
/// a handful of reallocs rather than one per directory.
/// @return: false when out of memory, in which case the store is left as it was.
static bool rf_products_reserve(void) {
	bool ret = true;
	if (rf_product_count >= rf_product_cap) {
		uint16_t cap = (rf_product_cap == 0) ? 8 : (uint16_t) (rf_product_cap * 2);
		remotefiles_product_st *n = realloc(rf_products,
				(size_t) cap * sizeof(*n));
		if (n == NULL) {
			ret = false;
		}
		else {
			rf_products = n;
			rf_product_cap = cap;
		}
	}
	return ret;
}


/// @brief: Makes room for one more version in *p*. Same doubling growth.
static bool rf_versions_reserve(remotefiles_product_st *p) {
	bool ret = true;
	if (p->version_count >= p->version_cap) {
		uint16_t cap = (p->version_cap == 0) ? 8 : (uint16_t) (p->version_cap * 2);
		remotefiles_version_st *n = realloc(p->versions,
				(size_t) cap * sizeof(*n));
		if (n == NULL) {
			ret = false;
		}
		else {
			p->versions = n;
			p->version_cap = cap;
		}
	}
	return ret;
}


/// @brief: Starts a product for directory *rel* of *fleet* ("" for the fleet's
/// own folder). Returns NULL when out of memory.
static remotefiles_product_st *rf_new_product(const char *fleet,
		const char *rel, bool prefix_names) {
	remotefiles_product_st *p = NULL;
	if (rf_products_reserve()) {
		p = &rf_products[rf_product_count];
		memset(p, 0, sizeof(*p));
		// The id doubles as the server-relative path every download of this
		// product is built from, so it carries the fleet and the whole nested
		// path, untruncated.
		if (rel[0] != '\0') {
			snprintf(p->id, sizeof(p->id), "%s/%s", fleet, rel);
		}
		else {
			strncpy(p->id, fleet, sizeof(p->id) - 1);
		}
		// Nested directories are named by their path relative to the fleet, so
		// "uv0d/rev2" reads as what it is. The fleet's own folder is named
		// after the fleet, prefixed or not - it is already the fleet.
		if (rel[0] == '\0') {
			strncpy(p->name, fleet, sizeof(p->name) - 1);
		}
		else if (prefix_names) {
			snprintf(p->name, sizeof(p->name), "%.60s / %.60s", fleet, rel);
		}
		else {
			strncpy(p->name, rel, sizeof(p->name) - 1);
		}
		rf_product_count++;
	}
	return p;
}


/// @brief: Walks directory *rel* of *fleet* recursively: the files in it become
/// one product, and every subdirectory is walked in turn.
///
/// Two passes over the listing on purpose. Files first, so a directory's own
/// product is created before the products of anything nested inside it and the
/// panel reads top-down; subdirectories second.
///
/// The product is created lazily, on the first file found, so a directory that
/// holds nothing but subdirectories does not show up as an empty row.
///
/// @param parsed: set (when non-NULL) to whether this directory's listing was
/// readable as an array. Only the top-level call cares; nested calls pass NULL,
/// because one unreadable subdirectory must not blank the whole panel.
/// @return: the HTTP status of *this* directory's listing.
static long rf_walk_dir(const char *fleet, const char *rel, bool prefix_names,
		unsigned int depth, bool *parsed) {
	if (parsed != NULL) {
		*parsed = false;
	}
	char *resp = NULL;
	long code = rf_fetch_dir(fleet, rel, &resp);
	if ((code != 200) || (resp == NULL)) {
		free(resp);
		return code;
	}

	char *wrap = NULL;
	parser_node_st root = rf_parse_listing(resp, &wrap);
	if (parser_node_is_valid(root) && (parser_get_type(root) == PARSER_ARRAY)) {
		if (parsed != NULL) {
			*parsed = true;
		}
		unsigned int n = parser_array_get_size(root);
		remotefiles_product_st *p = NULL;
		unsigned int i;

		for (i = 0; i < n; i++) {
			parser_node_st e = parser_array_at(root, i);
			if (!parser_node_is_valid(e) || rf_entry_is_dir(e)) {
				continue;
			}
			if (p == NULL) {
				p = rf_new_product(fleet, rel, prefix_names);
				if (p == NULL) {
					break;
				}
			}
			if (!rf_versions_reserve(p)) {
				break;
			}
			rf_parse_entry(e, p->id, &p->versions[p->version_count]);
			p->version_count++;
		}

		if ((depth + 1) < RF_MAX_DEPTH) {
			for (i = 0; i < n; i++) {
				parser_node_st e = parser_array_at(root, i);
				if (!parser_node_is_valid(e) || !rf_entry_is_dir(e)) {
					continue;
				}
				char dname[128] = { '\0' };
				parser_node_st c = parser_find_child(e, "name");
				if (parser_node_is_valid(c)) {
					parser_get_string(c, dname, sizeof(dname));
				}
				// the listing marks a directory by a trailing '/' in its name,
				// which would double up in every path built from it
				size_t dl = strlen(dname);
				if ((dl > 0) && (dname[dl - 1] == '/')) {
					dname[dl - 1] = '\0';
				}
				if (dname[0] == '\0') {
					continue;
				}
				char child[512];
				if (rel[0] != '\0') {
					snprintf(child, sizeof(child), "%s/%s", rel, dname);
				}
				else {
					snprintf(child, sizeof(child), "%s", dname);
				}
				rf_walk_dir(fleet, child, prefix_names, depth + 1, NULL);
			}
		}
	}
	free(wrap);
	free(resp);
	return code;
}


/// @brief: Adds the products of one fleet to the list.
///
/// Every path recorded here is relative to the server root and starts with the
/// fleet, because an account may hold several and a download has to know which
/// one a file came from.
///
/// @return: false only when the fleet could not be listed at all. A failure
/// further down the tree is skipped quietly.
static bool rf_list_fleet(const char *fleet, bool prefix_names, char *err,
		unsigned int err_len) {
	bool ret = false;
	bool parsed = false;
	long code = rf_walk_dir(fleet, "", prefix_names, 0, &parsed);
	if (code != 200) {
		rf_http_err(code, "listing files", err, err_len);
	}
	else if (!parsed) {
		rf_err(err, err_len, "The server did not answer with a file listing.");
	}
	else {
		ret = true;
	}
	return ret;
}


bool remotefiles_list(char *err, unsigned int err_len) {
	bool ret = false;
	// Releases the previous listing, so repeated opens of the panel do not leak
	// it. Safe here: the panel that could be holding pointers into it has been
	// closed by the time a new listing is asked for.
	rf_free_products();
	if (!rf_logged_in) {
		rf_err(err, err_len, "Not logged in.");
	}
	else if (rf_fleet_count == 0) {
		rf_err(err, err_len,
				"This account holds no fleets yet, so it has no files.");
	}
	else {
		// One account may hold several fleets. Listing them all keeps the file
		// view whole rather than making the user pick a fleet first; the fleet
		// is shown in the product name only when there is more than one, so the
		// common single-fleet account reads exactly as it did before.
		bool prefix = (rf_fleet_count > 1);
		char first_err[256] = { '\0' };
		for (uint8_t i = 0; i < rf_fleet_count; i++) {
			char one_err[256] = { '\0' };
			if (rf_list_fleet(rf_fleets[i], prefix, one_err, sizeof(one_err))) {
				// a single readable fleet is enough for the list to be usable
				ret = true;
			}
			else if (first_err[0] == '\0') {
				snprintf(first_err, sizeof(first_err), "%.63s: %.180s",
						rf_fleets[i], one_err);
			}
			else {
			}
		}
		if (!ret) {
			rf_err(err, err_len, first_err);
		}
		else {
		}
	}
	return ret;
}


bool remotefiles_download(const char *path, const char *dest_path,
		char *err, unsigned int err_len) {
	bool ret = false;
	if (!rf_logged_in) {
		rf_err(err, err_len, "Not logged in.");
	}
	else if ((path == NULL) || (path[0] == '\0')) {
		rf_err(err, err_len, "No file selected.");
	}
	else {
		// *path* already starts with the fleet (products are listed that way),
		// so it resolves against the server root rather than one fleet's folder.
		char epath[1024];
		char dest[1024];
		rf_cfg_sanitize(epath, sizeof(epath), path);
		rf_cfg_sanitize(dest, sizeof(dest), dest_path);

		char cfg[5120];
		int n = rf_cfg_common(cfg, sizeof(cfg), 300);
		snprintf(&cfg[n], sizeof(cfg) - n,
				"url = \"%s/%s\"\n"
				"output = \"%s\"\n"
				"write-out = \"%%{http_code}\"\n",
				rf_url, epath, dest);
		long code = rf_curl_with_cfg(cfg);

		if (code == 200) {
			ret = true;
		}
		else {
			rf_http_err(code, "downloading the file", err, err_len);
			remove(dest_path);
		}
	}
	return ret;
}


uint16_t remotefiles_get_product_count(void) {
	return rf_product_count;
}


const remotefiles_product_st *remotefiles_get_product(uint16_t index) {
	return (index < rf_product_count) ? &rf_products[index] : NULL;
}


bool remotefiles_is_logged_in(void) {
	return rf_logged_in;
}


void remotefiles_logout(void) {
	rf_logged_in = false;
	rf_user[0] = '\0';
	rf_pass[0] = '\0';
	// The list belonged to the session being dropped; holding it would only
	// hand the next caller stale files under credentials that no longer apply.
	rf_free_products();
}


#else /* CONFIG_TARGET_WIN: not wired up on the Windows build yet */


bool remotefiles_is_logged_in(void) {
	return false;
}

void remotefiles_logout(void) {
}

uint8_t remotefiles_get_fleet_count(void) {
	return 0;
}

const char *remotefiles_get_fleet(uint8_t index) {
	(void) index;
	return "";
}

bool remotefiles_login(const char *url, const char *username,
		const char *password, char *err, unsigned int err_len) {
	(void) url;
	(void) username;
	(void) password;
	if ((err != NULL) && (err_len > 0)) {
		strncpy(err, "The server file browser is not available in the Windows "
				"build yet.", err_len - 1);
		err[err_len - 1] = '\0';
	}
	return false;
}

bool remotefiles_list(char *err, unsigned int err_len) {
	(void) err;
	(void) err_len;
	return false;
}

uint16_t remotefiles_get_product_count(void) {
	return 0;
}

const remotefiles_product_st *remotefiles_get_product(uint16_t index) {
	(void) index;
	return NULL;
}

bool remotefiles_download(const char *path, const char *dest_path,
		char *err, unsigned int err_len) {
	(void) path;
	(void) dest_path;
	(void) err;
	(void) err_len;
	return false;
}


#endif
