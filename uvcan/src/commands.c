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



#include "export.h"
#include <uv_can.h>
#include <string.h>
#include "commands.h"
#include "main.h"
#include "help.h"
#include "load.h"
#include "listen.h"
#include "terminal.h"
#include "db.h"
#include "loadmedia.h"
#include "clearmedia.h"
#include "sdo.h"

#define this (&dev)


bool cmd_can(const char *arg);
bool cmd_baud(const char *arg);
bool cmd_node(const char *arg);
bool cmd_srcdest(const char *arg);
bool cmd_incdest(const char *arg);

commands_st commands[] = {
		{
				.cmd_long = "help",
				.cmd_short = 'h',
				.str = "Displays application info and help.",
				.args = ARG_NONE,
				.callback = &cmd_help
		},
		{
				.cmd_long = "can",
				.cmd_short = 'c',
				.str = "Selects the CAN-USB hardware for communication. Defaults to can0.",
				.args = ARG_REQUIRE,
				.callback = &cmd_can
		},
		{
				.cmd_long = "baud",
				.cmd_short = 'b',
				.str = "Sets the baudrate for the CAN-bus. Refer to CiA specification for valid values. "
						"Defaults to 250 kbaud.",
				.args = ARG_REQUIRE,
				.callback = &cmd_baud
		},
		{
				.cmd_long = "srcdest",
				.str = "Sets the source destination file path. This is used by various commands which output data.",
				.args = ARG_REQUIRE,
				.callback = &cmd_srcdest
		},
		{
				.cmd_long = "incdest",
				.str = "Sets the include destination file path. This is used by various commands which output data.",
				.args = ARG_REQUIRE,
				.callback = &cmd_incdest
		},
		{
				.cmd_long = "nodeid",
				.cmd_short = 'n',
				.str = "Selecs the CANopen Node via Node ID. This should be called prior to commands which "
						"Operate on CANopen nodes, such as *loadbin*.",
				.args = ARG_REQUIRE,
				.callback = &cmd_node
		},
		{
				.cmd_long = "loadbin",
				.cmd_short = 'L',
				.str = "Loads firmware to UV device with a CANopen 302 compatible bootloader. "
						"The device node id should be selected with 'node' option prior to this command.",
				.args = ARG_REQUIRE,
				.callback = &cmd_load
		},
		{
				.cmd_long = "loadbinwfr",
				.str = "Loads firmware to UV device with a CANopen 302 compatible bootloader"
						" by waiting for NMT boot up message."
						"The device node id should be selected with 'node' option prior to this command.",
				.args = ARG_REQUIRE,
				.callback = &cmd_loadwfr
		},
		{
				.cmd_long = "segloadbin",
				.str = "Loads firmware to UV device with a CANopen 302 compatible bootloader. "
						"The device node id should be selected with 'node' option prior to this command. "
						"Uses the SDO segmented transfer to load the binary. Note that this is more "
						"unsafe method compared to \"loadbin\".",
				.args = ARG_REQUIRE,
				.callback = &cmd_segload
		},
		{
				.cmd_long = "segloadbinwfr",
				.str = "Loads firmware to UV device with a CANopen 302 compatible bootloader"
						" by waiting for NMT boot up message."
						"The device node id should be selected with 'node' option prior to this command. "
						"Uses the SDO segmented transfer to load the binary. Note that this is more "
						"unsafe method compared to \"loadbinwfr\".",
				.args = ARG_REQUIRE,
				.callback = &cmd_segloadwfr
		},
		{
				.cmd_long = "uvloadbin",
				.str = "Loads firmware to UV device with an UV compatible bootloader. "
						"The device node id should be selected with 'node' option prior to this command.",
				.args = ARG_REQUIRE,
				.callback = &cmd_uvload
		},
		{
				.cmd_long = "uvloadbinwfr",
				.str = "Loads firmware to UV device with an UV compatible bootloader "
						"by waiting for NMT boot up message."
						"The device node id should be selected with 'node' option prior to this command.",
				.args = ARG_REQUIRE,
				.callback = &cmd_uvloadwfr
		},
		{
				.cmd_long = "listen",
				.cmd_short = 'l',
				.str = "Listens the CAN bus for x seconds, listing all messages received.",
				.args = ARG_NONE,
				.callback = &cmd_listen
		},
		{
				.cmd_long = "terminal",
				.cmd_short = 't',
				.str = "Communicates with the device chosen with **nodeid** via Usevolt SDO reply protocol.",
				.args = ARG_NONE,
				.callback = &cmd_terminal
		},
		{
				.cmd_long = "uwterminal",
				.str = "Communicates with the uw device chosen with **nodeid** via deprecated UW terminal protocol.",
				.args = ARG_NONE,
				.callback = &cmd_uwterminal
		},
		{
				.cmd_long = "db",
				.cmd_short = 'd',
				.str = "Provides uvcan a CANOpen device database file as an argument.",
				.args = ARG_REQUIRE,
				.callback = &cmd_db
		},
		{
				.cmd_long = "exportc",
				.str = "Exports database given with --db to UV embedded source file with a given name.\n"
						"The source file will be rewritten if it exists.",
				.args = ARG_REQUIRE,
				.callback = &cmd_exportc
		},
		{
				.cmd_long = "exporth",
				.str = "Exports database given with --db to UV embedded header file with a given name.\n"
						"The header file will be rewritten if it exists.",
				.args = ARG_REQUIRE,
				.callback = &cmd_exporth
		},
		{
				.cmd_long = "export",
				.cmd_short = 'e',
				.str = "Exports the database loaded with --db into a C .h and .c files. \n"
						"The export location is defined with --srcdest and --incdest.",
				.args = ARG_REQUIRE,
				.callback = &cmd_export
		},
		{
				.cmd_long = "loadmedia",
				.str = "Loads a media file with UV media download protocol. Note that the media file is loaded\n"
						"with the same file path as what is given to this command. In most cases this means\n"
						"that the media should be in the same directory where uvcan is run or in a subdirectory.\n"
						"If a directory is given to this command, all recognized media files will be loaded from \n"
						"that directory. Recursive loading from subdirectories is not supported.",
				.args = ARG_REQUIRE,
				.callback = &cmd_loadmedia
		},
		{
				.cmd_long = "clearmedia",
				.str = "Clears the all media in the device specified by the Node-ID.",
				.args = ARG_NONE,
				.callback = &cmd_clearmedia
		},
		{
				.cmd_long = "mindex",
				.str = "Sets the CANOpen Main index for SDO data transfer.",
				.args = ARG_REQUIRE,
				.callback = &cmd_mindex
		},
		{
				.cmd_long = "sindex",
				.str = "Sets the CANOpen Sub index for SDO data transfer.",
				.args = ARG_REQUIRE,
				.callback = &cmd_sindex
		},
		{
				.cmd_long = "datalen",
				.str = "Sets the data length for the CANOpen SDO read/write request.",
				.args = ARG_REQUIRE,
				.callback = &cmd_datalen
		},
		{
				.cmd_long = "sdoread",
				.str = "Reads data from a device with CANOpen SDO request",
				.args = ARG_NONE,
				.callback = &cmd_sdoread
		},
		{
				.cmd_long = "sdowrite",
				.str = "Writes data to a device with CANOpen SDO request",
				.args = ARG_REQUIRE,
				.callback = &cmd_sdowrite
		},
		{
				.cmd_long = "loadparam",
				.str = "Writes parameters to a UVCan device from the given file.",
				.args = ARG_REQUIRE,
				.callback = &cmd_loadparam
		},
		{
				.cmd_long = "saveparam",
				.str = "Reads parameters from a UVCan device and saves them into the given file.",
				.args = ARG_REQUIRE,
				.callback = &cmd_saveparam
		},
		{
				.cmd_long = "saveparamall",
				.str = "Reads parameters from a UVCan device and saves them into the given file.\n"
						"Reads also CANOpen 301 specified params.",
				.args = ARG_REQUIRE,
				.callback = &cmd_saveparamall
		}
};

