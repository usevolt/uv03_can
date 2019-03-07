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



#include <export.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <uv_terminal.h>
#include <libgen.h>
#include "db.h"
#include "main.h"



#define this (&dev)


bool get_header_objs(char *dest, const char *filename) {
	strcpy(dest, "\n\n\n/// @file: UVCAN generated header file describing the object dictionary\n"
			"/// of this device. DO NOT EDIT DIRECTLY\n\n");
	strcat(dest, "#ifndef ");
	char name[1024] = {};
	strcpy(name, filename);
	char *basenam = basename(name);
	strcat(dest, basenam);
	strcat(dest, "\n#define ");
	strcat(dest, basenam);
	strcat(dest, "\n\n");
	strcat(dest, "#include <stdint.h>\n"
			"#include <string.h>\n\n");

	// create symbol for node id
	char nameupper[1024] = {};
	char namelower[1024] = {};
	strcat(dest, "#define ");
	for (int i = 0; i < strlen(db_get_dev_name(&dev.db)); i++) {
		char c[2];
		c[0] = toupper(db_get_dev_name(&dev.db)[i]);
		c[1] = '\0';
		strcat(nameupper, c);
		c[0] = tolower(db_get_dev_name(&dev.db)[i]);
		strcat(namelower, c);
	}
	strcat(dest, nameupper);
	strcat(dest, "_NODEID");
	sprintf(&dest[strlen(dest)], "           0x%x\n\n\n\n", db_get_nodeid(&dev.db));

	// create symbols for EMCY messages
	strcat(dest, "enum {\n");
	for (int i = 0; i < db_get_emcy_count(&dev.db); i++) {
		db_emcy_st *emcy = db_get_emcy(&dev.db, i);

		char line[1024];
		line[0] = '\0';
		strcat(line, "    ");
		strcat(line, nameupper);
		strcat(line, "_EMCY_");
		strcat(line, emcy->name);
		strcat(line, " =            ");
		sprintf(&line[strlen(line)], "%i,\n", emcy->value);

		strcat(dest, line);
	}
	sprintf(&(dest[strlen(dest)]), "    %s_EMCY_COUNT =            %u\n};",
			nameupper, db_get_emcy_count(&dev.db));
	strcat(dest, "\n\n\n\n");

	// create symbols for defines
	for (int i = 0; i < db_get_define_count(&dev.db); i++) {
		db_define_st *define = db_get_define(&dev.db, i);

		char line[1024];
		if (define->type == DB_DEFINE_INT) {
			line[0] = '\0';
			strcat(line, "#define ");
			strcat(line, nameupper);
			strcat(line, "_");
			strcat(line, define->name);
			strcat(line, "            ");
			sprintf(&line[strlen(line)], "%i\n\n", define->value);
			strcat(dest, line);
		}
		else if (define->type == DB_DEFINE_ENUM) {
			if (define->data_type == CANOPEN_UNDEFINED) {
				sprintf(line, "typedef enum {\n");
			}
			else {
				sprintf(line, "enum {\n");
			}
			strcat(dest, line);
			for (int32_t i = 0; i < define->child_count; i++) {
				sprintf(line, "    %s_%s_%s",
						nameupper, define->name, define->childs[i]);
				if (i == 0) {
					strcat(line, " = 0");
				}
				if (i < define->child_count - 1) {
					strcat(line, ",");
				}
				strcat(line, "\n");
				strcat(dest, line);
			}
			char n[128] = {};
			for (int i = 0; i < strlen(define->name); i++) {
					n[i] = tolower(define->name[i]);
			}

			if (define->data_type == CANOPEN_UNDEFINED) {
				sprintf(line, "} %s_%s_e;\n\n", namelower, n);
			}
			else {
				char str[128];
				db_type_to_stdint(define->data_type, str);
				sprintf(line, "};\ntypedef %s %s_%s_e;\n\n",
						str, namelower, n);
			}
			strcat(dest, line);
		}
		else {
			printf("ERROR in export: Unknown define type\n");
		}
	}
	strcat(dest, "\n\n\n\n");

	// create symbols for PDO counts
	sprintf(&dest[strlen(dest)], "#define %s_RXPDO_COUNT            %u\n",
			nameupper, db_get_rxpdo_count(&dev.db));
	sprintf(&dest[strlen(dest)], "#define %s_TXPDO_COUNT            %u\n\n\n",
			nameupper, db_get_txpdo_count(&dev.db));


	// create header objects from object dictionary objects
	for (int i = 0; i < db_get_object_count(&dev.db); i++) {
		db_obj_st *obj = db_get_obj(&dev.db, i);
		// *name* contains the object name in upper case letters
		char name[1024];
		char namel[1024];
		int j = 0;
		while (obj->name[j] != '\0') {
			if (isspace(obj->name[j])) {
				name[j] = '_';
				namel[j] = '_';
			}
			else {
				name[j] = toupper(obj->name[j]);
				namel[j] = tolower(obj->name[j]);
			}
			j++;
		}
		name[j] = '\0';
		namel[j] = '\0';
		char line[65536];
		strcpy(line, "#define ");
		strcat(line, nameupper);
		strcat(line, "_");
		strcat(line, name);
		strcat(line, "_INDEX           ");
		sprintf(&line[strlen(line)], "0x%x\n", obj->obj.main_index);
		strcat(line, "#define ");
		strcat(line, nameupper);
		strcat(line, "_");
		strcat(line, name);
		if (uv_canopen_is_array(&obj->obj)) {
			strcat(line, "_ARRAY_MAX_SIZE            ");
			sprintf(&line[strlen(line)], "%u\n", obj->obj.array_max_size);
		}
		else {
			if (CANOPEN_IS_STRING(obj->obj.type)) {
				strcat(line, "_STRING_LEN            ");
				sprintf(&line[strlen(line)], "%u\n", obj->obj.string_len);
			}
			else {
				strcat(line, "_SUBINDEX            ");
				sprintf(&line[strlen(line)], "%u\n", obj->obj.sub_index);
			}
		}
		strcat(line, "#define ");
		strcat(line, nameupper);
		strcat(line, "_");
		strcat(line, name);
		strcat(line, "_TYPE            ");
		db_type_to_str(obj->obj.type, &line[strlen(line)]);
		strcat(line, "\n");

		strcat(line, "#define ");
		strcat(line, nameupper);
		strcat(line, "_");
		strcat(line, name);
		strcat(line, "_PERMISSIONS            ");
		db_permission_to_str(obj->obj.permissions, &line[strlen(line)]);
		strcat(line, "\n");

		if (uv_canopen_is_array(&obj->obj)) {
			db_array_child_st *child = obj->child_ptr;
			int index = 0;
			while (child != NULL) {
				strcat(line, "#define ");
				strcat(line, nameupper);
				strcat(line, "_");
				strcat(line, name);
				strcat(line, "_");
				char childname[1024] = { '\0' };
				char *c = child->name;
				while (*c != '\0') {
					if (isspace(*c)) {
						strcat(childname, "_");
					}
					else {
						childname[strlen(childname)] = toupper(*c);
					}
					c++;
				}
				strcat(line, childname);
				strcat(line, "_SUBINDEX            ");
				sprintf(&line[strlen(line)], "%u\n", index + 1);

				sprintf(line + strlen(line), "#define %s_%s_%s_MIN            %i\n",
						nameupper, name, childname, child->min.value_int);

				sprintf(line + strlen(line), "#define %s_%s_%s_MAX            %i\n",
						nameupper, name, childname, child->max.value_int);

				sprintf(line + strlen(line), "#define %s_%s_%s_DEFAULT            %i\n",
						nameupper, name, childname, child->def.value_int);

				index++;
				child = child->next_sibling;
			}
			char type[128];
			db_type_to_stdint(obj->obj.type, type);
			sprintf(line + strlen(line), "extern const %s %s_%s_defaults[%u];\n",
					type, namelower, namel, obj->obj.array_max_size);

		}
		else if (CANOPEN_IS_INTEGER(obj->obj.type)) {
			sprintf(&line[strlen(line)], "#define %s_%s_VALUE            %i\n",
					nameupper, name, obj->value.value_int);
			sprintf(&line[strlen(line)], "#define %s_%s_DEFAULT            %i\n",
					nameupper, name, obj->def.value_int);
			sprintf(&line[strlen(line)], "#define %s_%s_MIN            %i\n",
					nameupper, name, obj->min.value_int);
			sprintf(&line[strlen(line)], "#define %s_%s_MAX            %i\n",
					nameupper, name, obj->max.value_int);
		}
		else {

		}

		strcat(dest, line);

		strcat(dest, "\n\n");

	}
	strcat(dest, "\n/// @brief: returns the length of object dictionary in objects.\n"
			"uint32_t obj_dict_len(void);\n\n");

	strcat(dest, "#endif");

	return true;
}

