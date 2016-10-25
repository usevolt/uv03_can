/*
 ============================================================================
 Name        : uv_can.c
 Author      : 
 Version     :
 Copyright   : Your copyright notice
 Description : Hello World in C, Ansi-style
 ============================================================================
 */

#include "main.h"
#include "commands.h"
#include <stdio.h>
#include <stdlib.h>
#include <ncurses.h>

#define this ((app_st*)&app)

static app_st app = {
	.can_device = {}
};


int main(int argc, char **argv) {

	// try to open the default connection
	if (can_open(CAN_DEFAULT_DEV, CAN_DEFAULT_BAUDRATE, this)) {
		ui_errf("Session %s open with baudrate %u\n", CAN_DEFAULT_DEV, CAN_DEFAULT_BAUDRATE);
	}
	else {
		ui_errf("Error opening the device\n");
	}


	int len = 256;
	char line[len];

	while(true) {
		fgets(line, len, stdin);

		parse_command(&app, line);

	}

	return EXIT_SUCCESS;
}



