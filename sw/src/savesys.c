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


#include "savesys.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <uv_json.h>
#include <uv_rtos.h>
#include "main.h"
#include "system.h"
#include "saveparam.h"
#include "ui/uvui.h"


// <windows.h> defines ERROR as 0; undef so the colored-print macro can be defined.
#undef ERROR
#define ERROR(str, ...) printf(PRINT_BOLDRED str PRINT_RESET, __VA_ARGS__)
#define ERRORSTR(str) printf(PRINT_BOLDRED str PRINT_RESET)
#define WARNING(str, ...) printf(PRINT_BOLDYELLOW str PRINT_RESET, __VA_ARGS__)


// True once an asynchronous save has finished (idle by default).
static volatile bool finished = true;
// Destination .uvsys path for the asynchronous save.
static char out_file[1024];


bool savesys_is_finished(void) {
	return finished;
}


// Sets the log frame title (no-op when the UI is not compiled in).
static void set_title(const char *title) {
#if CONFIG_UI
	uvui_set_log_title(title);
#else
	(void) title;
#endif
}

static void reset_title(void) {
#if CONFIG_UI
	uvui_reset_log_title();
#else
#endif
}


// Builds a filesystem-safe token from *src* into *dst*: alphanumerics, '_', '-'
// and '.' are kept, spaces become '_', everything else is dropped. Falls back to
// "device" when nothing usable remains.
static void sanitize(const char *src, char *dst, size_t len) {
	size_t j = 0;
	for (size_t i = 0; (src[i] != '\0') && (j + 1 < len); i++) {
		char c = src[i];
		if (isalnum((unsigned char) c) || (c == '_') || (c == '-') ||
				(c == '.')) {
			dst[j++] = c;
		}
		else if (c == ' ') {
			dst[j++] = '_';
		}
		else {
			// drop anything else
		}
	}
	dst[j] = '\0';
	if (j == 0) {
		strncpy(dst, "device", len - 1);
		dst[len - 1] = '\0';
	}
}


