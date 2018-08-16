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


#include <stdio.h>
#include <string.h>
#include <uv_terminal.h>
#include "terminal.h"
#include "main.h"

#define this (&dev)


static void command_step(void *ptr);
static void command_tx(void *ptr);

bool cmd_terminal(const char *arg) {

	add_task(&command_step);

	uv_rtos_task_create(&command_tx, "tx",
			UV_RTOS_MIN_STACK_SIZE, NULL, UV_RTOS_IDLE_PRIORITY, NULL);

	return true;
}


static char rx_buffer[1024];
static uv_ring_buffer_st rx;


static void can_callb(void *ptr, uv_can_message_st *msg) {
	if ((msg->type == CAN_STD) &&
			(msg->id == (CANOPEN_SDO_RESPONSE_ID + this->nodeid)) &&
			(msg->data_8bit[0] == 0x42) &&
			(msg->data_length > 4) &&
			(msg->data_8bit[1] == UV_TERMINAL_CAN_INDEX % 256) &&
			(msg->data_8bit[2] == UV_TERMINAL_CAN_INDEX / 256) &&
			(msg->data_8bit[3] == UV_TERMINAL_CAN_SUBINDEX)) {
		for (int i = 4; i < msg->data_length; i++) {
			uv_ring_buffer_push(&rx, &msg->data_8bit[i]);
		}
	}
}


static void command_tx(void *ptr) {
	while (true) {
		char str[256];
		int ret = scanf(" %s", str);
		if (ret != EOF) {

			strcat(str, "\n");

			uint8_t len = 0;
			uv_can_msg_st msg;
			msg.type = CAN_STD;
			msg.id = UV_TERMINAL_CAN_RX_ID + this->nodeid;
			msg.data_8bit[0] = 0x22;
			msg.data_8bit[1] = UV_TERMINAL_CAN_INDEX % 256;
			msg.data_8bit[2] = UV_TERMINAL_CAN_INDEX / 256;
			msg.data_8bit[3] = UV_TERMINAL_CAN_SUBINDEX;
			for (int i = 0; i < strlen(str); i++) {
				msg.data_8bit[4 + len++] = str[i];
				if (len == 4) {
					msg.data_length = 8;
					uv_can_send(this->can_channel, &msg);
					len = 0;
				}
			}
			if (len != 0) {
				msg.data_length = 4 + len;
				uv_can_send(this->can_channel, &msg);
			}
		}

		uv_rtos_task_delay(20);
	}
}


static void command_step(void *ptr) {
	printf("Terminal opened for node ID 0x%x\n", this->nodeid);
	uv_canopen_set_can_callback(&can_callb);
	uv_ring_buffer_init(&rx, rx_buffer,
			sizeof(rx_buffer) / sizeof(rx_buffer[0]), sizeof(rx_buffer[0]));

	while (true) {

		char c;

		while (uv_ring_buffer_pop(&rx, &c) == ERR_NONE) {
			printf("%c", c);
		}

		uv_rtos_task_delay(20);
	}
}
