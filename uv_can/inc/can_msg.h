/*
 * can_msg.h
 *
 *  Created on: Jul 9, 2016
 *      Author: usevolt
 */

#ifndef CAN_MSG_H_
#define CAN_MSG_H_

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

typedef enum {
	TYPE_STD = 0,
	TYPE_EXT
} can_message_types_e;


typedef struct {
	can_message_types_e type;
	unsigned int id;
	unsigned int length;
	uint8_t data[8];
	bool hexa_numbers;
} can_message_st;

/// @brief: Parses the string *str* and returns the CAN message type from it.
/// If the type couldn't be recogniced, returns TYPE_NONE.
can_message_types_e can_msg_get_type(char *str);


/// @brief: Returns the message type string
char *can_msg_get_type_str(can_message_types_e type);


/// @brief: Parses *str* and returns it's numeric value. If *hexa* is set,
/// *str* is assumed to be hexadecimal. Otherwise 0x-prefix indicates hexa values,
/// all others are decimals.
int can_msg_get_value(char *str);

#endif /* CAN_MSG_H_ */
