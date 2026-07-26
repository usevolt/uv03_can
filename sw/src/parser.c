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
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "parser.h"
#include "main.h"
#if CONFIG_TARGET_WIN
#include "uv_win_compat.h"
#else
#include <libgen.h>
#endif



parser_formats_e parser_format_from_filename(const char *filename) {
	parser_formats_e ret = PARSER_FORMAT_JSON;

	if (filename != NULL) {
		const char *ext = strrchr(filename, '.');
		if (ext != NULL) {
			char lower[8] = {};
			ext++;
			for (unsigned int i = 0; (i < sizeof(lower) - 1) && (ext[i] != '\0'); i++) {
				lower[i] = tolower((int) ext[i]);
			}
			if ((strcmp(lower, "yaml") == 0) || (strcmp(lower, "yml") == 0)) {
				ret = PARSER_FORMAT_YAML;
			}
		}
	}

	return ret;
}


const char *parser_format_to_str(parser_formats_e format) {
	return (format == PARSER_FORMAT_YAML) ? "YAML" : "JSON";
}


bool parser_find_file(const char *dir, const char *name,
		char *dest, unsigned int dest_length) {
	bool ret = false;
	static const char *exts[] = { "json", "yaml", "yml" };

	if ((dir != NULL) && (name != NULL) && (dest != NULL)) {
		for (unsigned int i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
			snprintf(dest, dest_length, "%s/%s.%s", dir, name, exts[i]);
			FILE *f = fopen(dest, "rb");
			if (f != NULL) {
				fclose(f);
				ret = true;
				break;
			}
		}
		if (!ret) {
			// report the primary name in the caller's error messages
			snprintf(dest, dest_length, "%s/%s.json", dir, name);
		}
	}

	return ret;
}


const char *parser_type_to_str(parser_types_e type) {
	const char *ret = "UNDEFINED";

	switch (type) {
	case PARSER_OBJECT:
		ret = "OBJECT";
		break;
	case PARSER_ARRAY:
		ret = "ARRAY";
		break;
	case PARSER_INT:
		ret = "INT";
		break;
	case PARSER_BOOL:
		ret = "BOOL";
		break;
	case PARSER_STRING:
		ret = "STRING";
		break;
	case PARSER_UNSUPPORTED:
		ret = "UNSUPPORTED";
		break;
	default:
		break;
	}

	return ret;
}


/// @brief: Converts a JSON type to the common type
static parser_types_e json_type(uv_json_types_e type) {
	parser_types_e ret = PARSER_UNSUPPORTED;

	switch (type) {
	case JSON_OBJECT:
		ret = PARSER_OBJECT;
		break;
	case JSON_ARRAY:
		ret = PARSER_ARRAY;
		break;
	case JSON_INT:
		ret = PARSER_INT;
		break;
	case JSON_BOOL:
		ret = PARSER_BOOL;
		break;
	case JSON_STRING:
		ret = PARSER_STRING;
		break;
	default:
		break;
	}

	return ret;
}


/// @brief: Converts a YAML type to the common type
static parser_types_e yaml_type(uv_yaml_types_e type) {
	parser_types_e ret = PARSER_UNSUPPORTED;

	switch (type) {
	case YAML_MAP:
		ret = PARSER_OBJECT;
		break;
	case YAML_SEQ:
		ret = PARSER_ARRAY;
		break;
	case YAML_INT:
		ret = PARSER_INT;
		break;
	case YAML_BOOL:
		ret = PARSER_BOOL;
		break;
	case YAML_STRING:
		ret = PARSER_STRING;
		break;
	default:
		break;
	}

	return ret;
}


/// @brief: The buffer length which writing a hexadecimal value requires
#define HEXBUF_LEN		16


/// @brief: Writes *value* to *dest* in the hexadecimal notation which
/// uvcan's files use
static void hex_str(char *dest, unsigned int dest_length, uint32_t value) {
	snprintf(dest, dest_length, "0x%x", value);
}


/// @brief: Returns true if the *len* characters long string at *str* is a
/// hexadecimal value, e.g. "0x2100"
static bool is_hex_str(const char *str, unsigned int len) {
	bool ret = false;

	if ((str != NULL) && (len > 2) && (str[0] == '0') &&
			((str[1] == 'x') || (str[1] == 'X'))) {
		ret = true;
		for (unsigned int i = 2; i < len; i++) {
			if (!isxdigit((int) str[i])) {
				ret = false;
				break;
			}
		}
	}

	return ret;
}


