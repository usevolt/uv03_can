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



#include <export.h>
#include <stdio.h>
#include <string.h>
#include <uv_terminal.h>
#include "main.h"


static void step(void *ptr);


#define this (&dev)


bool cmd_export(const char *arg) {
	bool ret = false;

	uv_rtos_add_idle_task(&step);

	ret = true;

	return ret;
}



static void step(void *ptr) {
	printf("hephep\n");
}
