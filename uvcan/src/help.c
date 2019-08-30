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


#include "help.h"
#include "commands.h"
#include <stdio.h>


bool cmd_help(const char *arg) {
	printf( "*****************************\n"
			"Usevolt CAN command line tool\n"
			"*****************************\n"
			"\n\nCommands:\n");
	for (int i = 0; i < commands_count(); i++) {
		if (commands[i].cmd_short >= 'a') {
			printf("--%s -%c: %s\n\n", commands[i].cmd_long, commands[i].cmd_short, commands[i].str);
		}
		else {
			printf("--%s: %s\n\n", commands[i].cmd_long, commands[i].str);
		}
	}

	return true;
}
