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
#include <stdbool.h>
#include <stdarg.h>
#include <ctype.h>
#include <uv_terminal.h>
#include <libgen.h>
#include "db.h"
#include "main.h"



#define this (&dev)

#define ERROR(str, ...) printf(PRINT_BOLDRED str PRINT_RESET, __VA_ARGS__)
#define ERRORSTR(str) printf(PRINT_BOLDRED str PRINT_RESET)
#define WARNING(str, ...) printf(PRINT_BOLDYELLOW str PRINT_RESET, __VA_ARGS__)
#define WARNINGSTR(str) printf(PRINT_BOLDYELLOW str PRINT_RESET)

// Minimum column where #define values start (total line width including "#define ")
#define DEFINE_VALUE_COL		80

// Appends "#define <name>" padded to DEFINE_VALUE_COL, then the value and newline
static void append_define(char *dest, const char *name, const char *value) {
	char buf[4096];
	int len = sprintf(buf, "#define %s", name);
	int pad = DEFINE_VALUE_COL - len;
	if (pad < 1) {
		pad = 1;
	}
	sprintf(buf + len, "%*s%s\n", pad, "", value);
	strcat(dest, buf);
}

// Appends "    <name> = <value>" padded to DEFINE_VALUE_COL for enum entries
static void append_enum_value(char *dest, const char *name, const char *value,
		bool comma) {
	char buf[4096];
	int len = sprintf(buf, "    %s", name);
	int pad = DEFINE_VALUE_COL - len;
	if (pad < 1) {
		pad = 1;
	}
	sprintf(buf + len, "%*s= %s%s\n", pad, "", value, comma ? "," : "");
	strcat(dest, buf);
}

