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

#define this (&dev.loadmedia)


static void loadmedia_step(void *dev);
static void load(char *filename, uint32_t count, uint32_t index);
static bool is_known_mediafile(char *filename);




bool cmd_loadmedia(const char *arg) {
	bool ret = true;

	if (!arg) {
		printf("ERROR: Give media file path as an argument.\n");
	}
	else {
		printf("Media file %s selected\n", arg);
		strcpy(this->filename, arg);
		if (this->filename[strlen(arg) - 1] == '/') {
			this->filename[strlen(arg) - 1] = '\0';
		}
		add_task(loadmedia_step);
		uv_can_set_up(false);
	}

	return ret;
}


static bool is_known_mediafile(char *filename) {
	bool ret = false;
	if (strstr(filename, ".jpg") ||
			strstr(filename, ".JPG") ||
			strstr(filename, ".jpeg") ||
			strstr(filename, ".JPEG") ||
			strstr(filename, ".png") ||
			strstr(filename, ".PNG")) {
		ret = true;
	}
	return ret;
}


static void load(char *filename, uint32_t count, uint32_t index) {
	FILE *fptr = fopen(filename, "rb");

	if (fptr == NULL) {
		// failed to open the file, exit this task
		printf("Failed to open media file %s.\n", filename);
		fflush(stdout);
	}
	else {
		fflush(stdout);
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
	struct stat path_stat;
	stat(this->filename, &path_stat);
	if (S_ISDIR(path_stat.st_mode)) {
		DIR *dirp;
		// check media file count
		uint32_t count = 0;
		uint32_t index = 0;
		dirp = opendir(this->filename);
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

		dirp = opendir(this->filename);
		while(dirp) {
			struct dirent *d;
			if ((d = readdir(dirp)) != NULL) {
				if (is_known_mediafile(d->d_name)) {
					char str[1024];
					sprintf(str, "%s/%s", this->filename, d->d_name);
					load(str, count, index);
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
		load(this->filename, 1, 0);
	}
	else {
		printf("Unknown file '%s' given to *loadmedia*\n", this->filename);
	}

}
