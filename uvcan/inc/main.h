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



#define TASKS_LEN	5
typedef struct {
	uv_mutex_st mutex;
	void (*step)(void*);
} task_st;

struct _dev_st {

	/// @brief: Selects the CAN hardware to be used
	const char *can_dev;
	/// @brief: CAN-bus baudrate
	unsigned int baudrate;

	/// @brief: CANopen Node ID of the selected device
	uint8_t nodeid;

	/// @brief: operating tasks of the application. Commands can
	/// register their tasks via *add_task* function.
	/// The commands get execution order each in turn.
	task_st task_buffer[TASKS_LEN];
	uv_vector_st tasks;

	uv_data_start_t data_start;

	uv_data_end_t data_end;
};

extern struct _dev_st dev;


/// @brief: Registers a task
void add_task(void (*step_callback)(void*));

#endif /* MAIN_H_ */
