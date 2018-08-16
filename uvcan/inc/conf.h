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


#ifndef CONF_H_
#define CONF_H_

#include <stdbool.h>
#include <uv_utilities.h>
#include <uv_canopen.h>


#define CONF_OBJ_MAX_COUNT	128
#define CONF_MAX_FILE_SIZE	65536

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
} conf_obj_st;



typedef struct {
	conf_obj_st objects_buffer[CONF_OBJ_MAX_COUNT];
	uv_vector_st objects;

} conf_st;

/// @brief: Database command provides uvcan with CANOpen device database file.
/// Also works as an initializer.
bool cmd_conf(const char *arg);




#endif /* CONF_H_ */
