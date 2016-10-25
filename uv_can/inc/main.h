/*
 ============================================================================
 Name        : uv_can.c
 Author      :
 Version     :
 Copyright   : Your copyright notice
 Description : Hello World in C, Ansi-style
 ============================================================================
 */

#ifndef MAIN_H_
#define MAIN_H_

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "ui.h"
#include "can_msg.h"
#include "can.h"

#define UV_PREFIX		(0x1556 << 16)


typedef struct {
	bool connected;
	char name[128];
	char build_date[128];
	unsigned int id;
} can_device_st;


typedef struct {
	can_device_st can_device;
} app_st;


void rx_callback(void *me, can_message_st *msg);



#endif /* MAIN_H_ */
