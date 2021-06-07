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
#include "saveparam.h"
#include "main.h"
#include "db.h"

#define this (&dev.saveparam)




void saveparam_step(void *dev);




bool cmd_saveparamall(const char *arg) {
	bool ret = cmd_saveparam(arg);
	this->all = true;
	return ret;
}


bool cmd_saveparam(const char *arg) {
	bool ret = true;
	this->all = false;

	if (!arg) {
		printf("ERROR: Give filepath to the file where the parameters are stored.\n");
	}
	else if (!db_is_loaded(&dev.db)) {
		printf("ERROR: Database has to be loaded with --db in order to use --saveparam command.\n");
	}
	else {
		printf("Parameter file '%s' selected\n", arg);
		strcpy(this->file, arg);
		this->nodeid = db_get_nodeid(&dev.db);
		add_task(saveparam_step);
		uv_can_set_up();
	}

	return ret;
}


/// @brief: fetches the CANopen parameter from the device and writes it into the json
static uv_errors_e json_add_obj(uv_json_st *dest_json, db_obj_st *obj, char *info_str) {
	uv_errors_e ret = ERR_NONE;
	char bfr[1024] = {};
	uv_json_st json;
	// we create object on an own json file. Since JSON file itself wraps the data
	// inside an object, we dont need to create additional object for the data
	uv_jsonwriter_init(&json, bfr, sizeof(bfr));
	if (info_str != NULL && strlen(info_str) != 0) {
		uv_jsonwriter_add_string(&json, "INFO", info_str);
	}
	uv_jsonwriter_add_int(&json, "MAININDEX", obj->obj.main_index);
	if (CANOPEN_IS_INTEGER(obj->obj.type)) {
		uv_jsonwriter_add_int(&json, "SUBINDEX", obj->obj.sub_index);
	}
	char type[64];
	db_type_to_str(obj->obj.type, type);
	uv_jsonwriter_add_string(&json, "TYPE", type);

	printf("Reading parameter %s 0x%x, type: %s\n"
			"data: ",
			info_str,
			obj->obj.main_index,
			type);
	fflush(stdout);

	if (CANOPEN_IS_ARRAY(obj->obj.type)) {
		uint32_t arr_len = 0;
		// fetch the array length
		ret = uv_canopen_sdo_read(db_get_nodeid(&dev.db),
				obj->obj.main_index, 0, CANOPEN_SIZEOF(obj->obj.type), &arr_len);
		if (ret == ERR_NONE) {
			// fetch all the elements
			uv_jsonwriter_begin_array(&json, "DATA");
			for (uint32_t i = 0; i < arr_len; i++) {
				uint32_t data = 0;
				ret = uv_canopen_sdo_read(db_get_nodeid(&dev.db),
						obj->obj.main_index, i + 1, CANOPEN_SIZEOF(obj->obj.type), &data);
				if (ret != ERR_NONE) {
					break;
				}
				else {
					printf("0x%x ", data);
					fflush(stdout);
					uv_jsonwriter_array_add_int(&json, data);
				}
			}
			uv_jsonwriter_end_array(&json);
		}
		printf("\narray length: %u\n", arr_len);
		fflush(stdout);
	}
	else if (CANOPEN_IS_STRING(obj->obj.type)) {
		// string type objects are of variable length. Try to read the maximum length,
		// The INITIATE_DOMAIN_UPLOAD request should scale the read length
		// to the string size.
		char str[65536] = {};
		ret = uv_canopen_sdo_read(db_get_nodeid(&dev.db), obj->obj.main_index,
				0, sizeof(str), str);
		if (ret == ERR_NONE) {
			uv_jsonwriter_add_string(&json, "DATA", str);
		}
		printf("%s\n", str);
		fflush(stdout);
	}
	else {
		// expedited objects
		int32_t data = 0;
		ret = uv_canopen_sdo_read(db_get_nodeid(&dev.db), obj->obj.main_index,
				obj->obj.sub_index, CANOPEN_SIZEOF(obj->obj.type), &data);
		if (ret == ERR_NONE) {
			// write the received data to the json
			uv_jsonwriter_add_int(&json, "DATA", data);
		}
		printf("0x%x\n", data);
		fflush(stdout);
	}
	uv_json_errors_e e = ERR_NONE;
	uv_jsonwriter_end(&json, &e);
	ret |= e;
	// append the temp json file to the dest_json if everything was successful
	if (ret == ERR_NONE) {
		if (!uv_jsonwriter_append_json(dest_json, bfr)) {
			ret |= ERR_ABORTED;
		}
	}

	return ret;
}



