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
#include "loadmedia.h"
#include "main.h"


static void clearmedia_step(void *dev);





bool cmd_clearmedia(const char *arg) {
	bool ret = true;

	add_task(clearmedia_step);
	uv_can_set_up();

	return ret;
}





void clearmedia_step(void *ptr) {
	CANOPEN_TYPEOF(CONFIG_CANOPEN_EXMEM_CLEARREQ_TYPE) clear = 1;
	if (uv_canopen_sdo_write(db_get_nodeid(&dev.db), CONFIG_CANOPEN_EXMEM_CLEARREQ_INDEX,
			0, CANOPEN_SIZEOF(CONFIG_CANOPEN_EXMEM_CLEARREQ_TYPE), &clear) == ERR_NONE) {
		while (true) {
			if (uv_canopen_sdo_read(db_get_nodeid(&dev.db), CONFIG_CANOPEN_EXMEM_CLEARREQ_INDEX,
					0, CANOPEN_SIZEOF(CONFIG_CANOPEN_EXMEM_CLEARREQ_TYPE), &clear) == ERR_NONE) {
				if (clear == 0) {
					break;
				}
				else {
					printf(".");
					fflush(stdout);
				}
			}
			else {
				printf("Error while clearing media.\n");
				return;
			}
			uv_rtos_task_delay(1000);
		}
		printf("Media cleared from node 0x%x\n", db_get_nodeid(&dev.db));
		fflush(stdout);
	}
	else {
		printf("Couldn't clear media from node 0x%x\n", db_get_nodeid(&dev.db));
		fflush(stdout);
	}
}
