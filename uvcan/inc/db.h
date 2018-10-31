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


#define DB_OBJ_MAX_COUNT	128
#define DB_MAX_FILE_SIZE	65536


/// @brief: Data structure for each array object's children
typedef struct {
	char name[128];
	int32_t min;
	int32_t max;
	int32_t def;
	// pointer to the next sibling
	void *next_sibling;
} db_array_child_st;

/// @brief: A single object structure
typedef struct {
	// Descriptive name of the object
	char name[128];
	// object structure holding the embedded parameters
	canopen_object_st obj;
	// data pointer as a string for embedded system
	char dataptr[128];
	union {
		struct {
			// minimum value for integer objects
			int32_t min;
			// maximum value for integer objects
			int32_t max;
			// default (reset) value
			int32_t def;
		};
		// array object's children object pointer.
		// this points to dynamically allocated array of children.
		db_array_child_st *child_ptr;
		// string type parameters
		struct {
			char string_def[512];
		};
	};
} db_obj_st;


/// @brief: A single EMCY object
typedef struct {
	char name[128 - sizeof(int32_t)];
	int32_t value;
} db_emcy_st;

/// @brief: Additional database defines have the same variables as emcy objects
typedef db_emcy_st db_define_st;


typedef struct {
	db_obj_st objects_buffer[DB_OBJ_MAX_COUNT];
	uv_vector_st objects;
	// node id of the current database
	uint8_t node_id;
	// name of dev which is used to create a pre-processor macros
	char dev_name[128];

	char filepath[128];

	db_emcy_st emcys_buffer[128];
	uv_vector_st emcys;

	db_define_st defines_buffer[128];
	uv_vector_st defines;

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

static inline char *db_get_dev_name(db_st *this) {
	return this->dev_name;
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

void db_permission_to_str(canopen_permissions_e permissions, char *dest);
void db_permission_to_longstr(canopen_permissions_e permissions, char *dest);
void db_type_to_str(canopen_object_type_e type, char *dest);

/// @brief: Deinitializes the database and frees all allocated memory
void db_deinit(void);


#endif /* DB_H_ */
