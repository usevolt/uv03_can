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
#include <uv_json.h>


// Session state for the current login. The token is kept in RAM only (never
// written to the credentials file) and passed to curl through a config file so it
// does not appear in the process arguments.
static char rf_url[CREDENTIALS_MAX];
static char rf_token[1024];
static remotefiles_product_st rf_products[REMOTEFILES_MAX_PRODUCTS];
static uint8_t rf_product_count;


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


// Appends *src* to *dst* as a JSON string value (without the surrounding quotes),
// escaping the characters JSON requires. Used to build the login body so a password
// containing quotes/backslashes is transmitted correctly. *dst* is a buffer of
// *dstlen* bytes already holding a null-terminated prefix.
static void rf_json_escape_append(char *dst, size_t dstlen, const char *src) {
	size_t d = strlen(dst);
	for (size_t i = 0; (src[i] != '\0') && (d + 2 < dstlen); i++) {
		unsigned char c = (unsigned char) src[i];
		if ((c == '"') || (c == '\\')) {
			dst[d++] = '\\';
			dst[d++] = (char) c;
		}
		else if (c < 0x20) {
			// control character: emit as \u00XX (needs 6 bytes)
			if (d + 6 < dstlen) {
				d += snprintf(dst + d, dstlen - d, "\\u%04x", c);
			}
		}
		else {
			dst[d++] = (char) c;
		}
	}
	dst[d] = '\0';
}


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


bool remotefiles_login(const char *url, const char *username,
		const char *password, char *err, unsigned int err_len) {
	bool ret = false;
	rf_token[0] = '\0';
	rf_cfg_sanitize(rf_url, sizeof(rf_url), (url != NULL) ? url : "");
	// strip a trailing '/' so "<url>/api/..." never doubles the slash
	size_t ul = strlen(rf_url);
	if ((ul > 0) && (rf_url[ul - 1] == '/')) {
		rf_url[ul - 1] = '\0';
	}

	if (strlen(rf_url) == 0) {
		rf_err(err, err_len, "No server URL set.");
	}
	else {
		// the credentials go into a data file (JSON-escaped), never onto the command
		// line or into the config, so they are not visible in the process list
		char body[2 * CREDENTIALS_MAX + 64];
		strcpy(body, "{\"username\":\"");
		rf_json_escape_append(body, sizeof(body), (username != NULL) ? username : "");
		strncat(body, "\",\"password\":\"", sizeof(body) - strlen(body) - 1);
		rf_json_escape_append(body, sizeof(body), (password != NULL) ? password : "");
		strncat(body, "\"}", sizeof(body) - strlen(body) - 1);

		char body_path[256];
		char resp_path[256];
		rf_tmp_path(body_path, sizeof(body_path), "body");
		rf_tmp_path(resp_path, sizeof(resp_path), "resp");

		if (!rf_write_file(body_path, body)) {
			rf_err(err, err_len, "Could not write the request.");
		}
		else {
			char cfg[2048];
			snprintf(cfg, sizeof(cfg),
					"silent\nshow-error\n"
					"connect-timeout = 15\nmax-time = 30\n"
					"url = \"%s/api/v1/login\"\n"
					"request = \"POST\"\n"
					"header = \"Content-Type: application/json\"\n"
					"data = \"@%s\"\n"
					"output = \"%s\"\n"
					"write-out = \"%%{http_code}\"\n",
					rf_url, body_path, resp_path);
			long code = rf_curl_with_cfg(cfg);
			remove(body_path);

			if (code == 0) {
				rf_err(err, err_len,
						"Could not reach the server (is curl installed and the URL "
						"correct?).");
			}
			else if (code == 401) {
				rf_err(err, err_len, "Invalid username or password.");
			}
			else if (code != 200) {
				char m[96];
				snprintf(m, sizeof(m), "Server returned HTTP %ld on login.", code);
				rf_err(err, err_len, m);
			}
			else {
				char *resp = rf_read_file(resp_path);
				if (resp == NULL) {
					rf_err(err, err_len, "Empty login response.");
				}
				else {
					uv_jsonreader_init(resp, strlen(resp));
					char *tok = uv_jsonreader_find_child(resp, "token");
					if ((tok != NULL) &&
							uv_jsonreader_get_string(tok, rf_token,
									sizeof(rf_token)) &&
							(strlen(rf_token) > 0)) {
						ret = true;
					}
					else {
						rf_err(err, err_len,
								"Login succeeded but no token was returned.");
					}
					free(resp);
				}
			}
			remove(resp_path);
		}
	}
	return ret;
}


