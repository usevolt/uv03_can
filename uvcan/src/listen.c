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

#include "listen.h"
#include <stdio.h>
#include <string.h>
#include <uv_rtos.h>
#include <time.h>
#include "main.h"


#define this (&dev.listen)


void listen(void *);


bool cmd_listen(const char *arg) {
	bool ret = true;

	if (!arg) {
		this->time = 0xFFFFFFFF;
	}
	else {
		unsigned int value = strtol(arg, NULL, 0);
		printf("Listening on the CAN bus for %u seconds.\n", value);
		this->time = value * 1000;
	}

	add_task(&listen);

	return ret;
}



void can_callb(void *ptr, uv_can_msg_st *msg) {
	const char *type;
	if (msg->type == CAN_STD) {
		type = "STD";
	}
	else if (msg->type == CAN_EXT) {
		type = "EXT";
	}
	else {
		type = "ERR";
	}
	struct tm *time;
	struct timeval timev = uv_can_get_rx_time();
	time = localtime(&timev.tv_sec);

	printf("%02u:%02u:%02u:%03u ", time->tm_hour, time->tm_min, time->tm_sec, timev.tv_usec / 1000);

	printf("%s ID: 0x%x, DLC: %u, ", type, msg->id, msg->data_length);
	for (uint8_t i = 0; i < msg->data_length; i++) {
		printf("0x%02x ", msg->data_8bit[i]);
	}
	printf("\n");
}


void listen(void *ptr) {
	uint64_t time = 0;
	unsigned step_ms = 20;

	uv_canopen_set_can_callback(&can_callb);


	while (true) {
		// loop for waiting the specified time

		time += step_ms;

		if (time > this->time) {
			break;
		}
		uv_rtos_task_delay(200);


	}
}
