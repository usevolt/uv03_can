/*
 * commands.c
 *
 *  Created on: Jul 9, 2016
 *      Author: usevolt
 */


#include "commands.h"
#include "can.h"
#include "main.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>

const command_st commands[] = {
		{
				.cmd_enum = CMD_OPEN,
				.command_str = ":open",
				.shortcut_str = ":O",
				.instructions_str = "Opens a connection to CAN-adapter",
				.argument_names[0] = "Path to the device. Leave empty to use the default " CAN_DEFAULT_DEV,
				.argument_names[1] = "Desired baudrate. Leave empty to use the default " STRINGIFY(CAN_DEFAULT_BAUDRATE)
		},
		{
				.cmd_enum = CMD_CONNECT,
				.command_str = ":connect",
				.shortcut_str = ":C",
				.instructions_str = "Connects to a CAN device.",
				.argument_names[0] = "The CAN device ID"
		},
		{
				.cmd_enum = CMD_SEND,
				.command_str = ":send",
				.shortcut_str = ":S",
				.instructions_str = "Sends a CAN message.",
				.argument_names[0] = "message type",
				.argument_names[1] = "message ID",
				.argument_names[2] = "message data byte 1",
				.argument_names[3] = "message data byte 2",
				.argument_names[4] = "message data byte 3",
				.argument_names[5] = "message data byte 4",
				.argument_names[6] = "message data byte 5",
				.argument_names[7] = "message data byte 6",
				.argument_names[8] = "message data byte 7",
				.argument_names[9] = "message data byte 8"
		},
		{
				.cmd_enum = CMD_LIST_DEVS,
				.command_str = ":list",
				.shortcut_str = ":L",
				.instructions_str = "Searches the CAN bus and lists the devices found on current system.",
		},
		{
				.cmd_enum = CMD_QUIT,
				.command_str = ":quit",
				.shortcut_str = ":Q",
				.instructions_str = "Exits the program."
		},
		{
				.cmd_enum = CMD_HELP,
				.command_str = ":help",
				.shortcut_str = ":?",
				.instructions_str = "Displays instructions for all commands, asshole."
		}
};



unsigned int get_commands_count() {
	return sizeof(commands) / sizeof(command_st);
}


#define MSG(...)		ui_errf(__VA_ARGS__)

#define this ((app_st*) me)



void parse_command(void *me, char *line) {

	// replace \n with \r to prevent one additional new line
	char *nl = strchr(line, '\n');
	if (nl) {
		*nl = '\r';
	}

	char full_cmd[256];
	strcpy(full_cmd, line);
	char *cmd = strtok(full_cmd, " \n\r");

	if (!cmd) {
		canf("\r");
		return;
	}
	// take the given arguments from the command line
	char *args[ARG_COUNT] = {};
	for (int i = 0; i < ARG_COUNT; i++) {
		args[i] = strtok(NULL, " \n\r");
		if (args[i] == NULL) {
			break;
		}
	}


	char str[256] = {};
	bool cmdd = false;
	for (int i = 0; i < get_commands_count(); i++) {
		if (strcmp(cmd, commands[i].command_str) == 0 ||
				strcmp(cmd, commands[i].shortcut_str) == 0) {
			// found out a command
			cmdd = true;

			// if this command has named arguments, check if any arguments have been given.
			// If not, ask for them.
			if (!args[0]) {
				int c = 0;
				char *temp = str;
				while (commands[i].argument_names[c]) {
					MSG("%s:", commands[i].argument_names[c]);

					temp = fgets(temp + strlen(temp) + 1, 256, stdin);
					// if user entered only new line, end argument polling
					if (strlen(temp) <= 1) break;
					else {
						strtok(temp, " \n\r\t");
						if (strcmp(temp, "\x1B") == 0) {
							MSG("Cancel.\n");
							return;
						}
						args[c] = temp;
					}
					c++;
					if (c >= ARG_COUNT) break;
				}
			}

			can_message_st can_msg = {};
			int j;
			bool b;
			switch (commands[i].cmd_enum) {


			case CMD_OPEN:
				if (!args[0]) args[0] = CAN_DEFAULT_DEV;
				if (!args[1]) args[1] = STRINGIFY(CAN_DEFAULT_BAUDRATE);
				if (can_open(args[0], strtol(args[1], NULL, 0), this)) {
					MSG("Session %s open with baudrate %s\n", args[0], args[1]);
				}
				else {
					MSG("Error opening the device\n");
				}
				break;

			case CMD_CONNECT:
				if (!args[0]) {
					MSG("Give device ID to connect\n");
					break;
				}

				this->can_device.id = strtol(args[0], NULL, 0);

				// todo: read the can device and check if its opened.
				// save devices name and build date to this->can_device.***

				this->can_device.connected = true;
				MSG("Connected to device %u '%s' Build on %s\n", this->can_device.id, this->can_device.name,
						this->can_device.build_date);

				break;


			case CMD_LIST_DEVS:
				break;


			case CMD_SEND:
				// 3 arguments is the minimum for straight sending the data
				if (!args[2]) {
					MSG("Sending failed: Message needs at least 1 data byte.\n");
					break;
				}
				can_msg.type = can_msg_get_type(args[0]);

				can_msg.id = can_msg_get_value(args[1]);
				if (can_msg.type == TYPE_STD && can_msg.id > (2 ^ 11)) {
					MSG("Sending failed: Message ID too big for standard 11-bit identifier.\n");
				}
				else if (can_msg.type == TYPE_EXT && can_msg.id > (2 ^ 29)) {
					MSG("Sending failed: Message ID too big for extended 29-bit identifier.\n");
				}
				for (j = 0; j < 8; j++) {
					if (!args[2 + j]) {
						break;
					}
					can_msg.data[j] = can_msg_get_value(args[2 + j]);
				}
				can_msg.length = j;

				can_send(can_msg.type, can_msg.id, can_msg.data, can_msg.length);
				break;


			case CMD_QUIT:
				exit(0);


			case CMD_HELP:
				b = false;
				if (args[0]) {
					for (j = 0; j < get_commands_count(); j++) {
						if (strcmp(args[0], commands[j].command_str) == 0 ||
								strcmp(args[0], commands[j].shortcut_str) == 0) {
							// print details of the asked message
							MSG("%s (%s) %s\n", commands[j].command_str,
									commands[j].shortcut_str,
									commands[j].instructions_str);
							b = true;
							break;
						}
					}
				}
				if (!b) {
					// print all available messages
					for (j = 0; j < get_commands_count(); j++) {
						ui_logf("'%s'\n", commands[j].command_str);
					}
				}
				break;
			default:


				break;
			}
			return;
		}
		else {

		}
	}
	if (!cmdd) {
		// send string to the open CAN device here
		if (this->can_device.connected) {
			can_send_str(line, this);
		}
		else {
			MSG("No CAN device open. Try typing ':?' for info where to start.\n");
		}
	}
}