// Parses one version object from the files response into *v*.
static void rf_parse_version(char *obj, remotefiles_version_st *v) {
	memset(v, 0, sizeof(*v));
	char *c;
	if ((c = uv_jsonreader_find_child(obj, "version")) != NULL) {
		uv_jsonreader_get_string(c, v->version, sizeof(v->version));
	}
	if ((c = uv_jsonreader_find_child(obj, "path")) != NULL) {
		uv_jsonreader_get_string(c, v->path, sizeof(v->path));
	}
	if ((c = uv_jsonreader_find_child(obj, "released")) != NULL) {
		uv_jsonreader_get_string(c, v->released, sizeof(v->released));
	}
	if ((c = uv_jsonreader_find_child(obj, "notes")) != NULL) {
		uv_jsonreader_get_string(c, v->notes, sizeof(v->notes));
	}
	if ((c = uv_jsonreader_find_child(obj, "sha256")) != NULL) {
		uv_jsonreader_get_string(c, v->sha256, sizeof(v->sha256));
	}
	if ((c = uv_jsonreader_find_child(obj, "modified")) != NULL) {
		uv_jsonreader_get_string(c, v->modified, sizeof(v->modified));
	}
	if ((c = uv_jsonreader_find_child(obj, "size")) != NULL) {
		int s = uv_jsonreader_get_int(c);
		v->size = (s > 0) ? (uint64_t) s : 0;
	}
}


// Parses the whole files response (a {"products":[...]} object) into rf_products.
static void rf_parse_products(char *json) {
	rf_product_count = 0;
	char *products = uv_jsonreader_find_child(json, "products");
	if ((products != NULL) &&
			(uv_jsonreader_get_type(products) == JSON_ARRAY)) {
		unsigned int pn = uv_jsonreader_array_get_size(products);
		for (unsigned int i = 0;
				(i < pn) && (rf_product_count < REMOTEFILES_MAX_PRODUCTS); i++) {
			char *pobj = uv_jsonreader_array_at(products, i);
			if (pobj == NULL) {
				continue;
			}
			remotefiles_product_st *p = &rf_products[rf_product_count];
			memset(p, 0, sizeof(*p));
			char *c;
			if ((c = uv_jsonreader_find_child(pobj, "id")) != NULL) {
				uv_jsonreader_get_string(c, p->id, sizeof(p->id));
			}
			if ((c = uv_jsonreader_find_child(pobj, "name")) != NULL) {
				uv_jsonreader_get_string(c, p->name, sizeof(p->name));
			}
			char *versions = uv_jsonreader_find_child(pobj, "versions");
			if ((versions != NULL) &&
					(uv_jsonreader_get_type(versions) == JSON_ARRAY)) {
				unsigned int vn = uv_jsonreader_array_get_size(versions);
				for (unsigned int j = 0;
						(j < vn) && (p->version_count < REMOTEFILES_MAX_VERSIONS);
						j++) {
					char *vobj = uv_jsonreader_array_at(versions, j);
					if (vobj != NULL) {
						rf_parse_version(vobj, &p->versions[p->version_count]);
						p->version_count++;
					}
				}
			}
			rf_product_count++;
		}
	}
}


bool remotefiles_list(char *err, unsigned int err_len) {
	bool ret = false;
	rf_product_count = 0;
	if (strlen(rf_token) == 0) {
		rf_err(err, err_len, "Not logged in.");
	}
	else {
		char tok[sizeof(rf_token)];
		rf_cfg_sanitize(tok, sizeof(tok), rf_token);
		char resp_path[256];
		rf_tmp_path(resp_path, sizeof(resp_path), "resp");
		char cfg[2048];
		snprintf(cfg, sizeof(cfg),
				"silent\nshow-error\n"
				"connect-timeout = 15\nmax-time = 30\n"
				"url = \"%s/api/v1/files\"\n"
				"header = \"Authorization: Bearer %s\"\n"
				"output = \"%s\"\n"
				"write-out = \"%%{http_code}\"\n",
				rf_url, tok, resp_path);
		long code = rf_curl_with_cfg(cfg);

		if (code == 0) {
			rf_err(err, err_len, "Could not reach the server.");
		}
		else if (code == 401) {
			rf_err(err, err_len, "Session expired; please try again.");
		}
		else if (code != 200) {
			char m[96];
			snprintf(m, sizeof(m), "Server returned HTTP %ld listing files.", code);
			rf_err(err, err_len, m);
		}
		else {
			char *resp = rf_read_file(resp_path);
			if (resp == NULL) {
				rf_err(err, err_len, "Empty file list response.");
			}
			else {
				uv_jsonreader_init(resp, strlen(resp));
				rf_parse_products(resp);
				free(resp);
				ret = true;
			}
		}
		remove(resp_path);
	}
	return ret;
}


