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

/// @brief: A single object structure
typedef struct {
	// Descriptive name of the object
	char name[128];
	// object structure holding the embeded parameters
	canopen_object_st obj;
	// data pointer as a string for embedded system
	char data[128];
	// minimum value for integer objects
	int32_t min;
	// maximum value for integer objects
	int32_t max;
	// default (reset) value
	int32_t def;
} db_obj_st;



typedef struct {
	db_obj_st objects_buffer[DB_OBJ_MAX_COUNT];
	uv_vector_st objects;
	// node id of the current database
	uint8_t node_id;

} db_st;

/// @brief: Database command provides uvcan with CANOpen device database file.
/// Also works as an initializer.
bool cmd_db(const char *arg);


static inline int32_t db_get_object_count(db_st *this) {
	return uv_vector_size(&this->objects);
}


static inline db_obj_st *db_get_obj(db_st *this, uint32_t index) {
	return ((db_obj_st*) uv_vector_at(&this->objects, index));
}


#endif /* DB_H_ */
