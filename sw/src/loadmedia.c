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



#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include "loadmedia.h"
#include "main.h"
#include "uvdev.h"

#define this (&dev.loadmedia)


static void loadmedia_step(void *dev);
static void load(char *filename, const char *devname, uint32_t count, uint32_t index);
static bool is_known_mediafile(char *filename);
static const char *media_devname(const char *fullpath, const char *strip_prefix);
static uint32_t count_media_path(const char *path);
static void load_media_path(const char *path, const char *strip_prefix,
		uint32_t count, uint32_t *index);
static void run_media_path(const char *path, const char *strip_prefix);
static void run_media_paths(const char *const *paths, uint32_t n,
		const char *strip_prefix);
static bool loadmedia_uvdev(const char *uvdev_path, uint8_t nodeid);
static void loadmedia_devices(uint8_t start, uint8_t end);
static void loadmedia_dispatch_step(void *ptr);




bool loadmedia_load(const char *path) {
	bool ret = false;

	if (!path) {
		PRINT("Give the media file (or a directory of media files) as a file path.\n");
	}
	else {
		strncpy(this->file, path, sizeof(this->file) - 1);
		this->file[sizeof(this->file) - 1] = '\0';
		add_task(loadmedia_step);
		uv_can_set_up(false);
		ret = true;
	}

	return ret;
}


bool cmd_loadmedia(const char *arg) {
	// store the option's attached argument (may be empty); the dispatch task
	// resolves it against the non-option arguments and the loaded system once the
	// scheduler is running
	strncpy(this->file, (arg != NULL) ? arg : "", sizeof(this->file) - 1);
	this->file[sizeof(this->file) - 1] = '\0';
	add_task(loadmedia_dispatch_step);
	uv_can_set_up(false);
	return true;
}


static bool is_known_mediafile(char *filename) {
	bool ret = false;
	if (strstr(filename, ".jpg") ||
			strstr(filename, ".JPG") ||
			strstr(filename, ".jpeg") ||
			strstr(filename, ".JPEG") ||
			strstr(filename, ".png") ||
			strstr(filename, ".PNG") ||
			// raw binary blobs (e.g. custom font files) are stored verbatim and
			// read back by the device with the exmem file API
			strstr(filename, ".bin") ||
			strstr(filename, ".BIN")) {
		ret = true;
	}
	return ret;
}


// Loads a single media file onto the device. *filename* is the local path used
// to open and read the file; *devname* is the name stored on the device (kept
// relative to the package root, e.g. "media/test.png", so no host-side
// /tmp/... extraction path leaks onto the device).
// Derives the device-facing media filename from local path *fullpath* by
// removing *strip_prefix* (the package's temporary extraction directory) from
// its front, so a file extracted to "/tmp/xxx/media/test.png" from a package
// rooted at "/tmp/xxx" is stored on the device as "media/test.png". When
// *strip_prefix* is NULL/empty or is not a prefix of *fullpath*, the full path
// is used unchanged (legacy raw-file behavior).
static const char *media_devname(const char *fullpath, const char *strip_prefix) {
	const char *ret = fullpath;
	if (strip_prefix != NULL) {
		size_t len = strlen(strip_prefix);
		if ((len != 0) && (strncmp(fullpath, strip_prefix, len) == 0)) {
			ret = fullpath + len;
			// drop any path separators left between the prefix and the name
			while (*ret == '/') {
				ret++;
			}
		}
		else {
			// prefix does not apply; keep the full path
		}
	}
	else {
		// no prefix given; keep the full path
	}
	return ret;
}


