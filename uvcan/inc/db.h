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


#ifndef DB_H_
#define DB_H_

#include <stdbool.h>
#include <uv_utilities.h>
#include <uv_canopen.h>


#define DB_OBJ_MAX_COUNT	256
#define DB_MAX_FILE_SIZE	65536


enum {
	DBVALUE_INT = 0,
	DBVALUE_STRING
};
typedef uint8_t dbvalue_type_e;

typedef struct {
	dbvalue_type_e type;
	int32_t value_int;
	char *value_str;
} dbvalue_st;

void dbvalue_init(dbvalue_st *this);

/// @brief: Sets the dbvalue to integer and returns the dbvalue object
dbvalue_st dbvalue_set_int(int32_t value);

/// @brief: Sets the dbvalue to string, allocates memory for the string and returns
/// a new dbvalue object. dbvalues should be free'd with dbvalue_free after
/// calling this
dbvalue_st dbvalue_set_string(char *str, uint32_t str_len);

static inline int32_t dbvalue_get_int(dbvalue_st *this) {
	return this->value_int;
}

/// @brief: Returns a pointer to the dbvalue's string component. In case if integer value
/// was given to the dbvalue, it was converted to 10-base string and a pointer to that
/// strin is returned. Otherwise pointer to empty string is returned.
static inline char *dbvalue_get_string(dbvalue_st *this) {
	return this->value_str;
}

static inline dbvalue_type_e dbvalue_get_type(dbvalue_st *this) {
	return this->type;
}

void dbvalue_free(dbvalue_st *this);


/// @brief: Data structure for each array object's children
typedef struct {
	char name[128];
	dbvalue_st min;
	dbvalue_st max;
	dbvalue_st def;
	// pointer to the next sibling
	void *next_sibling;
} db_array_child_st;

void db_array_child_init(db_array_child_st *this);



/// @brief: Defines the type of the database object. NONVOL_PARAM types
/// should be Read/write data stored in the non-volatile flash memory. These
/// can be read and written with *loadparam* & *saveparam* commands.
typedef enum {
	DB_OBJ_TYPE_UNDEFINED = 0,
	// nonvolatile data paramter that is read / written with saveparam / loadparam commands
	DB_OBJ_TYPE_NONVOL_PARAM,
	// The operator database request object. CANOPEN_ARRAY8 type parameter,
	// with load, delete & copy commands as subindexes
	DB_OBJ_TYPE_OPDB,
	// the operator count parameter
	DB_OBJ_TYPE_OP_COUNT,
	// the current operator parameter
	DB_OBJ_TYPE_CURRENT_OP,
	// EMCY STR entires
	DB_OBJ_TYPE_EMCY,
	DB_OBJ_TYPE_COUNT
} db_obj_type_e;

/// @brief: A single object structure
typedef struct {
	// Descriptive name of the object
	char name[128];
	// object structure holding the embedded parameters
	canopen_object_st obj;
	// data pointer as a string for embedded system
	char dataptr[128];
	// the object data type as a string. This holds information about signed/unsigned
	char type_str[32];
	// the object type. Undefined, nonvol_param or something else.
	db_obj_type_e obj_type;
	union {
		// for integer objects
		struct {
			union {
				// for read-only integer objects
				dbvalue_st value;
				// default (reset) value
				dbvalue_st def;
			};
			// minimum value for integer objects
			dbvalue_st min;
			// maximum value for integer objects
			dbvalue_st max;
		};
		// for array objects
		struct {
			// array object's children object pointer.
			// this points to dynamically allocated array of children.
			db_array_child_st *child_ptr;
			dbvalue_st array_max_size;
		};
		// for string objects
		struct {
			dbvalue_st string_len;
			char string_def[512];
		};
	};
} db_obj_st;


/// @brief: A single EMCY object
typedef struct {
	char name[128 - sizeof(int32_t)];
	char info_strs[8][128];
	int32_t value;
} db_emcy_st;

typedef enum {
	DB_DEFINE_INT = 0,
	DB_DEFINE_ENUM
} db_define_types_e;

/// @brief: Additional database defines have the same variables as emcy objects
typedef struct {
	char name[128 - sizeof(int32_t)];
	db_define_types_e type;
	union {
		int32_t value;
		struct {
			// null-terminated array of pointer to strings. Used for enum type defines
			char (*childs)[128];
			canopen_object_type_e data_type;
			int32_t child_count;
		};
	};
} db_define_st;

