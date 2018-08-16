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



#include <db.h>
#include <stdio.h>
#include <string.h>
#include <uv_terminal.h>
#include <ncurses.h>
#include <uv_json.h>
#include "main.h"



void type_to_str(canopen_object_type_e type, char *dest) {
	if (type == CANOPEN_UNSIGNED32) {
		strcpy(dest, "UNSIGNED32");
	}
	else if (type == CANOPEN_SIGNED32) {
		strcpy(dest, "SIGNED32");
	}
	else if (type == CANOPEN_UNSIGNED16) {
		strcpy(dest, "UNSIGNED16");
	}
	else if (type == CANOPEN_SIGNED16) {
		strcpy(dest, "SIGNED16");
	}
	else if (type == CANOPEN_UNSIGNED8) {
		strcpy(dest, "UNSIGNED8");
	}
	else if (type == CANOPEN_SIGNED8) {
		strcpy(dest, "SIGNED8");
	}
	else if (type == CANOPEN_ARRAY32) {
		strcpy(dest, "ARRAY32");
	}
	else if (type == CANOPEN_ARRAY16) {
		strcpy(dest, "ARRAY16");
	}
	else if (type == CANOPEN_ARRAY8) {
		strcpy(dest, "ARRAY8");
	}
	else {
		strcpy(dest, "UNKNOWN");
	}
}

void permission_to_str(canopen_permissions_e permissions, char *dest) {
	if (permissions == CANOPEN_RO) {
		strcpy(dest, "RO");
	}
	else if (permissions == CANOPEN_RW) {
		strcpy(dest, "RW");
	}
	else if (permissions == CANOPEN_WO) {
		strcpy(dest, "WO");
	}
	else {
		strcpy(dest, "UNKNOWN");
	}
}

static canopen_object_type_e str_to_type(char *json_child) {
	char str[128];
	canopen_object_type_e ret;
	uv_jsonreader_get_string(json_child, str, 128);
	if (strcmp(str, "UNSIGNED32") == 0 || strcmp(str, "SIGNED32") == 0) {
		ret = CANOPEN_UNSIGNED32;
	}
	else if (strcmp(str, "UNSIGNED16") == 0 || strcmp(str, "SIGNED16") == 0) {
		ret = CANOPEN_UNSIGNED16;
	}
	else if (strcmp(str, "UNSIGNED8") == 0 || strcmp(str, "SIGNED8") == 0) {
		ret = CANOPEN_UNSIGNED8;
	}
	else if (strcmp(str, "ARRAY32") == 0) {
		ret = CANOPEN_ARRAY32;
	}
	else if (strcmp(str, "ARRAY16") == 0) {
		ret = CANOPEN_ARRAY16;
	}
	else if (strcmp(str, "ARRAY8") == 0) {
		ret = CANOPEN_ARRAY8;
	}
	else {
		ret = CANOPEN_UNSIGNED8;
	}
	return ret;
}

static canopen_permissions_e str_to_permissions(char *json_child) {
	char str[64];
	canopen_permissions_e ret;
	uv_jsonreader_get_string(json_child, str, 64);
	if (strcmp(str, "WO") == 0) {
		ret = CANOPEN_WO;
	}
	else if (strcmp(str, "RW") == 0) {
		ret = CANOPEN_RW;
	}
	else if (strcmp(str, "RO") == 0) {
		ret = CANOPEN_RO;
	}
	else {
		ret = CANOPEN_RO;
	}
	return ret;
}




