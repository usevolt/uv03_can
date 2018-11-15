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
#include <ctype.h>
#include "main.h"


static bool is_loaded = false;

static void str_to_upper_nonspace(char *str) {
	while (*str != '\0') {
		if (isspace(*str)) {
			*str = '_';
		}
		else {
			*str = toupper(*str);
		}
		str++;
	}
}


void dbvalue_init(dbvalue_st *this) {
	this->type = DBVALUE_INT;
	this->value_int = 0;
}


dbvalue_st dbvalue_set_int(int32_t value) {
	dbvalue_st this;
	this.type = DBVALUE_INT;
	this.value_int = value;
	return this;
}

dbvalue_st dbvalue_set_string(char *str, uint32_t str_len) {
	dbvalue_st this;
	this.type = DBVALUE_STRING;
	this.value_str = malloc(str_len + 1);
	memcpy(this.value_str, str, str_len);
	this.value_str[str_len] = '\0';

	str_to_upper_nonspace(this.value_str);

	// if string value was set, search defines and assign the value which
	// matches by name. Otherwise report an error.
	bool match = false;
	for (uint32_t i = 0; i < uv_vector_size(&dev.db.defines); i++) {
		db_define_st *d = uv_vector_at(&dev.db.defines, i);

		if (strcmp(d->name, this.value_str) == 0) {
			match = true;
			this.value_int = d->value;
			break;
		}
	}
	if (!match) {
		this.value_int = 0;
		printf("**** ERROR **** Cannot find '%s' definition from the database.\n", this.value_str);
	}
	return this;
}

void dbvalue_free(dbvalue_st *this) {
	if (this->type == DBVALUE_STRING) {
		free(this->value_str);
	}
}




void db_array_child_init(db_array_child_st *this) {
	dbvalue_init(&this->def);
	dbvalue_init(&this->max);
	dbvalue_init(&this->min);
}


bool db_is_loaded(db_st *this) {
	return is_loaded;
}

void db_type_to_str(canopen_object_type_e type, char *dest) {
	if (type == CANOPEN_UNSIGNED32) {
		strcpy(dest, "CANOPEN_UNSIGNED32");
	}
	else if (type == CANOPEN_SIGNED32) {
		strcpy(dest, "CANOPEN_SIGNED32");
	}
	else if (type == CANOPEN_UNSIGNED16) {
		strcpy(dest, "CANOPEN_UNSIGNED16");
	}
	else if (type == CANOPEN_SIGNED16) {
		strcpy(dest, "CANOPEN_SIGNED16");
	}
	else if (type == CANOPEN_UNSIGNED8) {
		strcpy(dest, "CANOPEN_UNSIGNED8");
	}
	else if (type == CANOPEN_SIGNED8) {
		strcpy(dest, "CANOPEN_SIGNED8");
	}
	else if (type == CANOPEN_ARRAY32) {
		strcpy(dest, "CANOPEN_ARRAY32");
	}
	else if (type == CANOPEN_ARRAY16) {
		strcpy(dest, "CANOPEN_ARRAY16");
	}
	else if (type == CANOPEN_ARRAY8) {
		strcpy(dest, "CANOPEN_ARRAY8");
	}
	else if (type == CANOPEN_STRING) {
		strcpy(dest, "CANOPEN_STRING");
	}
	else {
		strcpy(dest, "UNKNOWN");
	}
}

void db_permission_to_str(canopen_permissions_e permissions, char *dest) {
	if (permissions == CANOPEN_RO) {
		strcpy(dest, "CANOPEN_RO");
	}
	else if (permissions == CANOPEN_RW) {
		strcpy(dest, "CANOPEN_RW");
	}
	else if (permissions == CANOPEN_WO) {
		strcpy(dest, "CANOPEN_WO");
	}
	else {
		strcpy(dest, "UNKNOWN");
	}
}

void db_permission_to_longstr(canopen_permissions_e permissions, char *dest) {
	if (permissions == CANOPEN_RO) {
		strcpy(dest, "Read only");
	}
	else if (permissions == CANOPEN_RW) {
		strcpy(dest, "Read / Write");
	}
	else if (permissions == CANOPEN_WO) {
		strcpy(dest, "Write only");
	}
	else {
		strcpy(dest, "UNKNOWN");
	}
}


