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
#include "load.h"
#include "main.h"

#define this (&dev.load)


void load_step(void *dev);


#define BLOCK_SIZE	256
#define RESPONSE_DELAY_MS	2000
#define BOOTLOADER_INDEX	0x1F50
#define BOOTLOADER_SUBINDEX	1

bool cmd_load(const char *arg) {
	bool ret = true;

	if (!arg) {
		printf("ERROR: Give firmware as a file path to binary file.\n");
	}
	else {
		printf("Firmware %s selected\n", arg, dev.nodeid);
		strcpy(this->firmware, arg);
		this->response = false;
		uv_delay_init(&this->delay, RESPONSE_DELAY_MS);
		add_task(load_step);
	}

	return ret;
}

static void can_callb(void * ptr, uv_can_msg_st *msg) {
	if ((msg->id == CANOPEN_HEARTBEAT_ID + dev.nodeid) &&
			(msg->type == CAN_STD) &&
			(msg->data_length == 1) &&
			(msg->data_8bit[0] == CANOPEN_BOOT_UP)) {
		// canopen boot up message recieved, node found.
		this->response = true;

		// disable CAN callback, it's not needed anymore.
		uv_canopen_set_can_callback(NULL);
	}
}

void load_step(void *ptr) {
	FILE *fptr = fopen(this->firmware, "rb");

	if (fptr == NULL) {
		// failed to open the file, exit this task
		printf("Failed to open firmware file %s.\n", this->firmware);
	}
	else {
		int32_t size;
		fseek(fptr, 0, SEEK_END);
		size = ftell(fptr);
		rewind(fptr);

		printf("Opened file %s. Size: %i bytes.\n", this->firmware, size);

		// set canopen callback function
		uv_canopen_set_can_callback(&can_callb);

		printf("Resetting node 0x%x\n", dev.nodeid);
		uv_canopen_nmt_master_reset_node(dev.nodeid);


		// wait for a response to NMT reset command
		while (true) {
			uint16_t step_ms = 20;
			if (this->response) {
				this->response = false;
				break;
			}
			else {
				if (uv_delay(&this->delay, step_ms)) {
					printf("Couldn't reset node. No response to NMT Reset Node.\n");
					break;
				}
			}
			uv_rtos_task_delay(step_ms);
		}
		bool success = false;
		if (!this->response) {

			printf("Reset OK. Now downloading...\n");

			uint8_t data[BLOCK_SIZE];
			int32_t data_length;
			int32_t index = 0;
			uint32_t block = 0;
			success = true;
			while (index < size) {
				block++;
				if (index + BLOCK_SIZE <= size) {
					data_length = BLOCK_SIZE;
				}
				else {
					data_length = size - index;
				}
				size_t ret = fread(data, data_length, 1, fptr);

				if (!ret) {
					printf("ERROR: Reading file failed at byte %u / %u. "
							"Firmware download executed.\n", index, size);
					break;
				}
				else {
					if (uv_canopen_sdo_block_write(dev.nodeid, BOOTLOADER_INDEX, BOOTLOADER_SUBINDEX,
							data_length, data) != ERR_NONE) {
						printf("Error while downloading block %u. Trying again...\n", block);
						// try again ONCE
						if (uv_canopen_sdo_block_write(dev.nodeid, BOOTLOADER_INDEX, BOOTLOADER_SUBINDEX,
								data_length, data) != ERR_NONE) {
							printf("Second error while downloading block %u. Ending the transfer.\n", block);
							success = false;
							break;
						}
						else {
							printf("Block %u downloaded\n", block);
						}
					}
					else {
						printf("Block %u downloaded, %u / %u bytes (%u %%)\n", block, index + data_length, size,
								(index + data_length) * 100 / size);
					}
				}
				index += data_length;
			}
		}

		fclose(fptr);
		if (success) {
			printf("Loading done. Resetting device... OK!\n");
			uv_canopen_nmt_master_reset_node(dev.nodeid);
			printf("Binary file closed.\n");
		}
	}
}
