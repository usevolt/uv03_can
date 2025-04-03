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


#ifndef LOADMEDIA_H_
#define LOADMEDIA_H_


#include <uv_utilities.h>
#include <stdbool.h>



typedef struct {
} loadmedia_st;



/// @brief: Loads the media file with a uv media protocol
bool cmd_loadmedia(const char *arg);


#endif /* LOADMEDIA_H_ */