void saveparam_step(void *ptr) {
	this->finished = false;
	this->progress = 0;
	fflush(stdout);

	FILE *dest = fopen(this->file, "wb");
	if (dest == NULL) {
		printf("Failed creating the output file '%s'\n", this->file);
		fflush(stdout);
	}
	else {
		char json_buffer[65536] = {};
		uv_json_st json;
		uv_jsonwriter_init(&json, json_buffer, sizeof(json_buffer));
		uv_jsonwriter_begin_array(&json, "PARAMS");

		uv_errors_e e = ERR_NONE;
		db_obj_st obj;

		// CANopen fields that are not found from the database file
		if (this->all) {
			// nodeid
			obj.obj.main_index = CONFIG_CANOPEN_NODEID_INDEX;
			obj.obj.sub_index = 0;
			obj.obj.type = CANOPEN_UNSIGNED8;
			e |= json_add_obj(&json, &obj, "nodeid");

			// heartbeat producer time ms
			obj.obj.main_index = CONFIG_CANOPEN_PRODUCER_HEARTBEAT_INDEX;
			obj.obj.sub_index = 0;
			obj.obj.type = CANOPEN_UNSIGNED16;
			e |= json_add_obj(&json, &obj, "heartbeat producer");

			// heartbeat consumer
			obj.obj.main_index = CONFIG_CANOPEN_CONSUMER_HEARTBEAT_INDEX;
			obj.obj.type = CANOPEN_ARRAY32;
			// note: error checking disabled for heartbeat consumer since
			// it is not mandatory field on the device
			json_add_obj(&json, &obj, "heartbeat consumer");

			// rxpdo's
			for (uint32_t i = 0; i < db_get_rxpdo_count(&dev.db); i++) {
				obj.obj.main_index = CONFIG_CANOPEN_RXPDO_COM_INDEX + i;
				obj.obj.type = CANOPEN_ARRAY32;
				char str[64];
				sprintf(str, "rxpdo %i com", i);
				e |= json_add_obj(&json, &obj, str);

				obj.obj.main_index = CONFIG_CANOPEN_RXPDO_MAP_INDEX + i;
				obj.obj.type = CANOPEN_ARRAY32;
				sprintf(str, "rxpdo %i map", i);
				e |= json_add_obj(&json, &obj, str);
			}

			// txpdo's
			for (uint32_t i = 0; i < db_get_txpdo_count(&dev.db); i++) {
				obj.obj.main_index = CONFIG_CANOPEN_TXPDO_COM_INDEX + i;
				obj.obj.type = CANOPEN_ARRAY32;
				char str[64];
				sprintf(str, "txpdo %i com", i);
				e |= json_add_obj(&json, &obj, str);

				obj.obj.main_index = CONFIG_CANOPEN_TXPDO_MAP_INDEX + i;
				obj.obj.type = CANOPEN_ARRAY32;
				sprintf(str, "txpdo %i map", i);
				e |= json_add_obj(&json, &obj, str);
			}
		}


		// fetch the active operator and the operator count
		uint32_t current_op = 0;
		uint32_t op_count = 0;
		db_obj_st *opdb_obj = NULL;
		for (uint32_t i = 0; i < db_get_object_count(&dev.db); i++) {
			db_obj_st *o = db_get_obj(&dev.db, i);
			if (o->obj_type == DB_OBJ_TYPE_CURRENT_OP) {
				e |= uv_canopen_sdo_read(db_get_nodeid(&dev.db), o->obj.main_index,
						o->obj.sub_index, CANOPEN_SIZEOF(o->obj.type), &current_op);
			}
			else if (o->obj_type == DB_OBJ_TYPE_OP_COUNT) {
				e |= uv_canopen_sdo_read(db_get_nodeid(&dev.db), o->obj.main_index,
						o->obj.sub_index, CANOPEN_SIZEOF(o->obj.type), &op_count);
			}
			else if (o->obj_type == DB_OBJ_TYPE_OPDB) {
				opdb_obj = o;
			}
			else {

			}
		}

		if (op_count > 0 && opdb_obj != NULL) {
			// end PARAMS array
			uv_jsonwriter_end_array(&json);

			// multiple operators found on the device. group all the parameters
			// operator-wise in an array of objects.
			uv_jsonwriter_begin_array(&json, "OPERATORS");

			for (uint32_t i = 0; i < op_count; i++) {
				// change the operator on the device
				uint32_t val = i + 1;
				e |= uv_canopen_sdo_write(db_get_nodeid(&dev.db), opdb_obj->obj.main_index,
						1, CANOPEN_SIZEOF(opdb_obj->obj.type), &val);
				// wait for the device to switch the operator
				uv_rtos_task_delay(100);

				uv_jsonwriter_begin_array(&json, "");
				for (int32_t i = 0; i < db_get_object_count(&dev.db); i++) {
					obj = *db_get_obj(&dev.db, i);
					if (obj.obj_type == DB_OBJ_TYPE_NONVOL_PARAM) {
						e |= json_add_obj(&json, &obj, obj.name);
					}
				}
				// end PARAMS array on OP
				uv_jsonwriter_end_array(&json);
			}
			// end OPERATORS array
			uv_jsonwriter_end_array(&json);

			// add current op field
			uv_jsonwriter_add_int(&json, "CURRENT_OP", current_op);

			// add opdb information
			uv_jsonwriter_add_int(&json, "OPDB_MAININDEX", opdb_obj->obj.main_index);
			char str[128];
			db_type_to_str(opdb_obj->obj.type, str);
			uv_jsonwriter_add_string(&json, "OPDB_TYPE", str);

			// lastly set the current operator back to the default one on the device
			uint32_t val = current_op + 1;
			e |= uv_canopen_sdo_write(db_get_nodeid(&dev.db), opdb_obj->obj.main_index,
					1, CANOPEN_SIZEOF(opdb_obj->obj.type), &val);
		}
		else {
			// current_op and op_count parameters could not be found.
			// Thus just load all the parameters as-is.
			for (int32_t i = 0; i < db_get_object_count(&dev.db); i++) {
				obj = *db_get_obj(&dev.db, i);
				if (obj.obj_type == DB_OBJ_TYPE_NONVOL_PARAM) {
					e |= json_add_obj(&json, &obj, obj.name);
				}
			}
			// end PARAMS array
			uv_jsonwriter_end_array(&json);
		}



		// end the whole JSON file
		uv_jsonwriter_end(&json, NULL);

		if (e != ERR_NONE) {
			printf("\n**** ERROR *****\n"
					"Some or all of the parameters returned an error. Some parameters\n"
					"might not be stored correctly.\n\n");
			fflush(stdout);
		}
		if (opdb_obj == NULL || op_count == 0) {
			printf("\n**** WARNING *****\n"
					"Operator database parameter or operator count parameter not\n"
					"found on the device. All parameters were saved in PARAMS array.\n\n");
			fflush(stdout);
		}
		fwrite(json_buffer, 1, strlen(json_buffer), dest);
		fclose(dest);
		printf("Refactoring the JSON file\n");
		fflush(stdout);
		char cmd[1024];
		char tempname[270];
		sprintf(tempname, "__%s_temp", this->file);
		sprintf(cmd, "jq '.' %s > %s", this->file, tempname);
		printf("Running the command: %s\n", cmd);
		fflush(stdout);
		if (system(cmd) == 0) {
			// copy the refactored output to the actual file and remove the temp file
			sprintf(cmd, "cp %s %s", tempname, this->file);
			if (system(cmd));
		}
		sprintf(cmd, "rm %s", tempname);
		if (system(cmd));

		printf("All parameters stored successfully to '%s'\n", this->file);
		fflush(stdout);
	}
	this->finished = true;
}