bool get_source_objs(char *dest, const char *filename) {
	// create symbol for node id
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

	strcpy(dest, "#include <uv_utilities.h>\n"
			"#include <uv_canopen.h>\n"
			"#include \"main.h\"\n");
	strcat(dest, "#include \"");
	char name[1024];
	strcpy(name, filename);
	char *basenam = basename(name);
	strcat(dest, basenam);
	strcat(dest, ".h\"\n\n\n");

	// CANopen initializer structure
	sprintf(&dest[strlen(dest)], "const uv_canopen_non_volatile_st %s_canopen_init = {\n"
			"    .producer_heartbeat_time_ms = %u,\n"
			"    .rxpdo_coms = {\n",
			namelower,
			400);
	for (uint32_t i = 0; i < db_get_rxpdo_count(&dev.db); i++) {
		db_rxpdo_st *pdo = db_get_rxpdo(&dev.db, i);
		char transmission[128];
		db_transmission_to_str(pdo->transmission, transmission);
		sprintf(&dest[strlen(dest)], "        {\n"
				"            .cob_id = %s,\n"
				"            .transmission_type = %s\n"
				"        }%s\n",
				pdo->cobid,
				transmission,
				(i == db_get_rxpdo_count(&dev.db) - 1) ? "" : ",");
	}
	strcat(dest, "    },\n"
			"    .rxpdo_maps = {\n");
	for (uint32_t i = 0; i < db_get_rxpdo_count(&dev.db); i++) {
		db_rxpdo_st *pdo = db_get_rxpdo(&dev.db, i);
		strcat(dest, "        {\n"
				"            .mappings = {\n");
		int32_t mi = 0;
			canopen_pdo_mapping_st *mapping = &pdo->mappings.mappings[mi];
			while (true) {
				if (mapping->length == 0) {
					break;
				}
				sprintf(&dest[strlen(dest)], "                {\n"
						"                    .main_index = 0x%x,\n"
						"                    .sub_index = %u,\n"
						"                    .length = %u\n"
						"                },\n",
						mapping->main_index,
						mapping->sub_index,
						mapping->length);

				mi++;
				mapping = &pdo->mappings.mappings[mi];
			}

		sprintf(&dest[strlen(dest)], "            }\n"
				"        }%s\n",
				(i == db_get_rxpdo_count(&dev.db) - 1) ? "" : ",");
	}
	strcat(dest,"    },\n"
			"    .txpdo_coms = {\n");
	for (uint32_t i = 0; i < db_get_txpdo_count(&dev.db); i++) {
		db_txpdo_st *pdo = db_get_txpdo(&dev.db, i);
		char transmission[128];
		db_transmission_to_str(pdo->transmission, transmission);
		sprintf(&dest[strlen(dest)], "        {\n"
				"            .cob_id = %s,\n"
				"            .transmission_type = %s,\n"
				"            .inhibit_time = %u,\n"
				"            .event_timer = %u\n"
				"        }%s\n",
				pdo->cobid,
				transmission,
				pdo->inhibit_time,
				pdo->event_timer,
				(i == db_get_txpdo_count(&dev.db) - 1) ? "" : ",");
	}
	strcat(dest, "    },\n"
			"    .txpdo_maps = {\n");
	for (uint32_t i = 0; i < db_get_txpdo_count(&dev.db); i++) {
		db_txpdo_st *pdo = db_get_txpdo(&dev.db, i);
		strcat(dest, "        {\n"
				"            .mappings = {\n");
		int32_t mi = 0;
			canopen_pdo_mapping_st *mapping = &pdo->mappings.mappings[mi];
			while (true) {
				if (mapping->length == 0) {
					break;
				}
				sprintf(&dest[strlen(dest)], "                {\n"
						"                    .main_index = 0x%x,\n"
						"                    .sub_index = %u,\n"
						"                    .length = %u\n"
						"                },\n",
						mapping->main_index,
						mapping->sub_index,
						mapping->length);

				mi++;
				mapping = &pdo->mappings.mappings[mi];
			}

		sprintf(&dest[strlen(dest)], "            }\n"
				"        }%s\n",
				(i == db_get_txpdo_count(&dev.db) - 1) ? "" : ",");
	}
	strcat(dest, "\n"
			"    }\n"
			"};\n");


	// Object dictionary
	strcat(dest, "\n"
			"\n"
			"\n"
			"canopen_object_st obj_dict[] = {\n");

	for (int i = 0; i < db_get_object_count(&dev.db); i++) {
		if (i != 0) {
			strcat(dest, ",\n");
		}

		db_obj_st *obj = db_get_obj(&dev.db, i);
		// *name* contains the object name in upper case letters
		char name[1024] = {};
		int j = 0;
		while (obj->name[j] != '\0') {
			if (isspace(obj->name[j])) {
				name[j] = '_';
			}
			else {
				name[j] = toupper(obj->name[j]);
			}
			j++;
		}
		name[j] = '\0';
		char line[1024] = {};
		strcpy(line, "    {\n"
				"        .main_index = ");
		strcat(line, nameupper);
		strcat(line, "_");
		strcat(line, name);
		strcat(line, "_INDEX,\n");
		if (uv_canopen_is_array(&obj->obj)) {
			strcat(line, "        .array_max_size = ");
			strcat(line, nameupper);
			strcat(line, "_");
			strcat(line, name);
			strcat(line, "_ARRAY_MAX_SIZE,\n");
		}
		else {
			if (CANOPEN_IS_STRING(obj->obj.type)) {
				strcat(line, "        .string_len = ");
				strcat(line, nameupper);
				strcat(line, "_");
				strcat(line, name);
				strcat(line, "_STRING_LEN,\n");
			}
			else {
				strcat(line, "        .sub_index = ");
				strcat(line, nameupper);
				strcat(line, "_");
				strcat(line, name);
				strcat(line, "_SUBINDEX,\n");
			}
		}
		strcat(line, "        .type = ");
		strcat(line, nameupper);
		strcat(line, "_");
		strcat(line, name);
		strcat(line, "_TYPE,\n"
				"        .permissions = ");
		strcat(line, nameupper);
		strcat(line, "_");
		strcat(line, name);
		strcat(line, "_PERMISSIONS,\n"
				"        .data_ptr = (void *) ");
		strcat(line, obj->dataptr);
		strcat(line, "\n    }");

		strcat(dest, line);

	}
	strcat(dest, "\n};\n"
			"\n"
			"uint32_t obj_dict_len(void) {\n"
			"    return sizeof(obj_dict) / sizeof(canopen_object_st);\n"
			"}\n"
			"\n");


	// array type object initializers
	for (int i = 0; i < db_get_object_count(&dev.db); i++) {
		db_obj_st *obj = db_get_obj(&dev.db, i);
		char line[1024] = {};
		char type[128];
		char namel[1024] = {};
		for (int i = 0; i < strlen(obj->name); i++) {
			if (isspace(obj->name[i])) {
				namel[i] = '_';
			}
			else {
				namel[i] = tolower(obj->name[i]);
			}
		}
		db_type_to_stdint(obj->obj.type, type);
		if (CANOPEN_IS_ARRAY(obj->obj.type)) {
			char devname[1024] = {};
			for (int i = 0; i < strlen(db_get_dev_name(&dev.db)); i++) {
				devname[i] = tolower(db_get_dev_name(&dev.db)[i]);
			}
			sprintf(line + strlen(line), "const %s %s_%s_defaults[%u] = {\n",
					type, devname, namel, obj->obj.array_max_size);

			db_array_child_st *child = obj->child_ptr;
			while (child != NULL) {
				sprintf(line + strlen(line), "    %i",
						child->def.value_int);
				if (child->next_sibling != NULL) {
					strcat(line, ",");
				}
				strcat(line, "\n");
				child = child->next_sibling;
			}
			strcat(line, "};\n\n");
		}
		strcat(dest, line);
	}


	return true;
}