/// @brief: Returns the common type of a YAML node.
///
/// Both of uvcan's file formats store the hexadecimal values as strings,
/// i.e. quoted in YAML, since that keeps them distinguishable from the
/// decimal values. Such a string is reported as an integer, just like the
/// JSON reader does.
static parser_types_e yaml_node_type(uv_yaml_node_st node) {
	parser_types_e ret = yaml_type(uv_yamlreader_get_type(node));

	if ((ret == PARSER_STRING) &&
			is_hex_str(uv_yamlreader_get_string_ptr(node),
					uv_yamlreader_get_string_len(node))) {
		ret = PARSER_INT;
	}

	return ret;
}


/// @brief: Wraps a JSON node into a common node
static parser_node_st json_node(char *node) {
	parser_node_st ret;
	ret.format = PARSER_FORMAT_JSON;
	ret.json = node;
	return ret;
}


/// @brief: Wraps a YAML node into a common node
static parser_node_st yaml_node(uv_yaml_node_st node) {
	parser_node_st ret;
	ret.format = PARSER_FORMAT_YAML;
	ret.yaml = node;
	return ret;
}


parser_node_st parser_node_invalid(void) {
	return json_node(NULL);
}


bool parser_node_is_valid(parser_node_st node) {
	return (node.format == PARSER_FORMAT_JSON) ?
			(node.json != NULL) : uv_yaml_node_is_valid(node.yaml);
}



/***** READING FUNCTIONS ******/


parser_node_st parser_read_buffer(char *buffer, unsigned int buffer_length,
		parser_formats_e format) {
	parser_node_st ret = parser_node_invalid();

	if (buffer != NULL) {
		if (format == PARSER_FORMAT_YAML) {
			if (uv_yamlreader_init(buffer, buffer_length) == ERR_NONE) {
				ret = yaml_node(uv_yamlreader_get_root(buffer));
			}
		}
		else {
			if (uv_jsonreader_init(buffer, buffer_length) == ERR_NONE) {
				ret = json_node(buffer);
			}
		}
	}

	return ret;
}


parser_node_st parser_read_file(const char *path, char **dest_buffer) {
	parser_node_st ret = parser_node_invalid();
	char *data = NULL;

	if (path != NULL) {
		FILE *f = fopen(path, "rb");
		if (f != NULL) {
			fseek(f, 0, SEEK_END);
			long size = ftell(f);
			fseek(f, 0, SEEK_SET);
			if (size > 0) {
				data = malloc((size_t) size + 1);
				if (data != NULL) {
					if (fread(data, 1, (size_t) size, f) == (size_t) size) {
						data[size] = '\0';
						ret = parser_read_buffer(data, strlen(data),
								parser_format_from_filename(path));
					}
					if (!parser_node_is_valid(ret)) {
						free(data);
						data = NULL;
					}
				}
			}
			fclose(f);
		}
	}

	if (dest_buffer != NULL) {
		*dest_buffer = data;
	}

	return ret;
}


parser_node_st parser_find_child(parser_node_st parent, const char *child_name) {
	parser_node_st ret;

	if (parent.format == PARSER_FORMAT_JSON) {
		ret = json_node(parser_node_is_valid(parent) ?
				uv_jsonreader_find_child(parent.json, (char*) child_name) : NULL);
	}
	else {
		ret = yaml_node(uv_yamlreader_find_child(parent.yaml, child_name));
	}

	return ret;
}


parser_node_st parser_get_child(parser_node_st parent, uint16_t index) {
	parser_node_st ret;

	if (parent.format == PARSER_FORMAT_JSON) {
		ret = json_node(parser_node_is_valid(parent) ?
				uv_jsonreader_get_child(parent.json, index) : NULL);
	}
	else {
		ret = yaml_node(uv_yamlreader_get_child(parent.yaml, index));
	}

	return ret;
}


unsigned int parser_get_child_count(parser_node_st parent) {
	unsigned int ret = 0;

	if (parent.format == PARSER_FORMAT_YAML) {
		ret = uv_yamlreader_get_child_count(parent.yaml);
	}
	else if (parser_node_is_valid(parent)) {
		// a bare cell of a JSON array has no key, so the generic type check
		// doesn't work on it. Such a cell starts with the value itself.
		if ((*parent.json == '[') ||
				(uv_jsonreader_get_type(parent.json) == JSON_ARRAY)) {
			ret = uv_jsonreader_array_get_size(parent.json);
		}
		else {
			// the JSON reader has no child count of it's own, cycle the children
			char *child = uv_jsonreader_get_child(parent.json, 0);
			while (child != NULL) {
				ret++;
				if (!uv_jsonreader_get_next_sibling(child, &child)) {
					break;
				}
			}
		}
	}
	else {

	}

	return ret;
}


