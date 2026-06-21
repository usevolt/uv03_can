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


#ifndef EXPORT_H_
#define EXPORT_H_

#include <stdbool.h>
#include <uv_utilities.h>
#include <uv_canopen.h>
#include "db.h"


typedef struct {

} export_st;



/// @brief: Database command provides uvcan with CANOpen device database file.
/// Also works as an initializer.
bool cmd_export(const char *arg);

/// @brief: Exports the header file only
bool cmd_exporth(const char *arg);

/// @brief: Exports the source file only
bool cmd_exportc(const char *arg);


#endif /* EXPORT_H_ */
