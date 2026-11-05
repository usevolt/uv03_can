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
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <strings.h>
#include <uv_rtos.h>
#include "parser.h"
#include "http.h"


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
// set for the duration of remotefiles_login(), see remotefiles_login_is_running()
static volatile bool rf_login_running;

// One fleet's products: grown on demand rather than a fixed array, so no fleet
// layout can silently lose entries.
typedef struct {
	remotefiles_product_st *products;
	uint16_t count;
	uint16_t cap;
} rf_store_st;

// The listing of each fleet, by the fleet's index. Each fleet is listed on its
// own, when it is first asked for, so opening the panel costs the fleet list
// and nothing more, and a fleet's tab costs one request when it is opened.
//
// A listing is built in a store of its own and only moved in here once it is
// complete, while its state is still LOADING; the state turns READY after
// that. A reader only ever reads a READY fleet, so the task listing one fleet
// never moves the products of another under a tree that is pointing into them.
static rf_store_st rf_lists[REMOTEFILES_MAX_FLEETS];
static volatile remotefiles_fleet_state_e rf_state[REMOTEFILES_MAX_FLEETS];
static bool rf_indexed[REMOTEFILES_MAX_FLEETS];
static char rf_list_err[REMOTEFILES_MAX_FLEETS][256];
// Bumped by everything that makes the listings in flight meaningless: a login
// (the fleet indices may name other fleets now), a logout, and
// remotefiles_forget_lists(). A listing that finishes under another generation
// than it started in is thrown away instead of stored.
static volatile uint32_t rf_generation;
// The fleets waiting for the background listing task, one bit each, and the
// one asked for last, which is listed first: it is the tab on screen.
static volatile uint32_t rf_pending;
static volatile int rf_wanted = -1;
static volatile bool rf_list_task_running;

// What the server serves a fleet's file index as (see uv3b_iotbrkr's
// uvfilesctl): the fleet's whole tree in one document.
#define RF_INDEX_NAME		".uvindex.json"

static void rf_forget_all(void);


// The HTTP plumbing all of this is built on lives in http.c, shared with the
// self-updater: the curl-config discipline, the temp paths, the progress
// logging and the status wording are the same job in both places. What stays
// here is what is particular to the per-fleet file areas -- the credentials,
// the fleet list, the directory walk.
static void rf_tmp_path(char *out, size_t len, const char *suffix) {
	uvhttp_tmp_path(out, len, "rf", suffix);
}


static char *rf_read_file(const char *path) {
	return uvhttp_read_file(path);
}


static void rf_cfg_sanitize(char *dst, size_t dstlen, const char *src) {
	uvhttp_cfg_sanitize(dst, dstlen, src);
}


static long rf_curl_with_cfg(const char *cfg) {
	return uvhttp_curl(cfg);
}


static long rf_curl_logged_with_cfg(const char *cfg, const char *dest_path,
		uint64_t expected) {
	return uvhttp_curl_logged(cfg, dest_path, expected);
}


static void rf_err(char *err, unsigned int err_len, const char *msg) {
	uvhttp_err(err, err_len, msg);
}


/// @brief: Builds the base URL of one of this account's fleet directories.
static void rf_fleet_url(char *dst, size_t dstlen, const char *fleet) {
	char f[REMOTEFILES_FLEET_MAX];
	rf_cfg_sanitize(f, sizeof(f), (fleet != NULL) ? fleet : "");
	snprintf(dst, dstlen, "%s/%s", rf_url, f);
}


/// @brief: Emits the curl config lines every request here shares: the timeouts
/// and this account's Basic credentials.
static int rf_cfg_common(char *dst, size_t dstlen, int timeout_s) {
	return uvhttp_cfg_common(dst, dstlen, timeout_s, rf_user, rf_pass);
}


