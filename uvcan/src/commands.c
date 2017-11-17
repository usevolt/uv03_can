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



#include "commands.h"
#include "main.h"
#include "help.h"
#include "load.h"

#define this (&dev)


bool cmd_can(const char *arg);
bool cmd_baud(const char *arg);
bool cmd_node(const char *arg);


commands_st commands[] = {
		{
				.cmd = "help",
				.str = "Displays application info and help.",
				.callback = &cmd_help
		},
		{
				.cmd = "can",
				.str = "Selects the CAN-USB hardware for communication. Defaults to can0.",
				.callback = &cmd_can
		},
		{
				.cmd = "baud",
				.str = "Sets the baudrate for the CAN-bus. Refer to CiA specification for valid values. "
						"Defaults to 250 kbaud.",
				.callback = &cmd_baud
		},
		{
				.cmd = "node",
				.str = "Selecs the CANopen Node via Node ID. This should be called prior to commands which "
						"Operate on CANopen nodes, such as *load*.",
				.callback = &cmd_node
		},
		{
				.cmd = "loadbin",
				.str = "Loads firmware to UV device. "
						"The device node id should be selected with 'node' option prior to this command.",
				.callback = &cmd_load
		}
};

unsigned int commands_count(void) {
	return sizeof(commands) / sizeof(commands[0]);
}



bool cmd_can(const char *arg) {
	printf("selecting '%s' as CAN dev\n", arg);
	this->can_dev = arg;

	return true;
}

bool cmd_baud(const char *arg) {
	bool ret = false;
	if (!arg) {
		printf("Bad baudrate\n");
	}
	else {
		unsigned int baudrate = strtol(arg, NULL, 0);
		printf("Setting CAN baudrate: %u\n", baudrate);
		this->baudrate = baudrate;
		ret = true;
	}

	return ret;
}

bool cmd_node(const char *arg) {
	bool ret = true;
	if (!arg) {
		printf("Give Node ID.\n");
		ret = false;
	}
	else {
		uint8_t nodeid = strtol(arg, NULL, 0);
		printf("Selected Node ID 0x%x\n", nodeid);
		this->nodeid = nodeid;
	}

	return ret;
}

