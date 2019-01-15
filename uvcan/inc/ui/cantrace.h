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


#ifndef UI_CANTRACE_H_
#define UI_CANTRACE_H_

#include <uv_utilities.h>
#include <uv_rtos.h>
#include <uv_can.h>


#define CANTRACE_BUFFER_LEN		128
#define CANTRACE_CHILDREN_COUNT	150

struct _GObject;
typedef struct _GObject GObject;

struct _GtkWidget;
typedef struct _GtkWidget GtkWidget;

typedef struct _GtkBuilder GtkBuilder;

typedef struct {
	char id_str[32];
	char data_str[32];
	char type_str[8];
	char time_str[32];
	char dlc_str[8];
} cantrace_msg_st;

/// @brief: initializes the cantrace_msg structure from CAN message
void cantrace_msg_init(cantrace_msg_st *this, uv_can_msg_st *msg);



typedef struct {
	GtkWidget *traceview;
	uv_can_msg_st msg_buffer[CANTRACE_BUFFER_LEN];
	uv_ring_buffer_st msgs;
	int32_t children_count;

	uv_mutex_st mutex;
} cantrace_st;


void cantrace_init(cantrace_st *this, GtkBuilder *builder);

void cantrace_deinit(cantrace_st *this);

void cantrace_step(cantrace_st *this, uint16_t step_ms);

void cantrace_rx(cantrace_st *this, uv_can_msg_st *msg);

#endif /* UI_CANTRACE_H_ */
