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
#include <uv_json.h>
#include <ctype.h>
#include "main.h"
#include <libgen.h>


static bool parse_defines(db_st *this, char *obj);
static void remove_defines(db_st *this, char *obj);



static bool is_loaded = false;
static bool is_nodeid_set = false;

static void str_to_upper_nonspace(char *str) {
	while (*str != '\0') {
		if (isspace(*str) ||
				*str == ':' ||
				*str == ',' ||
				*str == '=') {
			*str = '_';
		}
		else {
			*str = toupper(*str);
		}
		str++;
	}
}



void db_obj_init(db_obj_st *this) {
	dbvalue_init(&this->name);
	dbvalue_init(&this->dataptr);
	strcpy(this->type_str, "");
	this->obj_type = DB_OBJ_TYPE_COUNT;
	this->numsys = DB_OBJ_NUMSYS_DEC;
	dbvalue_init(&this->value);
	dbvalue_init(&this->def);
	dbvalue_init(&this->min);
	dbvalue_init(&this->max);
	this->child_ptr = NULL;
	dbvalue_init(&this->array_max_size);
	dbvalue_init(&this->string_len);
	dbvalue_init(&this->string_def);
}


static void free_child(db_array_child_st *child) {
	if (child) {
		if (child->next_sibling != NULL) {
			free_child(child->next_sibling);
		}
		dbvalue_free(&child->name);
		dbvalue_free(&child->def);
		dbvalue_free(&child->max);
		dbvalue_free(&child->min);
		free(child);
	}
}

void db_obj_deinit(db_obj_st *this) {
	dbvalue_free(&this->name);
	dbvalue_free(&this->dataptr);
	dbvalue_free(&this->value);
	dbvalue_free(&this->def);
	dbvalue_free(&this->min);
	dbvalue_free(&this->max);
	dbvalue_free(&this->array_max_size);
	dbvalue_free(&this->string_len);
	dbvalue_free(&this->string_def);
	free_child(this->child_ptr);
}



void dbvalue_init(dbvalue_st *this) {
	this->type = DBVALUE_INT;
	this->value_int = 0;
	this->value_str = "\0";
}


void dbvalue_set_int(dbvalue_st *this, int32_t value) {
	dbvalue_free(this);
	// all dbvalues are stored as string. Integer has to be converted to a string
	this->type = DBVALUE_STRING;
	this->value_int = value;
	char str[1000] = {};
	snprintf(str, sizeof(str) - 1, "%i", value);
	this->value_str = malloc(strlen(str) + 1);
	strcpy(this->value_str, str);
}

void dbvalue_set_string(dbvalue_st *this, char *str, uint32_t str_len) {
	dbvalue_free(this);
	// strings that contain hexadecimal numbers are read as integer values
	bool is_digit = false;
	if (strncmp(str, "0x", 2) == 0) {
		is_digit = true;
		for (uint16_t i = 2; i < strlen(str); i++) {
			if (!isxdigit(str[i])) {
				is_digit = false;
				break;
			}
		}
	}

	if (is_digit) {
		dbvalue_set_int(this, strtol(str, NULL, 0));
	}
	// string was not starting as hex number, parse it and create string object
	else {
		this->type = DBVALUE_STRING;
		this->value_str = malloc(str_len + 1);
		memcpy(this->value_str, str, str_len);
		this->value_str[str_len] = '\0';

		char s[1024] = {};
		strcpy(s, this->value_str);
		str_to_upper_nonspace(s);


		// if string value was set, search defines and assign the value that
		// matches by name
		bool match = false;
		for (uint32_t i = 0; i < uv_vector_size(&dev.db.defines); i++) {
			db_define_st *d = uv_vector_at(&dev.db.defines, i);

			if (d->type == DB_DEFINE_INT) {
				if (strcmp(d->name, s) == 0) {
					this->value_int = d->value;
					strcpy(this->value_str, s);
					match = true;
					break;
				}
			}
			else if (d->type == DB_DEFINE_STRING) {
				// append the string as is
				if (strstr(s, d->name)) {
					// replace the define string from value_str
					int len_diff = strlen(d->str) - strlen(d->name);
					int len = strlen(this->value_str) + len_diff;
					char *strr = malloc(len + 1);
					memset(strr, 0, len + 1);
					char *match = strstr(s, d->name);
					strncpy(strr, this->value_str, match - s);
					strcat(strr, d->str);
					strcat(strr, this->value_str + (match - s) + strlen(d->name));

					// copy new string to dbvalue
					free(this->value_str);
					this->value_str = malloc(strlen(strr) + 1);
					strcpy(this->value_str, strr);
					// note: match has to be set to false, otherwise
					// the dev name would be appended to the string
					match = false;
					break;
				}
			}
			else if (d->type == DB_DEFINE_ENUM) {
				// check if the dbvalue string starts with the same substring as d
				if (strstr(s, d->name) == s) {
					if (strlen(s) < (strlen(d->name) + 1)) {
						printf("**** ERROR **** Define with ENUM type not found with a name of '%s'\n",
								s);
					}
					else {
						bool m = false;
						for (int32_t i = 0; i < d->child_count; i++) {
							char *str = s + strlen(d->name) + 1;
							char childname[256];
							strcpy(childname, d->childs[i]);
							// remove possible '=' characters from the name, as well as trailing space
							char *c = strstr(childname, "=");
							while (c != NULL && c != childname && (isspace(*c) || *c == '=')) {
								*c = '\0';
								c--;
							}
							if (strcmp(str, childname) == 0) {
								this->value_int = i;
								m = true;
								break;
							}
						}
						if (!m) {
							printf("**** ERROR **** No ENUM define found with name of '%s' for define '%s'\n",
									s, d->name);
						}
						else {
							match = true;
						}
					}
				}
			}
		}
		if (!match) {
			this->value_int = 0;
		}
		else {
			// match found, update the dbvalue string to contain the device name
			if (strncmp(dev.db.dev_name_upper, s, strlen(dev.db.dev_name_upper)) != 0) {
				free(this->value_str);
				this->value_str = malloc(str_len + 1 + strlen(dev.db.dev_name_upper) + 1);
				sprintf(this->value_str, "%s_", dev.db.dev_name_upper);
				strncat(this->value_str, str, str_len);
			}
		}
	}
}

