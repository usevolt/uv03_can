/*
 ============================================================================
 Name        : uvcan.c
 Author      : usevolt
 Version     :
 Copyright   : Your copyright notice
 Description : Hello World in C, Ansi-style
 ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <uv_rtos.h>
#include "main.h"
#include "help.h"
#include "commands.h"

struct _dev_st dev;
#define this (&dev)


void init(void *me) {
	// initialize the default settings
	this->can_dev = "can0";
	this->baudrate = 250000;
	uv_vector_init(&this->tasks, this->task_buffer, TASKS_LEN, sizeof(task_st));
}

/// @brief: Function which is registered as FreeRTOS task function for each task.
void task_step(void *ptr) {
	task_st *task = (task_st*) ptr;

	// wait until mutex is locked
	uv_mutex_lock(&task->mutex);
	printf("task starting\n");
	// this task has now execution order
	task->step(&dev);
	// task finished, unlock mutex
	uv_mutex_unlock(&task->mutex);
	printf("task finished\n");
	vTaskDelete(NULL);
}



void add_task(void (*step_callback)(void*)) {
	task_st task;
	// create and lock mutex for this task
	uv_mutex_init(&task.mutex);
	uv_mutex_lock(&task.mutex);
	task.step = step_callback;
	uv_vector_push_back(&this->tasks, &task);
}


void step(void *me) {
	uv_init(&dev);

	// cycle trough tasks and give each of them execution turn
	for (int i = 0; i < uv_vector_size(&this->tasks); i++) {
		uv_mutex_unlock(&((task_st*) uv_vector_at(&this->tasks, i))->mutex);
		uv_rtos_task_delay(1);
		// wait until task mutex can be locked again, task should now be finished
		uv_mutex_lock(&((task_st*) uv_vector_at(&this->tasks, i))->mutex);
	}

	exit(0);
}


int main(int argc, char *argv[]) {

	init(this);

	struct option opts[50];
	int i;
	for (i = 0; i < commands_count(); i++) {
		opts[i].name = commands[i].cmd;
		opts[i].has_arg = optional_argument;
		opts[i].flag = 0;
		opts[i].val = i;
	}
	opts[i + 1].name = NULL;
	opts[i + 1].has_arg = 0;
	opts[i + 1].flag = NULL;
	opts[i + 1].val = 0;

	bool error = false;
	char c = 'c';
	while ((c = getopt_long(argc, argv, "", opts, NULL)) != -1) {
		if (c != '?') {
			// execute command callback
			if (!commands[(unsigned int) c].callback(optarg)) {
				printf("command '%s' returned with FALSE, terminating.\n",
						commands[(unsigned int) c].cmd);
				error = true;
				break;
			}
		}
	}

	if (!error) {
		// register an own FreeRTOS task for all application tasks
		for (int i = 0; i < uv_vector_size(&this->tasks); i++) {
			uv_rtos_task_create(&task_step, "task", UV_RTOS_MIN_STACK_SIZE * 2,
					uv_vector_at(&this->tasks, i), UV_RTOS_IDLE_PRIORITY + 1, NULL);
		}
		// register main step task
		uv_rtos_task_create(&step, "Step", UV_RTOS_MIN_STACK_SIZE, this,
				UV_RTOS_IDLE_PRIORITY + 1, NULL);
		uv_rtos_start_scheduler();
	}

	return EXIT_SUCCESS;
}
