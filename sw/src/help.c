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


#include "help.h"
#include "commands.h"
#include "main.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <getopt.h>


// Writes the description of a command, rendering the command names it marks with
// *name* / **name** in bold. The asterisks are the markup the help texts are
// written with; they are dropped from the output.
static void print_description(const char *str) {
	bool bold = false;
	for (const char *c = str; *c != '\0'; c++) {
		if (*c == '*') {
			// a run of asterisks (either *name* or **name**) is one marker
			while (c[1] == '*') {
				c++;
			}
			bold = !bold;
			printf("%s", bold ? PRINT_BOLD : PRINT_RESET);
		}
		else {
			putchar(*c);
		}
	}
	// a text with an unclosed marker must not leave the terminal bold
	if (bold) {
		printf(PRINT_RESET);
	}
}


// Writes one command's help entry: the option name(s) in bold, followed by the
// description.
static void print_command(const commands_st *cmd) {
	// the short options below 'a' are not registered with getopt (see main()),
	// so only the long name is shown for them
	if (cmd->cmd_short >= 'a') {
		printf(PRINT_BOLD "--%s -%c" PRINT_RESET ": ", cmd->cmd_long, cmd->cmd_short);
	}
	else {
		printf(PRINT_BOLD "--%s" PRINT_RESET ": ", cmd->cmd_long);
	}
	print_description(cmd->str);
	printf("\n\n");
}


// Case insensitive string comparison. Written out rather than taken from
// strcasecmp(), which is not part of C11 and so is not guaranteed to be there
// for the Windows cross build.
static bool str_equals_nocase(const char *a, const char *b) {
	while ((*a != '\0') &&
			(tolower((unsigned char) *a) == tolower((unsigned char) *b))) {
		a++;
		b++;
	}
	return (*a == *b);
}


// Finds the command which *name* refers to, or NULL when it matches none. The
// name may be written with or without the leading dashes, and either as the long
// name ("loadparam", "--loadparam") or as the short one ("-n", "n").
static const commands_st *find_command(const char *name) {
	const commands_st *ret = NULL;

	while (*name == '-') {
		name++;
	}
	if (*name != '\0') {
		for (unsigned int i = 0; i < commands_count(); i++) {
			if (str_equals_nocase(name, commands[i].cmd_long)) {
				ret = &commands[i];
				break;
			}
			else if ((commands[i].cmd_short != '\0') &&
					(name[0] == commands[i].cmd_short) &&
					(name[1] == '\0')) {
				ret = &commands[i];
				break;
			}
			else {
				// no match, keep looking
			}
		}
	}

	return ret;
}


bool cmd_help(const char *arg) {
	// getopt attaches a value to an optional-argument option only when it is
	// written with '=' ("--help=loadparam"), so the space separated form
	// ("--help loadparam") leaves the name as the next unparsed token. Take it
	// from there, but only when it really names a command: the tokens getopt has
	// moved aside also hold the file arguments of the other commands, and those
	// must not turn "--help" into an error.
	const commands_st *cmd = (arg != NULL) ? find_command(arg) : NULL;
	if ((arg == NULL) && (optind < dev.argc) && (dev.argv[optind] != NULL)) {
		cmd = find_command(dev.argv[optind]);
	}

	if (cmd != NULL) {
		print_command(cmd);
	}
	else if (arg != NULL) {
		printf(PRINT_BOLDRED "ERROR: unknown command '%s'. Run --help without an "
				"argument to list every command.\n" PRINT_RESET, arg);
	}
	else {
		printf( "*****************************\n"
				"Usevolt CAN command line tool\n"
				"*****************************\n"
				"\n\nCommands:\n");
		for (unsigned int i = 0; i < commands_count(); i++) {
			print_command(&commands[i]);
		}
	}

	return true;
}
