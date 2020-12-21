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


// loads the given param from the JSON file to the target device
static uv_errors_e load_param(char *json_obj) {
	uv_errors_e ret = ERR_NONE;
	uint32_t mindex;
	uint32_t sindex = 0;
	canopen_object_type_e type;
	char *data;
	char info[128] = { };

	char *val = uv_jsonreader_find_child(json_obj, "MAININDEX");
	if (val == NULL) {
		ret = ERR_ABORTED;
	}
	mindex = uv_jsonreader_get_int(val);
	val = uv_jsonreader_find_child(json_obj, "SUBINDEX");
	if (val != NULL) {
		sindex = uv_jsonreader_get_int(val);
	}
	val = uv_jsonreader_find_child(json_obj, "TYPE");
	if (val == NULL) {
		ret = ERR_ABORTED;
	}
	type = db_jsonval_to_type(val);
	data = uv_jsonreader_find_child(json_obj, "DATA");
	if (data == NULL) {
		ret = ERR_ABORTED;
	}
	// check that that the data type and content actually match
	if (data != NULL) {
		if ((CANOPEN_IS_ARRAY(type) && uv_jsonreader_get_type(data) != JSON_ARRAY) ||
				(CANOPEN_IS_STRING(type) && uv_jsonreader_get_type(data) != JSON_STRING) ||
				(CANOPEN_IS_INTEGER(type) && uv_jsonreader_get_type(data) != JSON_INT)) {
			ret = ERR_ABORTED;
		}
	}
	val = uv_jsonreader_find_child(json_obj, "INFO");
	if (val == NULL) {
		ret = ERR_ABORTED;
	}
	else {
		uv_jsonreader_get_string(val, info, sizeof(info));
	}

	if (ret == ERR_NONE) {
		char t[64];
		db_type_to_str(type, t);
		printf("Writing object '%s'\n"
				"    of type %s to target.\n", info, t);
		fflush(stdout);
		if (CANOPEN_IS_ARRAY(type)) {
			for (uint32_t i = 0; i < uv_jsonreader_array_get_size(data); i++) {
				uint32_t d = uv_jsonreader_array_get_int(data, i);
				ret |= uv_canopen_sdo_write(db_get_nodeid(&dev.db), mindex, i + 1,
						CANOPEN_SIZEOF(type), &d);
				if (ret != ERR_NONE) {
					printf("*** ERROR ***\n"
							"Array loading failed for subindex %u\n", sindex);
				}
			}
		}
		else if (CANOPEN_IS_STRING(type)) {
			char str[65535] = {};
			uv_jsonreader_get_string(data, str, sizeof(str));
			ret |= uv_canopen_sdo_write(db_get_nodeid(&dev.db),
					mindex, 0, strlen(str) + 1, str);
			if (ret != ERR_NONE) {
				printf("*** ERROR ***\n"
						"Loading the string '%s' failed.\n", str);
			}
		}
		else {
			// data is integer data
			uint32_t d = uv_jsonreader_get_int(data);
			ret |= uv_canopen_sdo_write(db_get_nodeid(&dev.db),
					mindex, sindex, CANOPEN_SIZEOF(type), &d);
			if (ret != ERR_NONE) {
				printf("*** ERROR ***\n"
						"Parameter loading failed for sub index %u\n", sindex);
			}
		}
	}
	else {
		printf("\n**** ERROR ****\n"
				"Parameter in a wrong format\n");
		if (info != NULL) {
			printf("Parameter info: '%s'\n\n", info);
		}
		else {
			printf("'INFO' value not found from the parameter.\n\n");
		}
		fflush(stdout);
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
			char *obj = uv_jsonreader_find_child(json, "PARAMS");
			if (obj != NULL && uv_jsonreader_get_type(obj) == JSON_ARRAY) {
				char *arr = obj;
				printf("Found %u params. Downloading them to the target device.\n",
						uv_jsonreader_array_get_size(arr));
				fflush(stdout);
				obj = uv_jsonreader_array_at(arr, 0);
				for (int32_t i = 0; i < uv_jsonreader_array_get_size(arr); i++) {
					if (uv_jsonreader_get_type(obj) == JSON_OBJECT) {
						e |= load_param(obj);
					}
					else {
						printf("**** ERROR ****\n"
								"PARAMS array contained something else\n"
								"than objects at index %i\n", i + 1);
						e |= ERR_ABORTED;
					}

					uv_jsonreader_get_next_sibling(obj, &obj);
				}
				printf("\n\n");
				fflush(stdout);
			}
			else {
				printf("**** ERROR ****\n"
						"Couldn't find array type object 'PARAMS' from the json file.\n");
				fflush(stdout);
				e = ERR_ABORTED;
			}

			obj = uv_jsonreader_find_child(json, "OPERATORS");
			char *opdb_mindex_json = uv_jsonreader_find_child(json, "OPDB_MAININDEX");
			char *current_op_json = uv_jsonreader_find_child(json, "CURRENT_OP");
			char *opdb_type_json = uv_jsonreader_find_child(json, "OPDB_TYPE");

			if (obj != NULL && uv_jsonreader_get_type(obj) == JSON_ARRAY &&
					opdb_mindex_json != NULL && uv_jsonreader_get_type(opdb_mindex_json) == JSON_INT &&
					current_op_json != NULL && uv_jsonreader_get_type(current_op_json) == JSON_INT &&
					opdb_type_json != NULL && uv_jsonreader_get_type(opdb_type_json) == JSON_STRING) {

				char *operators = obj;
				uint32_t opdb_mindex = uv_jsonreader_get_int(opdb_mindex_json);
				canopen_object_type_e opdb_type = db_jsonval_to_type(opdb_type_json);

				// copy as many times as necessary to have all the operators
				uint8_t op_count = uv_jsonreader_array_get_size(operators);
				for (uint32_t i = 1; i < op_count; i++) {
					printf("Creating new operator by copying operator %u\n", 1);
					fflush(stdout);
					uint32_t data = 1;
					e |= uv_canopen_sdo_write(db_get_nodeid(&dev.db),
							opdb_mindex, 3, CANOPEN_SIZEOF(opdb_type), &data);
					// small delay to wait for the device to copy te operators
					uv_rtos_task_delay(200);
				}

				// cycle through all the operators
				for (uint32_t i = 0; i < op_count; i++) {
					// todo: load this op
					printf("Loading operator %i\n", i + 1);
					fflush(stdout);
					uint32_t data = i + 1;
					uv_canopen_sdo_write(db_get_nodeid(&dev.db), opdb_mindex,
							1, CANOPEN_SIZEOF(opdb_type), &data);
					uv_rtos_task_delay(300);

					char *op = uv_jsonreader_array_at(operators, i);
					obj = uv_jsonreader_array_at(op, 0);
					// cycle through all this operators parameters
					for (uint32_t j = 0; j < uv_jsonreader_array_get_size(op); j++) {
						if (uv_jsonreader_get_type(obj) == JSON_OBJECT) {
							e |= load_param(obj);
						}
						else {
							printf("*** ERROR ***\n"
									"OPERATORS array contained something else\n"
									"than object at operator %u, parameter index %u\n",
									i + 1, j + 1);
						}

						uv_jsonreader_get_next_sibling(obj, &obj);
					}
					printf("Saving the parameters for op %u...\n", i + 1);
					fflush(stdout);
					e |= uv_canopen_sdo_store_params(db_get_nodeid(&dev.db),
							MEMORY_ALL_PARAMS);
					if (e != ERR_NONE) {
						printf("*** ERROR ***\n"
								"Error encountered when storing the parameters for op %u\n", i + 1);
					}
					// wait for the parameters to be saved
					uv_rtos_task_delay(100);
				}

				// todo: load the current op
				uint32_t data = uv_jsonreader_get_int(current_op_json);
				printf("Setting the current operator to op %i\n", data + 1);
				fflush(stdout);
				e |= uv_canopen_sdo_write(db_get_nodeid(&dev.db), opdb_mindex, 1,
						 CANOPEN_SIZEOF(opdb_type), &data);
				uv_rtos_task_delay(300);
			}
			else {
				printf("**** WARNING ****\n"
						"OPERATORS array not found from the JSON.\n\n");
				fflush(stdout);
			}

			printf("Saving the parameters...\n");
			fflush(stdout);
			e |= uv_canopen_sdo_store_params(db_get_nodeid(&dev.db),
					MEMORY_ALL_PARAMS);
			uv_rtos_task_delay(300);
			printf("Resetting the device...\n");
			fflush(stdout);
			if (e == ERR_NONE) {
				uv_canopen_nmt_master_send_cmd(db_get_nodeid(&dev.db),
						CANOPEN_NMT_CMD_RESET_NODE);
				printf("Done.\n\nBinary file closed.\n");
				fflush(stdout);
			}
			else {
				printf("\n**** NOTICE ****\n"
						"The device was not reset due to errors. \n"
						"Manual reset is necessary.\n\n");
				fflush(stdout);
			}
			if (e != ERR_NONE) {
				printf("\n**** ERROR ****\n"
						"Error when fetching operator settings.\n"
						"Loading the parameters might have failed\n");
				fflush(stdout);
			}
		}
	}

	this->finished = true;
}