// Same as append_enum_value but with formatted value
static void append_enum_value_fmt(char *dest, const char *name, bool comma,
		const char *fmt, ...) {
	char value[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(value, sizeof(value), fmt, args);
	va_end(args);
	append_enum_value(dest, name, value, comma);
}

// Same but for formatted values
static void append_define_fmt(char *dest, const char *name, const char *fmt, ...) {
	char value[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(value, sizeof(value), fmt, args);
	va_end(args);
	append_define(dest, name, value);
}


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

	char nameupper[1024] = {};
	char namelower[1024] = {};
	for (int i = 0; i < strlen(db_get_dev_name(&dev.db)); i++) {
		char c[2];
		c[0] = toupper(db_get_dev_name(&dev.db)[i]);
		c[1] = '\0';
		strcat(nameupper, c);
		c[0] = tolower(db_get_dev_name(&dev.db)[i]);
		strcat(namelower, c);
	}

	// wrap everything in device name macro
	sprintf(dest + strlen(dest), "#ifndef %s\n"
			"#define %s 1\n\n",
			nameupper,
			nameupper);

	// create symbol for node id
	char defname[2048];
	sprintf(defname, "%s_NODEID", nameupper);
	append_define_fmt(dest, defname, "0x%x", db_get_nodeid(&dev.db));
	strcat(dest, "\n\n\n");

	// create symbol for vendor id
	sprintf(defname, "%s_VENDOR_ID", nameupper);
	append_define_fmt(dest, defname, "0x%x", db_get_vendor_id(&dev.db));
	strcat(dest, "\n\n");

	// for product code
	sprintf(defname, "%s_PRODUCT_CODE", nameupper);
	append_define_fmt(dest, defname, "0x%x", db_get_product_code(&dev.db));
	strcat(dest, "\n\n");

	// for revision number
	sprintf(defname, "%s_REVISION_NUMBER", nameupper);
	append_define_fmt(dest, defname, "0x%x", db_get_revision_number(&dev.db));
	strcat(dest, "\n\n");

	char line[65536];

	// create symbols for EMCY messages
	printf("Creating header symbols for EMCY messages\n");
	fflush(stdout);

	// create symbol for EMCY start index
	sprintf(defname, "%s_EMCY_START_INDEX", nameupper);
	append_define_fmt(dest, defname, "0x%x", db_get_emcy_index(&dev.db));
	strcat(dest, "\n");

	// create symbol for emcy str language count
	uint8_t str_count;
	for (str_count = 0;
			db_get_emcy(&dev.db, 0)->info_strs[str_count][0] != '\0';
			str_count++) { };
	sprintf(defname, "%s_EMCYSTR_INFO_STR_COUNT", nameupper);
	append_define_fmt(dest, defname, "%u", str_count);
	strcat(dest, "\n");

	// create enum for emcy err codes
	strcat(dest, "typedef enum {\n");
	for (int i = 0; i < db_get_emcy_count(&dev.db); i++) {
		db_emcy_st *emcy = db_get_emcy(&dev.db, i);

		char ename[2048];
		sprintf(ename, "%s_EMCY_%s", nameupper, emcy->name);
		append_enum_value_fmt(dest, ename, true, "%i", emcy->value);
	}
	{
		char ename[2048];
		sprintf(ename, "%s_EMCY_COUNT", nameupper);
		append_enum_value_fmt(dest, ename, false, "%u", db_get_emcy_count(&dev.db));
	}
	sprintf(&(dest[strlen(dest)]), "} %s_emcy_err_codes_e;\n\n", namelower);


	strcat(dest, "\n\n\n\n");

	// create symbols for defines
	printf("Creating header symbols for defines\n");
	fflush(stdout);
	for (int i = 0; i < db_get_define_count(&dev.db); i++) {
		db_define_st *define = db_get_define(&dev.db, i);

		if (define->type == DB_DEFINE_INT) {
			sprintf(defname, "%s_%s", nameupper, define->name);
			append_define_fmt(dest, defname, "%i", define->value);
			strcat(dest, "\n");
		}
		else if (define->type == DB_DEFINE_STRING) {
			sprintf(defname, "%s_%s", nameupper, define->name);
			append_define(dest, defname, define->str);
		}
		else if (define->type == DB_DEFINE_ENUM) {
			if (define->data_type == CANOPEN_UNDEFINED) {
				sprintf(line, "typedef enum {\n");
			}
			else {
				sprintf(line, "enum {\n");
			}
			strcat(dest, line);
			bool value_given = false;
			for (int32_t i = 0; i < define->child_count; i++) {
				bool comma = (i < define->child_count - 1);
				char ename[2048];
				sprintf(ename, "%s_%s_%s",
						nameupper, define->name, define->childs[i]);
				// only write the value if it is not defined
				if (strstr(define->childs[i], "=") != NULL) {
					// value already embedded in name, just pad and append
					value_given = true;
					int len = sprintf(line, "    %s", ename);
					sprintf(line + len, "%s\n", comma ? "," : "");
					strcat(dest, line);
				}
				else if (i == 0) {
					append_enum_value(dest, ename, "0", comma);
				}
				else if ((i == define->child_count - 1) && value_given) {
					append_enum_value_fmt(dest, ename, comma, "%u", i);
				}
				else {
					// no explicit value, just the name
					int len = sprintf(line, "    %s", ename);
					sprintf(line + len, "%s\n", comma ? "," : "");
					strcat(dest, line);
				}
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
			ERRORSTR("Unknown define type in export\n");
		}
	}
	strcat(dest, "\n\n\n\n");

	// create symbols for PDO counts
	printf("Creating header symbols for PDO counts\n");
	fflush(stdout);
	sprintf(defname, "%s_RXPDO_COUNT", nameupper);
	append_define_fmt(dest, defname, "%u", db_get_rxpdo_count(&dev.db));
	sprintf(defname, "%s_TXPDO_COUNT", nameupper);
	append_define_fmt(dest, defname, "%u", db_get_txpdo_count(&dev.db));
	strcat(dest, "\n\n");


	// create header objects from object dictionary objects
	printf("Creating header symbols from object dictionary entries\n");
	fflush(stdout);
	for (int i = 0; i < db_get_object_count(&dev.db); i++) {
		db_obj_st *obj = db_get_obj(&dev.db, i);
		// *name* contains the object name in upper case letters
		char name[1024];
		char namel[1024];
		char *objname = dbvalue_get_string(&obj->name);
		int j = 0;
		while (objname[j] != '\0') {
			if (isspace(objname[j])) {
				name[j] = '_';
				namel[j] = '_';
			}
			else {
				name[j] = toupper(objname[j]);
				namel[j] = tolower(objname[j]);
			}
			j++;
		}
		name[j] = '\0';
		namel[j] = '\0';
		line[0] = '\0';
		char objnameh[1024] = {};
		char objnamel[1024] = {};
		if (strncmp(name, nameupper, strlen(nameupper)) != 0) {
			strcpy(objnameh, nameupper);
			strcat(objnameh, "_");
			strcpy(objnamel, namelower);
			strcat(objnamel, "_");
		}
		strcat(objnameh, name);
		strcat(objnamel, namel);

		sprintf(defname, "%s_INDEX", objnameh);
		append_define_fmt(line, defname, "0x%x", obj->obj.main_index);
		if (uv_canopen_is_array(&obj->obj)) {
			sprintf(defname, "%s_ARRAY_MAX_SIZE", objnameh);
			append_define(line, defname, dbvalue_get(&obj->array_max_size));
		}
		else {
			if (CANOPEN_IS_STRING(obj->obj.type)) {
				sprintf(defname, "%s_STRING_LEN", objnameh);
				append_define(line, defname, dbvalue_get(&obj->string_len));
			}
			else {
				sprintf(defname, "%s_SUBINDEX", objnameh);
				append_define_fmt(line, defname, "%u", obj->obj.sub_index);
			}
		}
		sprintf(defname, "%s_TYPE", objnameh);
		append_define(line, defname, obj->type_str);

		sprintf(defname, "%s_PERMISSIONS", objnameh);
		{
			char permbuf[128] = {};
			db_permission_to_str(obj->obj.permissions, permbuf);
			append_define(line, defname, permbuf);
		}

		if (uv_canopen_is_array(&obj->obj)) {
			db_array_child_st *child = obj->child_ptr;
			int index = 0;
			while (child != NULL) {
				char childname[1024] = { '\0' };
				char *c = dbvalue_get_string(&child->name);
				while (*c != '\0') {
					if (isspace(*c)) {
						strcat(childname, "_");
					}
					else {
						childname[strlen(childname)] = toupper(*c);
					}
					c++;
				}
				sprintf(defname, "%s_%s_SUBINDEX", objnameh, childname);
				append_define_fmt(line, defname, "%u", index + 1);
				if (dbvalue_is_set(&child->min)) {
					sprintf(defname, "%s_%s_MIN", objnameh, childname);
					append_define(line, defname, dbvalue_get(&child->min));
				}
				if (dbvalue_is_set(&child->max)) {
					sprintf(defname, "%s_%s_MAX", objnameh, childname);
					append_define(line, defname, dbvalue_get(&child->max));
				}
				sprintf(defname, "%s_%s_DEFAULT", objnameh, childname);
				append_define(line, defname, dbvalue_get(&child->def));

				index++;
				child = child->next_sibling;
			}
			char type[128];
			db_type_to_stdint(obj->obj.type, type);
			sprintf(line + strlen(line), "extern const %s %s_defaults[];\n",
					type, objnamel);
			sprintf(line + strlen(line), "uint32_t %s_defaults_size(void);\n",
					objnamel);

		}
		else if (CANOPEN_IS_INTEGER(obj->obj.type)) {
			sprintf(defname, "%s_VALUE", objnameh);
			append_define(line, defname, dbvalue_get(&obj->value));
			sprintf(defname, "%s_DEFAULT", objnameh);
			append_define(line, defname, dbvalue_get(&obj->def));
			sprintf(defname, "%s_MIN", objnameh);
			append_define(line, defname, dbvalue_get(&obj->min));
			sprintf(defname, "%s_MAX", objnameh);
			append_define(line, defname, dbvalue_get(&obj->max));
		}
		else if (CANOPEN_IS_STRING(obj->obj.type)) {
			sprintf(defname, "%s_DEFAULT", objnameh);
			char strbuf[1024];
			sprintf(strbuf, "\"%s\"", dbvalue_get(&obj->string_def));
			append_define(line, defname, strbuf);
		}
		else {

		}

		strcat(dest, line);

		strcat(dest, "\n\n");

	}
	printf("Object dictionary symbols created\n");
	fflush(stdout);

	strcat(dest, "\n/// @brief: returns the length of object dictionary in objects.\n"
			"uint32_t obj_dict_len(void);\n\n");

	strcat(dest, "#endif\n");
	strcat(dest, "#endif\n");

	return true;
}

bool get_source_objs(char *dest, const char *filename) {
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
	printf("Creating source objects for CANopen initializer structure\n");
	fflush(stdout);
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
			for (int32_t i = 0; i < CONFIG_CANOPEN_PDO_MAPPING_COUNT; i++) {
				canopen_pdo_mapping_st *mapping = &pdo->mappings.mappings[i];

				sprintf(&dest[strlen(dest)], "                {\n"
						"                    .main_index = 0x%x,\n"
						"                    .sub_index = %u,\n"
						"                    .length = %u\n"
						"                },\n",
						mapping->main_index,
						mapping->sub_index,
						mapping->length);
			}

		sprintf(&dest[strlen(dest)], "            }\n"
				"        }%s\n",
				(i == db_get_txpdo_count(&dev.db) - 1) ? "" : ",");
	}
	strcat(dest, "\n"
			"    }\n"
			"};\n");


	// Object dictionary
	printf("Creating source objects for object dictionary\n");
	fflush(stdout);
	strcat(dest, "\n"
			"\n"
			"\n"
			"const canopen_object_st obj_dict[] = {\n");

	for (int i = 0; i < db_get_object_count(&dev.db); i++) {
		if (i != 0) {
			strcat(dest, ",\n");
		}

		db_obj_st *obj = db_get_obj(&dev.db, i);
		// *name* contains the object name in upper case letters
		char name[1024] = {};
		int j = 0;
		if (strncmp(dbvalue_get_string(&obj->name), nameupper, strlen(nameupper)) != 0) {
			strcpy(name, nameupper);
			strcat(name, "_");
			j = strlen(name);
		}
		char *objname = dbvalue_get_string(&obj->name);
		int k = 0;
		while (objname[k] != '\0') {
			if (isspace(objname[k])) {
				name[j] = '_';
			}
			else {
				name[j] = toupper(objname[k]);
			}
			j++;
			k++;
		}
		name[j] = '\0';
		char line[1024] = {};
		strcpy(line, "    {\n"
				"        .main_index = ");
		strcat(line, name);
		strcat(line, "_INDEX,\n");
		if (uv_canopen_is_array(&obj->obj)) {
			strcat(line, "        .array_max_size = ");
			strcat(line, name);
			strcat(line, "_ARRAY_MAX_SIZE,\n");
		}
		else {
			if (CANOPEN_IS_STRING(obj->obj.type)) {
				strcat(line, "        .string_len = ");
				strcat(line, name);
				strcat(line, "_STRING_LEN,\n");
			}
			else {
				strcat(line, "        .sub_index = ");
				strcat(line, name);
				strcat(line, "_SUBINDEX,\n");
			}
		}
		strcat(line, "        .type = ");
		strcat(line, name);
		strcat(line, "_TYPE,\n"
				"        .permissions = ");
		strcat(line, name);
		strcat(line, "_PERMISSIONS,\n"
				"        .data_ptr = (void *) ");
		strcat(line, dbvalue_get_string(&obj->dataptr));
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
	printf("Creating source objects for array initializers\n");
	fflush(stdout);
	for (int i = 0; i < db_get_object_count(&dev.db); i++) {
		db_obj_st *obj = db_get_obj(&dev.db, i);
		char line[65536] = {};
		char type[128];
		char namel[1024] = {};
		char *objname = dbvalue_get_string(&obj->name);
		for (int i = 0; i < strlen(objname); i++) {
			if (isspace(objname[i])) {
				namel[i] = '_';
			}
			else {
				namel[i] = tolower(objname[i]);
			}
		}
		db_type_to_stdint(obj->obj.type, type);
		if (CANOPEN_IS_ARRAY(obj->obj.type)) {
			char devname[1024] = {};
			for (int i = 0; i < strlen(db_get_dev_name(&dev.db)); i++) {
				devname[i] = tolower(db_get_dev_name(&dev.db)[i]);
			}
			sprintf(line + strlen(line), "const %s %s_%s_defaults[] = {\n",
					type, devname, namel);

			db_array_child_st *child = obj->child_ptr;
			while (child != NULL) {
				sprintf(line + strlen(line), "    %s_%s_%s_DEFAULT",
						nameupper, objname, dbvalue_get_string(&child->name));
				if (child->next_sibling != NULL) {
					strcat(line, ",");
				}
				strcat(line, "\n");
				child = child->next_sibling;
			}
			strcat(line, "};\n");

			sprintf(line + strlen(line), "uint32_t %s_%s_defaults_size(void) {\n",
					namelower, namel);
			sprintf(line + strlen(line),
					"    return sizeof(%s_%s_defaults) / sizeof(%s_%s_defaults[0]);\n",
					namelower, namel, namelower, namel);
			strcat(line, "}\n\n");
		}
		strcat(dest, line);
	}

	printf("Exporting done.\n");
	fflush(stdout);

	return true;
}



bool cmd_export(const char *arg) {
	bool ret = false;
	if (cmd_exporth(arg)) {
		ret = cmd_exportc(arg);
	}
	return ret;
}


bool cmd_exporth(const char *arg) {
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
		ERROR("Failed to open header file '%s'.\n", filename);
	}
	else {
		char objs[2000000] = "";
		get_header_objs(objs, arg);
		fwrite(objs, sizeof(char), strlen(objs), headerfile);
//		printf("header objects: \n%s\n", objs);

		fclose(headerfile);

	}


	ret = true;
	return ret;
}


bool cmd_exportc(const char *arg) {
	bool ret = false;

	char filename[1024];
	strcpy(filename, dev.srcdest);
	if (dev.srcdest[strlen(dev.srcdest) - 1] != '/') {
		strcat(filename, "/");
	}
	strcat(filename, arg);
	strcat(filename, ".c");
	FILE *sourcefile = fopen(filename, "w");

	if (sourcefile == NULL) {
		// failed to open source file
		ERROR("Failed to open source file '%s'.\n", filename);
	}
	else {
		char objs[2000000] = "";

		get_source_objs(objs, arg);
		fwrite(objs, sizeof(char), strlen(objs), sourcefile);
//		printf("source objects: \n %s\n", objs);

		fclose(sourcefile);
	}

	ret = true;
	return ret;
}

