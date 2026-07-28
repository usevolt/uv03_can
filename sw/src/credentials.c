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


#include "credentials.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>


#if CONFIG_TARGET_WIN
#include <direct.h>
#define CRED_MKDIR(path)	_mkdir(path)
#define CRED_SEP			'\\'
#else
#include <sys/stat.h>
#include <sys/types.h>
#define CRED_MKDIR(path)	mkdir((path), 0700)
#define CRED_SEP			'/'
#endif


// Current in-memory credentials, loaded from the shared file by credentials_init()
// and updated (and persisted) by credentials_set_*().
static char cred_username[CREDENTIALS_MAX];
static char cred_password[CREDENTIALS_MAX];
static char cred_url[CREDENTIALS_MAX];

// The fleet account (MQTT broker), kept strictly separate from the file server
// account above and persisted in the same file under "fleet_*" keys.
static char fleet_username[CREDENTIALS_MAX];
static char fleet_password[CREDENTIALS_MAX];
static char fleet_url[CREDENTIALS_MAX];


// Creates *path* and any missing parent directories (best effort, like mkdir -p).
// Existing directories and failures are ignored - the caller's fopen() reports any
// real problem.
static void cred_mkdir_p(const char *path) {
	char tmp[1024];
	strncpy(tmp, path, sizeof(tmp) - 1);
	tmp[sizeof(tmp) - 1] = '\0';
	for (char *p = tmp + 1; *p != '\0'; p++) {
		if ((*p == '/') || (*p == '\\')) {
			char c = *p;
			*p = '\0';
			(void) CRED_MKDIR(tmp);
			*p = c;
		}
	}
	(void) CRED_MKDIR(tmp);
}


// Builds the path of *filename* inside uvcan's per-user configuration directory
// into *out* (size *len*) and makes sure the directory exists. The location is
// per-user and identical for every uvcan install on the machine (see
// credentials.h). Returns false when no home directory is known from the
// environment.
bool credentials_config_path(char *out, size_t len, const char *filename) {
	bool ret = false;
	char dir[900];
#if CONFIG_TARGET_WIN
	const char *base = getenv("APPDATA");
	if ((base == NULL) || (base[0] == '\0')) {
		base = getenv("USERPROFILE");
	}
	if ((base != NULL) && (base[0] != '\0')) {
		snprintf(dir, sizeof(dir), "%s\\uvcan", base);
		ret = true;
	}
#else
	const char *xdg = getenv("XDG_CONFIG_HOME");
	if ((xdg != NULL) && (xdg[0] != '\0')) {
		snprintf(dir, sizeof(dir), "%s/uvcan", xdg);
		ret = true;
	}
	else {
		const char *home = getenv("HOME");
		if ((home != NULL) && (home[0] != '\0')) {
			snprintf(dir, sizeof(dir), "%s/.config/uvcan", home);
			ret = true;
		}
	}
#endif
	if (ret) {
		cred_mkdir_p(dir);
		snprintf(out, len, "%s%c%s", dir, CRED_SEP, filename);
	}
	return ret;
}


// Builds the shared account-file path into *out* (size *len*).
static bool cred_path(char *out, size_t len) {
	return credentials_config_path(out, len, "account.conf");
}


// Writes both accounts to the shared file as "key=value" lines. Returns true on
// success.
static bool cred_write_file(void) {
	bool ret = false;
	char path[1024];
	if (cred_path(path, sizeof(path))) {
		FILE *f = fopen(path, "w");
		if (f != NULL) {
			fprintf(f, "username=%s\n", cred_username);
			fprintf(f, "password=%s\n", cred_password);
			fprintf(f, "url=%s\n", cred_url);
			fprintf(f, "fleet_username=%s\n", fleet_username);
			fprintf(f, "fleet_password=%s\n", fleet_password);
			fprintf(f, "fleet_url=%s\n", fleet_url);
			fclose(f);
			ret = true;
		}
	}
	return ret;
}


