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





bool cmd_saveparam(const char *arg) {
	bool ret = true;

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
static uv_errors_e json_add_obj(uv_json_st *json, db_obj_st *obj) {
	uv_errors_e ret = ERR_NONE;

	static uint32_t i = 0;

	char name[64];
	sprintf(name, "obj %u", i++);
	uv_jsonwriter_begin_object(json, name);
	uv_jsonwriter_add_int(json, "MAININDEX", obj->obj.main_index);
	uv_jsonwriter_add_int(json, "SUBINDEX", obj->obj.sub_index);
	char type[64];
	db_type_to_str(obj->obj.type, type);
	uv_jsonwriter_add_string(json, "TYPE", type);

	printf("Reading parameter 0x%x, type: %s, data: ",
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
			uv_jsonwriter_begin_array(json, "DATA");
			for (uint32_t i = 0; i < arr_len; i++) {
				uint32_t data = 0;
				ret = uv_canopen_sdo_read(db_get_nodeid(&dev.db),
						obj->obj.main_index, i, CANOPEN_SIZEOF(obj->obj.type), &data);
				if (ret != ERR_NONE) {
					break;
				}
				else {
					printf("0x%x ", data);
					fflush(stdout);
					uv_jsonwriter_array_add_int(json, data);
				}
			}
			uv_jsonwriter_end_array(json);
		}
		printf(", array length: %u\n", arr_len);
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
			uv_jsonwriter_add_string(json, "DATA", str);
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
			uv_jsonwriter_add_int(json, "DATA", data);
		}
		printf("0x%x\n", data);
		fflush(stdout);
	}
	uv_jsonwriter_end_object(json);

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

		uv_errors_e e = ERR_NONE;

		// CANopen fields that are not found from the database file
		// nodeid
		db_obj_st obj;
		obj.obj.main_index = CONFIG_CANOPEN_NODEID_INDEX;
		obj.obj.sub_index = 0;
		obj.obj.type = CANOPEN_UNSIGNED8;
		e = json_add_obj(&json, &obj);

		// heartbeat producer time ms
		obj.obj.main_index = CONFIG_CANOPEN_PRODUCER_HEARTBEAT_INDEX;
		obj.obj.sub_index = 0;
		obj.obj.type = CANOPEN_UNSIGNED16;
		e = json_add_obj(&json, &obj);

		// heartbeat consumer
		obj.obj.main_index = CONFIG_CANOPEN_CONSUMER_HEARTBEAT_INDEX;
		obj.obj.type = CANOPEN_ARRAY32;
		// note: error checking disabled for heartbeat consumer since
		// it is not mandatory field on the device
		e = json_add_obj(&json, &obj);
		e = ERR_NONE;

		// rxpdo's
		for (uint32_t i = 0; i < db_get_rxpdo_count(&dev.db); i++) {
			obj.obj.main_index = CONFIG_CANOPEN_RXPDO_COM_INDEX + i;
			obj.obj.type = CANOPEN_ARRAY32;
			json_add_obj(&json, &obj);

			obj.obj.main_index = CONFIG_CANOPEN_RXPDO_MAP_INDEX + i;
			obj.obj.type = CANOPEN_ARRAY32;
			json_add_obj(&json, &obj);
		}

		// txpdo's
		for (uint32_t i = 0; i < db_get_txpdo_count(&dev.db); i++) {
			obj.obj.main_index = CONFIG_CANOPEN_TXPDO_COM_INDEX + i;
			obj.obj.type = CANOPEN_ARRAY32;
			json_add_obj(&json, &obj);

			obj.obj.main_index = CONFIG_CANOPEN_TXPDO_MAP_INDEX + i;
			obj.obj.type = CANOPEN_ARRAY32;
			json_add_obj(&json, &obj);
		}

		int32_t i;
		for (i = 0; i < db_get_object_count(&dev.db); i++) {
			obj = *db_get_obj(&dev.db, i);
			if (obj.obj_type == DB_OBJ_TYPE_NONVOL_PARAM) {
				e = json_add_obj(&json, &obj);

				if (e != ERR_NONE) {
					break;
				}
			}
		}
		if (e != ERR_NONE) {
			printf("**** ERROR *****\n"
					"Reading the parameters from the device was suspended since\n"
					"errors were encountered. \n"
					"Parameter Main index: 0x%x, subindex: %u.\n"
					"Error code: %u, last SDO error code: 0x%08x\n"
					"parameter %u of %u\n\n",
					obj.obj.main_index,
					obj.obj.sub_index,
					e,
					uv_canopen_sdo_get_error(),
					i,
					db_get_object_count(&dev.db));
			fflush(stdout);
		}
		else {
			fwrite(json_buffer, 1, strlen(json_buffer), dest);
			fclose(dest);
			printf("All parameters stored successfully to '%s'\n", this->file);
			fflush(stdout);
		}
	}
	this->finished = true;
}







