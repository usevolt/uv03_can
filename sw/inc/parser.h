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

#ifndef PARSER_H_
#define PARSER_H_


#include <uv_utilities.h>
#include <uv_json.h>
#include <uv_yaml.h>
#include <stdbool.h>
#include <stdint.h>


/// @file: A common interface for reading and writing both JSON and YAML files.
///
/// Every file which uvcan reads or writes can be in either of the two formats.
/// The format is selected with *parser_format_from_filename*, i.e. by the
/// file's extension: '.yaml' and '.yml' are parsed as YAML and everything
/// else as JSON.
///
/// The interface follows the uv_json module: The documents are parsed in place
/// from a buffer which the application owns and the nodes returned by the
/// reader merely refer to that buffer. The buffer has to stay valid and
/// unmodified as long as the nodes are used.
///
/// Note that as JSON is a subset of YAML, a JSON document can also be read
/// with the YAML parser. The formats are still kept separate, since the JSON
/// parser is more permissive with the things which uvcan's own files rely on,
/// such as hexadecimal values written as strings.


/// @brief: The file formats which the parser supports
typedef enum {
	PARSER_FORMAT_JSON = 0,
	PARSER_FORMAT_YAML
} parser_formats_e;


/// @brief: Defines all supported value types.
/// Values which can contain child nodes are negative and
/// other values are positive.
typedef enum {
	PARSER_OBJECT = -100,
	PARSER_ARRAY,
	PARSER_UNSUPPORTED = 0,
	PARSER_INT = 1,
	PARSER_BOOL,
	PARSER_STRING
} parser_types_e;


/// @brief: Refers to a single node in a parsed document. Nodes are passed
/// around by value. Use *parser_node_is_valid* to check if the node refers
/// to anything.
typedef struct {
	parser_formats_e format;
	union {
		/// @brief: Valid when *format* is PARSER_FORMAT_JSON
		char *json;
		/// @brief: Valid when *format* is PARSER_FORMAT_YAML
		uv_yaml_node_st yaml;
	};
} parser_node_st;


/// @brief: The state of a document which is being written
typedef struct {
	parser_formats_e format;
	uv_json_st json;
	uv_yaml_st yaml;
} parser_writer_st;


/// @brief: Returns the format in which the file at *filename* should be
/// read or written. '.yaml' and '.yml' select YAML, anything else JSON.
parser_formats_e parser_format_from_filename(const char *filename);


/// @brief: Returns the name of *format* for user visible messages
const char *parser_format_to_str(parser_formats_e format);


/// @brief: Searches the directory *dir* for a file which is named *name*
/// with any of the supported extensions, i.e. '<name>.json', '<name>.yaml'
/// or '<name>.yml'. Used for the manifests inside the packages, which can
/// be written in either of the formats.
///
/// @return: true if a matching file was found
///
/// @param dest: The path of the found file is stored here
bool parser_find_file(const char *dir, const char *name,
		char *dest, unsigned int dest_length);


/// @brief: Returns the type as a string, e.g. for error messages
const char *parser_type_to_str(parser_types_e type);


/// @brief: Returns true if *type* is an object or an array, i.e. a node
/// which can contain children
static inline bool parser_is_objarray(parser_types_e type) {
	return (type < PARSER_UNSUPPORTED);
}


/// @brief: Returns true if *node* refers to an existing node. The reader
/// functions return an invalid node when the requested node was not found.
bool parser_node_is_valid(parser_node_st node);


/// @brief: Returns an invalid node, e.g. for initializing a variable
parser_node_st parser_node_invalid(void);



/***** READING FUNCTIONS ******/


/// @brief: Parses the document in *buffer* and returns it's root node.
/// The buffer is modified in place and it has to stay valid as long as
/// the returned node or any of it's children are used.
///
/// @return: The root node of the document, or an invalid node on failure
///
/// @param buffer: A null terminated buffer containing the whole document
/// @param buffer_length: The length of the buffer in bytes
/// @param format: The format which the document is parsed as
parser_node_st parser_read_buffer(char *buffer, unsigned int buffer_length,
		parser_formats_e format);


/// @brief: Reads the whole file at *path* into a freshly malloc'd buffer and
/// parses it in the format given by the file's extension.
///
/// @return: The root node of the document, or an invalid node if the file
/// could not be read.
///
/// @param path: The path of the file to read
/// @param dest_buffer: The malloc'd buffer is stored here. The caller has to
/// free() it once the returned node is not used anymore. Set to NULL on failure.
parser_node_st parser_read_file(const char *path, char **dest_buffer);


/// @brief: Finds and returns a child node with a name *child_name* from the
/// *parent* node.
///
/// @note: Since the children of an array don't have names, they are evaluated
/// as empty strings.
parser_node_st parser_find_child(parser_node_st parent, const char *child_name);


/// @brief: Returns the *index*'th child of the *parent* node, or an invalid
/// node if the parent doesn't have that many children.
parser_node_st parser_get_child(parser_node_st parent, uint16_t index);


/// @brief: Returns the count of children which *parent* has. Works for both
/// objects and arrays.
unsigned int parser_get_child_count(parser_node_st parent);


