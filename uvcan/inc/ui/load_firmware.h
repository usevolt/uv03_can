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


#ifndef UI_LOAD_FIRMWARE_H_
#define UI_LOAD_FIRMWARE_H_


#include <uv_utilities.h>




struct _GObject;
typedef struct _GObject GObject;

struct _GtkWidget;
typedef struct _GtkWidget GtkWidget;

struct _GtkBuilder;
typedef struct _GtkBuilder GtkBuilder;


/// @brief: Flash firmware structure
typedef struct {
	GtkWidget *filechooser;
	GtkWidget *firmwarelog;
	char buffer[1024];
	FILE *fp;
} load_firmware_st;


void load_firmware_init(load_firmware_st *this, GtkBuilder *builder);


#endif /* UI_LOAD_FIRMWARE_H_ */