void dbvalue_set(dbvalue_st *this, char *jsonobj) {
	uv_json_types_e type = uv_jsonreader_get_type(jsonobj);
	if (type == JSON_INT) {
		dbvalue_set_int(this, uv_jsonreader_get_int(jsonobj));
	}
	else if (type == JSON_STRING) {
		dbvalue_set_string(this,
				uv_jsonreader_get_string_ptr(jsonobj),
				uv_jsonreader_get_string_len(jsonobj));
	}
	else {
		dbvalue_init(this);
	}

}



void dbvalue_free(dbvalue_st *this) {
	if (this->type == DBVALUE_STRING) {
		free(this->value_str);
	}
}




void db_array_child_init(db_array_child_st *this, void *parent) {
	dbvalue_init(&this->name);
	dbvalue_init(&this->def);
	dbvalue_init(&this->max);
	dbvalue_init(&this->min);
	this->numsys = ((db_obj_st*) parent)->numsys;
	this->next_sibling = NULL;
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
	else if (type == CANOPEN_ARRAYSIGNED32) {
		strcpy(dest, "CANOPEN_ARRAYSIGNED32");
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
void db_type_to_stdint(canopen_object_type_e type, char *dest) {
	if (type == CANOPEN_UNSIGNED32) {
		strcpy(dest, "uint32_t");
	}
	else if (type == CANOPEN_SIGNED32) {
		strcpy(dest, "int32_t");
	}
	else if (type == CANOPEN_UNSIGNED16) {
		strcpy(dest, "uint16_t");
	}
	else if (type == CANOPEN_SIGNED16) {
		strcpy(dest, "int16_t");
	}
	else if (type == CANOPEN_UNSIGNED8) {
		strcpy(dest, "uint8_t");
	}
	else if (type == CANOPEN_SIGNED8) {
		strcpy(dest, "int8_t");
	}
	else if (type == CANOPEN_ARRAY32) {
		strcpy(dest, "uint32_t");
	}
	else if (type == CANOPEN_ARRAY16) {
		strcpy(dest, "uint16_t");
	}
	else if (type == CANOPEN_ARRAY8) {
		strcpy(dest, "uint8_t");
	}
	else if (type == CANOPEN_STRING) {
		strcpy(dest, "char *");
	}
	else {
		strcpy(dest, "void");
	}
}





void db_transmission_to_str(canopen_pdo_transmission_types_e transmission, char *dest) {
	if (transmission == CANOPEN_PDO_TRANSMISSION_ASYNC) {
		strcpy(dest, "CANOPEN_PDO_TRANSMISSION_ASYNC");
	}
	else {
		strcpy(dest, "CANOPEN_PDO_TRANSMISSION_UNKNOWN");
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


canopen_object_type_e db_jsonval_to_type(char *json_child) {
	char str[128];
	canopen_object_type_e ret;
	uv_jsonreader_get_string(json_child, str, sizeof(str));
	if (strcmp(str, "CANOPEN_UNSIGNED32") == 0 || strcmp(str, "CANOPEN_SIGNED32") == 0) {
		ret = CANOPEN_UNSIGNED32;
	}
	else if (strcmp(str, "CANOPEN_UNSIGNED16") == 0 || strcmp(str, "CANOPEN_SIGNED16") == 0) {
		ret = CANOPEN_UNSIGNED16;
	}
	else if (strcmp(str, "CANOPEN_UNSIGNED8") == 0 || strcmp(str, "CANOPEN_SIGNED8") == 0) {
		ret = CANOPEN_UNSIGNED8;
	}
	else if (strcmp(str, "CANOPEN_ARRAY32") == 0 ||
			strcmp(str, "CANOPEN_ARRAYUNSIGNED32") == 0 ||
			strcmp(str, "CANOPEN_ARRAYSIGNED32") == 0) {
		ret = CANOPEN_ARRAY32;
	}
	else if (strcmp(str, "CANOPEN_ARRAY16") == 0 ||
			strcmp(str, "CANOPEN_ARRAYUNSIGNED16") == 0 ||
			strcmp(str, "CANOPEN_ARRAYSIGNED16") == 0) {
		ret = CANOPEN_ARRAY16;
	}
	else if (strcmp(str, "CANOPEN_ARRAY8") == 0 ||
			strcmp(str, "CANOPEN_ARRAYUNSIGNED8") == 0 ||
			strcmp(str, "CANOPEN_ARRAYSIGNED8") == 0) {
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

static canopen_pdo_transmission_types_e str_to_transmission(char *json_child) {
	char str[64];
	canopen_pdo_transmission_types_e ret;
	uv_jsonreader_get_string(json_child, str, sizeof(str));
	if (strcmp(str, "CANOPEN_PDO_TRANSMISSION_ASYNC") == 0) {
		ret = CANOPEN_PDO_TRANSMISSION_ASYNC;
	}
	else {
		ret = CANOPEN_PDO_TRANSMISSION_ASYNC;
	}
	return ret;
}


static db_obj_type_e str_to_objtype(char *json_child) {
	char str[64];
	db_obj_type_e ret = DB_OBJ_TYPE_UNDEFINED;
	if (json_child != NULL) {
		uv_jsonreader_get_string(json_child, str, sizeof(str));
		if (strcmp(str, "NONVOL_PARAM") == 0 ||
				strcmp(str, "NONVOL PARAM") == 0) {
			ret = DB_OBJ_TYPE_NONVOL_PARAM;
		}
		else if (strcmp(str, "OPDB") == 0 ||
				strcmp(str, "OPERATOR DATABASE") == 0) {
			ret = DB_OBJ_TYPE_OPDB;
		}
		else if (strcmp(str, "OP_COUNT") == 0 ||
				strcmp(str, "OP COUNT") == 0 ||
				strcmp(str, "OPERATOR COUNT") == 0 ||
				strcmp(str, "OPERATOR_COUNT") == 0) {
			ret = DB_OBJ_TYPE_OP_COUNT;
		}
		else if (strcmp(str, "CURRENT_OP") == 0 ||
				strcmp(str, "CURRENT OP") == 0 ||
				strcmp(str, "CURRENT OPERATOR") == 0 ||
				strcmp(str, "CURRENT_OPERATOR") == 0) {
			ret = DB_OBJ_TYPE_CURRENT_OP;
		}
		else if (strcmp(str, "EMCY") == 0 ||
				strcmp(str, "EMERGENCY") == 0) {
			ret = DB_OBJ_TYPE_EMCY;
		}
		else if (strcmp(str, "CAN_IF") == 0 ||
				strcmp(str, "CAN IF") == 0) {
			ret = DB_OBJ_TYPE_IF_VERSION;
		}
		else {
			ret = DB_OBJ_TYPE_UNDEFINED;
		}
	}
	return ret;
}




// Checks the object dict object for mandatory fields. If a variable named
// check_obj_disable is set to true, skips the check
static bool check_obj(char *object, char *objname, char *parametername, bool echo) {
	bool ret = true;
	if (object == NULL) {
		if (echo) {
			printf("*** ERROR *** \"%s\" field not found in parameter \"%s\"\n",
					objname, parametername);
		}
		ret = false;
	}
	return ret;
}

#define CHECK_EMCY(object, objname, index) { \
		if (object == NULL) { \
			printf("*** ERROR *** \"%s\" field not found in emcy %u\n", \
					objname, index); \
			return false; \
		} \
	}

#define CHECK_RXPDO(object, objname, pdoindex) { \
		if (object == NULL) { \
			printf("*** ERROR *** \"%s\" field not found in rxpdo at index \"%u\"\n", \
					objname, pdoindex); \
			return false; \
		} \
	}

#define CHECK_TXPDO(object, objname, pdoindex) { \
		if (object == NULL) { \
			printf("*** ERROR *** \"%s\" field not found in txpdo at index \"%u\"\n", \
					objname, pdoindex); \
			return false; \
		} \
	}

static bool pdo_parse_mappings(char *mappingsjson, canopen_pdo_mapping_parameter_st *mappings) {
	bool ret = true;
	memset(mappings, 0, sizeof(*mappings));

	if (uv_jsonreader_get_type(mappingsjson) != JSON_ARRAY) {
		printf("PDO \"mappings\" object should be an array\n");
		ret = false;
	}
	else {
		int32_t index = 0;
		char *mapping;
		while ((mapping = uv_jsonreader_array_at(mappingsjson, index)) != NULL) {
			if (uv_jsonreader_get_type(mapping) != JSON_OBJECT) {
				printf("**** ERROR ****: PDO \"mappings\" has to be an array of JSON objects\n");
				ret = false;
				break;
			}
			char *data = uv_jsonreader_find_child(mapping, "param");
			if (data == NULL) {
				printf("*** ERROR *** PDO mapping parameter doesnt have \"param\" field\n");
				ret = false;
			}
			else {
				char name[128];
				uv_jsonreader_get_string(data, name, sizeof(name));
				str_to_upper_nonspace(name);
				bool match = false;
				for (int32_t i = 0; i < db_get_object_count(&dev.db); i++) {
					db_obj_st *obj = db_get_obj(&dev.db, i);
					if (strcmp(dbvalue_get_string(&obj->name), name) == 0) {
						mappings->mappings[index].main_index = obj->obj.main_index;
						mappings->mappings[index].length = CANOPEN_TYPE_LEN(obj->obj.type);

						if (CANOPEN_IS_ARRAY(obj->obj.type)) {
							data = uv_jsonreader_find_child(mapping, "subindex");
							CHECK_RXPDO(data, "subindex", i);
							uv_json_types_e type = uv_jsonreader_get_type(data);
							if (type == JSON_STRING) {
								uv_jsonreader_get_string(data, name, sizeof(name));
								db_array_child_st *child = obj->child_ptr;
								bool match = false;
								int32_t childindex = 1;
								while (child != NULL) {
									if (strcmp(dbvalue_get_string(&child->name), name) == 0) {
										mappings->mappings[index].sub_index = childindex;
										match = true;
										break;
									}
									childindex++;
									child = child->next_sibling;
								}
								if (!match) {
									printf("*** ERROR *** PDO mapping subindex name not found in array\n");
									ret = false;
									return ret;
								}
							}
							else if (type == JSON_INT) {
								mappings->mappings[index].sub_index = uv_jsonreader_get_int(data);
							}
							else {
								printf("*** ERROR *** PDO mapping subindex has to be integer or a string.\n");
								ret = false;
								return ret;
							}
						}
						else {
							mappings->mappings[index].sub_index = obj->obj.sub_index;
						}
						match = true;
						break;
					}
				}
				if (!match) {
					printf("*** ERROR *** PDO mapping parameter refers to a parameter \"%s\" "
							"which doesn't exist in object dictionary\n", name);
					ret = false;
					return ret;
				}
			}
			index++;
		}
	}
	return ret;
}


static bool parse_obj_dict_obj(db_st *this, char *child, char *path) {
	bool ret = true;

	// start parsing data from child, e.g. object dictionary entry
	// CONTAINER types are objects that contain child "content" either as array
	// defining objects or string defining file name where to search for objects.
	// CONTAINERS can be recursive
	char *content = uv_jsonreader_find_child(child, "content");
	if (content != NULL) {
		if (uv_jsonreader_get_type(content) == JSON_ARRAY) {
			// 'content' defined, CONTAINER type object
			// 'require' array can define mandatory defines
			char *require = uv_jsonreader_find_child(child, "require");
			if (require != NULL &&
					uv_jsonreader_get_type(require) == JSON_ARRAY) {
				for (uint16_t i = 0; i < uv_jsonreader_array_get_size(require); i++) {
					char str[1024] = {};
					uv_jsonreader_array_get_string(require, i, str, sizeof(str) - 1);
					db_define_st *def = db_define_find(this, str);
					if (def == NULL) {
						fprintf(stderr,
								"*** ERROR *** CONTAINER requires define '%s' to be defined\n",
								str);
						ret = false;
						break;
					}
				}
			}

			for (uint16_t i = 0; i < uv_jsonreader_array_get_size(content); i++) {
				char *child = uv_jsonreader_array_at(content, i);
				// recursively parse each child found in 'content' array
				if (uv_jsonreader_get_type(child) == JSON_OBJECT) {
					ret = parse_obj_dict_obj(this, child, path);
				}
				else {
					printf("*** ERROR *** 'content' array in CONTAINER contained\n"
							"other types than objects.\n");
					ret = false;
				}
				if (!ret) {
					break;
				}
			}
		}
		else if (uv_jsonreader_get_type(content) == JSON_STRING) {
			// check for *defines* array for local definitions
			char *defines = uv_jsonreader_find_child(child, "defines");
			if (defines != NULL) {
				ret = parse_defines(this, defines);
			}
			char name[128] = {};
			strcpy(name, path);
			uv_jsonreader_get_string(content, name + strlen(name),
					sizeof(name) - strlen(name));
			if (ret) {
				printf("reading content file '%s'\n", name);
				FILE *fptr = fopen(name, "r");

				if (fptr == NULL) {
					// failed to open the file, exit this task
					printf("Failed to open content file '%s'.\n", name);
				}
				else {
					int32_t size;
					fseek(fptr, 0, SEEK_END);
					size = ftell(fptr);
					rewind(fptr);
					char *data = malloc(size);
					if (fread(data, 1, size, fptr)) {
						uv_jsonreader_init(data, size);
						parse_obj_dict_obj(this, data, path);
					}
					free(data);
				}
			}
			else {
				printf("*** ERROR *** Skipping content file '%s' because of error in defines\n",
						name);
			}
			if (defines != NULL) {
				remove_defines(this, defines);
			}
		}
		else {
			printf("*** ERROR *** 'content' not array or string in CONTAINER\n");
			ret = false;
		}

		// after reading content array, look through other childs that overwrite
		// just loaded parameters
		char *data = uv_jsonreader_find_child(child, "data");
		if (check_obj(data, "", "", false) &&
				uv_jsonreader_get_type(data) == JSON_ARRAY) {
			for (uint16_t i = 0; i < uv_jsonreader_array_get_size(data); i++) {
				char *c = uv_jsonreader_array_at(data, i);
				if (uv_jsonreader_array_get_type(data, i) == JSON_OBJECT &&
						c) {
					parse_obj_dict_obj(this, c, path);
				}
			}
		}
	}
	else {
		char *data = uv_jsonreader_find_child(child, "name");
		if (data == NULL) {
			printf("### ERROR ### Object without name found.\n");
			return false;
		}
		char name[128] = {};
		uv_jsonreader_get_string(data, name, sizeof(name));
		str_to_upper_nonspace(name);


		// check if any same named objects were already loaded
		db_obj_st *obj = db_find_obj(this, name);
		bool echo = true;
		if (obj == NULL) {
			// new object found, push this to objects array
			db_obj_st o;
			uv_vector_push_back(&this->objects, &o);
			obj = uv_vector_at(&this->objects,
					uv_vector_size(&this->objects) - 1);
			db_obj_init(obj);
			dbvalue_set_string(&obj->name, name, strlen(name));
			// copy value to dbvalue in case if defines were expanded
			strcpy(name, dbvalue_get_string(&obj->name));
		}
		else {
			// object checking is disabled as same named object was already loaded.
			// All children are used to udpate already downloaded data
			echo = false;
		}

		data = uv_jsonreader_find_child(child, "index");
		if (check_obj(data, "index", name, echo)) {
			dbvalue_st dbval;
			dbvalue_init(&dbval);
			dbvalue_set(&dbval, data);
			obj->obj.main_index = dbvalue_get_int(&dbval);
			dbvalue_free(&dbval);
		}

		data = uv_jsonreader_find_child(child, "index_offset");
		if (check_obj(data, "", NULL, false)) {
			int offset = uv_jsonreader_get_int(data);
			obj->obj.main_index += offset;
		}

		data = uv_jsonreader_find_child(child, "type");
		if (check_obj(data, "type", name, echo)) {
			obj->obj.type = db_jsonval_to_type(data);
			uv_jsonreader_get_string(data, obj->type_str, sizeof(obj->type_str));
		}

		data = uv_jsonreader_find_child(child, "numsystem");
		if (check_obj(data, "", "", false)) {
			char str[64] = {};
			uv_jsonreader_get_string(data, str, sizeof(str));
			if (strcmp(str, "HEX") == 0) {
				obj->numsys = DB_OBJ_NUMSYS_HEX;
			}
		}

		data = uv_jsonreader_find_child(child, "permissions");
		if (check_obj(data, "permissions", name, echo)) {
			obj->obj.permissions = str_to_permissions(data);
		}

		data = uv_jsonreader_find_child(child, "data_type");
		if (check_obj(data, "", "", false)) {
			obj->obj_type = str_to_objtype(data);
		}

		if (!CANOPEN_IS_ARRAY(obj->obj.type)) {
			// string parameters
			if (CANOPEN_IS_STRING(obj->obj.type)) {
				data = uv_jsonreader_find_child(child, "stringsize");
				if (check_obj(data, "stringsize", name, echo)) {
					dbvalue_set(&obj->string_len, data);
				}
				data = uv_jsonreader_find_child(child, "default");
				if (check_obj(data, "default", name, echo)) {
					dbvalue_set(&obj->string_def, data);
					if (!dbvalue_is_set(&obj->string_len)) {
						dbvalue_set(&obj->string_len, data);
					}
				}
			}
			// integer parameters
			else {
				data = uv_jsonreader_find_child(child, "subindex");
				if (check_obj(data, "subindex", name, echo)) {
					obj->obj.sub_index = uv_jsonreader_get_int(data);
				}

				data = uv_jsonreader_find_child(child, "min");
				if (check_obj(data, "min", name,
						(obj->obj.permissions != CANOPEN_RO) ? echo : false)) {
					dbvalue_set(&obj->min, data);
				}

				data = uv_jsonreader_find_child(child, "max");
				if (check_obj(data, "max", name,
						(obj->obj.permissions != CANOPEN_RO) ? echo : false)) {
					dbvalue_set(&obj->max, data);
				}
				// writable integer parameters
				if (obj->obj.permissions != CANOPEN_RO) {

					data = uv_jsonreader_find_child(child, "value");
					if (data == NULL) {
						data = uv_jsonreader_find_child(child, "default");
					}
					if (check_obj(data, "default", name, echo)) {
						dbvalue_set(&obj->def, data);
						dbvalue_set(&obj->value, data);
					}
				}
				// read-only integer parameters
				else {
					data = uv_jsonreader_find_child(child, "value");
					if (!check_obj(data, "", "", false)) {
						data = uv_jsonreader_find_child(child, "default");
					}
					if (check_obj(data, "default", name, echo)) {
						dbvalue_set(&obj->value, data);
						dbvalue_set(&obj->def, data);
						if (!dbvalue_is_set(&obj->min)) {
							dbvalue_set_int(&obj->min, dbvalue_get_int(&obj->value));
						}
						if (!dbvalue_is_set(&obj->max)) {
							dbvalue_set_int(&obj->max, dbvalue_get_int(&obj->value));
						}
					}
				}
			}
		}
		// array parameters
		else {
			data = uv_jsonreader_find_child(child, "arraysize");
			if (check_obj(data, "", "", false)) {
				dbvalue_set(&obj->array_max_size, data);
			}

			obj->obj.array_max_size = dbvalue_get_int(&obj->array_max_size);
		}

		data = uv_jsonreader_find_child(child, "dataptr");
		if (check_obj(data, "dataptr", name, echo)) {
			dbvalue_set(&obj->dataptr, data);
		}


		if (CANOPEN_IS_ARRAY(obj->obj.type)) {
			// cycle trough children and make a new object for each of them
			// obj main index and type are already inherited from the array object

			db_array_child_st *thischild;
			void **last_ptr = (void**) &obj->child_ptr;
			char *children = uv_jsonreader_find_child(child, "data");

			if (children == NULL && dbvalue_get_int(&obj->array_max_size) == 0) {
				printf("ERROR: array type object '%s' should define children count\n"
						"either with \"arraysize\" or \"data\".\n",
						dbvalue_get_string(&obj->name));
			}
			else {
				// if array size was not given, calculate it from data children count
				if ((dbvalue_get_type(&obj->array_max_size) == DBVALUE_INT) &&
						dbvalue_get_int(&obj->array_max_size) == 0) {
					dbvalue_set_int(&obj->array_max_size,
							uv_jsonreader_array_get_size(children));
				}

				for (uint8_t i = 0;
						i < MAX(uv_jsonreader_array_get_size(children),
								dbvalue_get_int(&obj->array_max_size)); i++) {
					if (*last_ptr == NULL) {
						thischild = malloc(sizeof(db_array_child_st));
						db_array_child_init(thischild, obj);
						*last_ptr = thischild;
					}
					else {
						thischild = *last_ptr;
					}
					last_ptr = &(thischild->next_sibling);

					char *str = uv_jsonreader_array_at(children, i);
					if (str == NULL) {
						if (strlen(dbvalue_get_string(&thischild->name)) == 0) {
							char str[1024] = {};
							sprintf(str, "CHILD%i", i + 1);
							dbvalue_set_string(&thischild->name, str, strlen(str));
							dbvalue_set_int(&thischild->def, 0);
						}
					}
					else {
						data = uv_jsonreader_find_child(str, "name");
						if (check_obj(data, "", "", false)) {
							dbvalue_set(&thischild->name, data);
							str_to_upper_nonspace(dbvalue_get_string(&thischild->name));
						}
						else if (strlen(dbvalue_get_string(&thischild->name)) == 0) {
							char str[1024];
							sprintf(str, "CHILD%i", i + 1);
							dbvalue_set_string(&thischild->name, str, strlen(str));
						}
						else {

						}

						data = uv_jsonreader_find_child(str, "numsystem");
						if (check_obj(data, "", "", false)) {
							char str[64] = {};
							uv_jsonreader_get_string(data, str, sizeof(str));
							if (strcmp(str, "HEX") == 0) {
								thischild->numsys = DB_OBJ_NUMSYS_HEX;
							}
						}

						data = uv_jsonreader_find_child(str, "min");
						if (check_obj(data, "", "", false)) {
							dbvalue_set(&thischild->min, data);
						}
						data = uv_jsonreader_find_child(str, "max");
						if (check_obj(data, "", "", false)) {
							dbvalue_set(&thischild->max, data);
						}

						data = uv_jsonreader_find_child(str, "default");
						if (data == NULL) {
							data = uv_jsonreader_find_child(str, "value");
						}
						if (data != NULL) {
							dbvalue_set(&thischild->def, data);
						}
					}
				}
			}
		}
	}


	return ret;
}


static bool define_push(db_st *this, char *define_name, char *v,
		char *parent, char *parentname) {
	bool ret = true;
	db_define_st define;
	strcpy(define.name, define_name);
	str_to_upper_nonspace(define.name);
	uv_json_types_e type = uv_jsonreader_get_type(v);
	if (type == JSON_INT) {
		define.type = DB_DEFINE_INT;
		define.value = uv_jsonreader_get_int(v);
		uv_vector_push_back(&this->defines, &define);
	}
	else if (type == JSON_STRING) {
		define.type = DB_DEFINE_STRING;
		memset(define.str, 0, sizeof(define.str));
		uv_jsonreader_get_string(v, define.str, sizeof(define.str));
		uv_vector_push_back(&this->defines, &define);
	}
	else if (type == JSON_ARRAY) {
		define.type = DB_DEFINE_ENUM;
		int32_t len = uv_jsonreader_array_get_size(v);
		define.child_count = len + 1;
		define.childs = malloc(128 * (len + 1));
		for (int32_t i = 0; i < len; i++) {
			char c[128];
			uv_jsonreader_array_get_string(v, i, c, sizeof(c));
			strcpy(define.childs[i], c);
		}
		strcpy(define.childs[define.child_count - 1], "COUNT");

		// in case of array, search for a key "type" which specifies
		// the data type of the enum
		v = uv_jsonreader_find_child(parent, "type");
		if (v != NULL) {
			define.data_type = db_jsonval_to_type(v);
		}
		else {
			define.data_type = CANOPEN_UNDEFINED;
		}


		uv_vector_push_back(&this->defines, &define);
	}
	else {
		ret = false;
		printf("*** ERROR *** DEFINES array '%s' had an illegal type of value. "
				"Only integers, strings and arrays are supported\n",
				parentname);
	}
	return ret;
}

static bool parse_defines(db_st *this, char *obj) {
	bool ret = true;
	if (obj != NULL) {
		char name[128] = "UNDEFINED";
		uv_jsonreader_get_obj_name(obj, name, sizeof(name));

		if (uv_jsonreader_get_type(obj) != JSON_ARRAY) {
			printf("*** JSON ERROR **** DEFINES array '%s' is not an array\n",
					name);
			ret = false;
		}
		else {
			for (unsigned int i = 0; i < uv_jsonreader_array_get_size(obj); i++) {
				char *d = uv_jsonreader_array_at(obj, i);
				if (d != NULL) {
					char *v = uv_jsonreader_find_child(d, "name");
					if (v == NULL) {
						// old syntax where key-value pair key defines name
						// and value defines the value
						char *define = uv_jsonreader_get_child(d, 0);
						if (define != NULL) {
							char dname[128];
							uv_jsonreader_get_obj_name(define, dname, sizeof(dname));
							ret = define_push(this, dname, define, NULL, name);
						}
						else {
							printf("*** ERROR *** Define '%s' contained empty definition\n",
									name);
							ret = false;
						}
					}
					else {
						// old syntax with "name" and "value" pairs
						char dname[128];
						uv_jsonreader_get_string(v, dname, sizeof(dname));
						v = uv_jsonreader_find_child(d, "value");
						ret = define_push(this, dname, v, d, name);
					}
				}
				else {
					ret = false;
					printf("*** ERROR *** DEFINES array '%s' member was not an object at index %u\n",
							name, i);
				}
			}
		}
	}
	else {

	}
	return ret;
}


static void remove_defines(db_st *this, char *obj) {
	if (obj != NULL) {
		char name[128] = "";
		if (uv_jsonreader_get_type(obj) != JSON_ARRAY) {
			printf("*** JSON ERROR **** DEFINES array is not an array\n");
		}
		else {
			for (unsigned int i = 0; i < uv_jsonreader_array_get_size(obj); i++) {
				char *d = uv_jsonreader_array_at(obj, i);
				if (d != NULL) {
					char *v = uv_jsonreader_find_child(d, "name");
					if (v == NULL) {
						v = uv_jsonreader_get_child(d, 0);
						if (v != NULL) {
							uv_jsonreader_get_obj_name(v, name, sizeof(name));
						}
					}
					else {
						// old protocol
						uv_jsonreader_get_string(v, name, sizeof(name));
					}
					if (strlen(name) != 0) {
						str_to_upper_nonspace(name);
						// remove define from vector
						for (uint16_t i = 0; i < uv_vector_size(&this->defines); i++) {
							db_define_st *def = uv_vector_at(&this->defines, i);
							if (strcmp(def->name, name) == 0) {
								uv_vector_remove(&this->defines, i, 1);
							}
						}
					}
					strcpy(name, "");
				}
			}
		}
	}
}


static bool parse_json(db_st *this, char *json, char *path) {
	bool ret = true;

	uv_jsonreader_init(json, strlen(json));

	char *data = json;
	char *obj;

	// dev name
	obj = uv_jsonreader_find_child(data, "DEV");
	if (obj != NULL) {
		uv_jsonreader_get_string(obj, this->dev_name, sizeof(this->dev_name));
	}
	else {
		printf("*** ERROR *** 'DEV' object not found in the JSON\n");
		strcpy(this->dev_name, "");
	}
	for (int i = 0; i < strlen(this->dev_name) + 1; i++) {
		if (isspace(this->dev_name[i])) {
			this->dev_name_upper[i] = '_';
		}
		else {
			this->dev_name_upper[i] = toupper(this->dev_name[i]);
		}
	}

	char nameupper[1024] = { '\0' };
	char namelower[1024] = { '\0' };
	for (int i = 0; i < strlen(db_get_dev_name(&dev.db)); i++) {
		char c[2];
		c[0] = toupper(db_get_dev_name(&dev.db)[i]);
		c[1] = '\0';
		strcat(nameupper, c);
		c[0] = tolower(db_get_dev_name(&dev.db)[i]);
		c[1] = '\0';
		strcat(namelower, c);
	}

	// vendor id
	obj = uv_jsonreader_find_child(data, "VENDORID");
	if (obj != NULL) {
		this->vendor_id = uv_jsonreader_get_int(obj);
	}
	else {
		this->vendor_id = CANOPEN_USEVOLT_VENDOR_ID;
	}

	// product code
	obj = uv_jsonreader_find_child(data, "PRODUCTCODE");
	if (obj != NULL) {
		this->product_code = uv_jsonreader_get_int(obj);
	}
	else {
		this->product_code = 0;
	}

	// revision number
	obj = uv_jsonreader_find_child(data, "REVISIONNUMBER");
	if (obj != NULL) {
		this->revision_number = uv_jsonreader_get_int(obj);
	}
	else {
		this->revision_number = 0;
	}

	// nodeid
	obj = uv_jsonreader_find_child(data, "NODEID");
	if (obj != NULL) {
		this->node_id = uv_jsonreader_get_int(obj);
	}
	else {
		printf("*** ERROR *** 'NODEID' object not found in the JSON\n");
	}


	// emcy
	obj = uv_jsonreader_find_child(data, "EMCY_INDEX");
	if (obj == NULL ||
			(uv_jsonreader_get_type(obj) != JSON_INT &&
					uv_jsonreader_get_type(obj) != JSON_STRING)) {
		printf("No EMCY_INDEX integer parameter defined. Skipping EMCY handling.\n");
	}
	else {
		this->emcys_index = uv_jsonreader_get_int(obj);

		obj = uv_jsonreader_find_child(data, "EMCY");
		uint8_t str_count = 1;
		if (uv_jsonreader_get_type(obj) != JSON_ARRAY) {
			printf("*** JSON ERROR **** EMCY array is not an array\n");
		}
		else if (obj != NULL) {
			for (unsigned int i = 0; i < uv_jsonreader_array_get_size(obj); i++) {
				char *e = uv_jsonreader_array_at(obj, i);
				if (e != NULL) {
					db_emcy_st emcy = {};
					char *v = uv_jsonreader_find_child(e, "name");
					CHECK_EMCY(v, "name", i);
					uv_jsonreader_get_string(v, emcy.name, sizeof(emcy.name));


					v = uv_jsonreader_find_child(e, "str");
					if (v != NULL) {
						if (uv_jsonreader_get_type(v) == JSON_ARRAY) {
							uint32_t stri;
							for (stri = 0; stri < uv_jsonreader_array_get_size(v); stri++) {
								uv_jsonreader_array_get_string(v, stri,
										emcy.info_strs[stri], sizeof(emcy.info_strs[stri]));
							}
							if (i == 0) {
								// set the string count depending on the first emcy's string count
								str_count = stri;
							}
						}
						else {
							printf("** JSON ERROR *** EMCY \"str\" object "
									"has to be array (in emcy %u).\n", i);
						}
					}

					emcy.value = i * str_count;

					uv_vector_push_back(&this->emcys, &emcy);
				}
			}
		}
		else {

		}
	}

	// defines
	obj = uv_jsonreader_find_child(data, "DEFINES");
	parse_defines(this, obj);


	// object dictionary
	obj = uv_jsonreader_find_child(data, "OBJ_DICT");
	if ((obj != NULL) && (uv_jsonreader_get_type(obj) == JSON_ARRAY)) {

		for (int i = 0; i < uv_jsonreader_array_get_size(obj); i++) {
			char *child = uv_jsonreader_array_at(obj, i);
			if (child == NULL) {
				ret = false;
				break;
			}
			else {
				ret = parse_obj_dict_obj(this, child, path);
			}
		}

	}
	else {
		printf("ERROR: OBJ_DICT array not found from JSON.\n");
		ret = false;
	}

	// append EMCY STR entries into object dictionary parameters. Note
	// that the EMCY STRING_LEN symbol includes the null-termination character.
	bool br = false;
	for (uint32_t i = 0; i < db_get_emcy_count(this); i++) {
		db_emcy_st *emcy = db_get_emcy(this, i);
		uint8_t j = 0;
		while (strlen(emcy->info_strs[j]) != 0) {
			db_obj_st obj;
			db_obj_init(&obj);
			obj.obj_type = DB_OBJ_TYPE_EMCY;
			char name[128] = {};
			strcpy(name, "EMCY_");
			strcat(name, emcy->name);
			sprintf(name + strlen(name),
					"_STR%u", j);
			dbvalue_set_string(&obj.name, name, strlen(name));
			char str[1024];
			sprintf(str, "%s_%s_DEFAULT", nameupper, name);
			dbvalue_set_string(&obj.dataptr, str, strlen(str));
			strcpy(obj.type_str, "CANOPEN_STRING");
			obj.obj.permissions = CANOPEN_RO;
			dbvalue_set_int(&obj.string_len, strlen(emcy->info_strs[j]) + 1);
			dbvalue_set_string(&obj.string_def, emcy->info_strs[j],
					strlen(emcy->info_strs[j]));
			obj.obj.type = CANOPEN_STRING;
			obj.obj.string_len = strlen(emcy->info_strs[j]) + 1;
			obj.obj.main_index = db_get_emcy_index(this) + emcy->value + j;

			uv_errors_e e = uv_vector_push_back(&dev.db.objects, &obj);
			if (e != ERR_NONE) {
				printf("**** Error adding a new object to object dictionary. \n"
						"Object dictionary full. ***\n");
				br = true;
				break;
			}
			j++;
		}
		if (br) {
			break;
		}
	}

	// Order object dictionary to rising order based on main index
	for (int i = 0; i < uv_vector_size(&dev.db.objects); i++) {
		db_obj_st *obj_i = uv_vector_at(&dev.db.objects, i);
		for (int j = 0; j < i; j++) {
			db_obj_st *obj_j = uv_vector_at(&dev.db.objects, j);
			if (obj_i->obj.main_index < obj_j->obj.main_index) {
				// swap i and j
				db_obj_st swap = *obj_j;
				memmove(obj_j, obj_i, sizeof(db_obj_st));
				memcpy(obj_i, &swap, sizeof(db_obj_st));
			}
			else if (obj_i->obj.main_index == obj_j->obj.main_index) {
				// check subindex
				if (uv_canopen_is_array(&obj_i->obj) ||
						uv_canopen_is_string(&obj_i->obj) ||
						obj_i->obj.sub_index < obj_j->obj.sub_index) {
					// swap i and j
					db_obj_st swap = *obj_j;
					memmove(obj_j, obj_i, sizeof(db_obj_st));
					memcpy(obj_i, &swap, sizeof(db_obj_st));
				}
			}
			else {

			}
		}
	}

	// RXPDOs
	obj = uv_jsonreader_find_child(data, "RXPDO");
	if ((obj != NULL) && (uv_jsonreader_get_type(obj) == JSON_ARRAY)) {
		uint16_t rxpdo_count = 0;
		for (int32_t i = 0; i < uv_jsonreader_array_get_size(obj); i++) {
			char *pdojson = uv_jsonreader_array_at(obj, i);
			if (pdojson == NULL) {
				break;
			}
			db_rxpdo_st pdo;

			char *data = uv_jsonreader_find_child(pdojson, "cobid");
			CHECK_RXPDO(data, "cobid", i);
			uv_jsonreader_get_string(data, pdo.cobid, sizeof(pdo.cobid));

			data = uv_jsonreader_find_child(pdojson, "transmission");
			CHECK_RXPDO(data, "transmission", i);
			pdo.transmission = str_to_transmission(data);

			char *mappingsjson = uv_jsonreader_find_child(pdojson, "mappings");
			CHECK_RXPDO(mappingsjson, "mappings", i);
			ret = pdo_parse_mappings(mappingsjson, &pdo.mappings);

			char *index = uv_jsonreader_find_child(pdojson, "index");
			if (index != NULL) {
				int16_t in = uv_jsonreader_get_int(index);
				for (int16_t i = uv_vector_size(&this->rxpdos); i < in; i++) {
					db_rxpdo_st p = {};
					strcpy(p.cobid, "CANOPEN_PDO_DISABLED");
					p.transmission = CANOPEN_PDO_TRANSMISSION_ASYNC;
					printf("RXPDO %u initialized as empty\n", rxpdo_count + 1);
					uv_vector_push_back(&this->rxpdos, &p);
					rxpdo_count++;
				}
			}

			if (ret) {
				int32_t bytes = 0;
				for (int32_t i = 0; i < 8; i++) {
					bytes += pdo.mappings.mappings[i].length;
				}
				printf("RXPDO %u used bytes: %u / 8\n", rxpdo_count + 1, bytes);

				uv_vector_push_back(&this->rxpdos, &pdo);
				rxpdo_count++;
			}
		}
	}
	else if ((obj = uv_jsonreader_find_child(data, "RXPDO_COUNT")) != NULL) {
		// RXPDO_COUNT can be used to initialize RXPDOs
		int count = uv_jsonreader_get_int(obj);
		for (uint16_t i = 0; i < count; i++) {
			db_rxpdo_st pdo = {
					.cobid = {},
					.mappings = {},
					.transmission = CANOPEN_PDO_TRANSMISSION_ASYNC
			};
			strcpy(pdo.cobid, "CANOPEN_PDO_DISABLED");
			uv_vector_push_back(&this->rxpdos, &pdo);
		}
		printf("%i RXPDO's initialized\n", count);
	}
	else {
		printf("WARNING: RXPDO array not found in JSON.\n");
	}


	// TXPDOs
	obj = uv_jsonreader_find_child(data, "TXPDO");
	if ((obj != NULL) && (uv_jsonreader_get_type(obj) == JSON_ARRAY)) {
		for (int32_t i = 0; i < uv_jsonreader_array_get_size(obj); i++) {
			char *pdojson = uv_jsonreader_array_at(obj, i);
			if (pdojson == NULL) {
				break;
			}
			db_txpdo_st pdo;

			char *data = uv_jsonreader_find_child(pdojson, "cobid");
			CHECK_TXPDO(data, "cobid", i);
			uv_jsonreader_get_string(data, pdo.cobid, sizeof(pdo.cobid));

			data = uv_jsonreader_find_child(pdojson, "transmission");
			CHECK_TXPDO(data, "transmission", i);
			pdo.transmission = str_to_transmission(data);

			data = uv_jsonreader_find_child(pdojson, "inhibittime");
			CHECK_TXPDO(data, "inhibittime", i);
			pdo.inhibit_time = uv_jsonreader_get_int(data);

			data = uv_jsonreader_find_child(pdojson, "eventtimer");
			CHECK_TXPDO(data, "eventtimer", i);
			pdo.event_timer = uv_jsonreader_get_int(data);

			char *mappingsjson = uv_jsonreader_find_child(pdojson, "mappings");
			CHECK_TXPDO(mappingsjson, "mappings", i);
			ret = pdo_parse_mappings(mappingsjson, &pdo.mappings);

			if (ret) {
				int32_t bytes = 0;
				for (int32_t i = 0; i < 8; i++) {
					bytes += pdo.mappings.mappings[i].length;
				}
				printf("TXPDO %u used bytes: %u / 8\n", i + 1, bytes);

				uv_vector_push_back(&this->txpdos, &pdo);
			}
		}
	}
	else if ((obj = uv_jsonreader_find_child(data, "TXPDO_COUNT")) != NULL) {
		// TXPDO_COUNT can be used to initialize TXPDOs
		int count = uv_jsonreader_get_int(obj);
		for (uint16_t i = 0; i < count; i++) {
			db_txpdo_st pdo = {
					.cobid = {},
					.inhibit_time = 10,
					.event_timer = 100,
					.mappings = {},
					.transmission = CANOPEN_PDO_TRANSMISSION_ASYNC
			};
			strcpy(pdo.cobid, "CANOPEN_PDO_DISABLED");
			uv_vector_push_back(&this->rxpdos, &pdo);
		}
		printf("%i TXPDO's initialized\n", count);
	}
	else {
		printf("WARNING: TXPDO array not found in JSON.\n");
	}


	return ret;
}


db_obj_st *db_find_obj(db_st *this, char *name) {
	db_obj_st *ret = NULL;
	for (uint16_t i = 0; i < uv_vector_size(&this->objects); i++) {
		db_obj_st *obj = uv_vector_at(&this->objects, i);
		if (strcmp(dbvalue_get_string(&obj->name), name) == 0) {
			ret = obj;
			break;
		}
	}
	return ret;
}



db_define_st *db_define_find(db_st *this, char *name) {
	db_define_st *ret = NULL;
	// first parse name to uppercase and nonspace
	char *namestr = malloc(strlen(name) + 1);
	strcpy(namestr, name);
	str_to_upper_nonspace(namestr);

	for (uint16_t i = 0; i < db_get_define_count(this); i++) {
		db_define_st *def = db_get_define(this, i);
		if (strcmp(def->name, namestr) == 0) {
			ret = def;
			break;
		}
	}

	free(namestr);
	return ret;
}


uint8_t db_get_nodeid(db_st *this) {
	uint8_t ret = 0;
	if (db_is_loaded(this) || is_nodeid_set) {
		ret = this->node_id;
	}
	return ret;
}


void db_set_nodeid(db_st *this, uint8_t value) {
	this->node_id = value;
	is_nodeid_set = true;
}



#define this (&dev.db)


bool cmd_db(const char *arg) {
	bool ret = false;

	this->revision_number = 0;

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

	uv_vector_init(&this->txpdos, this->txpdo_buffer,
			sizeof(this->txpdo_buffer) / sizeof(this->txpdo_buffer[0]),
			sizeof(this->txpdo_buffer[0]));

	uv_vector_init(&this->rxpdos, this->rxpdo_buffer,
			sizeof(this->rxpdo_buffer) / sizeof(this->rxpdo_buffer[0]),
			sizeof(this->rxpdo_buffer[0]));

	// try to load the CANOpen database

	FILE *fptr = fopen(arg, "r");

	if (fptr == NULL) {
		// failed to open the file, exit this task
		printf("Failed to open database file %s.\n", arg);
	}
	else {
		char *path = malloc(strlen(dirname(arg) + 2));
		strcpy(path, dirname(arg));
		strcat(path, "/");
		int32_t size;
		fseek(fptr, 0, SEEK_END);
		size = ftell(fptr);
		rewind(fptr);
		printf("file size: %u\n", size);

		char *data = malloc(size);
		if (fread(data, 1, size, fptr)) {
			if (parse_json(this, data, path)) {
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
		free(data);
		free(path);
	}

	is_loaded = ret;

	if (ret == true) {
//		printf("PARSED DATA:\n\n");
//
//		for (uint8_t i = 0; i < uv_vector_size(&this->rxpdos); i++) {
//			db_rxpdo_st *pdo = uv_vector_at(&this->rxpdos, i);
//			char transmission[128];
//			db_transmission_to_str(pdo->transmission, transmission);
//			printf("PDO %u\n"
//					"cob id: %s\n"
//					"transmission: %s\n"
//					"mappings:\n",
//					i, pdo->cobid, transmission);
//			for (uint8_t i = 0; i < 8; i++) {
//				printf("mapping %u\n"
//						"main index: 0x%x\n"
//						"subindex: %u\n"
//						"length: %u\n",
//						i,
//						pdo->mappings.mappings[i].main_index,
//						pdo->mappings.mappings[i].sub_index,
//						pdo->mappings.mappings[i].length);
//			}
//		}
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


void db_deinit(void) {
	is_loaded = false;
	for (int i = 0; i < db_get_define_count(this); i++) {
		db_define_st *d = db_get_define(this, i);
		if (d->type == DB_DEFINE_ENUM) {
			free(d->childs);
		}
	}
	for (int i = 0; i < db_get_object_count(this); i++) {
		db_obj_st *obj = db_get_obj(this, i);
		db_obj_deinit(obj);
	}
}


