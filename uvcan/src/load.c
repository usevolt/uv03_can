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


static void update(void *ptr) {
	int32_t data_index = 0;
	while (true) {
		if (_canopen.sdo.client.data_index != data_index) {
			int32_t percent = (_canopen.sdo.client.data_index) * 100 / _canopen.sdo.client.data_count;
			printf("downloaded %u / %u bytes (%u %%)\n",
					_canopen.sdo.client.data_index,
					_canopen.sdo.client.data_count,
					percent);
			fflush(stdout);
			if (percent == 100) {
				break;
			}
		}
		data_index = _canopen.sdo.client.data_index;
		uv_rtos_task_delay(20);
	}
}

bool cmd_load(const char *arg) {
	bool ret = true;

	if (!arg) {
		printf("ERROR: Give firmware as a file path to binary file.\n");
	}
	else {
		printf("Firmware %s selected\n", arg);
		strcpy(this->firmware, arg);
		this->response = false;
		this->wfr = false;
		this->uv = false;
		this->block_transfer = true;
		uv_delay_init(&this->delay, RESPONSE_DELAY_MS);
		add_task(load_step);
		uv_can_set_up();
	}

	return ret;
}


bool cmd_loadwfr(const char *arg) {
	bool ret = true;

	if (!arg) {
		printf("ERROR: Give firmware as a file path to binary file.\n");
	}
	else {
		printf("Firmware %s selected\n", arg);
		strcpy(this->firmware, arg);
		this->response = false;
		this->wfr = true;
		this->uv = false;
		this->block_transfer = true;
		uv_delay_init(&this->delay, LOADWFR_WAIT_TIME_MS);
		add_task(load_step);
		uv_can_set_up();
	}

	return ret;
}

bool cmd_segload(const char *arg) {
	bool ret = true;

	if (!arg) {
		printf("ERROR: Give firmware as a file path to binary file.\n");
	}
	else {
		printf("Firmware %s selected\n", arg);
		strcpy(this->firmware, arg);
		this->response = false;
		this->wfr = false;
		this->uv = false;
		this->block_transfer = false;
		uv_delay_init(&this->delay, RESPONSE_DELAY_MS);
		add_task(load_step);
		uv_can_set_up();
	}

	return ret;
}


bool cmd_segloadwfr(const char *arg) {
	bool ret = true;

	if (!arg) {
		printf("ERROR: Give firmware as a file path to binary file.\n");
	}
	else {
		printf("Firmware %s selected\n", arg);
		strcpy(this->firmware, arg);
		this->response = false;
		this->wfr = true;
		this->uv = false;
		this->block_transfer = false;
		uv_delay_init(&this->delay, LOADWFR_WAIT_TIME_MS);
		add_task(load_step);
		uv_can_set_up();
	}

	return ret;
}


bool cmd_uvload(const char *arg) {
	bool ret = true;

	if (!arg) {
		printf("ERROR: Give firmware as a file path to binary file.\n");
	}
	else {
		printf("Firmware %s selected\n", arg);
		strcpy(this->firmware, arg);
		this->response = false;
		this->wfr = false;
		this->uv = true;
		this->block_transfer = true;
		uv_delay_init(&this->delay, RESPONSE_DELAY_MS);
		add_task(load_step);
		uv_can_set_up();
	}

	return ret;
}


bool cmd_uvloadwfr(const char *arg) {
	bool ret = true;

	if (!arg) {
		printf("ERROR: Give firmware as a file path to binary file.\n");
	}
	else {
		printf("Firmware %s selected\n", arg);
		strcpy(this->firmware, arg);
		this->response = false;
		this->wfr = true;
		this->uv = true;
		this->block_transfer = true;
		uv_delay_init(&this->delay, LOADWFR_WAIT_TIME_MS);
		add_task(load_step);
		uv_can_set_up();
	}

	return ret;
}



static void can_callb(void * ptr, uv_can_msg_st *msg) {
	if ((msg->id == CANOPEN_HEARTBEAT_ID + db_get_nodeid(&dev.db)) &&
			(msg->type == CAN_STD) &&
			(msg->data_length == 1) &&
			(msg->data_8bit[0] == CANOPEN_BOOT_UP)) {
		// canopen boot up message recieved, node found.
		this->response = true;

		// disable CAN callback, it's not needed anymore.
		uv_canopen_set_can_callback(NULL);
	}
	printf("0x%x\n", msg->id);
}

