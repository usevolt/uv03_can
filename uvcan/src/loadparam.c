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
#include <uv_json.h>
#include "loadparam.h"
#include "main.h"

#define this (&dev.loadparam)


void loadparam_step(void *dev);





bool cmd_loadparam(const char *arg) {
	bool ret = true;

	if (!arg) {
		printf("ERROR: Give parameter file as a file path to binary file.\n");
	}
	else {
		printf("Parameter file '%s' selected\n", arg);
		strcpy(this->file, arg);
		add_task(loadparam_step);
		uv_can_set_up();
	}

	return ret;
}



void loadparam_step(void *ptr) {
	this->finished = false;

	FILE *fptr = fopen(this->file, "rb");

	if (fptr == NULL) {
		// failed to open the file, exit this task
		printf("Failed to open parameter file '%s'.\n", this->file);
		fflush(stdout);
	}
	else if (!db_is_loaded(&dev.db)) {
		printf("*** ERROR ****\n"
				"The database has to be loaded with --db in order to load params.\n");
	}
	else {
		int32_t size;
		fseek(fptr, 0, SEEK_END);
		size = ftell(fptr);
		rewind(fptr);

		printf("Opened file '%s'. Size: %i bytes.\n", this->file, size);
		fflush(stdout);

		char json[size];
		size_t ret = fread(json, size, 1, fptr);
		fclose(fptr);

		if (!ret) {
			printf("ERROR: Reading file failed. "
					"Parameter download cancelled.\n");
			fflush(stdout);
		}
		else {
			uv_errors_e e = ERR_NONE;

			uv_jsonreader_init(json, strlen(json));
			char *obj = uv_jsonreader_find_child(json, "obj 0");
			if (obj != NULL) {
				while (obj != NULL) {


					if (!uv_jsonreader_get_next_sibling(obj, &obj)) {
						break;
					}
				}
			}
			else {
				printf("*** ERROR ****\n"
						"Couldn't find 'obj 0' from the json file.\n");
				e = ERR_ABORTED;
			}

			if (e == ERR_NONE) {
				fflush(stdout);
				uv_rtos_task_delay(1000);
				printf("Saving the parameters...\n");
				e = uv_canopen_sdo_store_params(db_get_nodeid(&dev.db),
						MEMORY_ALL_PARAMS);
				fflush(stdout);
				uv_rtos_task_delay(1000);
				printf("Resetting the device...\n");
				uv_canopen_nmt_master_send_cmd(db_get_nodeid(&dev.db),
						CANOPEN_NMT_CMD_RESET_NODE);
				if (e == ERR_NONE) {
					printf("Done!\n");
					fflush(stdout);
				}
				else {
					printf("\n**** ERROR: Saving the parameters failed ****\n");
					fflush(stdout);
				}
				printf("Binary file closed.\n");
				fflush(stdout);
			}
			else {
				printf("Error when fetching operator settings: 0x%x. \n"
						"Loading the parameters failed\n", uv_canopen_sdo_get_error());
				fflush(stdout);
			}
		}
	}

	this->finished = true;
}
