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


#ifndef UI_H_
#define UI_H_

#include <stdbool.h>
#include <uv_utilities.h>
#include <uv_canopen.h>
#include "db.h"
#include "obj_dict.h"
#include "load_firmware.h"
#include "uiterminal.h"
#include "cantrace.h"

#define UI_KNOWN_NODEID_COUNT			64

struct _GObject;
typedef struct _GObject GObject;

struct _GtkWidget;
typedef struct _GtkWidget GtkWidget;


typedef struct {
	GtkWidget *window;
	GObject *stackswitcher;
	GObject *stack;
	GObject *can_dev;
	GObject *can_baudrate;
	GObject *can_switch;
	GObject *db;

	uv_mutex_st mutex;

	uint8_t nodeid_buffer[UI_KNOWN_NODEID_COUNT];
	uv_vector_st nodeids;

	cantrace_st cantrace;
	obj_dict_st obj_dict;
	load_firmware_st load_firmware;
	terminal_st terminal;
} ui_st;



/// @brief: Database command provides uvcan with CANOpen device database file.
/// Also works as an initializer.
bool cmd_ui(const char *arg);


static inline uint16_t ui_get_nodeid_count(ui_st *this) {
	return uv_vector_size(&this->nodeids);
}

static inline uint8_t ui_get_nodeid(ui_st *this, uint16_t index) {
	uint8_t *value = uv_vector_at(&this->nodeids, index);
	return *value;
}


void uican_callb(void *ptr, uv_can_message_st *msg);



#endif /* UI_H_ */