bool savesys_save(const char *file) {
	bool ret = false;
	if ((file == NULL) || (strlen(file) == 0)) {
		ERRORSTR("ERROR: savesys_save: no output file given.\n");
		return false;
	}

#if CONFIG_TARGET_WIN
	ERRORSTR("ERROR: writing .uvsys packages is not supported on Windows.\n");
#else
	// ensure a .uvsys extension on the output path
	char outpath[1024];
	strncpy(outpath, file, sizeof(outpath) - 1);
	outpath[sizeof(outpath) - 1] = '\0';
	if ((strlen(outpath) < 6) ||
			(strcmp(outpath + strlen(outpath) - 6, ".uvsys") != 0)) {
		strncat(outpath, ".uvsys", sizeof(outpath) - strlen(outpath) - 1);
	}

	system_st *sys = &dev.system;
	uv_can_set_up(false);

	// staging directory the package contents are assembled in before zipping
	char stage[] = "/tmp/uvcan_uvsys.XXXXXX";
	if (mkdtemp(stage) == NULL) {
		ERRORSTR("ERROR: failed to create a temporary directory for the system "
				"package.\n");
		return false;
	}

	printf("Saving system configuration to '%s'\n", outpath);
	fflush(stdout);

	// count the devices we will read parameters from (operational + configured),
	// so the progress can be shown as "device i/N"
	// only Usevolt, non-offline, configured devices contribute to the system
	// configuration; count them for the progress indicator
	uint8_t param_total = 0;
	for (uint8_t i = 0; i < system_get_dev_count(sys); i++) {
		device_st *d = system_get_dev(sys, i);
		if ((d->nodeid != 0) && !d->thirdparty &&
				(d->state != DEV_STATE_OFFLINE) && (strlen(d->filepath) != 0)) {
			param_total++;
		}
	}

	// build the uvsys.json manifest while copying/saving the per-device files
	static char json_buffer[65536];
	uv_json_st json;
	uv_errors_e e = ERR_NONE;
	e |= uv_jsonwriter_init(&json, json_buffer, sizeof(json_buffer));
	e |= uv_jsonwriter_begin_array(&json, "DEVS");

	uint8_t param_index = 0;
	// Usevolt devices that were offline, so their data could not be saved
	uint8_t offline_count = 0;
	char cmd[3072];
	for (uint8_t i = 0; i < system_get_dev_count(sys); i++) {
		device_st *d = system_get_dev(sys, i);
		if (d->nodeid == 0) {
			continue;
		}
		// a system configuration holds only Usevolt devices: third-party devices
		// have no .uvdev package or readable parameters
		if (d->thirdparty) {
			continue;
		}
		// only devices currently on the bus contribute data; an offline device's
		// configuration and parameters are intentionally left out (warned below)
		if (d->state == DEV_STATE_OFFLINE) {
			offline_count++;
			continue;
		}
		// without a configuration package there is no object dictionary to read
		// parameters with, and nothing to bundle, so there is nothing to save
		if (strlen(d->filepath) == 0) {
			continue;
		}

		e |= uv_jsonwriter_begin_object(&json);
		char nodeid_str[8];
		snprintf(nodeid_str, sizeof(nodeid_str), "0x%x",
				(unsigned int) d->nodeid);
		e |= uv_jsonwriter_add_string(&json, "NODEID", nodeid_str);

		// copy the device's .uvdev package, renamed <name>_0x<nodeid>.uvdev.
		// d->name may already end with the _0x<nodeid> suffix when the system was
		// loaded from a .uvsys (its packages were renamed this way on a previous
		// save); strip any such trailing suffix first so it is not doubled.
		char suffix[16];
		snprintf(suffix, sizeof(suffix), "_0x%x", (unsigned int) d->nodeid);
		char base[256];
		strncpy(base, d->name, sizeof(base) - 1);
		base[sizeof(base) - 1] = '\0';
		size_t slen = strlen(suffix);
		size_t blen = strlen(base);
		while ((blen >= slen) && (strcmp(base + blen - slen, suffix) == 0)) {
			base[blen - slen] = '\0';
			blen -= slen;
		}
		char uvdev_name[320];
		snprintf(uvdev_name, sizeof(uvdev_name), "%s%s.uvdev", base, suffix);
		printf("Adding configuration package '%s'\n", uvdev_name);
		fflush(stdout);
		snprintf(cmd, sizeof(cmd), "cp \"%s\" \"%s/%s\"",
				d->filepath, stage, uvdev_name);
		if (system(cmd) == 0) {
			e |= uv_jsonwriter_add_string(&json, "UVDEV", uvdev_name);
		}
		else {
			ERROR("ERROR: failed to add the configuration package for "
					"node %s.\n", nodeid_str);
		}

		// read and store parameters over SDO (the device is known to be on the bus)
		param_index++;
		char title[256];
		const char *dname = (strlen(d->devname) > 0) ? d->devname : d->name;
		snprintf(title, sizeof(title),
				"Saving system: parameters from device %u/%u (%s)",
				(unsigned int) param_index, (unsigned int) param_total, dname);
		set_title(title);

		char nametok[160];
		sanitize(dname, nametok, sizeof(nametok));
		char param_name[320];
		snprintf(param_name, sizeof(param_name), "%s_0x%x.json",
				nametok, (unsigned int) d->nodeid);
		char param_path[1400];
		snprintf(param_path, sizeof(param_path), "%s/%s", stage, param_name);

		if (saveparam_save_device(d, param_path)) {
			e |= uv_jsonwriter_add_string(&json, "PARAM", param_name);
		}
		else {
			ERROR("ERROR: failed to read parameters from node %s.\n",
					nodeid_str);
		}

		e |= uv_jsonwriter_end_object(&json);
	}

	e |= uv_jsonwriter_end_array(&json);
	e |= uv_jsonwriter_end(&json, NULL);
	LOG_END();

	// warn about Usevolt devices that were offline and therefore left out of the
	// system configuration entirely (no configuration package, no parameters)
	if (offline_count > 0) {
		WARNING("WARNING: %u device(s) were offline; they were not included in "
				"the system configuration (no parameters saved).\n",
				(unsigned int) offline_count);
		fflush(stdout);
	}

	// write the manifest into the staging directory
	set_title("Saving system: packaging .uvsys file...");
	char manifest[1100];
	snprintf(manifest, sizeof(manifest), "%s/uvsys.json", stage);
	FILE *mf = fopen(manifest, "wb");
	if ((mf == NULL) || (e != ERR_NONE)) {
		ERRORSTR("ERROR: failed to write the uvsys.json manifest.\n");
		if (mf != NULL) {
			fclose(mf);
		}
	}
	else {
		fwrite(json_buffer, 1, strlen(json_buffer), mf);
		fclose(mf);

		// zip the staging directory into the (absolute) output path. Remove any
		// existing file first so zip does not append to a stale archive.
		printf("Packaging the system configuration...\n");
		fflush(stdout);
		snprintf(cmd, sizeof(cmd),
				"rm -f \"%s\"; ( cd \"%s\" && zip -qr \"%s\" . )",
				outpath, stage, outpath);
		if (system(cmd) == 0) {
			printf("System configuration saved to '%s'\n", outpath);
			ret = true;
		}
		else {
			ERRORSTR("ERROR: failed to create the .uvsys archive. Is 'zip' "
					"installed?\n");
		}
		fflush(stdout);
	}

	// clean up the staging directory
	snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", stage);
	if (system(cmd)) {
		// ignore; /tmp is reaped eventually
	}

	reset_title();
#endif

	return ret;
}


/// @brief: Task body running savesys_save() off the UI thread.
static void savesys_task(void *ptr) {
	savesys_save(out_file);
	finished = true;
	uv_rtos_task_delete(NULL);
}


void savesys_save_async(const char *file) {
	strncpy(out_file, (file != NULL) ? file : "", sizeof(out_file) - 1);
	out_file[sizeof(out_file) - 1] = '\0';
	// mark in-progress before the task starts so a caller can poll immediately
	finished = false;
	uv_rtos_task_create(&savesys_task, "savesys_task",
			UV_RTOS_MIN_STACK_SIZE * 5, NULL, UV_RTOS_IDLE_PRIORITY + 1, NULL);
}