static canopen_object_type_e str_to_type(char *json_child) {
	char str[128];
	canopen_object_type_e ret;
	uv_jsonreader_get_string(json_child, str, 128);
	if (strcmp(str, "CANOPEN_UNSIGNED32") == 0 || strcmp(str, "CANOPEN_SIGNED32") == 0) {
		ret = CANOPEN_UNSIGNED32;
	}
	else if (strcmp(str, "CANOPEN_UNSIGNED16") == 0 || strcmp(str, "CANOPEN_SIGNED16") == 0) {
		ret = CANOPEN_UNSIGNED16;
	}
	else if (strcmp(str, "CANOPEN_UNSIGNED8") == 0 || strcmp(str, "CANOPEN_SIGNED8") == 0) {
		ret = CANOPEN_UNSIGNED8;
	}
	else if (strcmp(str, "CANOPEN_ARRAY32") == 0) {
		ret = CANOPEN_ARRAY32;
	}
	else if (strcmp(str, "CANOPEN_ARRAY16") == 0) {
		ret = CANOPEN_ARRAY16;
	}
	else if (strcmp(str, "CANOPEN_ARRAY8") == 0) {
		ret = CANOPEN_ARRAY8;
	}
	else if (strcmp(str, "CANOPEN_STRING") == 0) {
		ret = CANOPEN_STRING;
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
	if (strcmp(str, "CANOPEN_WO") == 0) {
		ret = CANOPEN_WO;
	}
	else if (strcmp(str, "CANOPEN_RW") == 0) {
		ret = CANOPEN_RW;
	}
	else if (strcmp(str, "CANOPEN_RO") == 0) {
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

	// dev name
	obj = uv_jsonreader_find_child(data, "DEV", 1);
	if (obj != NULL) {
		uv_jsonreader_get_string(obj, this->dev_name, sizeof(this->dev_name));
	}
	else {
		printf("*** ERROR *** 'DEV' object not found in the JSON\n");
	}

	// nodeid
	obj = uv_jsonreader_find_child(data, "NODEID", 1);
	if (obj != NULL) {
		this->node_id = uv_jsonreader_get_int(obj);
		if (dev.nodeid == 0) {
			dev.nodeid = this->node_id;
		}
	}
	else {
		printf("*** ERROR *** 'NODEID' object not found in the JSON\n");
	}


	// emcy
	obj = uv_jsonreader_find_child(data, "EMCY", 1);
	if (uv_jsonreader_get_type(obj) != JSON_ARRAY) {
		printf("*** JSON ERROR **** EMCY array is not an array\n");
	}
	else if (obj != NULL) {
		for (unsigned int i = 0; i < uv_jsonreader_array_get_size(obj); i++) {
			db_emcy_st emcy;
			uv_jsonreader_array_get_string(obj, i, emcy.name, sizeof(emcy.name));
			emcy.value = i;
			uv_vector_push_back(&this->emcys, &emcy);
		}
	}
	else {

	}

	// defines
	obj = uv_jsonreader_find_child(data, "DEFINES", 1);
	if (obj != NULL) {
		if (uv_jsonreader_get_type(obj) != JSON_ARRAY) {
			printf("*** JSON ERROR **** DEFINES array is not an array\n");
		}
		else {
			for (unsigned int i = 0; i < uv_jsonreader_array_get_size(obj); i++) {
				db_define_st define;
				char *d = uv_jsonreader_array_at(obj, i);
				if (d != NULL) {
					char *v = uv_jsonreader_find_child(d, "name", 1);
					uv_jsonreader_get_string(v, define.name, sizeof(define.name));
					char *s = define.name;
					while(*s != '\0') {
						if (isspace(*s)) {
							*s = '_';
						}
						else {
							*s = toupper(*s);
						}
						s++;
					}
					v = uv_jsonreader_find_child(d, "value", 1);
					define.value = uv_jsonreader_get_int(v);
					uv_vector_push_back(&this->defines, &define);
				}
				else {
					printf("*** ERROR *** DEFINES array member was not an object at index %u\n", i);
				}
			}
		}
	}
	else {

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

				data = uv_jsonreader_find_child(child, "type", 1);
				obj.obj.type = str_to_type(data);

				data = uv_jsonreader_find_child(child, "permissions", 1);
				obj.obj.permissions = str_to_permissions(data);

				if (!CANOPEN_IS_ARRAY(obj.obj.type)) {
					// string parameters
					if (CANOPEN_IS_STRING(obj.obj.type)) {
						data = uv_jsonreader_find_child(child, "stringsize", 1);
						uv_json_types_e type = uv_jsonreader_get_type(data);
						if (type == JSON_INT) {
							obj.obj.string_len = uv_jsonreader_get_int(data);
							obj.string_len = dbvalue_set_int(uv_jsonreader_get_int(data));
						}
						else if (type == JSON_STRING) {
							obj.string_len = dbvalue_set_string(uv_jsonreader_get_string_ptr(data),
									uv_jsonreader_get_string_len(data));
							obj.obj.string_len = obj.string_len.value_int;
						}
						else {
							dbvalue_init(&obj.string_len);
						}

						data = uv_jsonreader_find_child(child, "default", 1);
						uv_jsonreader_get_string(data, obj.string_def, sizeof(obj.string_def));
					}
					// integer parameters
					else {
						data = uv_jsonreader_find_child(child, "subindex", 1);
						obj.obj.sub_index = uv_jsonreader_get_int(data);

						// writable integer parameters
						if (obj.obj.permissions != CANOPEN_RO) {

							data = uv_jsonreader_find_child(child, "min", 1);
							uv_json_types_e type = uv_jsonreader_get_type(data);
							if (type == JSON_INT) {
								obj.min = dbvalue_set_int(uv_jsonreader_get_int(data));
							}
							else if (type == JSON_STRING) {
								obj.min = dbvalue_set_string(uv_jsonreader_get_string_ptr(data),
										uv_jsonreader_get_string_len(data));
							}
							else {
								dbvalue_init(&obj.min);
							}

							data = uv_jsonreader_find_child(child, "max", 1);
							type = uv_jsonreader_get_type(data);
							if (type == JSON_INT) {
								obj.max = dbvalue_set_int(uv_jsonreader_get_int(data));
							}
							else if (type == JSON_STRING) {
								obj.max = dbvalue_set_string(uv_jsonreader_get_string_ptr(data),
										uv_jsonreader_get_string_len(data));
							}
							else {
								dbvalue_init(&obj.max);
							}

							data = uv_jsonreader_find_child(child, "default", 1);
							type = uv_jsonreader_get_type(data);
							if (type == JSON_INT) {
								obj.def = dbvalue_set_int(uv_jsonreader_get_int(data));
							}
							else if (type == JSON_STRING) {
								obj.def = dbvalue_set_string(uv_jsonreader_get_string_ptr(data),
										uv_jsonreader_get_string_len(data));
							}
							else {
								dbvalue_init(&obj.def);
							}
						}
						// read-only integer parameters
						else {
							data = uv_jsonreader_find_child(child, "value", 1);
							if (data == NULL) {
								data = uv_jsonreader_find_child(child, "default", 1);
							}
							uv_json_types_e type = uv_jsonreader_get_type(data);
							if (type == JSON_INT) {
								obj.value = dbvalue_set_int(uv_jsonreader_get_int(data));
							}
							else if (type == JSON_STRING) {
								obj.value = dbvalue_set_string(uv_jsonreader_get_string_ptr(data),
										uv_jsonreader_get_string_len(data));
							}
							else {
								dbvalue_init(&obj.value);
							}
						}
					}
				}
				// array parameters
				else {
					data = uv_jsonreader_find_child(child, "arraysize", 1);
					obj.obj.array_max_size = uv_jsonreader_get_int(data);
				}

				data = uv_jsonreader_find_child(child, "dataptr", 1);
				uv_jsonreader_get_string(data, obj.dataptr, sizeof(obj.dataptr));



				if (CANOPEN_IS_ARRAY(obj.obj.type)) {
					// cycle trough children and make a new object for each of them
					// obj main index and type are already inherited from the array object

					db_array_child_st *thischild;

					void **last_ptr = (void**) &obj.child_ptr;
					char *children = uv_jsonreader_find_child(child, "data", 1);

					if (children == NULL || uv_jsonreader_get_type(children) != JSON_ARRAY) {
						printf("children array not an array! 0x%x\n", children);
					}
					for (uint8_t i = 0; i < obj.obj.array_max_size; i++) {
						thischild = malloc(sizeof(db_array_child_st));
						db_array_child_init(thischild);

						*last_ptr = thischild;
						last_ptr = &(thischild->next_sibling);
						thischild->next_sibling = NULL;

						char *str = uv_jsonreader_array_at(children, i);
						if (str == NULL) {
							printf("ERROR: Array object didn't have enought children. \n"
									"Number of children: %u, should be: %u\n", i, obj.obj.array_max_size);
						}
						else {
							data = uv_jsonreader_find_child(str, "name", 1);
							uv_jsonreader_get_string(data, thischild->name, sizeof(thischild->name));

							data = uv_jsonreader_find_child(str, "min", 1);
							uv_json_types_e type = uv_jsonreader_get_type(data);
							if (obj.obj.permissions != CANOPEN_RO) {
								if (type == JSON_INT) {
									thischild->min = dbvalue_set_int(uv_jsonreader_get_int(data));
								}
								else if (type == JSON_STRING) {
									thischild->min = dbvalue_set_string(uv_jsonreader_get_string_ptr(data),
											uv_jsonreader_get_string_len(data));
								}
								else {
									dbvalue_init(&thischild->min);
								}
								data = uv_jsonreader_find_child(str, "max", 1);
								type = uv_jsonreader_get_type(data);
								if (type == JSON_INT) {
									thischild->max = dbvalue_set_int(uv_jsonreader_get_int(data));
								}
								else if (type == JSON_STRING) {
									thischild->max = dbvalue_set_string(uv_jsonreader_get_string_ptr(data),
											uv_jsonreader_get_string_len(data));
								}
								else {
									dbvalue_init(&thischild->max);
								}
							}

							data = uv_jsonreader_find_child(str, "default", 1);
							if (data == NULL) {
								data = uv_jsonreader_find_child(str, "value", 1);
							}
							type = uv_jsonreader_get_type(data);
							if (type == JSON_INT) {
								thischild->def = dbvalue_set_int(uv_jsonreader_get_int(data));
							}
							else if (type == JSON_STRING) {
								thischild->def = dbvalue_set_string(uv_jsonreader_get_string_ptr(data),
										uv_jsonreader_get_string_len(data));
							}
							else {
								dbvalue_init(&thischild->def);
							}
						}
					}
				}

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


#define this (&dev.db)


bool cmd_db(const char *arg) {
	bool ret = false;

	// if database is already loaded, free the old memory
	if (is_loaded) {
		db_deinit();
	}

	uv_vector_init(&this->objects, this->objects_buffer,
			sizeof(this->objects_buffer) / sizeof(this->objects_buffer[0]),
			sizeof(this->objects_buffer[0]));

	uv_vector_init(&this->emcys, this->emcys_buffer,
			sizeof(this->emcys_buffer) / sizeof(this->emcys_buffer[0]),
			sizeof(this->emcys_buffer[0]));

	uv_vector_init(&this->defines, this->defines_buffer,
			sizeof(this->defines_buffer) / sizeof(this->defines_buffer[0]),
			sizeof(this->defines_buffer[0]));

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
				if (parse_json(this, data)) {
					printf("JSON parsed succesfully.\n");
					strcpy(dev.db.filepath, arg);
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

	is_loaded = ret;

	if (ret == true) {
//		printf("PARSED DATA:\n\n");
//
//		printf("Node ID: %u\nObject Dictionary:\n", this->nodeid);
//		for (uint8_t i = 0; i < uv_vector_size(&this->objects); i++) {
//			db_obj_st *obj = uv_vector_at(&this->objects, i);
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


static void free_child(db_array_child_st *child) {
	if (child->next_sibling != NULL) {
		free_child(child->next_sibling);
	}
	dbvalue_free(&child->def);
	dbvalue_free(&child->max);
	dbvalue_free(&child->min);
	free(child);
}

void db_deinit(void) {
	is_loaded = false;
	for (int i = 0; i < db_get_object_count(this); i++) {
		db_obj_st *obj = db_get_obj(this, i);
		if (CANOPEN_IS_ARRAY(obj->obj.type)) {
			free_child(obj->child_ptr);
		}
		else if (CANOPEN_IS_INTEGER(obj->obj.type)) {
			if (obj->obj.permissions == CANOPEN_RO) {
				dbvalue_free(&obj->value);
			}
			else {
				dbvalue_free(&obj->def);
				dbvalue_free(&obj->max);
				dbvalue_free(&obj->min);
			}
		}
		else if (CANOPEN_IS_STRING(obj->obj.type)) {
			dbvalue_free(&obj->string_len);
		}
		else {

		}
	}
}