// Loads a single media file onto the device. *filename* is the local path used
// to open and read the file; *devname* is the name stored on the device (kept
// relative to the package root, e.g. "media/test.png", so no host-side
// /tmp/... extraction path leaks onto the device).
static void load(char *filename, const char *devname, uint32_t count, uint32_t index) {
	FILE *fptr = fopen(filename, "rb");

	if (fptr == NULL) {
		// failed to open the file, exit this task
		PRINT("Failed to open media file %s.\n", filename);
		fflush(stderr);
	}
	else {
		fflush(stderr);
		CANOPEN_TYPEOF(CONFIG_CANOPEN_EXMEM_FILESIZE_TYPE) size;
		fseek(fptr, 0, SEEK_END);
		size = ftell(fptr);
		rewind(fptr);
		bool success = true;
		uint8_t data[size];
		printf("Opened file %s. Size: %i bytes.\n", filename, size);
		size_t ret = fread(data, size, 1, fptr);
		if (!ret) {
			printf("ERROR: Reading media file '%s' failed. "
					"Firmware download cancelled.\n", filename);
			fflush(stdout);
		}
		else {
			uint32_t block_size = 0;
			uv_canopen_sdo_read(db_get_nodeid(&dev.db), CONFIG_CANOPEN_EXMEM_BLOCKSIZE_INDEX,
					0, CANOPEN_SIZEOF(CONFIG_CANOPEN_EXMEM_DATA_TYPE), &block_size);
			if (block_size == 0) {
				printf("ERROR: Couldn't read the block size for media transfer.\n");
				success = false;
			}
			else {
				// filename (package-relative, not the host extraction path)
				uv_errors_e e = uv_canopen_sdo_write(db_get_nodeid(&dev.db), CONFIG_CANOPEN_EXMEM_FILENAME_INDEX,
						0, strlen(devname) + 1, (void *) devname);

				// filesize
				e |= uv_canopen_sdo_write(db_get_nodeid(&dev.db), CONFIG_CANOPEN_EXMEM_FILESIZE_INDEX,
						0, CANOPEN_SIZEOF(CONFIG_CANOPEN_EXMEM_FILESIZE_TYPE), &size);

				if (e == ERR_NONE) {
					CANOPEN_TYPEOF(CONFIG_CANOPEN_EXMEM_OFFSET_TYPE) addr = 0;

					while (addr < size && (e == ERR_NONE)) {
						uint32_t len = uv_mini(block_size, size - addr);

						// write the actual data
						e = uv_canopen_sdo_write(db_get_nodeid(&dev.db),
								CONFIG_CANOPEN_EXMEM_DATA_INDEX, 0, len, &data[addr]);

						// download address offset
						e |= uv_canopen_sdo_write(db_get_nodeid(&dev.db), CONFIG_CANOPEN_EXMEM_OFFSET_INDEX,
								0, CANOPEN_SIZEOF(CONFIG_CANOPEN_EXMEM_OFFSET_TYPE), &addr);

						if (e == ERR_NONE) {
							// set the write request to save the loaded data.
							// write request indicates the count of data to be saved
							uv_canopen_sdo_write(db_get_nodeid(&dev.db), CONFIG_CANOPEN_EXMEM_WRITEREQ_INDEX,
									0, CANOPEN_SIZEOF(CONFIG_CANOPEN_EXMEM_WRITEREQ_TYPE), &len);

							while (true) {
								// wait until writereq is again zero, this indicates that the data
								// was processed and we can continue
								CANOPEN_TYPEOF(CONFIG_CANOPEN_EXMEM_WRITEREQ_TYPE) response = len;
								uv_canopen_sdo_read(db_get_nodeid(&dev.db), CONFIG_CANOPEN_EXMEM_WRITEREQ_INDEX,
										0, CANOPEN_SIZEOF(CONFIG_CANOPEN_EXMEM_WRITEREQ_TYPE), &response);

								if (response == 0) {
									// block saved to the device, we're ready to download new data
									break;
								}
								uv_rtos_task_delay(1);
							}
						}
						else {
							printf("Error while downloading the media data.\n");
							success = false;
						}

						addr += len;
						printf("\33[2K\r");
						printf("Loaded %u / %u bytes (%u %%) file %i/%i",
								addr, size, 100 * addr / size,
								index + 1,
								count);
						fflush(stdout);
					}
					printf("\n");
				}
				else {
					printf("Errors (%i) while downloading the media settings. Media download discarded.\n", e);
					success = false;
				}
			}
		}




		fclose(fptr);
		if (!success) {
			printf("*** ERROR ***:\n"
					"    Media file %s loading failed\n", filename);
			fflush(stdout);
		}
	}
}



static void loadmedia_step(void *ptr) {
	// raw file/directory load: store the path as given (no package to relativize)
	run_media_path(this->file, NULL);
}


// Counts the recognized media files reachable from *path*: 1 for a single media
// file, or the number of recognized files directly inside a directory (0 for an
// unreadable or unrecognized path). Used to size the batch progress counter.
static uint32_t count_media_path(const char *path) {
	uint32_t count = 0;
	struct stat path_stat;
	if (stat(path, &path_stat) != 0) {
		// unreadable path: nothing to count (load_media_path reports it)
	}
	else if (S_ISDIR(path_stat.st_mode)) {
		DIR *dirp = opendir(path);
		while (dirp) {
			struct dirent *d;
			if ((d = readdir(dirp)) != NULL) {
				if (is_known_mediafile(d->d_name)) {
					count++;
				}
			}
			else {
				closedir(dirp);
				break;
			}
		}
	}
	else if (S_ISREG(path_stat.st_mode)) {
		count = 1;
	}
	else {
		// not a regular file or a directory
	}
	return count;
}


