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
#include "loadparam.h"
#include "main.h"

#define this (&dev.loadparam)


void loadparam_step(void *dev);


#define BLOCK_SIZE			256
#define RESPONSE_DELAY_MS	2000
// loads the parameters into this CANOpen index.
// The parameter should be of type ARRAY8, where the first index
// defines the parameter size.
#define PARAM_INDEX			0x2004


static void update(void *ptr) {
	int32_t data_index = 0;
	while (true) {
		if (_canopen.sdo.client.data_index != data_index) {
			int32_t percent = (_canopen.sdo.client.data_index * 100 +
					_canopen.sdo.client.data_count / 2) /
					_canopen.sdo.client.data_count;
			printf("downloaded %u / %u bytes (%u %%)\n",
					_canopen.sdo.client.data_index,
					_canopen.sdo.client.data_count,
					percent);
			fflush(stdout);
			this->progress = percent;
			if (percent == 100) {
				break;
			}
		}
		data_index = _canopen.sdo.client.data_index;
		uv_rtos_task_delay(20);
	}
}





bool cmd_loadparam(const char *arg) {
	bool ret = true;

	if (!arg) {
		printf("ERROR: Give parameter file as a file path to binary file.\n");
	}
	else {
		printf("Parameter file '%s' selected\n", arg);
		strcpy(this->params, arg);
		this->nodeid = db_get_nodeid(&dev.db);
		uv_delay_init(&this->delay, RESPONSE_DELAY_MS);
		add_task(loadparam_step);
		uv_can_set_up();
	}

	return ret;
}



void loadparam_step(void *ptr) {
	this->finished = false;
	this->progress = 0;


	FILE *fptr = fopen(this->params, "rb");

	if (fptr == NULL) {
		// failed to open the file, exit this task
		printf("Failed to open parameter file '%s'.\n", this->params);
		fflush(stdout);
	}
	else {
		int32_t size;
		fseek(fptr, 0, SEEK_END);
		size = ftell(fptr);
		rewind(fptr);
		bool success = false;

		printf("Opened file '%s'. Size: %i bytes.\n", this->params, size);
		fflush(stdout);

		printf("Downloading the parameters as a SDO block transfer\n");
		uint8_t data[size];
		size_t ret = fread(data, size, 1, fptr);

		if (!ret) {
			printf("ERROR: Reading file failed. "
					"Parameter download cancelled.\n");
			fflush(stdout);
		}
		else {
			// create task which will update the screen with the loading process
			uv_rtos_task_create(&update, "segload update", UV_RTOS_MIN_STACK_SIZE,
					NULL, UV_RTOS_IDLE_PRIORITY + 100, NULL);

			uv_errors_e e = uv_canopen_sdo_write(this->nodeid,
					PARAM_INDEX, 0, size, data);
			if (e != ERR_NONE) {
				printf("Downloading the parameters failed. Error code: %u\n", e);
				success = false;
			}
			else {
				success = true;
			}
		}

		fclose(fptr);
		if (success) {
			printf("Loading done. Saving the parameters... ");
			fflush(stdout);
			uv_errors_e e = uv_canopen_sdo_write(this->nodeid, CONFIG_CANOPEN_STORE_PARAMS_INDEX,
					CONFIG_CANOPEN_STORE_ALL_PARAMS_SUBINDEX, 4, "save");
			if (e == ERR_NONE) {
				printf("Done!\n");
			}
			else {
				printf("\n**** ERROR: Saving the parameters failed ****\n");
			}
			printf("Binary file closed.\n");
			fflush(stdout);
		}
	}

	this->finished = true;
}
