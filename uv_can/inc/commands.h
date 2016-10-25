/*
 * commands.h
 *
 *  Created on: Jul 9, 2016
 *      Author: usevolt
 */

#ifndef COMMANDS_H_
#define COMMANDS_H_



#define ARG_COUNT	10

#define _STRINGIFY(s)	#s
#define STRINGIFY(s)	_STRINGIFY(s)


#define CAN_DEFAULT_DEV		"/dev/pcanusb32"
#define CAN_DEFAULT_BAUDRATE	250000

typedef enum {
	CMD_OPEN,
	CMD_CONNECT,
	CMD_SEND,
	CMD_LIST_DEVS,
	CMD_QUIT,
	CMD_HELP
} commands_e;


typedef struct {
	char *command_str;
	char *shortcut_str;
	char *instructions_str;
	commands_e cmd_enum;
	char *argument_names[ARG_COUNT];
} command_st;



extern const command_st commands[];


unsigned int get_commands_count();



void parse_command(void *this, char *line);


#endif /* COMMANDS_H_ */
