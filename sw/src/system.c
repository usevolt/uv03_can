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


#include "system.h"
#include "main.h"
#include "uvdev.h"
#include "db.h"
#include "archive.h"
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <uv_json.h>

// Platform null device, used to silence a child command's stdout.
#if CONFIG_TARGET_WIN
#define NULL_DEVICE		"NUL"
#else
#define NULL_DEVICE		"/dev/null"
#endif


/// @brief: Derives a human readable name from a file path by taking the file
/// name and stripping any directory prefix and file extension.
static void name_from_path(const char *filepath, char *dest, size_t dest_len) {
	// find the start of the file name (after the last '/')
	const char *base = strrchr(filepath, '/');
	base = (base != NULL) ? base + 1 : filepath;

	strncpy(dest, base, dest_len - 1);
	dest[dest_len - 1] = '\0';

	// strip the file extension, if any
	char *dot = strrchr(dest, '.');
	if (dot != NULL) {
		*dot = '\0';
	}
}


void system_reset(system_st *this) {
	memset(this, 0, sizeof(*this));
}


// Temp directories the loaded .uvsys packages were extracted into. Their .uvdev
// files back the loaded devices (device->filepath points into them), so they must
// outlive the devices. Because loading a system file appends to (rather than
// replaces) the device list, several may be alive at once; they are all removed
// at program exit.
#define SYSFILE_TMPDIR_MAX		16
static char sysfile_tmpdirs[SYSFILE_TMPDIR_MAX][1024];
static uint8_t sysfile_tmpdir_count;


// Removes a single extraction directory (best effort).
static void sysfile_rmdir(const char *dir) {
	archive_rmtree(dir);
}


// Removes every tracked extraction directory. Registered with atexit().
static void sysfile_remove_all_tmpdirs(void) {
	for (uint8_t i = 0; i < sysfile_tmpdir_count; i++) {
		sysfile_rmdir(sysfile_tmpdirs[i]);
	}
	sysfile_tmpdir_count = 0;
}


// Remembers a newly created extraction directory so it is cleaned up at exit.
static void sysfile_track_tmpdir(const char *dir) {
	if (sysfile_tmpdir_count < SYSFILE_TMPDIR_MAX) {
		strncpy(sysfile_tmpdirs[sysfile_tmpdir_count], dir, 1023);
		sysfile_tmpdirs[sysfile_tmpdir_count][1023] = '\0';
		sysfile_tmpdir_count++;
	}
}


void system_init_tmp_cleanup(void) {
	// clean exits (the UI closing, exit(0)) remove the active extraction dirs
	atexit(&sysfile_remove_all_tmpdirs);

	// sweep leftover extraction dirs from an earlier run that skipped atexit
	// (a crash, or the HAL's _exit()-based SIGINT handler)
	archive_sweep_stale_tmpdirs();
}


void system_remove_tmpdirs(void) {
	sysfile_remove_all_tmpdirs();
}


// Parses the DEVS array of an already-loaded uvsys.json (*json* is the manifest
// root) and appends one device per entry, each from its UVDEV package (extracted
// under *tmpdir*) and initialized with the entry's NODEID. The entry's PARAM file
// (if any) is recorded on the device. Returns true when a valid DEVS array was
// found (even if empty).
static bool system_parse_sysjson(system_st *this, char *json, const char *tmpdir) {
	bool ret = false;
	char *devs = uv_jsonreader_find_child(json, "DEVS");
	if ((devs == NULL) || (uv_jsonreader_get_type(devs) != JSON_ARRAY)) {
		PRINT("ERROR: the system file has no DEVS array.\n");
	}
	else {
		unsigned int count = uv_jsonreader_array_get_size(devs);
		for (unsigned int i = 0; i < count; i++) {
			char *obj = uv_jsonreader_array_at(devs, i);
			if (obj == NULL) {
				continue;
			}

			// node id: stored as a hexadecimal string ("0x7"), but also accept a
			// plain integer. 0 means "read the default from the UVDEV package".
			uint8_t nodeid = 0;
			char *nid = uv_jsonreader_find_child(obj, "NODEID");
			if (nid != NULL) {
				if (uv_jsonreader_get_type(nid) == JSON_STRING) {
					char buf[16];
					uv_jsonreader_get_string(nid, buf, sizeof(buf));
					nodeid = (uint8_t) strtoul(buf, NULL, 0);
				}
				else if (uv_jsonreader_get_type(nid) == JSON_INT) {
					nodeid = (uint8_t) uv_jsonreader_get_int(nid);
				}
				else {
					// unknown form: leave nodeid at 0 (read from the package)
				}
			}

			// UVDEV package file name, relative to the extracted system package
			char *ud = uv_jsonreader_find_child(obj, "UVDEV");
			if ((ud == NULL) || (uv_jsonreader_get_type(ud) != JSON_STRING)) {
				PRINT("System file device %u has no UVDEV package; skipping it.\n",
						i);
				continue;
			}
			char uvdev_name[512];
			uv_jsonreader_get_string(ud, uvdev_name, sizeof(uvdev_name));
			char uvdev_path[1600];
			snprintf(uvdev_path, sizeof(uvdev_path), "%s/%s", tmpdir, uvdev_name);

			// add the device exactly like --dev does, with the given node id
			if (system_add_device(this, uvdev_path, nodeid)) {
				// record the saved parameter file (PARAM) for this device, if any,
				// so the UI can later offer to load it onto the online device
				device_st *added = &this->devs[this->dev_count - 1];
				added->param_file[0] = '\0';
				char *pf = uv_jsonreader_find_child(obj, "PARAM");
				if ((pf != NULL) &&
						(uv_jsonreader_get_type(pf) == JSON_STRING)) {
					char param_name[512];
					uv_jsonreader_get_string(pf, param_name, sizeof(param_name));
					snprintf(added->param_file, sizeof(added->param_file),
							"%s/%s", tmpdir, param_name);
				}
				PRINT("Loaded device '%s' (node 0x%x) from the system file.\n",
						uvdev_name, (unsigned int) nodeid);
			}
			else {
				PRINT("System is full; ignoring the remaining devices.\n");
				break;
			}
		}
		ret = true;
	}
	return ret;
}


