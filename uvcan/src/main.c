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




void step(void *me) {

	while (true) {
		printf("hephep\n");
		uv_rtos_task_delay(100);
	}
}

int main(int argc, char *argv[]) {

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

	char c = 'c';
	while ((c = getopt_long(argc, argv, "", opts, NULL)) != -1) {
		if (c != '?') {
			if (!commands[(unsigned int) c].callback(optarg)) {
				printf("command '%s' returned with FALSE\n", commands[(unsigned int) c].cmd);
			}
		}
	}


//	uv_rtos_start_scheduler();

	return EXIT_SUCCESS;
}