bool cmd_export(const char *arg) {
	bool ret = false;

	char filename[1024];
	strcpy(filename, dev.incdest);
	if (dev.incdest[strlen(dev.incdest) - 1] != '/') {
		strcat(filename, "/");
	}
	strcat(filename, arg);
	strcat(filename, ".h");
	FILE *headerfile = fopen(filename, "w");
	if (headerfile == NULL) {
		// failed to open the file, exit this task
		printf("Failed to open header file '%s'.\n", filename);
	}
	else {
		strcpy(filename, dev.srcdest);
		if (dev.srcdest[strlen(dev.srcdest) - 1] != '/') {
			strcat(filename, "/");
		}
		strcat(filename, arg);
		strcat(filename, ".c");
		FILE *sourcefile = fopen(filename, "w");

		if (sourcefile == NULL) {
			// failed to open source file
			printf("Failed to open source file '%s'.\n", filename);
		}
		else {
			char objs[65536] = "";
			get_header_objs(objs, arg);
			fwrite(objs, sizeof(char), strlen(objs), headerfile);
	//		printf("header objects: \n%s\n", objs);

			get_source_objs(objs, arg);
			fwrite(objs, sizeof(char), strlen(objs), sourcefile);
	//		printf("source objects: \n %s\n", objs);


			fclose(headerfile);
			fclose(sourcefile);
		}

	}


	ret = true;
	return ret;
}