bool system_set_file(system_st *this, const char *filepath) {
	bool ret = false;

	// NOTE: the existing devices are intentionally NOT cleared. The system file's
	// devices are appended to whatever is already loaded.

	// .uvsys is a plain zip archive; extract it into a fresh temporary directory
	char tmpdir[1024];
	bool extracted = false;
	if (!archive_mktempdir("uvcan_uvsys", tmpdir, sizeof(tmpdir))) {
		PRINT("Failed to create a temporary directory for '%s'.\n", filepath);
	}
	else {
		extracted = true;
		if (!archive_extract(filepath, tmpdir)) {
			PRINT("Failed to extract '%s'. Is it a valid .uvsys package and is "
					"the extraction tool (unzip / tar) available?\n", filepath);
		}
		else {
			// read the uvsys.json manifest
			char manifest_path[1100];
			snprintf(manifest_path, sizeof(manifest_path), "%s/uvsys.json",
					tmpdir);
			FILE *fptr = fopen(manifest_path, "r");
			if (fptr == NULL) {
				PRINT("Package '%s' does not contain a uvsys.json manifest.\n",
						filepath);
			}
			else {
				fseek(fptr, 0, SEEK_END);
				long size = ftell(fptr);
				rewind(fptr);
				char *data = malloc(size + 1);
				if ((data != NULL) &&
						(fread(data, 1, size, fptr) == (size_t) size)) {
					data[size] = '\0';
					if (uv_jsonreader_init(data, size) == ERR_NONE) {
						ret = system_parse_sysjson(this, data, tmpdir);
					}
					else {
						PRINT("Failed to parse the uvsys.json manifest of '%s'.\n",
								filepath);
					}
				}
				else {
					PRINT("Failed to read the uvsys.json manifest of '%s'.\n",
							filepath);
				}
				free(data);
				fclose(fptr);
			}
		}
	}

	if (ret) {
		// keep the extraction alive (the device files live in it) and record the
		// system; the extraction is removed at program exit
		sysfile_track_tmpdir(tmpdir);
		strncpy(this->filepath, filepath, sizeof(this->filepath) - 1);
		this->filepath[sizeof(this->filepath) - 1] = '\0';
		name_from_path(filepath, this->name, sizeof(this->name));
		this->loaded = true;
	}
	else if (extracted) {
		// nothing usable was loaded: drop the just-created extraction
		sysfile_rmdir(tmpdir);
	}
	else {
		// extraction directory was never created; nothing to clean up
	}

	return ret;
}


/// @brief: Normalizes an object name to its macro form (uppercase, spaces to
/// underscores), matching how the exporter derives index macro names.
static void normalize_objname(const char *name, char *dest, size_t len) {
	size_t i = 0;
	for (; (name[i] != '\0') && (i < len - 1); i++) {
		dest[i] = isspace((unsigned char) name[i]) ?
				'_' : toupper((unsigned char) name[i]);
	}
	dest[i] = '\0';
}