bool parser_get_next_sibling(parser_node_st node, parser_node_st *dest) {
	bool ret = false;

	if (parser_node_is_valid(node)) {
		if (node.format == PARSER_FORMAT_JSON) {
			char *sibling = NULL;
			ret = uv_jsonreader_get_next_sibling(node.json, &sibling);
			if (ret && (dest != NULL)) {
				*dest = json_node(sibling);
			}
		}
		else {
			uv_yaml_node_st sibling;
			ret = uv_yamlreader_get_next_sibling(node.yaml, &sibling);
			if (ret && (dest != NULL)) {
				*dest = yaml_node(sibling);
			}
		}
	}

	return ret;
}


bool parser_get_obj_name(parser_node_st node, char *dest, unsigned int dest_length) {
	bool ret = false;

	if (parser_node_is_valid(node)) {
		ret = (node.format == PARSER_FORMAT_JSON) ?
				uv_jsonreader_get_obj_name(node.json, dest, dest_length) :
				uv_yamlreader_get_obj_name(node.yaml, dest, dest_length);
	}
	else if ((dest != NULL) && (dest_length != 0)) {
		dest[0] = '\0';
	}
	else {

	}

	return ret;
}


parser_types_e parser_get_type(parser_node_st node) {
	parser_types_e ret = PARSER_UNSUPPORTED;

	if (parser_node_is_valid(node)) {
		ret = (node.format == PARSER_FORMAT_JSON) ?
				json_type(uv_jsonreader_get_type(node.json)) :
				yaml_node_type(node.yaml);
	}

	return ret;
}


int parser_get_int(parser_node_st node) {
	int ret = 0;

	if (parser_node_is_valid(node)) {
		ret = (node.format == PARSER_FORMAT_JSON) ?
				uv_jsonreader_get_int(node.json) :
				uv_yamlreader_get_int(node.yaml);
	}

	return ret;
}


bool parser_get_bool(parser_node_st node) {
	bool ret = false;

	if (parser_node_is_valid(node)) {
		ret = (node.format == PARSER_FORMAT_JSON) ?
				uv_jsonreader_get_bool(node.json) :
				uv_yamlreader_get_bool(node.yaml);
	}

	return ret;
}


bool parser_get_string(parser_node_st node, char *dest, unsigned int dest_length) {
	bool ret = false;

	if (parser_node_is_valid(node)) {
		ret = (node.format == PARSER_FORMAT_JSON) ?
				uv_jsonreader_get_string(node.json, dest, dest_length) :
				uv_yamlreader_get_string(node.yaml, dest, dest_length);
	}
	else if ((dest != NULL) && (dest_length != 0)) {
		dest[0] = '\0';
	}
	else {

	}

	return ret;
}


char *parser_get_string_ptr(parser_node_st node) {
	char *ret = NULL;

	if (parser_node_is_valid(node)) {
		ret = (node.format == PARSER_FORMAT_JSON) ?
				uv_jsonreader_get_string_ptr(node.json) :
				uv_yamlreader_get_string_ptr(node.yaml);
	}

	return ret;
}


unsigned int parser_get_string_len(parser_node_st node) {
	unsigned int ret = 0;

	if (parser_node_is_valid(node)) {
		ret = (node.format == PARSER_FORMAT_JSON) ?
				uv_jsonreader_get_string_len(node.json) :
				uv_yamlreader_get_string_len(node.yaml);
	}

	return ret;
}


unsigned int parser_array_get_size(parser_node_st array) {
	unsigned int ret = 0;

	if (parser_node_is_valid(array)) {
		ret = (array.format == PARSER_FORMAT_JSON) ?
				uv_jsonreader_array_get_size(array.json) :
				uv_yamlreader_seq_get_size(array.yaml);
	}

	return ret;
}


parser_node_st parser_array_at(parser_node_st array, unsigned int index) {
	parser_node_st ret;

	if (array.format == PARSER_FORMAT_JSON) {
		ret = json_node(parser_node_is_valid(array) ?
				uv_jsonreader_array_at(array.json, index) : NULL);
	}
	else {
		ret = yaml_node(uv_yamlreader_seq_at(array.yaml, index));
	}

	return ret;
}


