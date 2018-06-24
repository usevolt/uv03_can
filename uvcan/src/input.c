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


#include "input.h"
#include <stdio.h>
#include <string.h>
#include <uv_terminal.h>
#include <ncurses.h>
#include "main.h"


#define this (&dev)

static void input_step(void *ptr);


bool cmd_input(const char *arg) {
	bool ret = true;

	uint16_t id = strtol(arg, NULL, 0);
	if (id >= (1 << 11)) {
		printf("11-bit identifier only supported\n");
		ret = false;
	}
	else {
		initscr();
		timeout(-1);

		this->cmd_input.id = id;
		printw("Input CAN ID set to 0x%x\n", this->cmd_input.id);
		add_task(&input_step);
		ret = true;
	}


	return ret;
}



static void input_step(void *ptr) {
	while (true) {
		uint8_t step_ms = 50;

		char c = (char) getch();
		if (c != EOF) {
			uv_can_msg_st msg;
			msg.type = CAN_STD;
			msg.id = this->cmd_input.id;
			msg.data_length = 1;
			msg.data_8bit[0] = c;
			uv_can_send(this->can_channel, &msg);
		}

		uv_rtos_task_delay(step_ms);
	}
}