unsigned int commands_count(void) {
	return sizeof(commands) / sizeof(commands[0]);
}



bool cmd_can(const char *arg) {
	fprintf(stderr, "selecting '%s' as CAN dev\n", arg);
	strcpy(this->can_channel, arg);

	fprintf(stderr, "Setting CAN dev name and baudrate: %u\n", arg, this->baudrate);
	uv_can_set_baudrate(this->can_channel, this->baudrate);

	return true;
}

bool cmd_baud(const char *arg) {
	bool ret = false;
	if (!arg) {
		fprintf(stderr, "Bad baudrate\n");
	}
	else {
		unsigned int baudrate = strtol(arg, NULL, 0);
		fprintf(stderr, "Setting CAN baudrate: %u\n", baudrate);
		this->baudrate = baudrate;
		uv_can_set_baudrate(this->can_channel, baudrate);
		// force bus state up
		uv_can_set_up(true);
		ret = true;
	}

	return ret;
}

bool cmd_node(const char *arg) {
	bool ret = true;
	if (!arg) {
		fprintf(stderr, "Give Node ID.\n");
		ret = false;
	}
	else {
		uint8_t nodeid = strtol(arg, NULL, 0);
		fprintf(stderr, "Selected Node ID 0x%x\n", nodeid);
		db_set_nodeid(&dev.db, nodeid);
	}

	return ret;
}

bool cmd_srcdest(const char *arg) {
	strcpy(dev.srcdest, arg);
	fprintf(stderr, "Source destination file path set to '%s'\n", arg);
	return true;
}

bool cmd_incdest(const char *arg) {
	strcpy(dev.incdest, arg);
	fprintf(stderr, "Include destination file path set to '%s'\n", arg);
	return true;
}