/// @brief: Reads the DEV_NAME object's default value from the currently loaded
/// database (dev.db) into *dest*, resolving it through the defines when it names
/// one. Leaves *dest* empty when no DEV_NAME object is found.
static void read_devname(char *dest, size_t len) {
	dest[0] = '\0';
	for (int32_t i = 0; i < db_get_object_count(&dev.db); i++) {
		db_obj_st *obj = db_get_obj(&dev.db, i);
		char norm[128];
		normalize_objname(dbvalue_get_string(&obj->name), norm, sizeof(norm));
		if (strcmp(norm, "DEV_NAME") == 0) {
			char *def = dbvalue_get_string(&obj->string_def);
			db_define_st *d = (def != NULL) ? db_define_find(&dev.db, def) : NULL;
			if ((d != NULL) && (d->type == DB_DEFINE_STRING)) {
				// the default names a string define, e.g. "DEV_NAME_PREFIX"
				strncpy(dest, d->str, len - 1);
			}
			else if (def != NULL) {
				// the default is a literal string
				strncpy(dest, def, len - 1);
			}
			else {
				// no default; leave empty
			}
			dest[len - 1] = '\0';
			break;
		}
	}
}


/// @brief: Loads the database of the .uvdev package at *filepath* into dev.db so
/// its identification fields can be read. The database's "content" includes are
/// resolved relative to the package root, so the load runs with the working
/// directory temporarily changed into the extracted package (mirroring the
/// firmware build). cmd_db's progress output is suppressed. Returns true on
/// success; the caller must db_deinit() once the fields have been read.
/// *version_out* receives the package version and *has_media_out* whether it
/// bundles a media directory; both may be NULL.
static bool load_uvdev_database(const char *filepath, char *version_out,
		size_t version_len, bool *has_media_out) {
	bool ret = false;
	uvdev_st pkg;
	if (uvdev_open(&pkg, filepath)) {
		if ((version_out != NULL) && (version_len != 0)) {
			strncpy(version_out, pkg.version, version_len - 1);
			version_out[version_len - 1] = '\0';
		}
		// whether the package bundles a media directory (read from the manifest,
		// independent of whether the database below loads)
		if (has_media_out != NULL) {
			*has_media_out = (strlen(pkg.media) != 0);
		}
		char cwd[1024] = {};
		if ((strlen(pkg.database) != 0) &&
				(getcwd(cwd, sizeof(cwd)) != NULL) &&
				(chdir(pkg.dir) == 0)) {
			// silence cmd_db's progress prints, irrelevant here
			fflush(stdout);
			int saved = dup(STDOUT_FILENO);
			int devnull = open(NULL_DEVICE, O_WRONLY);
			if (devnull >= 0) {
				dup2(devnull, STDOUT_FILENO);
			}

			ret = cmd_db(pkg.database);

			fflush(stdout);
			if (devnull >= 0) {
				dup2(saved, STDOUT_FILENO);
				close(devnull);
			}
			close(saved);
			if (chdir(cwd)) {
				// best effort; the original cwd should always be restorable
			}
		}
		uvdev_close(&pkg);
	}
	return ret;
}


/// @brief: Assigns *filepath* to *device*, deriving its file-based name and
/// reading the node id, CAN revision number, vendor id, product code and
/// DEV_NAME from the configuration file's DATABASE. When *nodeid* is non-zero it
/// overrides the node id read from the file.
static void device_assign_file(device_st *device, const char *filepath,
		uint8_t nodeid) {
	strncpy(device->filepath, filepath, sizeof(device->filepath) - 1);
	device->filepath[sizeof(device->filepath) - 1] = '\0';
	name_from_path(filepath, device->name, sizeof(device->name));

	device->devname[0] = '\0';
	device->revision = 0;
	device->vendor_id = 0;
	device->product_code = 0;
	device->nodeid = nodeid;
	device->default_nodeid = 0;
	device->has_media = false;
	device->conf_version[0] = '\0';
	device->if_version_mindex = 0;
	device->if_version_sindex = 0;
	device->if_version_size = 0;
	// the device's own revision is read over the bus; reset it so it is re-read
	device->dev_revision = 0;

	if (load_uvdev_database(filepath, device->conf_version,
			sizeof(device->conf_version), &device->has_media)) {
		device->revision = db_get_revision_number(&dev.db);
		device->vendor_id = db_get_vendor_id(&dev.db);
		device->product_code = db_get_product_code(&dev.db);
		// always record the file's default node id (for the "Default Node-ID"
		// display); only adopt it as the device's actual node id when the caller
		// did not pass an explicit one to keep
		device->default_nodeid = db_get_nodeid(&dev.db);
		if (nodeid == 0) {
			device->nodeid = db_get_nodeid(&dev.db);
		}
		read_devname(device->devname, sizeof(device->devname));
		// capture the CAN IF VERSION object's location so the device's own
		// revision can be read over the bus later (see find_read_device_revision)
		for (int32_t i = 0; i < db_get_object_count(&dev.db); i++) {
			db_obj_st *obj = db_get_obj(&dev.db, i);
			if (obj->obj_type == DB_OBJ_TYPE_IF_VERSION) {
				device->if_version_mindex = obj->obj.main_index;
				device->if_version_sindex = obj->obj.sub_index;
				device->if_version_size = CANOPEN_SIZEOF(obj->obj.type);
				break;
			}
		}
		// unload so the rest of the program still sees "no database loaded"
		// (e.g. saveparam's per-device .uvdev handling)
		db_deinit();
	}

	device->loaded = true;
}


