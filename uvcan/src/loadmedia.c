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
#include "loadmedia.h"
#include "main.h"

#define this (&dev.loadmedia)


void loadmedia_step(void *dev);





bool cmd_loadmedia(const char *arg) {
	bool ret = true;

	if (!arg) {
		printf("ERROR: Give media file path as an argument.\n");
	}
	else {
		printf("Media file %s selected\n", arg);
		strcpy(this->filename, arg);
		add_task(loadmedia_step);
		uv_can_set_up();
	}

	return ret;
}





void loadmedia_step(void *ptr) {
	FILE *fptr = fopen(this->filename, "rb");

	if (fptr == NULL) {
		// failed to open the file, exit this task
		printf("Failed to open media file %s.\n", this->filename);
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
		printf("Opened file %s. Size: %i bytes.\n", this->filename, size);
		size_t ret = fread(data, size, 1, fptr);
		if (!ret) {
			printf("ERROR: Reading media file '%s' failed at byte %u / %u. "
					"Firmware download cancelled.\n", this->filename, index, size);
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
						0, strlen(this->filename) + 1, this->filename);

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
						printf("Loaded %u / %u bytes (%u %%)\n",
								addr, size, 100 * addr / size);
					}
				}
				else {
					printf("Errors (%i) while downloading the media settings. Media download discarded.\n", e);
					success = false;
				}
			}
		}




		fclose(fptr);
		if (success) {
			printf("Media file %s loaded\n", this->filename);
			fflush(stdout);
		}
	}
}
