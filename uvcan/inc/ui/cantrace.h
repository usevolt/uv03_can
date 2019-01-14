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
#define CANTRACE_MSG_STR_LEN	64
#define CANTRACE_CHILDREN_COUNT	150

struct _GObject;
typedef struct _GObject GObject;

struct _GtkWidget;
typedef struct _GtkWidget GtkWidget;

typedef struct _GtkBuilder GtkBuilder;

typedef struct {
	char id_str[CANTRACE_MSG_STR_LEN];
	char data_str[CANTRACE_MSG_STR_LEN];
	char type_str[CANTRACE_MSG_STR_LEN];
	char time_str[CANTRACE_MSG_STR_LEN];
	char dlc_str[8];
	void *next_sibling;
	void *previous_sibling;
} cantrace_msg_st;

/// @brief: initializes the cantrace_msg structure from CAN message
cantrace_msg_st *cantrace_msg_new(uv_can_msg_st *msg);

static inline void cantrace_msg_set_next_sibling(cantrace_msg_st *this, void *sibling) {
	this->next_sibling = sibling;
}
static inline void cantrace_msg_set_previous_sibling(cantrace_msg_st *this, void *sibling) {
	this->previous_sibling = sibling;
}
static inline void *cantrace_msg_get_next_sibling(cantrace_msg_st *this) {
	return this->previous_sibling;
}

/// @brief: Frees the memory allocated to the cantrace message and all its next siblings
void cantrace_msg_free(cantrace_msg_st *this);


typedef struct {
	GtkWidget *traceview;
	uv_can_msg_st msg_buffer[CANTRACE_BUFFER_LEN];
	uv_ring_buffer_st msgs;


	// count of the trace messages on the screen
	uint32_t children_count;
	// linked list of the trace message content. This points to the newest child, e.g. last one
	cantrace_msg_st *last_child;
	// this points to the olders child.
	cantrace_msg_st *first_child;
	uv_mutex_st mutex;
} cantrace_st;


void cantrace_init(cantrace_st *this, GtkBuilder *builder);

void cantrace_deinit(cantrace_st *this);

void cantrace_step(cantrace_st *this, uint16_t step_ms);

void cantrace_rx(cantrace_st *this, uv_can_msg_st *msg);

#endif /* UI_CANTRACE_H_ */
