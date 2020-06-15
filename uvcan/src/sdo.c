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

#include "sdo.h"
#include <stdio.h>
#include <string.h>
#include <uv_rtos.h>
#include <time.h>
#include "main.h"


#define this (&dev.sdo)


void sdowrite(void *ptr);
void sdoread(void *ptr);



bool cmd_mindex(const char *arg) {
	bool ret = true;
	if (!arg) {
		printf("Give Main index.\n");
		ret = false;
	}
	else {
		this->mindex = strtol(arg, NULL, 0);
	}
	return ret;
}

bool cmd_sindex(const char *arg) {
	bool ret = true;
	if (!arg) {
		printf("Give Sub index.\n");
		ret = false;
	}
	else {
		this->sindex = strtol(arg, NULL, 0);
	}
	return ret;
}

bool cmd_datalen(const char *arg) {
	bool ret = true;
	if (!arg) {
		printf("Give Data length\n");
		ret = false;
	}
	else {
		this->datalen = strtol(arg, NULL, 0);
	}
	return ret;
}


bool cmd_sdoread(const char *arg) {
	bool ret = true;

	uv_can_set_up();

	add_task(&sdoread);

	return ret;
}


bool cmd_sdowrite(const char *arg) {
	bool ret = true;

	uv_can_set_up();

	add_task(&sdowrite);

	this->value = strtol(arg, NULL, 0);

	return ret;
}



void sdoread(void *ptr) {

	if (this->mindex == 0) {
		printf("Error: The CANOpen main index has to be set with *--mindex* command\n");
	}
	else {
		uint8_t *databuffer = malloc((this->datalen == 0) ? 4 : this->datalen);
		uv_errors_e e = uv_canopen_sdo_read(db_get_nodeid(&dev.db),
				this->mindex, this->sindex, (this->datalen == 0) ? 4 : this->datalen,
						databuffer);
		if (e != ERR_NONE) {
			printf("SDO read returned an error: %u (0x%x)\n", e, uv_canopen_sdo_get_error());
		}
		else {
			if (this->datalen <= 4) {
				int32_t val = 0;
				memcpy(&val, databuffer, (this->datalen == 0) ? 4 : this->datalen);
				printf("0x%x\n", val);
			}
			else {
				for (uint32_t i = 0; i < this->datalen; i++) {
					printf("0x%x ", databuffer[i]);
				}
			}
		}
		free(databuffer);

	}
}



void sdowrite(void *ptr) {

	if (this->mindex == 0) {
		printf("Error: The CANOpen main index has to be set with *--mindex* command\n");
	}
	else if (this->datalen > 4) {
		printf("Error: SDO write command supports only expedited transfers\n");
	}
	else {
		uv_errors_e e = uv_canopen_sdo_write(db_get_nodeid(&dev.db),
				this->mindex, this->sindex, (this->datalen == 0) ? 4 : this->datalen, &this->value);
		if (e != ERR_NONE) {
			printf("SDO write returned an error: %u\n", e);
		}
		else {
			printf("SDO write succesful\n");
		}
	}
}