// Loads every recognized media file reachable from *path* (a single file or a
// directory of files) onto the device at the node id currently set in dev.db.
// *strip_prefix* is removed from each local path to form the package-relative
// name stored on the device (see media_devname); NULL for legacy raw loads.
// *count* is the batch total shown in the progress line and *index points at the
// running file number, advanced past each file loaded here.
static void load_media_path(const char *path, const char *strip_prefix,
		uint32_t count, uint32_t *index) {
	const char *str = path;
	struct stat path_stat;
	if (stat(str, &path_stat) != 0) {
		printf("Unknown file '%s' given to *loadmedia*\n", str);
	}
	else if (S_ISDIR(path_stat.st_mode)) {
		DIR *dirp = opendir(str);
		while(dirp) {
			struct dirent *d;
			if ((d = readdir(dirp)) != NULL) {
				if (is_known_mediafile(d->d_name)) {
					char s[1024];
					sprintf(s, "%s", str);
					if (str[strlen(str) - 1] != '/') {
						strcat(s, "/");
					}
					strcat(s, d->d_name);
					load(s, media_devname(s, strip_prefix), count, *index);
					(*index)++;
				}
			}
			else {
				closedir(dirp);
				break;
			}
		}
	}
	else if (S_ISREG(path_stat.st_mode)) {
		char s[1024];
		strncpy(s, str, sizeof(s) - 1);
		s[sizeof(s) - 1] = '\0';
		load(s, media_devname(s, strip_prefix), count, *index);
		(*index)++;
	}
	else {
		printf("Unknown file '%s' given to *loadmedia*\n", str);
	}
}


// Loads every recognized media file under *path* (a single file or a directory
// of files) onto the device at the node id currently set in dev.db.
// *strip_prefix* is removed from each local path to form the package-relative
// name stored on the device (NULL for legacy raw loads).
static void run_media_path(const char *path, const char *strip_prefix) {
	uint32_t index = 0;
	load_media_path(path, strip_prefix, count_media_path(path), &index);
}


// Loads media from each of *n* paths (files and/or directories) as one logical
// batch, so the per-file progress counter runs across all of them. Used when the
// shell expands a glob (e.g. "*_hd.png") into several file arguments.
static void run_media_paths(const char *const *paths, uint32_t n,
		const char *strip_prefix) {
	uint32_t count = 0;
	for (uint32_t i = 0; i < n; i++) {
		count += count_media_path(paths[i]);
	}
	uint32_t index = 0;
	for (uint32_t i = 0; i < n; i++) {
		load_media_path(paths[i], strip_prefix, count, &index);
	}
}


// Arguments for the asynchronous single-device media load task.
static char async_media_filepath[1024];
static uint8_t async_media_nodeid;
static volatile bool async_media_finished = true;


bool loadmedia_load_device_is_finished(void) {
	return async_media_finished;
}


// Opens the device's .uvdev package, points the media protocol at *nodeid*, loads
// every media file from the package's MEDIA directory, then releases the package.
// The transfer runs while the package is still extracted. Returns true when the
// package had a media directory (whether or not every file loaded). Warns when the
// package is unreadable or bundles no media.
static bool loadmedia_uvdev(const char *uvdev_path, uint8_t nodeid) {
	bool ret = false;
	uvdev_st pkg;
	if (!uvdev_open(&pkg, uvdev_path)) {
		printf(PRINT_BOLDRED "Failed to open the device package '%s' for media "
				"loading.\n" PRINT_RESET, uvdev_path);
	}
	else {
		if (strlen(pkg.media) == 0) {
			printf(PRINT_BOLDYELLOW "WARNING: device package '%s' bundles no media "
					"files to load.\n" PRINT_RESET, uvdev_path);
		}
		else {
			// loadmedia's transfer targets db_get_nodeid(&dev.db); force it to the
			// device's node id so the media goes to the right device
			db_set_nodeid_force(&dev.db, nodeid);
			char mediadir[2048];
			snprintf(mediadir, sizeof(mediadir), "%s/%s", pkg.dir, pkg.media);
			// strip the package's extraction dir so the device stores the media
			// under its package-relative name (e.g. "media/test.png"), not the
			// host-side /tmp/... path
			run_media_path(mediadir, pkg.dir);
			ret = true;
		}
		uvdev_close(&pkg);
	}
	return ret;
}


