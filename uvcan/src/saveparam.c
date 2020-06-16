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
#include "saveparam.h"
#include "main.h"

#define this (&dev.saveparam)


void saveparam_step(void *dev);


#define BLOCK_SIZE			256
#define RESPONSE_DELAY_MS	2000
// saves the parameters into this CANOpen index.
// The parameter should be of type ARRAY8, where the first index
// defines the parameter size.
#define PARAM_INDEX			0x2004
#define PARAM_SIZE			0x1000


static void update(void *ptr) {
	int32_t data_index = 0;
	while (true) {
		if (_canopen.sdo.client.data_index != data_index) {
			int32_t percent = (_canopen.sdo.client.data_index * 100 +
					_canopen.sdo.client.data_count / 2) /
					_canopen.sdo.client.data_count;
			printf("uploaded %u / %u bytes (%u %%)\n",
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



bool cmd_saveparam(const char *arg) {
	bool ret = true;

	if (!arg) {
		printf("ERROR: Give destination file as a file path to binary file.\n");
	}
	else {
		printf("Parameter file '%s' selected\n", arg);
		strcpy(this->params, arg);
		this->nodeid = db_get_nodeid(&dev.db);
		uv_delay_init(&this->delay, RESPONSE_DELAY_MS);
		add_task(saveparam_step);
		uv_can_set_up();
	}

	return ret;
}



void saveparam_step(void *ptr) {
	this->finished = false;
	this->progress = 0;

	printf("Reading the parameters as a SDO block transfer\n");
	uint8_t data[PARAM_SIZE] = {};
	// create task which will update the screen with the saveing process
	uv_rtos_task_create(&update, "segsave update", UV_RTOS_MIN_STACK_SIZE,
			NULL, UV_RTOS_IDLE_PRIORITY + 100, NULL);

	uv_errors_e e = uv_canopen_sdo_read(this->nodeid, PARAM_INDEX, 0, PARAM_SIZE, data);
	if (e == ERR_NONE) {
		printf("Read the parameters succesfully. Saving them to file '%s'\n", this->params);

		FILE *dest = fopen(this->params, "wb");
		if (dest == NULL) {
			printf("Failed creating the output file '%s'\n", this->params);
		}
		else {
			fwrite(data, 1, sizeof(data), dest);
			fclose(dest);
		}
	}
	else {
		printf("*** ERROR: reading the parameters from the device failed ***\n");
	}
	this->finished = true;
}