// Copies the value part of a "key=value" *line* (everything after the first '=')
// into *out*, dropping the trailing newline. A value may itself contain '=' - only
// the first one separates key from value. Lines with no '=' leave *out* unchanged.
static void cred_parse_value(const char *line, char *out, size_t out_len) {
	const char *eq = strchr(line, '=');
	if (eq != NULL) {
		strncpy(out, eq + 1, out_len - 1);
		out[out_len - 1] = '\0';
		size_t l = strlen(out);
		while ((l > 0) && ((out[l - 1] == '\n') || (out[l - 1] == '\r'))) {
			out[l - 1] = '\0';
			l--;
		}
	}
	else {
		// not a key=value line: ignore it
	}
}


void credentials_init(void) {
	cred_username[0] = '\0';
	cred_password[0] = '\0';
	cred_url[0] = '\0';
	fleet_username[0] = '\0';
	fleet_password[0] = '\0';
	fleet_url[0] = '\0';
	char path[1024];
	if (cred_path(path, sizeof(path))) {
		FILE *f = fopen(path, "r");
		if (f != NULL) {
			char line[CREDENTIALS_MAX + 32];
			while (fgets(line, sizeof(line), f) != NULL) {
				if (strncmp(line, "username=", 9) == 0) {
					cred_parse_value(line, cred_username, sizeof(cred_username));
				}
				else if (strncmp(line, "password=", 9) == 0) {
					cred_parse_value(line, cred_password, sizeof(cred_password));
				}
				else if (strncmp(line, "url=", 4) == 0) {
					cred_parse_value(line, cred_url, sizeof(cred_url));
				}
				else if (strncmp(line, "fleet_username=", 15) == 0) {
					cred_parse_value(line, fleet_username, sizeof(fleet_username));
				}
				else if (strncmp(line, "fleet_password=", 15) == 0) {
					cred_parse_value(line, fleet_password, sizeof(fleet_password));
				}
				else if (strncmp(line, "fleet_url=", 10) == 0) {
					cred_parse_value(line, fleet_url, sizeof(fleet_url));
				}
				else if (strncmp(line, "fleet_name=", 11) == 0) {
					// a fleet name used to be stored here, back when the client
					// had to name its own fleet at login. The server decides
					// that now, so the line is read and dropped rather than
					// tripping the "unknown key" branch on an older file.
				}
				else {
					// unknown key: ignore
				}
			}
			fclose(f);
		}
	}

	// a never-set (or emptied) server address falls back to the Usevolt default,
	// so a fresh install can connect without the user typing an address
	if (cred_url[0] == '\0') {
		strcpy(cred_url, CREDENTIALS_URL_DEFAULT);
	}
	if (fleet_url[0] == '\0') {
		strcpy(fleet_url, CREDENTIALS_FLEET_URL_DEFAULT);
	}
}


const char *credentials_get_username(void) {
	return cred_username;
}


const char *credentials_get_password(void) {
	return cred_password;
}


void credentials_set_username(const char *username) {
	strncpy(cred_username, (username != NULL) ? username : "",
			sizeof(cred_username) - 1);
	cred_username[sizeof(cred_username) - 1] = '\0';
	cred_write_file();
}


void credentials_set_password(const char *password) {
	strncpy(cred_password, (password != NULL) ? password : "",
			sizeof(cred_password) - 1);
	cred_password[sizeof(cred_password) - 1] = '\0';
	cred_write_file();
}


const char *credentials_get_url(void) {
	return cred_url;
}


void credentials_set_url(const char *url) {
	strncpy(cred_url, (url != NULL) ? url : "", sizeof(cred_url) - 1);
	cred_url[sizeof(cred_url) - 1] = '\0';
	cred_write_file();
}


// Shared implementation of the fleet-account setters: copies *value* into *dst*
// and persists the whole file.
static void fleet_set(char *dst, const char *value) {
	strncpy(dst, (value != NULL) ? value : "", CREDENTIALS_MAX - 1);
	dst[CREDENTIALS_MAX - 1] = '\0';
	cred_write_file();
}


const char *credentials_fleet_get_url(void) {
	return fleet_url;
}


const char *credentials_fleet_get_username(void) {
	return fleet_username;
}


const char *credentials_fleet_get_password(void) {
	return fleet_password;
}


void credentials_fleet_set_url(const char *url) {
	fleet_set(fleet_url, url);
}


void credentials_fleet_set_username(const char *username) {
	fleet_set(fleet_username, username);
}


void credentials_fleet_set_password(const char *password) {
	fleet_set(fleet_password, password);
}