// Task body: loads a single device's bundled media off the UI thread.
static void loadmedia_device_task(void *ptr) {
	loadmedia_uvdev(async_media_filepath, async_media_nodeid);
	async_media_finished = true;
	uv_rtos_task_delete(NULL);
}


// Loads bundled media onto each system device in the index range [start, end).
// Each device's own .uvdev package supplies the media; a device with no package,
// or whose package bundles no media, is skipped with a warning.
static void loadmedia_devices(uint8_t start, uint8_t end) {
	for (uint8_t i = start; i < end; i++) {
		device_st *d = &dev.system.devs[i];
		if (strlen(d->filepath) == 0) {
			printf(PRINT_BOLDYELLOW "Skipping device '%s' (node 0x%x): no "
					"configuration package.\n" PRINT_RESET,
					d->name, (unsigned int) d->nodeid);
			continue;
		}
		printf("\n=== Loading media to device '%s' (node 0x%x) ===\n",
				d->name, (unsigned int) d->nodeid);
		loadmedia_uvdev(d->filepath, d->nodeid);
	}
}


// Task body for the --loadmedia command. Dispatches on the effective argument:
//   - a .uvsys package: load it and push each device's bundled media
//   - a .uvdev package:  push that package's media to the selected node
//   - any other path:    load it as a raw media file or directory (legacy)
//   - no argument:       push bundled media for every --dev / --sys device
static void loadmedia_dispatch_step(void *ptr) {
	const char *arg = cmdline_load_arg(this->file);

	if (path_is_uvsys(arg)) {
		uint8_t prev = dev.system.dev_count;
		if (!system_set_file(&dev.system, arg)) {
			printf(PRINT_BOLDRED "ERROR: failed to load system package '%s'.\n"
					PRINT_RESET, arg);
		}
		else {
			loadmedia_devices(prev, dev.system.dev_count);
		}
	}
	else if (path_is_uvdev(arg)) {
		uint8_t prev = dev.system.dev_count;
		// use the node id forced with --nodeid when given (0 -> the package's own)
		if (!system_add_device(&dev.system, arg, db_get_nodeid(&dev.db))) {
			printf(PRINT_BOLDRED "ERROR: failed to add device package '%s'.\n"
					PRINT_RESET, arg);
		}
		else {
			loadmedia_devices(prev, dev.system.dev_count);
		}
	}
	else if (arg != NULL) {
		// raw media file(s) or directory (legacy behavior). When the shell expands
		// a glob such as "*_hd.png", every match arrives as a separate non-option
		// argument; load them all, not just the first.
		const char *paths[dev.argv_count + 1];
		uint32_t n = 0;
		paths[n++] = arg;
		// when the value was NOT attached to the option, arg is nonopt_argv[0];
		// skip that token below so it is not loaded twice
		unsigned int start = ((strlen(this->file) == 0) &&
				(dev.argv_count > 0)) ? 1 : 0;
		for (unsigned int i = start; i < dev.argv_count; i++) {
			paths[n++] = dev.nonopt_argv[i];
		}
		// legacy raw load: store paths as given (no package to relativize)
		run_media_paths(paths, n, NULL);
	}
	else if (dev.system.dev_count != 0) {
		// no file given: push bundled media for the loaded --dev / --sys devices
		loadmedia_devices(0, dev.system.dev_count);
	}
	else {
		printf(PRINT_BOLDRED "ERROR: no media given. Provide a media file or "
				"directory, a .uvdev / .uvsys package, or load devices with "
				"--dev / --sys first.\n" PRINT_RESET);
	}
}


void loadmedia_load_device_async(const char *uvdev_path, uint8_t nodeid) {
	strncpy(async_media_filepath, (uvdev_path != NULL) ? uvdev_path : "",
			sizeof(async_media_filepath) - 1);
	async_media_filepath[sizeof(async_media_filepath) - 1] = '\0';
	async_media_nodeid = nodeid;
	// mark in-progress before the task starts so a caller can poll immediately
	async_media_finished = false;
	uv_rtos_task_create(&loadmedia_device_task, "loadmedia_task",
			UV_RTOS_MIN_STACK_SIZE * 5, NULL, UV_RTOS_IDLE_PRIORITY + 1, NULL);
}