parser_types_e parser_array_get_type(parser_node_st array, unsigned int index) {
	parser_types_e ret = PARSER_UNSUPPORTED;

	if (parser_node_is_valid(array)) {
		ret = (array.format == PARSER_FORMAT_JSON) ?
				json_type(uv_jsonreader_array_get_type(array.json, index)) :
				yaml_node_type(uv_yamlreader_seq_at(array.yaml, index));
	}

	return ret;
}


int parser_array_get_int(parser_node_st array, unsigned int index) {
	int ret = 0;

	if (parser_node_is_valid(array)) {
		ret = (array.format == PARSER_FORMAT_JSON) ?
				uv_jsonreader_array_get_int(array.json, index) :
				uv_yamlreader_seq_get_int(array.yaml, index);
	}

	return ret;
}


bool parser_array_get_bool(parser_node_st array, unsigned int index) {
	bool ret = false;

	if (parser_node_is_valid(array)) {
		ret = (array.format == PARSER_FORMAT_JSON) ?
				uv_jsonreader_array_get_bool(array.json, index) :
				uv_yamlreader_seq_get_bool(array.yaml, index);
	}

	return ret;
}


bool parser_array_get_string(parser_node_st array, unsigned int index,
		char *dest, unsigned int dest_length) {
	bool ret = false;

	if (parser_node_is_valid(array)) {
		ret = (array.format == PARSER_FORMAT_JSON) ?
				uv_jsonreader_array_get_string(array.json, index, dest, dest_length) :
				uv_yamlreader_seq_get_string(array.yaml, index, dest, dest_length);
	}
	else if ((dest != NULL) && (dest_length != 0)) {
		dest[0] = '\0';
	}
	else {

	}

	return ret;
}



/***** WRITING FUNCTIONS ******/


uv_errors_e parser_writer_init(parser_writer_st *this, char *buffer,
		unsigned int buffer_length, parser_formats_e format) {
	uv_errors_e ret;

	this->format = format;
	if (format == PARSER_FORMAT_YAML) {
		ret = uv_yamlwriter_init(&this->yaml, buffer, buffer_length);
		// uvcan's files quote all the string values, keeping the values
		// distinguishable from the YAML syntax at a glance
		uv_yamlwriter_set_quote_strings(&this->yaml, true);
	}
	else {
		ret = uv_jsonwriter_init(&this->json, buffer, buffer_length);
	}

	return ret;
}


uv_errors_e parser_writer_end(parser_writer_st *this) {
	return (this->format == PARSER_FORMAT_YAML) ?
			uv_yamlwriter_end(&this->yaml, NULL) :
			uv_jsonwriter_end(&this->json, NULL);
}


uv_errors_e parser_writer_begin_object(parser_writer_st *this, const char *name) {
	return (this->format == PARSER_FORMAT_YAML) ?
			uv_yamlwriter_begin_map(&this->yaml, name) :
			uv_jsonwriter_begin_object_named(&this->json, (char*) name);
}


uv_errors_e parser_writer_end_object(parser_writer_st *this) {
	return (this->format == PARSER_FORMAT_YAML) ?
			uv_yamlwriter_end_map(&this->yaml) :
			uv_jsonwriter_end_object(&this->json);
}


uv_errors_e parser_writer_begin_array(parser_writer_st *this, const char *name) {
	return (this->format == PARSER_FORMAT_YAML) ?
			uv_yamlwriter_begin_seq(&this->yaml, name) :
			uv_jsonwriter_begin_array(&this->json, (name != NULL) ? (char*) name : "");
}


uv_errors_e parser_writer_end_array(parser_writer_st *this) {
	return (this->format == PARSER_FORMAT_YAML) ?
			uv_yamlwriter_end_seq(&this->yaml) :
			uv_jsonwriter_end_array(&this->json);
}


uv_errors_e parser_writer_add_int(parser_writer_st *this, const char *name, int value) {
	return (this->format == PARSER_FORMAT_YAML) ?
			uv_yamlwriter_add_int(&this->yaml, name, value) :
			uv_jsonwriter_add_int(&this->json, (char*) name, value);
}


uv_errors_e parser_writer_add_int_hex(parser_writer_st *this,
		const char *name, uint32_t value) {
	uv_errors_e ret;

	if (this->format == PARSER_FORMAT_YAML) {
		// uvcan's files store the hexadecimal values as strings.
		// The reading side maps them back to integers.
		char str[HEXBUF_LEN];
		hex_str(str, sizeof(str), value);
		ret = uv_yamlwriter_add_string(&this->yaml, name, str);
	}
	else {
		ret = uv_jsonwriter_add_int_hex(&this->json, (char*) name, value);
	}

	return ret;
}


