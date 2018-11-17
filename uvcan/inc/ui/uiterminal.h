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


#ifndef UI_UITERMINAL_H_
#define UI_UITERMINAL_H_

#include <uv_utilities.h>
#include <uv_rtos.h>
#include <uv_can.h>


struct _GObject;
typedef struct _GObject GObject;

struct _GtkWidget;
typedef struct _GtkWidget GtkWidget;

typedef struct _GtkBuilder GtkBuilder;

typedef struct {
	GtkWidget *entry;
	GtkWidget *terminal;
	uv_mutex_st mutex;
} terminal_st;


void terminal_init(terminal_st *this, GtkBuilder *builder);

void terminal_step(terminal_st *this, uint16_t step_ms);

void terminal_can_rx(terminal_st *this, uv_can_msg_st *msg);

#endif /* UI_UITERMINAL_H_ */
