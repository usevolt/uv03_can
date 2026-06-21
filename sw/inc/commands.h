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


#ifndef COMMANDS_H_
#define COMMANDS_H_


#include <uv_utilities.h>
#include <getopt.h>


typedef enum {
	ARG_NONE = no_argument,
	ARG_OPTIONAL = optional_argument,
	ARG_REQUIRE = required_argument
} argument_e;


/// @brief: Defines the command structure
typedef struct {
	/// @brief: Name of the command. Name also works as a
	/// GNU option name.
	const char *cmd_long;
	const char cmd_short;
	/// @brief: A string introducing the command
	const char *str;
	argument_e args;
	/// @brief: Command callback. Should return true if the command
	/// execution was succeeded, otherwise false.
	/// **arg** contains the argument given to the command from the command line.
	bool (*callback)(const char *arg);
} commands_st;

/// @brief: array containing all the commands
extern commands_st commands[];

/// @brief: Returns the command count.
unsigned int commands_count(void);


#endif /* COMMANDS_H_ */