uv_errors_e parser_writer_add_string(parser_writer_st *this,
		const char *name, const char *value) {
	return (this->format == PARSER_FORMAT_YAML) ?
			uv_yamlwriter_add_string(&this->yaml, name, value) :
			uv_jsonwriter_add_string(&this->json, (char*) name, (char*) value);
}


uv_errors_e parser_writer_add_bool(parser_writer_st *this,
		const char *name, bool value) {
	return (this->format == PARSER_FORMAT_YAML) ?
			uv_yamlwriter_add_bool(&this->yaml, name, value) :
			uv_jsonwriter_add_bool(&this->json, (char*) name, value);
}


uv_errors_e parser_writer_array_add_int(parser_writer_st *this, int value) {
	return (this->format == PARSER_FORMAT_YAML) ?
			uv_yamlwriter_seq_add_int(&this->yaml, value) :
			uv_jsonwriter_array_add_int(&this->json, value);
}


uv_errors_e parser_writer_array_add_int_hex(parser_writer_st *this, uint32_t value) {
	uv_errors_e ret;

	if (this->format == PARSER_FORMAT_YAML) {
		char str[HEXBUF_LEN];
		hex_str(str, sizeof(str), value);
		ret = uv_yamlwriter_seq_add_string(&this->yaml, str);
	}
	else {
		ret = uv_jsonwriter_array_add_int_hex(&this->json, value);
	}

	return ret;
}


uv_errors_e parser_writer_array_add_string(parser_writer_st *this, const char *value) {
	return (this->format == PARSER_FORMAT_YAML) ?
			uv_yamlwriter_seq_add_string(&this->yaml, value) :
			uv_jsonwriter_array_add_string(&this->json, (char*) value);
}


uv_errors_e parser_writer_array_add_bool(parser_writer_st *this, bool value) {
	return (this->format == PARSER_FORMAT_YAML) ?
			uv_yamlwriter_seq_add_bool(&this->yaml, value) :
			uv_jsonwriter_array_add_bool(&this->json, value);
}


bool parser_writer_array_append_doc(parser_writer_st *this, char *data) {
	bool ret;

	if (this->format == PARSER_FORMAT_YAML) {
		// the appended document is a mapping of it's own. Opening it as an
		// entry of the currently open sequence indents it and prefixes it
		// with the sequence entry's '-'.
		ret = (uv_yamlwriter_begin_map(&this->yaml, NULL) == ERR_NONE);
		if (ret) {
			ret = uv_yamlwriter_append_yaml(&this->yaml, data);
			uv_yamlwriter_end_map(&this->yaml);
		}
	}
	else {
		ret = uv_jsonwriter_append_json(&this->json, data);
	}

	return ret;
}


/// @brief: Pretty-prints (refactors) the JSON file at *file* in place with jq.
/// Relies on the jq/cp/rm shell utilities, which are not available on native
/// Windows, so the step is skipped there.
static void prettify_json_file(const char *file) {
#if CONFIG_TARGET_LINUX
	printf("Refactoring the JSON file\n");
	fflush(stdout);
	char cmd[1024];
	char tempname[270];
	char fcopy[256];
	strcpy(fcopy, file);
	char *base = basename(fcopy);
	char *dir = dirname(fcopy);
	sprintf(tempname, "%s/__%s_temp", dir, base);
	sprintf(cmd, "jq '.' %s > %s", file, tempname);
	printf("Running the command: %s\n", cmd);
	fflush(stdout);
	if (system(cmd) == 0) {
		// copy the refactored output to the actual file and remove the temp file
		sprintf(cmd, "cp %s %s", tempname, file);
		if (system(cmd));
	}
	sprintf(cmd, "rm %s", tempname);
	if (system(cmd));
#else
	(void) file;
#endif
}


bool parser_write_file(const char *path, const char *buffer, parser_formats_e format) {
	bool ret = false;

	if ((path != NULL) && (buffer != NULL)) {
		FILE *dest = fopen(path, "wb");
		if (dest != NULL) {
			fwrite(buffer, 1, strlen(buffer), dest);
			fclose(dest);
			// the YAML writer already outputs the document indented, only
			// the JSON output is written on a single line
			if (format == PARSER_FORMAT_JSON) {
				prettify_json_file(path);
			}
			ret = true;
		}
	}

	return ret;
}
