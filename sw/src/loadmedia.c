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
static void load(char *filename, uint32_t count, uint32_t index);
static bool is_known_mediafile(char *filename);
static void run_media_path(const char *path);




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
	return loadmedia_load(arg);
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


static void load(char *filename, uint32_t count, uint32_t index) {
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
				// filename
				uv_errors_e e = uv_canopen_sdo_write(db_get_nodeid(&dev.db), CONFIG_CANOPEN_EXMEM_FILENAME_INDEX,
						0, strlen(filename) + 1, filename);

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
	run_media_path(this->file);
}


// Loads every recognized media file under *path* (a single file or a directory
// of files) onto the device at the node id currently set in dev.db.
static void run_media_path(const char *path) {
	const char *str = path;
	struct stat path_stat;
	stat(str, &path_stat);
	if (S_ISDIR(path_stat.st_mode)) {
		DIR *dirp;
		// check media file count
		uint32_t count = 0;
		uint32_t index = 0;
		dirp = opendir(str);
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

		dirp = opendir(str);
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
					load(s, count, index);
					index++;
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
		load(s, 1, 0);
	}
	else {
		printf("Unknown file '%s' given to *loadmedia*\n", str);
	}
}


// Arguments for the asynchronous single-device media load task.
static char async_media_filepath[1024];
static uint8_t async_media_nodeid;
static volatile bool async_media_finished = true;


bool loadmedia_load_device_is_finished(void) {
	return async_media_finished;
}


// Task body: opens the device's .uvdev package, points the media protocol at the
// target node, loads every media file from the package's MEDIA directory, then
// releases the package. Runs off the UI thread.
static void loadmedia_device_task(void *ptr) {
	uvdev_st pkg;
	if (!uvdev_open(&pkg, async_media_filepath)) {
		printf("Failed to open the device package '%s' for media loading.\n",
				async_media_filepath);
	}
	else {
		if (strlen(pkg.media) == 0) {
			printf("Device package '%s' bundles no media to load.\n",
					async_media_filepath);
		}
		else {
			// loadmedia's transfer targets db_get_nodeid(&dev.db); force it to the
			// device's node id so the media goes to the right device
			db_set_nodeid_force(&dev.db, async_media_nodeid);
			char mediadir[2048];
			snprintf(mediadir, sizeof(mediadir), "%s/%s", pkg.dir, pkg.media);
			run_media_path(mediadir);
		}
		uvdev_close(&pkg);
	}
	async_media_finished = true;
	uv_rtos_task_delete(NULL);
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