static bool parse_json(db_st *this, char *data) {
	bool ret = true;

	uv_jsonreader_init(data, strlen(data));

	char *obj;

	// nodeid
	obj = uv_jsonreader_find_child(data, "nodeid", 1);
	if (obj != NULL) {
		this->node_id = uv_jsonreader_get_int(obj);
	}

	// object dictionary
	obj = uv_jsonreader_find_child(data, "OBJ_DICT", 1);
	if ((obj != NULL) && (uv_jsonreader_get_type(obj) == JSON_ARRAY)) {

		for (int i = 0; i < uv_jsonreader_array_get_size(obj); i++) {
			char *child = uv_jsonreader_array_at(obj, i);
			if (child == NULL) {
				ret = false;
				break;
			}
			else {
				// start parsing data from child
				db_obj_st obj;

				char *data = uv_jsonreader_find_child(child, "name", 1);
				uv_jsonreader_get_string(data, obj.name, 128);

				data = uv_jsonreader_find_child(child, "index", 1);
				obj.obj.main_index = uv_jsonreader_get_int(data);

				data = uv_jsonreader_find_child(child, "subindex", 1);
				obj.obj.sub_index = uv_jsonreader_get_int(data);

				data = uv_jsonreader_find_child(child, "type", 1);
				obj.obj.type = str_to_type(data);

				data = uv_jsonreader_find_child(child, "permissions", 1);
				obj.obj.permissions = str_to_permissions(data);

				data = uv_jsonreader_find_child(child, "data", 1);
				uv_jsonreader_get_string(data, obj.data, sizeof(obj.data));

				data = uv_jsonreader_find_child(child, "min", 1);
				obj.min = uv_jsonreader_get_int(data);

				data = uv_jsonreader_find_child(child, "max", 1);
				obj.max = uv_jsonreader_get_int(data);

				data = uv_jsonreader_find_child(child, "default", 1);
				obj.def = uv_jsonreader_get_int(data);


				uv_vector_push_back(&dev.db.objects, &obj);

			}
		}

	}
	else {
		printf("ERROR: OBJ_DICT array not found from JSON.\n");
		ret = false;
	}

	return ret;
}


#define this (&dev)


bool cmd_db(const char *arg) {
	bool ret = false;

	uv_vector_init(&this->db.objects, this->db.objects_buffer,
			sizeof(this->db.objects_buffer) / sizeof(this->db.objects_buffer[0]),
			sizeof(this->db.objects_buffer[0]));

	// try to load the CANOpen database

	FILE *fptr = fopen(arg, "r");

	if (fptr == NULL) {
		// failed to open the file, exit this task
		printf("Failed to open database file %s.\n", arg);
	}
	else {
		int32_t size;
		fseek(fptr, 0, SEEK_END);
		size = ftell(fptr);
		rewind(fptr);
		printf("file size: %u\n", size);

		if (size >= DB_MAX_FILE_SIZE) {
			printf("*********\n"
					"JSON max file size exceeded. Parsing failed.\n"
					"*********\n");
		}
		else {
			char data[DB_MAX_FILE_SIZE];
			if (fread(data, 1, size, fptr)) {
				if (parse_json(&this->db, data)) {
					printf("JSON parsed succesfully.\n");
					ret = true;
				}
				else {
					printf("ERROR: Parsing the JSON file failed.\n");
				}
			}
			else {
				printf("ERROR: Reading the JSON file failed.\n");
			}

		}
	}

	if (ret == true) {
//		printf("PARSED DATA:\n\n");
//
//		printf("Node ID: %u\nObject Dictionary:\n", this->nodeid);
//		for (uint8_t i = 0; i < uv_vector_size(&this->db.objects); i++) {
//			db_obj_st *obj = uv_vector_at(&this->db.objects, i);
//
//			char type[64], perm[64];
//			type_to_str(obj->obj.type, type);
//			permission_to_str(obj->obj.permissions, perm);
//
//			printf("%u: \n"
//					"   name: %s\n"
//					"   index: 0x%x\n"
//					"   subindex: %u\n"
//					"   type: %s\n"
//					"   permissions: %s\n"
//					"   dataptr: %s\n"
//					"   min: %i\n"
//					"   max: %i\n"
//					"   default: %i\n\n",
//					i, obj->name, obj->obj.main_index,
//					obj->obj.sub_index, type, perm,
//					obj->data, obj->min, obj->max, obj->def);
//		}
	}

	return ret;
}



