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


#ifndef LISTEN_H_
#define LISTEN_H_


#include <stdbool.h>
#include <stdint.h>


typedef struct {
	uint32_t time;
} listen_st;



static inline int32_t listen_get_time(listen_st *this) {
	return this->time;
}

bool cmd_listen(const char *arg);


#endif /* LISTEN_H_ */
