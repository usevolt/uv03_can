/*
 * can_msg.c
 *
 *  Created on: Jul 9, 2016
 *      Author: usevolt
 */

#include "can_msg.h"
#include <string.h>



can_message_types_e can_msg_get_type(char *str) {
	if (strcmp(str, "x") == 0 ||
			strcmp(str, "ext") == 0 ||
			strcmp(str, "EXT") == 0 ||
			strcmp(str, "2") == 0 ||
			strcmp(str, "extended") == 0) {
		return TYPE_EXT;
	}
	else return TYPE_STD;
}

char *can_msg_get_type_str(can_message_types_e type) {
	switch (type) {
	case TYPE_EXT:
		return "ext";
	default:
		return "std";
	}
}



int can_msg_get_value(char *str) {
	return strtol(str, NULL, 0);
}