/// @brief: Database RXPDO structure
typedef struct {
	char cobid[128];
	canopen_pdo_transmission_types_e transmission;
	canopen_pdo_mapping_parameter_st mappings;
} db_rxpdo_st;


/// @brief: Database TXPDO structure
typedef struct {
	char cobid[128];
	canopen_pdo_transmission_types_e transmission;
	int32_t inhibit_time;
	uint32_t event_timer;
	canopen_pdo_mapping_parameter_st mappings;
} db_txpdo_st;

typedef struct {
	db_obj_st objects_buffer[DB_OBJ_MAX_COUNT];
	uv_vector_st objects;
	// node id of the current database
	uint8_t node_id;
	// name of dev which is used to create a pre-processor macros
	char dev_name[128];
	// the name of the device in uppercase
	char dev_name_upper[128];

	char filepath[128];

	db_emcy_st emcys_buffer[128];
	uv_vector_st emcys;
	uint16_t emcys_index;

	db_define_st defines_buffer[128];
	uv_vector_st defines;

	db_rxpdo_st rxpdo_buffer[128];
	uv_vector_st rxpdos;
	db_txpdo_st txpdo_buffer[128];
	uv_vector_st txpdos;

	uint32_t vendor_id;
	uint32_t product_code;
	uint32_t revision_number;

} db_st;

/// @brief: Database command provides uvcan with CANOpen device database file.
/// Also works as an initializer.
bool cmd_db(const char *arg);


bool db_is_loaded(db_st *this);

static inline int32_t db_get_object_count(db_st *this) {
	return uv_vector_size(&this->objects);
}


static inline char *db_get_file(db_st *this) {
	return this->filepath;
}


static inline db_obj_st *db_get_obj(db_st *this, uint32_t index) {
	return ((db_obj_st*) uv_vector_at(&this->objects, index));
}

static inline uint8_t db_get_nodeid(db_st *this) {
	return this->node_id;
}

static inline void db_set_nodeid(db_st *this, uint8_t value) {
	this->node_id = value;
}

static inline uint32_t db_get_vendor_id(db_st *this) {
	return this->vendor_id;
}

static inline uint32_t db_get_product_code(db_st *this) {
	return this->product_code;
}

static inline uint32_t db_get_revision_number(db_st *this) {
	return this->revision_number;
}

static inline char *db_get_dev_name(db_st *this) {
	return this->dev_name;
}

static inline uint16_t db_get_emcy_index(db_st *this) {
	return this->emcys_index;
}

static inline uint32_t db_get_emcy_count(db_st *this) {
	return uv_vector_size(&this->emcys);
}

static inline db_emcy_st *db_get_emcy(db_st *this, uint32_t index) {
	return ((db_emcy_st*) uv_vector_at(&this->emcys, index));
}

static inline uint32_t db_get_define_count(db_st *this) {
	return uv_vector_size(&this->defines);
}

static inline db_define_st *db_get_define(db_st *this, uint32_t index) {
	return (db_define_st*) uv_vector_at(&this->defines, index);
}

static inline uint16_t db_get_rxpdo_count(db_st *this) {
	return uv_vector_size(&this->rxpdos);
}

static inline db_rxpdo_st *db_get_rxpdo(db_st *this, uint16_t index) {
	return uv_vector_at(&this->rxpdos, index);
}

static inline uint16_t db_get_txpdo_count(db_st *this) {
	return uv_vector_size(&this->txpdos);
}

static inline db_txpdo_st *db_get_txpdo(db_st *this, uint16_t index) {
	return uv_vector_at(&this->txpdos, index);
}

void db_permission_to_str(canopen_permissions_e permissions, char *dest);
void db_permission_to_longstr(canopen_permissions_e permissions, char *dest);
void db_type_to_str(canopen_object_type_e type, char *dest);
void db_type_to_stdint(canopen_object_type_e type, char *dest);
void db_transmission_to_str(canopen_pdo_transmission_types_e transmission, char *dest);

/// @brief: Deinitializes the database and frees all allocated memory
void db_deinit(void);


#endif /* DB_H_ */