/// @brief: Gives the next sibling coming after *node*
///
/// @return: true if the next sibling could be found, false otherwise.
///
/// @param dest: The found sibling is stored here. If the sibling couldn't be
/// found, this function doesn't modify dest at all.
bool parser_get_next_sibling(parser_node_st node, parser_node_st *dest);


/// @brief: Stores the name of the *node* to 'dest'. Nodes inside an array
/// don't have a name, in which case a null string is returned.
///
/// @return: true if the name could be stored in 'dest'. false if the name
/// was too long to fit into 'dest'.
bool parser_get_obj_name(parser_node_st node, char *dest, unsigned int dest_length);


/// @brief: Returns the type of the node
parser_types_e parser_get_type(parser_node_st node);


/// @brief: Returns the node's value as an integer
int parser_get_int(parser_node_st node);


/// @brief: Returns the node's value as a bool
bool parser_get_bool(parser_node_st node);


/// @brief: Passes the node's value as a null-terminated string to 'dest'.
/// If the string is longer than dest_length (including the termination '\0'
/// char), returns false.
bool parser_get_string(parser_node_st node, char *dest, unsigned int dest_length);


/// @brief: Returns a pointer to the string value of *node*.
/// Note: The string is **not** null-terminated!
char *parser_get_string_ptr(parser_node_st node);


/// @brief: Returns the length of the string value of *node*
unsigned int parser_get_string_len(parser_node_st node);


/// @brief: Returns the array's child count
unsigned int parser_array_get_size(parser_node_st array);


/// @brief: Indexes the array's values
parser_node_st parser_array_at(parser_node_st array, unsigned int index);


/// @brief: Returns the type of the array value at index *index*
parser_types_e parser_array_get_type(parser_node_st array, unsigned int index);


/// @brief: Returns the array cell's value as an integer
int parser_array_get_int(parser_node_st array, unsigned int index);


/// @brief: Returns the array cell's value as a bool
bool parser_array_get_bool(parser_node_st array, unsigned int index);


/// @brief: Passes the array cell's value as a null-terminated string to 'dest'.
/// If the string is longer than dest_length, returns false.
bool parser_array_get_string(parser_node_st array, unsigned int index,
		char *dest, unsigned int dest_length);



/***** WRITING FUNCTIONS ******/


/// @brief: Init's a writer which constructs a document in *format* to *buffer*.
/// The document's root is an object, i.e. the writing can be started with
/// the *parser_writer_add_...* functions right away.
uv_errors_e parser_writer_init(parser_writer_st *this, char *buffer,
		unsigned int buffer_length, parser_formats_e format);


/// @brief: Should be called as the last function when finishing the writing.
/// At this point all objects and arrays should be terminated accordingly.
uv_errors_e parser_writer_end(parser_writer_st *this);


/// @brief: Starts to write an object
///
/// @param name: The name of the object. Pass NULL or an empty string when
/// the object is written as a value of an array.
uv_errors_e parser_writer_begin_object(parser_writer_st *this, const char *name);

/// @brief: Ends a write of an object
uv_errors_e parser_writer_end_object(parser_writer_st *this);

/// @brief: Starts to write an array
///
/// @param name: The name of the array. Pass NULL or an empty string when
/// the array is written as a value of another array.
uv_errors_e parser_writer_begin_array(parser_writer_st *this, const char *name);

/// @brief: Ends a write of an array
uv_errors_e parser_writer_end_array(parser_writer_st *this);

/// @brief: Writes an integer key-value pair
uv_errors_e parser_writer_add_int(parser_writer_st *this, const char *name, int value);

/// @brief: As *parser_writer_add_int* except writes the value as hexadecimal
///
/// @note: *value* is considered as unsigned value
uv_errors_e parser_writer_add_int_hex(parser_writer_st *this,
		const char *name, uint32_t value);

/// @brief: Writes a string key-value pair
uv_errors_e parser_writer_add_string(parser_writer_st *this,
		const char *name, const char *value);

/// @brief: Writes a boolean key-value pair
uv_errors_e parser_writer_add_bool(parser_writer_st *this,
		const char *name, bool value);

/// @brief: Writes an integer value to the currently open array
uv_errors_e parser_writer_array_add_int(parser_writer_st *this, int value);

/// @brief: Writes a hexadecimal integer value to the currently open array
uv_errors_e parser_writer_array_add_int_hex(parser_writer_st *this, uint32_t value);

/// @brief: Writes a string value to the currently open array
uv_errors_e parser_writer_array_add_string(parser_writer_st *this, const char *value);

/// @brief: Writes a boolean value to the currently open array
uv_errors_e parser_writer_array_add_bool(parser_writer_st *this, bool value);


/// @brief: Appends a document which was written with a writer of it's own as
/// a value of the currently open array. This makes it possible to build a
/// single value separately and to discard it if the building failed.
///
/// @return: True on success, false if the destination buffer would overflow
///
/// @param data: A document written with a separate writer of the same format
bool parser_writer_array_append_doc(parser_writer_st *this, char *data);


/// @brief: Writes the document which was constructed to *buffer* into the
/// file at *path*. For JSON files the output is additionally pretty-printed
/// with jq, when it is available.
///
/// @return: true if the file could be written
bool parser_write_file(const char *path, const char *buffer, parser_formats_e format);


#endif /* PARSER_H_ */
