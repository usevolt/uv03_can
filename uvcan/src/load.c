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



#include <uv_rtos.h>
#include "load.h"
#include "main.h"

#define this (&dev)


void load_step(void *dev);



bool cmd_load(const char *arg) {
	bool ret = true;

	printf("Firmware %s selected\n", arg, this->nodeid);
	add_task(load_step);

	return ret;
}


void load_step(void *dev) {
	while (true) {

		printf("loading firmware...\n");

		uv_rtos_task_delay(1000);
		break;
	}
}
