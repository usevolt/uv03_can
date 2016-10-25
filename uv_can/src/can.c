/*
 * can.c
 *
 *  Created on: Jul 10, 2016
 *      Author: usevolt
 */


#include "can.h"
#include "ui.h"
#include "main.h"
#include <libpcan.h>
#include <pthread.h>
#include <fcntl.h>
#include <time.h>


static HANDLE h;
pthread_t thread;
char can_rx_buf[1024];

#define this ((app_st*)me)


void can_send_str(char *str, void *me) {
	int i = 1;
	char *ptr = str;
	while (*ptr != '\0') {
		if (i == 8) {
			can_send(TYPE_EXT, this->can_device.id + UV_PREFIX, (uint8_t*) ptr - i + 1, 8);
			i = 0;
		}
		i++;
		ptr++;
	}
	if (i > 1) {
		can_send(TYPE_EXT, this->can_device.id + UV_PREFIX, (uint8_t*) ptr - i + 1, i - 1);
	}

}



void *can_rx(void *me) {
	// endless loop
	while (true) {
		TPCANMsg msg;
		CAN_Read(h, &msg);

		ui_errf("received: id: 0x%x len: %u, type %u data: ", msg.ID, msg.LEN, msg.MSGTYPE);
		for (int i = 0; i < msg.LEN; i++) {
			ui_errf("0x%x ", msg.DATA[i]);
		}
		ui_errf("\n");

		if (this->can_device.connected && msg.ID == this->can_device.id + UV_PREFIX) {
			for (int i = 0; i < msg.LEN; i++) {
				if (msg.DATA[i] != 0xD) {
					ui_printf("%c", msg.DATA[i]);
				}
			}
		}

	}
}

bool can_open(char *dev, unsigned int baudrate, void *me) {

	h = LINUX_CAN_Open(dev, O_RDWR);

	if (!h) {
		ui_errf("Opening '%s' failed\n", dev);
		return false;
	}

	ui_logf("'%s' opened\n", dev);

	// baudrate is BTR0BTR1 format
	//BTR1 half is always 0x1C => Tclk = 16 Tscl

	uint16_t btr0btr1 = ((500000 / baudrate) - 1) << 8;
	btr0btr1 += 0x1c;

	ui_errf("Setting baudrate to %u, BTR0BTR1: 0x%x\n", baudrate, btr0btr1);

	CAN_Init(h, btr0btr1, CAN_INIT_TYPE_EX);

	pthread_create(&thread, NULL, can_rx, me);

	return true;
}


void can_send(can_message_types_e type, unsigned int id, uint8_t *data, unsigned int count) {
	TPCANMsg msg;
	msg.ID = id;
	msg.LEN = count;
	if (id > 0x800) {
		msg.MSGTYPE = MSGTYPE_EXTENDED;
	}
	else {
		msg.MSGTYPE = MSGTYPE_STANDARD;
	}
	for (int i = 0; i < count; i++) {
		msg.DATA[i] = data[i];
	}

	CAN_Write(h, &msg);

//	if (msg.MSGTYPE == MSGTYPE_EXTENDED) {
//		ui_logf("EXT ");
//	}
//	else {
//		ui_logf("STD ");
//	}
//	ui_logf("message sent. id: 0x%x, data: ", id);
//	for (int i = 0; i < count; i++) {
//		ui_logf("0x%x ", data[i]);
//	}
//	ui_logf("\n");
}
