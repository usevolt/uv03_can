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


#ifndef MAIN_H_
#define MAIN_H_


#include <uv_memory.h>
#include <uv_rtos.h>
#include <uv_utilities.h>
#include <uv_canopen.h>
#include "db.h"
#include "listen.h"
#include "load.h"
#include "export.h"
#include "ui.h"
#include "loadmedia.h"



#define TASKS_LEN	5
typedef struct {
	uv_mutex_st mutex;
	void (*step)(void*);
} task_st;



struct _dev_st {


	/// @brief CAN netdev name
	char can_channel[64];

	unsigned int baudrate;

	char srcdest[1024];
	char incdest[1024];

	/// @brief: operating tasks of the application. Commands can
	/// register their tasks via *add_task* function.
	/// The commands get execution order each in turn.
	task_st task_buffer[TASKS_LEN];
	uv_vector_st tasks;

	// ** modules **
	load_st load;
	listen_st listen;
	db_st db;
	export_st export;
	ui_st ui;
	loadmedia_st loadmedia;

	uv_data_start_t data_start;

	uv_data_end_t data_end;
};


extern struct _dev_st dev;


extern const canopen_object_st obj_dict[];

unsigned int obj_dict_len(void);

/// @brief: Registers a task
void add_task(void (*step_callback)(void*));

#endif /* MAIN_H_ */
