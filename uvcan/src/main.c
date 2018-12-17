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



const canopen_object_st obj_dict[] = {

};

unsigned int obj_dict_len(void) {
	return (sizeof(obj_dict) / sizeof(canopen_object_st));
}


void init(void *me) {
	// initialize the default settings
	strcpy(this->can_channel, "can0");
	char cmd[128];
#if CONFIG_TARGET_LINUX
	// get the net dev baudrate. If dev was not available, baudrate will be 0.
	sprintf(cmd, "ip -det link show %s | grep bitrate | awk '{print $2}'", this->can_channel);
	FILE *fp = popen(cmd, "r");
	if (fgets(cmd, sizeof(cmd), fp)) {
		this->baudrate = strtol(cmd, NULL, 0);
	}
#endif
	// if baudrate was not set on the device, initialize it to 250000
	if (this->baudrate == 0) {
		this->baudrate = 250000;
	}
	strcpy(this->srcdest, ".");
	strcpy(this->incdest, ".");

	uv_can_set_baudrate(this->can_channel, this->baudrate);
	uv_vector_init(&this->tasks, this->task_buffer, TASKS_LEN, sizeof(task_st));
}


/// @brief: Function which is registered as FreeRTOS task function for each task.
void task_step(void *ptr) {
	task_st *task = (task_st*) ptr;

	// wait until mutex is locked
	uv_mutex_lock(&task->mutex);
	// this task has now execution order
	task->step(&dev);
	// task finished, unlock mutex
	uv_mutex_unlock(&task->mutex);
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

	// cycle trough tasks and give each of them execution turn
	for (int i = 0; i < uv_vector_size(&this->tasks); i++) {
		uv_mutex_unlock(&((task_st*) uv_vector_at(&this->tasks, i))->mutex);
		uv_rtos_task_delay(1);
		// wait until task mutex can be locked again, task should now be finished
		uv_mutex_lock(&((task_st*) uv_vector_at(&this->tasks, i))->mutex);
	}


	uv_deinit();

	if (!uv_rtos_idle_task_set()) {
		db_deinit();
		printf("Finished\n");
		exit(0);
	}
}


int main(int argc, char *argv[]) {

	init(this);

	struct option opts[50];
	char optstr[256] = "";
	int i;
	for (i = 0; i < commands_count(); i++) {
		opts[i].name = commands[i].cmd;
		opts[i].has_arg = commands[i].args;
		opts[i].flag = 0;
		opts[i].val = i;
		char str[2];
		str[0] = (char) opts[i].val;
		str[1] = '\0';
		strcat(optstr, str);
		if (opts[i].has_arg == ARG_REQUIRE) {
			strcat(optstr, ":");
		}
		else if (opts[i].has_arg == ARG_OPTIONAL) {
			strcat(optstr, "::");
		}
		else {

		}
	}
	opts[i + 1].name = NULL;
	opts[i + 1].has_arg = 0;
	opts[i + 1].flag = NULL;
	opts[i + 1].val = 0;

	bool error = false;
	char c = 'c';
	bool none_args = true;
	while ((c = getopt_long(argc, argv, "", opts, NULL)) != -1) {
		if (c != '?') {
			// execute command callback
			if (!commands[(unsigned int) c].callback(optarg)) {
				printf("command '%s' returned with FALSE, terminating.\n",
						commands[(unsigned int) c].cmd);
				error = true;
				break;
			}
			none_args = false;
		}
	}
	// if none arguments were given,
	// uvcan starts with --ui command
	if (none_args) {
		cmd_ui("");
	}

	if (!error) {
		// register an own FreeRTOS task for all application tasks
		for (int i = 0; i < uv_vector_size(&this->tasks); i++) {
			uv_rtos_task_create(&task_step, "task", UV_RTOS_MIN_STACK_SIZE * 5,
					uv_vector_at(&this->tasks, i), UV_RTOS_IDLE_PRIORITY + 1, NULL);
		}
		// register main step task
		uv_rtos_task_create(&step, "Step", UV_RTOS_MIN_STACK_SIZE, this,
				UV_RTOS_IDLE_PRIORITY + 1, NULL);

		uv_init(&dev);

		uv_rtos_start_scheduler();
	}

	return EXIT_SUCCESS;
}