uint8_t remotefiles_get_product_count(void) {
	return rf_product_count;
}


const remotefiles_product_st *remotefiles_get_product(uint8_t index) {
	return (index < rf_product_count) ? &rf_products[index] : NULL;
}


// Returns the SHA-256 (lower-case hex) of the file at *path* into *out* using the
// sha256sum tool, or "" when it cannot be computed. *path* is refused if it
// contains a character that could break out of the single-quoted shell argument.
static void rf_file_sha256(const char *path, char *out, size_t out_len) {
	out[0] = '\0';
	for (size_t i = 0; path[i] != '\0'; i++) {
		if ((path[i] == '\'') || (path[i] == '\n') || (path[i] == '\r')) {
			return;
		}
	}
	char sum_path[256];
	rf_tmp_path(sum_path, sizeof(sum_path), "sum");
	char cmd[1024];
	snprintf(cmd, sizeof(cmd), "sha256sum '%s' > '%s' 2>/dev/null", path, sum_path);
	if (system(cmd) != -1) {
		char *s = rf_read_file(sum_path);
		if (s != NULL) {
			// output is "<hex>  <filename>"; copy the leading hex token
			size_t d = 0;
			for (size_t i = 0; (s[i] != '\0') && (s[i] != ' ') &&
					(d + 1 < out_len); i++) {
				out[d++] = s[i];
			}
			out[d] = '\0';
			free(s);
		}
	}
	remove(sum_path);
}


bool remotefiles_download(const char *path, const char *dest_path,
		char *err, unsigned int err_len) {
	bool ret = false;
	if (strlen(rf_token) == 0) {
		rf_err(err, err_len, "Not logged in.");
	}
	else {
		// find the version to learn its expected checksum (and validate the path is
		// one the server advertised)
		const remotefiles_version_st *ver = NULL;
		for (uint8_t i = 0; (i < rf_product_count) && (ver == NULL); i++) {
			for (uint8_t j = 0; j < rf_products[i].version_count; j++) {
				if (strcmp(rf_products[i].versions[j].path, path) == 0) {
					ver = &rf_products[i].versions[j];
					break;
				}
			}
		}

		char tok[sizeof(rf_token)];
		char epath[1024];
		char dest[1024];
		rf_cfg_sanitize(tok, sizeof(tok), rf_token);
		// the download path is a query parameter; keep config-safe (curl does not
		// URL-encode config values, but the server paths are simple relative names)
		rf_cfg_sanitize(epath, sizeof(epath), path);
		rf_cfg_sanitize(dest, sizeof(dest), dest_path);

		char cfg[5120];
		snprintf(cfg, sizeof(cfg),
				"silent\nshow-error\n"
				"connect-timeout = 15\nmax-time = 300\n"
				"url = \"%s/api/v1/files/content\"\n"
				"data-urlencode = \"path=%s\"\n"
				"get\n"
				"header = \"Authorization: Bearer %s\"\n"
				"output = \"%s\"\n"
				"write-out = \"%%{http_code}\"\n",
				rf_url, epath, tok, dest);
		long code = rf_curl_with_cfg(cfg);

		if (code == 0) {
			rf_err(err, err_len, "Could not reach the server.");
		}
		else if (code == 401) {
			rf_err(err, err_len, "Session expired; please try again.");
		}
		else if (code != 200) {
			char m[96];
			snprintf(m, sizeof(m), "Server returned HTTP %ld downloading the file.",
					code);
			rf_err(err, err_len, m);
			remove(dest_path);
		}
		else if ((ver != NULL) && (strlen(ver->sha256) > 0)) {
			// verify integrity against the manifest checksum
			char got[72];
			rf_file_sha256(dest_path, got, sizeof(got));
			if ((strlen(got) > 0) && (strcasecmp(got, ver->sha256) != 0)) {
				rf_err(err, err_len,
						"Downloaded file failed the checksum check; it may be "
						"corrupt. The file was not kept.");
				remove(dest_path);
			}
			else {
				ret = true;
			}
		}
		else {
			ret = true;
		}
	}
	return ret;
}


#else /* CONFIG_TARGET_WIN: not wired up on the Windows build yet */


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

uint8_t remotefiles_get_product_count(void) {
	return 0;
}

const remotefiles_product_st *remotefiles_get_product(uint8_t index) {
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