/// @brief: Turns an HTTP status into the reason the caller shows.
static void rf_http_err(long code, const char *what, char *err,
		unsigned int err_len) {
	uvhttp_status_err(code, what, err, err_len);
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
	rf_login_running = true;
	rf_logged_in = false;
	// the fleet indices the listings are kept by belong to the old fleet list
	rf_forget_all();
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
	rf_login_running = false;
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


/// @brief: How many directories are asked for in one curl run.
///
/// The walk used to run one curl process per directory, and with a couple of
/// dozen directories almost all of the wait was TCP connections and TLS
/// handshakes -- the listings themselves are a few hundred bytes each. One
/// process fetching a batch keeps the connection open between them, which is
/// the whole of the difference.
///
/// Sixteen rather than "all of them": the config file is built in one buffer,
/// and a fleet with hundreds of directories should not need a buffer sized for
/// its worst case.
#define RF_BATCH			16

/// @brief: How long a directory path relative to its fleet may be.
#define RF_REL_MAX			512


/// @brief: One directory waiting to be listed.
typedef struct {
	uint8_t fleet;
	// path relative to the fleet; "" is the fleet's own folder
	char rel[RF_REL_MAX];
} rf_dir_st;


/// @brief: Fetches up to *n* directory listings in one curl run.
///
/// @param bodies: filled with each listing's body, or NULL where the fetch
/// failed. The caller frees every non-NULL one.
/// @param codes: filled with each transfer's HTTP status, 0 where curl never
/// got that far.
static void rf_fetch_dirs(const rf_dir_st *dirs, int n,
		char **bodies, long *codes) {
	char resp_paths[RF_BATCH][256];
	for (int i = 0; i < n; i++) {
		bodies[i] = NULL;
		codes[i] = 0;
		char suffix[32];
		snprintf(suffix, sizeof(suffix), "resp%d", i);
		rf_tmp_path(resp_paths[i], sizeof(resp_paths[i]), suffix);
		remove(resp_paths[i]);
	}

	// Built on the heap: sixteen URLs and sixteen output paths do not belong on
	// the stack of whichever task happens to be listing.
	size_t cfglen = 2048 + (size_t) n * 1200;
	char *cfg = malloc(cfglen);
	if (cfg == NULL) {
		return;
	}
	int w = rf_cfg_common(cfg, cfglen, 30);
	// One status line per transfer, in order, so a failure can be told from an
	// empty directory afterwards.
	w += snprintf(&cfg[w], cfglen - (size_t) w,
			"header = \"Accept: application/json\"\n"
			"write-out = \"%%{http_code}\\n\"\n");
	for (int i = 0; i < n; i++) {
		char base[1024];
		rf_fleet_url(base, sizeof(base), remotefiles_get_fleet(dirs[i].fleet));
		char edir[RF_REL_MAX];
		rf_cfg_sanitize(edir, sizeof(edir), dirs[i].rel);
		// A directory has to be asked for WITH its trailing slash. The server
		// answers a directory addressed without one with a 308 redirect to the
		// slashed form, and this client deliberately does not follow redirects
		// -- there is no `location` in the curl config, because every request
		// carries Basic credentials -- so the fetch would come back 308, the
		// caller would see no listing at all, and the directory would look
		// empty rather than broken. The fleet's own folder passes rel="" and
		// the "%s/" already ends it in a slash.
		w += snprintf(&cfg[w], cfglen - (size_t) w,
				"url = \"%s/%s%s\"\n"
				"output = \"%s\"\n",
				base, edir, (edir[0] != '\0') ? "/" : "", resp_paths[i]);
	}

	long got[RF_BATCH];
	int ncodes = uvhttp_curl_multi(cfg, got, n);
	free(cfg);

	for (int i = 0; i < n; i++) {
		if (i < ncodes) {
			codes[i] = got[i];
			if (codes[i] == 200) {
				bodies[i] = rf_read_file(resp_paths[i]);
			}
			else {
			}
		}
		else {
			// curl stopped before this one; nothing was fetched for it
		}
		remove(resp_paths[i]);
	}
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
static void rf_free_products(rf_store_st *st) {
	if (st->products != NULL) {
		for (uint16_t i = 0; i < st->count; i++) {
			free(st->products[i].versions);
			st->products[i].versions = NULL;
		}
		free(st->products);
		st->products = NULL;
	}
	st->count = 0;
	st->cap = 0;
}


/// @brief: Makes room for one more product. Doubling growth, so a listing costs
/// a handful of reallocs rather than one per directory.
/// @return: false when out of memory, in which case the store is left as it was.
static bool rf_products_reserve(rf_store_st *st) {
	bool ret = true;
	if (st->count >= st->cap) {
		uint16_t cap = (st->cap == 0) ? 8 : (uint16_t) (st->cap * 2);
		remotefiles_product_st *n = realloc(st->products,
				(size_t) cap * sizeof(*n));
		if (n == NULL) {
			ret = false;
		}
		else {
			st->products = n;
			st->cap = cap;
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
static remotefiles_product_st *rf_new_product(rf_store_st *st,
		uint8_t fleet_i, const char *rel) {
	const char *fleet = remotefiles_get_fleet(fleet_i);
	remotefiles_product_st *p = NULL;
	if (rf_products_reserve(st)) {
		p = &st->products[st->count];
		memset(p, 0, sizeof(*p));
		p->fleet = fleet_i;
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
		// "uv0d/rev2" reads as what it is. The fleet's own folder is named after
		// the fleet - it is already the fleet. The fleet is not prefixed onto the
		// others: the panel shows one fleet per tab, so the name would repeat
		// what the tab above it says.
		if (rel[0] == '\0') {
			strncpy(p->name, fleet, sizeof(p->name) - 1);
		}
		else {
			strncpy(p->name, rel, sizeof(p->name) - 1);
		}
		st->count++;
	}
	return p;
}


/// @brief: Alphabetical order of two names, the way a file browser lists them:
/// case does not decide it ("Readme" sits between "parameters" and "uv0d"),
/// and only breaks a tie, so the order is still total and repeatable.
static int rf_cmp_name(const char *a, const char *b) {
	int ret = strcasecmp(a, b);
	if (ret == 0) {
		ret = strcmp(a, b);
	}
	else {
	}
	return ret;
}


/// @brief: qsort predicate ordering a product's versions by file name.
static int rf_cmp_version(const void *a, const void *b) {
	const remotefiles_version_st *va = a;
	const remotefiles_version_st *vb = b;
	return rf_cmp_name(va->version, vb->version);
}


/// @brief: qsort predicate ordering products by their path.
///
/// Within a fleet only: the panel puts one fleet on a tab of its own, so
/// products of different fleets are never on screen together and interleaving
/// them would only scatter each tab's rows through the array.
static int rf_cmp_product(const void *a, const void *b) {
	const remotefiles_product_st *pa = a;
	const remotefiles_product_st *pb = b;
	int ret;
	if (pa->fleet != pb->fleet) {
		ret = (pa->fleet < pb->fleet) ? -1 : 1;
	}
	else {
		ret = rf_cmp_name(pa->id, pb->id);
	}
	return ret;
}


/// @brief: Puts the whole listing in the order it is shown in: alphabetical,
/// both the files inside a product and the products themselves.
///
/// Done once here rather than in the panel, so that every reader of the listing
/// sees the same order and the indices the panel hands back (which product,
/// which version) keep meaning what they meant when the rows were built.
static void rf_sort_products(rf_store_st *st) {
	for (uint16_t i = 0; i < st->count; i++) {
		if (st->products[i].version_count > 1) {
			qsort(st->products[i].versions, st->products[i].version_count,
					sizeof(st->products[i].versions[0]), &rf_cmp_version);
		}
		else {
		}
	}
	if (st->count > 1) {
		qsort(st->products, st->count, sizeof(st->products[0]),
				&rf_cmp_product);
	}
	else {
	}
}


/// @brief: Reads one directory's listing: its files become a product, and its
/// subdirectories are appended to *next* to be fetched in the round after this.
///
/// Two passes over the listing on purpose. Files first, so a directory's own
/// product is created before the products of anything nested inside it;
/// subdirectories second.
///
/// Every subdirectory becomes a product, whether it holds any files or not: an
/// empty directory is still something on the server, and leaving it out made
/// the panel's tree disagree with what the server holds. Only the fleet's own
/// folder is created lazily, on its first file, because the panel shows that
/// folder as a tab rather than as a directory, and a row saying the tab itself
/// holds no files says nothing.
///
/// @return: false when the body was not a listing at all, which the caller only
/// cares about for a fleet's own root.
static bool rf_read_listing(rf_store_st *st, const rf_dir_st *dir,
		const char *body,
		bool want_children, rf_dir_st **next, uint16_t *next_count,
		uint16_t *next_cap) {
	bool ret = false;
	char *wrap = NULL;
	// An empty directory is listed as null (the server's empty list) or as [],
	// neither of which the reader takes for an array; it is a listing all the
	// same, and of an empty fleet the whole of it.
	const char *b = body;
	while ((*b == ' ') || (*b == '\t') || (*b == '\r') || (*b == '\n')) {
		b++;
	}
	size_t bl = strlen(b);
	while ((bl > 0) && ((b[bl - 1] == ' ') || (b[bl - 1] == '\t') ||
			(b[bl - 1] == '\r') || (b[bl - 1] == '\n'))) {
		bl--;
	}
	bool empty = ((bl == 4) && (strncmp(b, "null", 4) == 0)) ||
			((bl == 2) && (strncmp(b, "[]", 2) == 0));
	parser_node_st root = empty ? parser_node_invalid() :
			rf_parse_listing(body, &wrap);
	if (empty) {
		ret = true;
		if (dir->rel[0] != '\0') {
			ret = (rf_new_product(st, dir->fleet, dir->rel) != NULL);
		}
		else {
		}
	}
	else if (parser_node_is_valid(root) &&
			(parser_get_type(root) == PARSER_ARRAY)) {
		ret = true;
		unsigned int n = parser_array_get_size(root);
		remotefiles_product_st *p = NULL;
		unsigned int i;

		if (dir->rel[0] != '\0') {
			p = rf_new_product(st, dir->fleet, dir->rel);
		}
		else {
		}

		for (i = 0; i < n; i++) {
			parser_node_st e = parser_array_at(root, i);
			if (!parser_node_is_valid(e) || rf_entry_is_dir(e)) {
				continue;
			}
			if (p == NULL) {
				p = rf_new_product(st, dir->fleet, dir->rel);
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

		if (want_children) {
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
				if (*next_count >= *next_cap) {
					uint16_t cap = (*next_cap == 0) ? 16 :
							(uint16_t) (*next_cap * 2);
					rf_dir_st *grown = realloc(*next, (size_t) cap * sizeof(**next));
					if (grown == NULL) {
						break;
					}
					*next = grown;
					*next_cap = cap;
				}
				rf_dir_st *d = &(*next)[*next_count];
				d->fleet = dir->fleet;
				// A path that would not fit is dropped rather than truncated:
				// a truncated path names a different directory, and asking the
				// server for that is worse than admitting this one is too deep
				// to reach.
				int need = (dir->rel[0] != '\0') ?
						snprintf(d->rel, sizeof(d->rel), "%.*s/%s",
								(int) (sizeof(d->rel) - 2), dir->rel, dname) :
						snprintf(d->rel, sizeof(d->rel), "%s", dname);
				if ((need > 0) && ((size_t) need < sizeof(d->rel))) {
					(*next_count)++;
				}
				else {
				}
			}
		}
		else {
			// at the depth limit; whatever is below stays unlisted
		}
	}
	else {
	}
	free(wrap);
	return ret;
}


/// @brief: Lists one fleet by walking its listings, a level of the directory
/// tree at a time.
///
/// What a fleet is listed with when the server has no file index for it (a
/// host from before the indexes, or one whose index is unreadable). Breadth
/// first, and deliberately: every directory at one depth is fetched in one
/// curl run, so the walk costs a connection per level rather than one per
/// directory.
///
/// @return: false when the fleet's own folder could not be listed. A
/// subdirectory that fails is skipped quietly: one unreadable folder must not
/// blank the whole tab.
static bool rf_walk_fleet(rf_store_st *st, uint8_t fleet,
		char *err, unsigned int err_len) {
	bool ret = false;

	rf_dir_st *level = malloc(sizeof(*level));
	uint16_t level_count = 0;
	rf_dir_st *next = NULL;
	uint16_t next_count = 0;
	uint16_t next_cap = 0;

	if (level == NULL) {
		rf_err(err, err_len, "Not enough memory to list the files.");
		return false;
	}
	// the first level is the fleet's own folder
	level[0].fleet = fleet;
	level[0].rel[0] = '\0';
	level_count = 1;

	for (unsigned int depth = 0; (depth < RF_MAX_DEPTH) && (level_count > 0);
			depth++) {
		next_count = 0;
		for (uint16_t off = 0; off < level_count; off += RF_BATCH) {
			int n = (int) ((level_count - off > RF_BATCH) ?
					RF_BATCH : (level_count - off));
			char *bodies[RF_BATCH];
			long codes[RF_BATCH];
			rf_fetch_dirs(&level[off], n, bodies, codes);

			for (int i = 0; i < n; i++) {
				const rf_dir_st *d = &level[off + i];
				bool ok = false;
				if ((codes[i] == 200) && (bodies[i] != NULL)) {
					ok = rf_read_listing(st, d, bodies[i],
							(depth + 1) < RF_MAX_DEPTH,
							&next, &next_count, &next_cap);
				}
				else {
				}
				free(bodies[i]);

				if (d->rel[0] != '\0') {
				}
				else if (ok) {
					ret = true;
				}
				else if (codes[i] == 200) {
					rf_err(err, err_len,
							"The server did not answer with a file listing.");
				}
				else {
					rf_http_err(codes[i], "listing files", err, err_len);
				}
			}
		}

		// the directories just discovered become the next level
		free(level);
		level = next;
		level_count = next_count;
		next = NULL;
		next_count = 0;
		next_cap = 0;
	}
	free(level);
	free(next);
	return ret;
}


/// @brief: Fetches the fleet's file index into *body* (the caller frees it).
/// @return: the HTTP status, 0 when the server could not be reached.
static long rf_fetch_index(uint8_t fleet, char **body) {
	*body = NULL;
	char resp_path[256];
	rf_tmp_path(resp_path, sizeof(resp_path), "index");
	remove(resp_path);

	char base[1024];
	rf_fleet_url(base, sizeof(base), remotefiles_get_fleet(fleet));
	char cfg[4096];
	int w = rf_cfg_common(cfg, sizeof(cfg), 30);
	snprintf(&cfg[w], sizeof(cfg) - (size_t) w,
			"url = \"%s/" RF_INDEX_NAME "\"\n"
			"output = \"%s\"\n"
			"write-out = \"%%{http_code}\"\n",
			base, resp_path);
	long code = rf_curl_with_cfg(cfg);
	if (code == 200) {
		*body = rf_read_file(resp_path);
	}
	else {
	}
	remove(resp_path);
	return code;
}


/// @brief: The product for directory *rel* of *fleet* in *st*, created when
/// there is none yet. *hint* is the product found last, which is usually the
/// one asked for again: the index lists a directory's files right after it.
static remotefiles_product_st *rf_index_product(rf_store_st *st,
		uint8_t fleet, const char *rel, uint16_t *hint) {
	remotefiles_product_st *ret = NULL;
	char id[sizeof(ret->id)];
	const char *fname = remotefiles_get_fleet(fleet);
	if (rel[0] != '\0') {
		snprintf(id, sizeof(id), "%s/%s", fname, rel);
	}
	else {
		snprintf(id, sizeof(id), "%s", fname);
	}
	if ((*hint < st->count) && (strcmp(st->products[*hint].id, id) == 0)) {
		ret = &st->products[*hint];
	}
	else {
		for (uint16_t i = st->count; i > 0; i--) {
			if (strcmp(st->products[i - 1].id, id) == 0) {
				ret = &st->products[i - 1];
				*hint = (uint16_t) (i - 1);
				break;
			}
			else {
			}
		}
	}
	if (ret == NULL) {
		ret = rf_new_product(st, fleet, rel);
		if (ret != NULL) {
			*hint = (uint16_t) (st->count - 1);
		}
		else {
		}
	}
	else {
	}
	return ret;
}


/// @brief: Reads a fleet's file index (see uv3b_iotbrkr's uvfilesctl):
///
///   {"fleet":"acme","generated":"…","entries":[
///    {"path":"uv07","is_dir":true,"size":0,"mod_time":"…"},
///    {"path":"uv07/uv07_300.uvdev","is_dir":false,"size":123,"mod_time":"…"}]}
///
/// into the same products the walk makes: every directory one, and the files
/// in the fleet's own folder one named after the fleet, made only when there
/// is such a file.
///
/// The entries are stepped through as siblings rather than by index: finding
/// the n'th element scans the array from its start, which over an index of a
/// few thousand entries would be the slowest part of the whole listing.
///
/// @return: false when the body is not an index at all.
static bool rf_read_index(rf_store_st *st, uint8_t fleet, char *body) {
	bool ret = false;
	parser_node_st root = parser_read_buffer(body, strlen(body),
			PARSER_FORMAT_JSON);
	parser_node_st arr = parser_node_is_valid(root) ?
			parser_find_child(root, "entries") : parser_node_invalid();
	if (parser_node_is_valid(arr) && (parser_get_type(arr) == PARSER_ARRAY)) {
		ret = true;
		unsigned int n = parser_array_get_size(arr);
		parser_node_st e = (n > 0) ? parser_array_at(arr, 0) :
				parser_node_invalid();
		uint16_t hint = 0;
		for (unsigned int i = 0; (i < n) && parser_node_is_valid(e); i++) {
			char path[RF_REL_MAX] = { '\0' };
			parser_node_st c = parser_find_child(e, "path");
			if (parser_node_is_valid(c)) {
				parser_get_string(c, path, sizeof(path));
			}
			else {
			}

			if (path[0] == '\0') {
			}
			else if (rf_entry_is_dir(e)) {
				if (rf_index_product(st, fleet, path, &hint) == NULL) {
					ret = false;
				}
				else {
				}
			}
			else {
				// the directory part of the path is the product, the rest
				// the file
				char *slash = strrchr(path, '/');
				const char *name = path;
				const char *rel = "";
				if (slash != NULL) {
					*slash = '\0';
					name = slash + 1;
					rel = path;
				}
				else {
				}
				remotefiles_product_st *p =
						rf_index_product(st, fleet, rel, &hint);
				remotefiles_version_st *v = NULL;
				if ((p == NULL) || !rf_versions_reserve(p)) {
					ret = false;
				}
				else {
					v = &p->versions[p->version_count];
					memset(v, 0, sizeof(*v));
					// A path that would not fit is left out rather than cut:
					// cut, it names some other file, and that is what the
					// Download button would fetch.
					int need = snprintf(v->path, sizeof(v->path), "%s/%.*s",
							p->id, (int) (sizeof(v->path) - 1), name);
					if ((need < 0) || ((size_t) need >= sizeof(v->path))) {
						v = NULL;
					}
					else {
					}
				}
				if (v != NULL) {
					snprintf(v->version, sizeof(v->version), "%.*s",
							(int) (sizeof(v->version) - 1), name);
					if (parser_node_is_valid(c = parser_find_child(e, "size"))) {
						v->size = (uint64_t) parser_get_int(c);
					}
					else {
					}
					if (parser_node_is_valid(c = parser_find_child(e, "mod_time"))) {
						parser_get_string(c, v->modified, sizeof(v->modified));
						// the date alone is what the list shows
						strncpy(v->released, v->modified, 10);
						v->released[10] = '\0';
					}
					else {
					}
					p->version_count++;
				}
			}

			if (!ret) {
				// out of memory: a partial list would look like the whole one
				break;
			}
			else if (((i + 1) < n) && !parser_get_next_sibling(e, &e)) {
				break;
			}
			else {
			}
		}
	}
	else {
	}
	return ret;
}


/// @brief: Lists *fleet* into *st*: with its file index when the server has
/// one, by walking its listings when it does not.
static bool rf_list_into(rf_store_st *st, uint8_t fleet, bool *indexed,
		char *err, unsigned int err_len) {
	bool ret = false;
	*indexed = false;
	char *body = NULL;
	long code = rf_fetch_index(fleet, &body);
	if ((code == 200) && (body != NULL) && rf_read_index(st, fleet, body)) {
		*indexed = true;
		ret = true;
	}
	else if ((code == 200) || (code == 404)) {
		// No index (404), or one that could not be read: the listings say the
		// same, a request per level instead of one.
		rf_free_products(st);
		ret = rf_walk_fleet(st, fleet, err, err_len);
	}
	else {
		rf_http_err(code, "listing files", err, err_len);
	}
	free(body);

	if (ret) {
		rf_sort_products(st);
	}
	else {
		rf_free_products(st);
	}
	return ret;
}


/// @brief: Stores the listing of *fleet* made in *st*, unless the lists have
/// been forgotten since it was started in generation *gen* -- then it belongs
/// to a session or a fleet list that is gone, and is thrown away.
static void rf_commit(uint8_t fleet, uint32_t gen, rf_store_st *st, bool ok,
		bool indexed, const char *err) {
	rf_store_st old = { 0 };
	uv_enter_critical();
	if (gen == rf_generation) {
		old = rf_lists[fleet];
		rf_lists[fleet] = *st;
		rf_indexed[fleet] = indexed;
		snprintf(rf_list_err[fleet], sizeof(rf_list_err[fleet]), "%s",
				(err != NULL) ? err : "");
		// last, so that a reader who sees READY sees the products too
		__sync_synchronize();
		rf_state[fleet] = ok ? REMOTEFILES_FLEET_READY :
				REMOTEFILES_FLEET_FAILED;
	}
	else {
		old = *st;
	}
	uv_exit_critical();
	rf_free_products(&old);
}


/// @brief: Forgets every fleet's listing and the ones waiting to be made.
static void rf_forget_all(void) {
	rf_store_st old[REMOTEFILES_MAX_FLEETS];
	uv_enter_critical();
	rf_generation++;
	rf_pending = 0;
	rf_wanted = -1;
	for (uint8_t i = 0; i < REMOTEFILES_MAX_FLEETS; i++) {
		old[i] = rf_lists[i];
		memset(&rf_lists[i], 0, sizeof(rf_lists[i]));
		rf_state[i] = REMOTEFILES_FLEET_UNLISTED;
		rf_indexed[i] = false;
		rf_list_err[i][0] = '\0';
	}
	uv_exit_critical();
	for (uint8_t i = 0; i < REMOTEFILES_MAX_FLEETS; i++) {
		rf_free_products(&old[i]);
	}
}


void remotefiles_forget_lists(void) {
	rf_forget_all();
}


bool remotefiles_list_fleet(uint8_t fleet, char *err, unsigned int err_len) {
	bool ret = false;
	if (!rf_logged_in) {
		rf_err(err, err_len, "Not logged in.");
	}
	else if (fleet >= rf_fleet_count) {
		rf_err(err, err_len, "No such fleet.");
	}
	else {
		uint32_t gen = rf_generation;
		rf_state[fleet] = REMOTEFILES_FLEET_LOADING;
		rf_store_st st = { 0 };
		bool indexed = false;
		char e[256] = { '\0' };
		ret = rf_list_into(&st, fleet, &indexed, e, sizeof(e));
		rf_commit(fleet, gen, &st, ret, indexed, e);
		rf_err(err, err_len, e);
	}
	return ret;
}


// Task body: lists the fleets asked for with remotefiles_list_fleet_async(),
// the one asked for last first, until none is left waiting.
static void rf_list_task(void *ptr) {
	(void) ptr;
	while (true) {
		int f = -1;
		uv_enter_critical();
		uint32_t gen = rf_generation;
		if (rf_pending == 0) {
			rf_list_task_running = false;
		}
		else if ((rf_wanted >= 0) && (rf_pending & (1u << rf_wanted))) {
			f = rf_wanted;
		}
		else {
			f = __builtin_ctz(rf_pending);
		}
		if (f >= 0) {
			rf_pending &= ~(1u << f);
		}
		else {
		}
		uv_exit_critical();
		if (f < 0) {
			break;
		}
		else {
			rf_store_st st = { 0 };
			bool indexed = false;
			char e[256] = { '\0' };
			bool ok = rf_list_into(&st, (uint8_t) f, &indexed, e, sizeof(e));
			rf_commit((uint8_t) f, gen, &st, ok, indexed, e);
		}
	}
	uv_rtos_task_delete(NULL);
}


void remotefiles_list_fleet_async(uint8_t fleet) {
	bool start = false;
	if (rf_logged_in && (fleet < rf_fleet_count)) {
		uv_enter_critical();
		if ((rf_state[fleet] == REMOTEFILES_FLEET_UNLISTED) ||
				(rf_state[fleet] == REMOTEFILES_FLEET_FAILED)) {
			rf_state[fleet] = REMOTEFILES_FLEET_LOADING;
			rf_list_err[fleet][0] = '\0';
			rf_pending |= (1u << fleet);
		}
		else {
		}
		if (rf_pending & (1u << fleet)) {
			// the tab on screen goes ahead of the ones asked for before it
			rf_wanted = fleet;
		}
		else {
		}
		if ((rf_pending != 0) && !rf_list_task_running) {
			rf_list_task_running = true;
			start = true;
		}
		else {
		}
		uv_exit_critical();
	}
	else {
	}
	if (start) {
		uv_rtos_task_create(&rf_list_task, "rf_list",
				UV_RTOS_MIN_STACK_SIZE * 10, NULL,
				UV_RTOS_IDLE_PRIORITY + 1, NULL);
	}
	else {
	}
}


remotefiles_fleet_state_e remotefiles_get_fleet_state(uint8_t fleet,
		char *err, unsigned int err_len) {
	remotefiles_fleet_state_e ret = (fleet < rf_fleet_count) ?
			rf_state[fleet] : REMOTEFILES_FLEET_FAILED;
	if (ret == REMOTEFILES_FLEET_FAILED) {
		__sync_synchronize();
		rf_err(err, err_len, (fleet < rf_fleet_count) ?
				rf_list_err[fleet] : "No such fleet.");
	}
	else {
	}
	return ret;
}


bool remotefiles_fleet_is_indexed(uint8_t fleet) {
	return (fleet < rf_fleet_count) &&
			(rf_state[fleet] == REMOTEFILES_FLEET_READY) && rf_indexed[fleet];
}


bool remotefiles_download(const char *path, const char *dest_path,
		uint64_t size, char *err, unsigned int err_len) {
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

		// The transfer is the one thing here that takes long enough for the user
		// to wonder whether anything is happening, so it says what it is doing on
		// stdout - which is the terminal, and the UI's log view.
		const char *base = strrchr(path, '/');
		base = (base != NULL) ? (base + 1) : path;
		if (size > 0) {
			printf("Downloading '%s' (%llu KB) to '%s'...\n", base,
					(unsigned long long) (size / 1024u), dest_path);
		}
		else {
			printf("Downloading '%s' to '%s'...\n", base, dest_path);
		}
		fflush(stdout);

		long code = rf_curl_logged_with_cfg(cfg, dest_path, size);

		if (code == 200) {
			printf("Downloaded '%s'.\n", dest_path);
			fflush(stdout);
			ret = true;
		}
		else {
			rf_http_err(code, "downloading the file", err, err_len);
			remove(dest_path);
			printf("Download of '%s' failed: %s\n", base,
					(err != NULL) ? err : "");
			fflush(stdout);
		}
	}
	return ret;
}


// The asynchronous download's job and its outcome. One at a time, so a single
// set of these serves: the task is started by remotefiles_download_async() and
// read back by remotefiles_download_result() once it has finished.
static char rf_async_path[512];
static char rf_async_dest[1024];
static uint64_t rf_async_size;
static bool rf_async_finished = true;
static bool rf_async_ok;
static char rf_async_err[256];


// Task body: the transfer, off the caller's thread. Everything it says goes to
// stdout, which is where the user reads it - in the terminal, and in the UI's
// log view, which keeps updating because this is not the UI's own task.
static void rf_download_task(void *ptr) {
	(void) ptr;
	rf_async_err[0] = '\0';
	rf_async_ok = remotefiles_download(rf_async_path, rf_async_dest,
			rf_async_size, rf_async_err, sizeof(rf_async_err));
	rf_async_finished = true;
	uv_rtos_task_delete(NULL);
}


void remotefiles_download_async(const char *path, const char *dest_path,
		uint64_t size) {
	if (!rf_async_finished) {
		// a download is already running; starting a second one would have the
		// two of them writing over each other's job here
	}
	else {
		strncpy(rf_async_path, (path != NULL) ? path : "",
				sizeof(rf_async_path) - 1);
		rf_async_path[sizeof(rf_async_path) - 1] = '\0';
		strncpy(rf_async_dest, (dest_path != NULL) ? dest_path : "",
				sizeof(rf_async_dest) - 1);
		rf_async_dest[sizeof(rf_async_dest) - 1] = '\0';
		rf_async_size = size;
		rf_async_ok = false;
		// marked in-progress before the task starts, so a caller can poll
		// immediately
		rf_async_finished = false;
		uv_rtos_task_create(&rf_download_task, "rf_download",
				UV_RTOS_MIN_STACK_SIZE * 5, NULL,
				UV_RTOS_IDLE_PRIORITY + 1, NULL);
	}
}


bool remotefiles_download_is_finished(void) {
	return rf_async_finished;
}


bool remotefiles_download_result(char *dest, unsigned int dest_len,
		char *err, unsigned int err_len) {
	if ((dest != NULL) && (dest_len > 0)) {
		snprintf(dest, dest_len, "%s", rf_async_ok ? rf_async_dest : "");
	}
	if ((err != NULL) && (err_len > 0)) {
		snprintf(err, err_len, "%s", rf_async_ok ? "" : rf_async_err);
	}
	return rf_async_ok;
}


uint16_t remotefiles_get_product_count(uint8_t fleet) {
	return ((fleet < rf_fleet_count) &&
			(rf_state[fleet] == REMOTEFILES_FLEET_READY)) ?
			rf_lists[fleet].count : 0;
}


const remotefiles_product_st *remotefiles_get_product(uint8_t fleet,
		uint16_t index) {
	return (index < remotefiles_get_product_count(fleet)) ?
			&rf_lists[fleet].products[index] : NULL;
}


bool remotefiles_is_logged_in(void) {
	return rf_logged_in;
}


bool remotefiles_login_is_running(void) {
	return rf_login_running;
}


void remotefiles_logout(void) {
	rf_logged_in = false;
	rf_user[0] = '\0';
	rf_pass[0] = '\0';
	// The list belonged to the session being dropped; holding it would only
	// hand the next caller stale files under credentials that no longer apply.
	rf_forget_all();
}


#else /* CONFIG_TARGET_WIN: not wired up on the Windows build yet */


bool remotefiles_is_logged_in(void) {
	return false;
}

bool remotefiles_login_is_running(void) {
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

bool remotefiles_list_fleet(uint8_t fleet, char *err, unsigned int err_len) {
	(void) fleet;
	(void) err;
	(void) err_len;
	return false;
}

void remotefiles_list_fleet_async(uint8_t fleet) {
	(void) fleet;
}

remotefiles_fleet_state_e remotefiles_get_fleet_state(uint8_t fleet,
		char *err, unsigned int err_len) {
	(void) fleet;
	(void) err;
	(void) err_len;
	return REMOTEFILES_FLEET_FAILED;
}

bool remotefiles_fleet_is_indexed(uint8_t fleet) {
	(void) fleet;
	return false;
}

void remotefiles_forget_lists(void) {
}

uint16_t remotefiles_get_product_count(uint8_t fleet) {
	(void) fleet;
	return 0;
}

const remotefiles_product_st *remotefiles_get_product(uint8_t fleet,
		uint16_t index) {
	(void) fleet;
	(void) index;
	return NULL;
}

void remotefiles_download_async(const char *path, const char *dest_path,
		uint64_t size) {
	(void) path;
	(void) dest_path;
	(void) size;
}

bool remotefiles_download_is_finished(void) {
	return true;
}

bool remotefiles_download_result(char *dest, unsigned int dest_len,
		char *err, unsigned int err_len) {
	if ((dest != NULL) && (dest_len > 0)) {
		dest[0] = '\0';
	}
	if ((err != NULL) && (err_len > 0)) {
		strncpy(err, "The server file browser is not available in the Windows "
				"build yet.", err_len - 1);
		err[err_len - 1] = '\0';
	}
	return false;
}

bool remotefiles_download(const char *path, const char *dest_path,
		uint64_t size, char *err, unsigned int err_len) {
	(void) path;
	(void) dest_path;
	(void) size;
	(void) err;
	(void) err_len;
	return false;
}


#endif
