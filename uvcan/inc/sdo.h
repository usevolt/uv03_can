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


#ifndef SDO_H_
#define SDO_H_


#include <stdbool.h>
#include <stdint.h>


typedef struct {
	uint16_t mindex;
	uint8_t sindex;
	uint32_t datalen;
	int32_t value;
} sdo_st;


bool cmd_mindex(const char *arg);
bool cmd_sindex(const char *arg);
bool cmd_datalen(const char *arg);

bool cmd_sdoread(const char *arg);
bool cmd_sdowrite(const char *arg);



#endif /* SDO_H_ */