bool system_add_device(system_st *this, const char *filepath, uint8_t nodeid) {
	bool ret = true;
	if (system_is_full(this)) {
		ret = false;
	}
	else {
		device_st *device = &this->devs[this->dev_count];
		memset(device, 0, sizeof(*device));
		device_assign_file(device, filepath, nodeid);
		this->dev_count++;
	}
	return ret;
}


device_st *system_add_empty_device(system_st *this) {
	device_st *ret = NULL;
	if (!system_is_full(this)) {
		ret = &this->devs[this->dev_count];
		memset(ret, 0, sizeof(*ret));
		snprintf(ret->name, sizeof(ret->name), "Device %u",
				(unsigned int) (this->dev_count + 1));
		ret->loaded = false;
		this->dev_count++;
	}
	return ret;
}


device_st *system_add_found_device(system_st *this, uint8_t nodeid,
		uint32_t vendor_id, uint32_t product_code) {
	device_st *ret = NULL;
	if (!system_is_full(this)) {
		ret = &this->devs[this->dev_count];
		memset(ret, 0, sizeof(*ret));
		// no configuration file: the path stays empty and the user can assign
		// one later from the device tab
		ret->filepath[0] = '\0';
		snprintf(ret->name, sizeof(ret->name), "Node 0x%x",
				(unsigned int) nodeid);
		ret->nodeid = nodeid;
		ret->vendor_id = vendor_id;
		ret->product_code = product_code;
		// the device was just discovered on the bus; the caller (find_devices)
		// refines this to the device's actual NMT state right after
		ret->state = DEV_STATE_OP;
		// treat as loaded so the device tab shows the discovered identity (node
		// id, vendor id, product code) even though no file is attached yet
		ret->loaded = true;
		this->dev_count++;
	}
	return ret;
}


void system_clear_devices(system_st *this) {
	memset(this->devs, 0, sizeof(this->devs));
	this->dev_count = 0;
}


bool system_remove_device(system_st *this, device_st *device) {
	bool ret = false;
	// locate the device inside the array so we know how many entries trail it
	uint8_t index = this->dev_count;
	for (uint8_t i = 0; i < this->dev_count; i++) {
		if (&this->devs[i] == device) {
			index = i;
			break;
		}
	}
	if (index < this->dev_count) {
		// shift the trailing devices down over the removed slot
		uint8_t trailing = this->dev_count - index - 1;
		if (trailing > 0) {
			memmove(&this->devs[index], &this->devs[index + 1],
					trailing * sizeof(this->devs[0]));
		}
		this->dev_count--;
		memset(&this->devs[this->dev_count], 0, sizeof(this->devs[0]));
		ret = true;
	}
	return ret;
}


void system_set_device_file(device_st *device, const char *filepath) {
	// A device that is live on the CAN bus (discovered by --find / "Search
	// devices", or otherwise seen) already knows its real node id. Assigning a
	// configuration file to it must NOT change that node id, even when the file's
	// DATABASE defines a default one — the file's default is only shown as the
	// "Default Node-ID" label. The node id is taken from the file only when the
	// device is offline (this also covers freshly created devices, which start
	// offline): those are devices we are configuring before they exist on the bus.
	uint8_t nodeid = (device->state != DEV_STATE_OFFLINE) ? device->nodeid : 0;
	device_assign_file(device, filepath, nodeid);
}


uint8_t system_read_file_nodeid(const char *filepath) {
	uint8_t ret = 0;
	if ((filepath != NULL) && (strlen(filepath) != 0) &&
			load_uvdev_database(filepath, NULL, 0, NULL)) {
		ret = db_get_nodeid(&dev.db);
		// unload so the rest of the program still sees "no database loaded"
		db_deinit();
	}
	return ret;
}


