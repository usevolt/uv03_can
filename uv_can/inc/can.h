/*
 * can.h
 *
 *  Created on: Jul 10, 2016
 *      Author: usevolt
 */

#ifndef CAN_H_
#define CAN_H_

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include "can_msg.h"



/// @brief: Opens a connection to the CAN device
///
/// @return: true if opening succeeded, false otherwise. errno is written
/// with a description of the problem.
bool can_open(char *dev, unsigned int baudrate, void *me);


extern char can_rx_buf[1024];
#define canf(...)		sprintf(can_rx_buf, __VA_ARGS__); \
	can_send_str(can_rx_buf, this)


void can_send_str(char *str, void *me);



/// @brief: Sends *count* amount of bytes to the CAN-bus device
void can_send(can_message_types_e type, unsigned int id, uint8_t *data, unsigned int count);



#endif /* CAN_H_ */
