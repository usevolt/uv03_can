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


#include <stdio.h>
#include <unistd.h>
#include <uv_memory.h>
#include <uv_rtos.h>
#include <uv_utilities.h>
#include <uv_canopen.h>


/// @brief: Interactive prompt output to stderr so it is visible
/// even when stdout is redirected to a file.
#define PROMPT(fmt, ...) do { \
	fprintf(stderr, PRINT_BOLD fmt PRINT_RESET, ##__VA_ARGS__); \
	fflush(stderr); \
} while (0)
#define PROMPTSTR(str) do { \
	fprintf(stderr, PRINT_BOLD str PRINT_RESET); \
	fflush(stderr); \
} while (0)


/// @brief: Progress log that overwrites the same terminal line.
/// On a terminal: uses \\r and erase-to-EOL so successive calls
/// overwrite each other. When output is redirected (pipe/file),
/// prints a normal line instead.
#define LOG(fmt, ...) do { \
	if (isatty(STDOUT_FILENO)) { \
		printf("\r" fmt "\033[K", ##__VA_ARGS__); \
	} \
	else { \
		printf(fmt "\n", ##__VA_ARGS__); \
	} \
	fflush(stdout); \
} while (0)

/// @brief: Finalize the current LOG line so that subsequent output
/// (errors, warnings, final messages) starts on a fresh line.
#define LOG_END() do { \
	if (isatty(STDOUT_FILENO)) { \
		printf("\n"); \
	} \
} while (0)
#include "db.h"
#include "listen.h"
#include "load.h"
#include "export.h"
#include "loadmedia.h"
#include "sdo.h"
#include "loadparam.h"
#include "saveparam.h"



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
	loadmedia_st loadmedia;
	sdo_st sdo;
	loadparam_st loadparam;
	saveparam_st saveparam;

	uv_data_start_t data_start;

	uv_data_end_t data_end;

	char **nonopt_argv;
	uint32_t argv_count;

};




extern const canopen_object_st obj_dict[];

unsigned int obj_dict_len(void);

/// @brief: Registers a task
void add_task(void (*step_callback)(void*));

#endif /* MAIN_H_ */