bool path_ends_with(const char *path, const char *suffix) {
	bool ret = false;
	if ((path != NULL) && (suffix != NULL)) {
		size_t pl = strlen(path);
		size_t sl = strlen(suffix);
		if (sl <= pl) {
			ret = true;
			const char *p = path + (pl - sl);
			for (size_t i = 0; i < sl; i++) {
				if (tolower((unsigned char) p[i]) !=
						tolower((unsigned char) suffix[i])) {
					ret = false;
					break;
				}
			}
		}
	}
	return ret;
}


const char *cmdline_load_arg(const char *stored) {
	const char *ret = NULL;
	if ((stored != NULL) && (strlen(stored) != 0)) {
		// the file was attached to the option (e.g. "--loadbin=fw.bin")
		ret = stored;
	}
	else if (dev.argv_count > 0) {
		// fall back to the first non-option token so the space-separated form
		// ("--loadbin fw.bin") keeps working now that the option's argument is
		// optional (getopt does not attach a space-separated value to an
		// optional-argument long option)
		ret = dev.nonopt_argv[0];
	}
	else {
		// no file given at all: operate on the already-loaded --dev/--sys devices
	}
	return ret;
}


bool cmd_system(const char *arg) {
	bool ret = system_set_file(&dev.system, arg);
	if (ret) {
		PRINT("System configuration file set to '%s'\n", arg);
	}
	return ret;
}


/// @brief: Splits a --dev argument of the form "<path>:<nodeid>" into its file
/// path and node id. The ":<nodeid>" suffix is optional: when absent (or when
/// the part after the last colon is not a number, e.g. a Windows drive letter)
/// *nodeid_out* is set to 0, meaning "read the default node id from the file".
/// Returns false when an explicit node id is present but out of the valid
/// CANopen range (1..127), in which case the device should be ignored.
static bool parse_device_arg(const char *arg, char *path, size_t path_len,
		uint8_t *nodeid_out) {
	bool ret = true;
	*nodeid_out = 0;
	// the node id, if any, follows the last colon; searching from the end keeps
	// a Windows drive-letter colon (e.g. "C:\...") from being mistaken for it
	const char *colon = strrchr(arg, ':');
	const char *path_end = arg + strlen(arg);
	if ((colon != NULL) && (colon[1] != '\0')) {
		char *numend = NULL;
		unsigned long parsed = strtoul(colon + 1, &numend, 0);
		if (*numend == '\0') {
			// the suffix is fully numeric: treat it as an explicit node id
			path_end = colon;
			if ((parsed >= 1) && (parsed <= 127)) {
				*nodeid_out = (uint8_t) parsed;
			}
			else {
				ret = false;
			}
		}
		else {
			// not a number: the colon belongs to the path, no node id given
		}
	}
	else {
		// no colon, or nothing after it: the whole argument is the path
	}

	size_t len = (size_t) (path_end - arg);
	if (len >= path_len) {
		len = path_len - 1;
	}
	memcpy(path, arg, len);
	path[len] = '\0';

	return ret;
}


bool cmd_device(const char *arg) {
	char path[1024];
	uint8_t nodeid;

	if (system_is_sysfile_loaded(&dev.system)) {
		// a system file already provides the devices; adding individual device
		// files on top of it is not allowed. Keep going so later commands run.
		PRINT(PRINT_BOLDRED
				"ERROR: a system file is already loaded ('%s'); ignoring device "
				"'%s'. Device files cannot be combined with a --sys file.\n"
				PRINT_RESET,
				dev.system.filepath, arg);
	}
	else if (!parse_device_arg(arg, path, sizeof(path), &nodeid)) {
		// an explicit node id was given but it is out of range
		PRINT(PRINT_BOLDRED
				"ERROR: invalid node id in device '%s'; it must be in the range "
				"1..127 (0x1..0x7f). Ignoring this device.\n"
				PRINT_RESET, arg);
	}
	else if (system_add_device(&dev.system, path, nodeid)) {
		if (nodeid != 0) {
			PRINT("Added device configuration file '%s' with node id 0x%x\n",
					path, nodeid);
		}
		else {
			PRINT("Added device configuration file '%s' "
					"(node id read from file)\n", path);
		}
	}
	else {
		// Warn but keep going so that any following commands (e.g. --ui) still
		// run; the extra device is simply ignored.
		PRINT("Ignoring device '%s': a system can hold at most %d devices.\n",
				path, SYSTEM_DEV_MAX_COUNT);
	}
	return true;
}