void load_step(void *ptr) {
	FILE *fptr = fopen(this->firmware, "rb");

	if (fptr == NULL) {
		// failed to open the file, exit this task
		printf("Failed to open firmware file %s.\n", this->firmware);
		fflush(stdout);
	}
	else {
		int32_t size;
		fseek(fptr, 0, SEEK_END);
		size = ftell(fptr);
		rewind(fptr);
		bool success = false;

		printf("Opened file %s. Size: %i bytes.\n", this->firmware, size);
		fflush(stdout);

		// uv-version of bootloader, i.e. the old version where
		// node had to be reset before downloading
		this->response = !this->uv;
		if (this->uv) {
			// set canopen callback function
			uv_canopen_set_can_callback(&can_callb);

			if (!this->wfr) {
				printf("Resetting node 0x%x\n", dev.nodeid);
				fflush(stdout);
				uv_canopen_nmt_master_reset_node(dev.nodeid);
			}

			// wait for a response to NMT reset command
			printf("Waiting to receive boot up message from node 0x%x...\n", dev.nodeid);
			fflush(stdout);
			while (true) {
				uint16_t step_ms = 1;
				if (this->response) {
					break;
				}
				else {
					if (uv_delay(&this->delay, step_ms)) {
						printf("Couldn't reset node. No response to NMT Reset Node.\n");
						fflush(stdout);
						break;
					}
				}
				uv_rtos_task_delay(step_ms);
			}
			printf("Reset OK. Now downloading...\n");
			fflush(stdout);

			// download with multiple block transfers
			if (this->block_transfer) {
				printf("Downloading the firmware as SDO block transfer\n");
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
								"Firmware download cancelled.\n", index, size);
						fflush(stdout);
						break;
					}
					else {
						if (uv_canopen_sdo_block_write(dev.nodeid, BOOTLOADER_INDEX, BOOTLOADER_SUBINDEX,
								data_length, data) != ERR_NONE) {
							printf("Error while downloading block %u.\n", block);
							fflush(stdout);
							success = false;
							break;
						}
						else {
							printf("Block %u downloaded, %u / %u bytes (%u %%)\n", block, index + data_length, size,
									(index + data_length) * 100 / size);
							fflush(stdout);
						}
					}
					index += data_length;
				}
			}
		}
		// new 302 compatible protocol download
		else {
			if (this->wfr) {
				this->response = false;
				// set canopen callback function
				uv_canopen_set_can_callback(&can_callb);
				// wait for a response to NMT reset command
				printf("Waiting to receive boot up message from node 0x%x...\n", dev.nodeid);
				fflush(stdout);
				while (true) {
					uint16_t step_ms = 1;
					if (this->response) {
						break;
					}
					else {
						if (uv_delay(&this->delay, step_ms)) {
							printf("Couldn't reset node. No response to NMT Reset Node.\n");
							fflush(stdout);
							break;
						}
					}
					uv_rtos_task_delay(step_ms);
				}
			}

			if (this->response) {

				printf("Reset OK. Now downloading...\n");
				fflush(stdout);

				// download with block transfer
				if (this->block_transfer) {
					printf("Downloading the firmware as SDO block transfer\n");
					uint8_t data[size];
					size_t ret = fread(data, size, 1, fptr);

					if (!ret) {
						printf("ERROR: Reading file failed at byte %u / %u. "
								"Firmware download cancelled.\n", index, size);
						fflush(stdout);
					}
					else {
						// create task which will update the screen with the loading process
						uv_rtos_task_create(&update, "segload update", UV_RTOS_MIN_STACK_SIZE,
								NULL, UV_RTOS_IDLE_PRIORITY + 100, NULL);

						uv_errors_e e = uv_canopen_sdo_block_write(dev.nodeid,
								BOOTLOADER_INDEX, BOOTLOADER_SUBINDEX, size, data);
						if (e != ERR_NONE) {
							printf("Downloading the binary failed. Error code: %u\n", e);
							success = false;
						}
						else {
							success = true;
						}
					}
				}
				// download with segmented transfer
				else {
					printf("Downloading the firmware as SDO segmented transfer\n");
					uint8_t data[size];
					size_t ret = fread(data, size, 1, fptr);

					if (!ret) {
						printf("ERROR: Reading file failed at byte %u / %u. "
								"Firmware download cancelled.\n", index, size);
						fflush(stdout);
					}
					else {
						// create task which will update the screen with the loading process
						uv_rtos_task_create(&update, "segload update", UV_RTOS_MIN_STACK_SIZE,
								NULL, UV_RTOS_IDLE_PRIORITY + 100, NULL);

						uv_errors_e e = uv_canopen_sdo_write(dev.nodeid,
								BOOTLOADER_INDEX, BOOTLOADER_SUBINDEX, size, data);
						if (e != ERR_NONE) {
							printf("Downloading the binary failed. Error code: %u\n", e);
							success = false;
						}
						else {
							success = true;
						}
					}
				}
			}
		}

		fclose(fptr);
		if (success) {
			printf("Loading done. Resetting device... OK!\n");
			fflush(stdout);
			uv_canopen_nmt_master_reset_node(dev.nodeid);
			printf("Binary file closed.\n");
			fflush(stdout);
		}
	}
}
