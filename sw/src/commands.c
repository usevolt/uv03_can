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
#include "find.h"
#include "terminal.h"
#include "db.h"
#include "loadmedia.h"
#include "clearmedia.h"
#include "makeuvdev.h"
#include "sdo.h"
#include "system.h"
#include "simrun.h"
#include "credentials.h"
#include "selfupdate.h"
#include "remotefiles.h"
#include <time.h>
#include "ui/uvui.h"
#include <uv_ui.h>
#include <uv_rtos.h>

#define this (&dev)


bool cmd_can(const char *arg);
bool cmd_baud(const char *arg);
bool cmd_node(const char *arg);
bool cmd_forcenode(const char *arg);
bool cmd_srcdest(const char *arg);
bool cmd_incdest(const char *arg);
bool cmd_silent(const char *arg);
bool cmd_ui(const char *arg);
bool cmd_sim(const char *arg);
bool cmd_user(const char *arg);
bool cmd_pwd(const char *arg);
bool cmd_version(const char *arg);
bool cmd_serverfiles(const char *arg);
bool cmd_checkupdate(const char *arg);
bool cmd_update(const char *arg);

// Node id note shared by the help of the commands which operate on the devices
// loaded with *dev* / *sys*, so each of them tells where a device's node id comes
// from without repeating the explanation.
#define DEV_NODEID_HELP \
		"A device given with *dev* is addressed with the node id in its\n"\
		"'<file>:0x14' postfix, or with a *nodeid* option given after the\n"\
		"device; a device coming from *sys* uses the node id stored in the\n"\
		"system file."

// Node id note shared by the help of the load commands, which all take their
// file argument with the same '<file>:<nodeid>' postfix as *dev* does.
#define LOAD_NODEID_HELP \
		"The file argument may carry a ':<nodeid>' postfix (e.g. 'file:0xd'),\n"\
		"which selects the target node exactly as a *forcenodeid* option given\n"\
		"before this command would."

commands_st commands[] = {
		{
				.cmd_long = "help",
				.cmd_short = 'h',
				.str = "Displays application info and help. Given the name of a single "
						"command as its argument (e.g. '--help loadparam', '-h loadparam' "
						"or '--help --loadparam'), only that command's description is shown. "
						"Without an argument every command is listed.",
				.args = ARG_OPTIONAL,
				.callback = &cmd_help
		},
		{
				.cmd_long = "serverfiles",
				.str = "Lists the files the account in *user* / *pwd* can see on the file "
						"server, as the UI's \"Server files\" panel does, and prints how long "
						"each step took. Downloads nothing: only the directory listings are "
						"fetched, which is what the panel reads too.\n"
						"Useful for telling a slow file server from a slow network, and for "
						"seeing what an account actually holds without opening the UI.",
				.args = ARG_NONE,
				.callback = &cmd_serverfiles
		},
		{
				.cmd_long = "version",
				.cmd_short = 'V',
				.str = "Prints this uvcan's version and exits. Two numbers: the readable "
						"release name (e.g. '1.1.1-204-g5746') and, in brackets, the build "
						"number that *checkupdate* compares against. The build number counts "
						"every commit in the version's history, so it only ever grows; the "
						"name counts commits since the last release and restarts at each one.",
				.args = ARG_NONE,
				.callback = &cmd_version
		},
		{
				.cmd_long = "checkupdate",
				.str = "Asks the Usevolt file server whether a newer uvcan has been published "
						"and reports what it finds, without installing anything. uvcan is "
						"public software, so this needs no account and ignores the one in "
						"*user* / *pwd*. Install what it finds with *update*.\n"
						"The UI makes the same check once in the background when it opens "
						"and writes what it finds into its log; a check that fails there is "
						"silent, which is what this command is for.",
				.args = ARG_NONE,
				.callback = &cmd_checkupdate
		},
		{
				.cmd_long = "update",
				.str = "Downloads the newest published uvcan and installs it over this one.\n"
						"The download is checked against the size and the checksum the server "
						"publishes before anything is replaced, and the uvcan it replaces is "
						"kept next to it as 'uvcan.old', so a bad build can be put back by "
						"hand. The running uvcan keeps running from the file it started with: "
						"restart it to use the new one.\n"
						"A machine-wide install (install.sh --system) is owned by root, so it "
						"takes 'sudo uvcan --update'. A per-user install needs no privileges.",
				.args = ARG_NONE,
				.callback = &cmd_update
		},
		{
				.cmd_long = "ui",
				.cmd_short = 'u',
				.str = "Opens the graphical configuration window for selecting the CAN device "
						"and baudrate, followed by uvcan's main display showing the system "
						"and its devices. Blocks until the window is closed.",
				.args = ARG_NONE,
				.callback = &cmd_ui
		},
		{
				.cmd_long = "sys",
				.str = "Loads a system configuration (.uvsys) file. The system file bundles the "
						"devices of the whole system. Individual --dev files can still be given "
						"alongside it; they are added on top of the system file's devices.",
				.args = ARG_REQUIRE,
				.callback = &cmd_system
		},
		{
				.cmd_long = "dev",
				.str = "Adds a device configuration (.uvdev) file to the system, given as "
						"'<path>:<nodeid>' (e.g. '/path/to/dev.uvdev:0x10'). The "
						"':<nodeid>' suffix is optional; when omitted the default node id "
						"is read from the .uvdev file, unless a *nodeid* option follows the "
						"device, which assigns the node id to it just like the suffix does. "
						"Can be given multiple times, and also together with --sys, in which "
						"case the devices are added on top of the system file's ones.",
				.args = ARG_REQUIRE,
				.callback = &cmd_device
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
						"Operate on CANopen nodes, such as *loadbin*. Never changes the node id of the "
						"device; *loadparam* loads the parameters to this node id and, for parameter files "
						"which contain more than one device, ignores this option altogether. When given "
						"after a *dev* file, the node id is also assigned to that device, i.e. "
						"'--dev file.uvdev -n 0x14' means the same as '--dev file.uvdev:0x14'.",
				.args = ARG_REQUIRE,
				.callback = &cmd_node
		},
		{
				.cmd_long = "forcenodeid",
				.str = "Selects the CANopen Node via Node ID like *nodeid*, but forces it: every "
						"command which follows targets this node id instead of the one its device "
						"package, system file or parameter file carries. A single node id cannot name "
						"any one of several devices, so it is ignored, with a warning, by a command "
						"which operates on more than one device (e.g. a whole *sys* system, or a "
						"parameter file holding more than one device). "
						"It also permits *loadparam* to assign the node id found from the parameter "
						"file to the selected device. That node id is written to the device only after "
						"the user has confirmed it with an empty line, and it applies only after the "
						"device's settings have been saved and the device rebooted. "
						"Like *nodeid*, when given after a *dev* file it also assigns the node id to "
						"that device, i.e. '--dev file.uvdev --forcenodeid 0x14' means the same as "
						"'--dev file.uvdev:0x14'. The load commands take the same node id in their own "
						"'<file>:<nodeid>' postfix, i.e. '--loadparam params.json:0xd' means the same "
						"as '--forcenodeid 0xd --loadparam params.json'. "
						"The selection ends with the command line: it does not carry into *ui*, "
						"where every device is addressed by its own node id.",
				.args = ARG_REQUIRE,
				.callback = &cmd_forcenode
		},
		{
				.cmd_long = "loadbin",
				.cmd_short = 'L',
				.str = "Loads firmware to UV device with a CANopen 302 compatible bootloader. "
						"The argument may be a raw firmware binary (the device node id is then "
						"selected with the 'node' option), a .uvdev package (its FIRMWARE binary is "
						"flashed), or a .uvsys package (every device inside is flashed). If the "
						"argument is omitted, the devices loaded earlier with --dev / --sys are flashed, "
						"or, when no devices were given, the binary given with 'firmware'.\n"
						DEV_NODEID_HELP "\n"
						LOAD_NODEID_HELP,
				.args = ARG_OPTIONAL,
				.callback = &cmd_load
		},
		{
				.cmd_long = "loadbinwfr",
				.str = "Loads firmware to UV device with a CANopen 302 compatible bootloader"
						" by waiting for NMT boot up message. "
						"Accepts a raw binary, a .uvdev or .uvsys package, or no argument to flash "
						"the devices loaded earlier with --dev / --sys, or the binary given with "
						"'firmware' when no devices were given. "
						"The device node id should be selected with 'node' option prior to this command.\n"
						DEV_NODEID_HELP "\n"
						LOAD_NODEID_HELP,
				.args = ARG_OPTIONAL,
				.callback = &cmd_loadwfr
		},
		{
				.cmd_long = "segloadbin",
				.str = "Loads firmware to UV device with a CANopen 302 compatible bootloader. "
						"Accepts a raw binary, a .uvdev or .uvsys package, or no argument to flash "
						"the devices loaded earlier with --dev / --sys, or the binary given with "
						"'firmware' when no devices were given. "
						"The device node id should be selected with 'node' option prior to this command.\n"
						DEV_NODEID_HELP "\n"
						LOAD_NODEID_HELP "\n"
						"Uses the SDO segmented transfer to load the binary. Note that this is more "
						"unsafe method compared to \"loadbin\".",
				.args = ARG_OPTIONAL,
				.callback = &cmd_segload
		},
		{
				.cmd_long = "segloadbinwfr",
				.str = "Loads firmware to UV device with a CANopen 302 compatible bootloader"
						" by waiting for NMT boot up message. "
						"Accepts a raw binary, a .uvdev or .uvsys package, or no argument to flash "
						"the devices loaded earlier with --dev / --sys, or the binary given with "
						"'firmware' when no devices were given. "
						"The device node id should be selected with 'node' option prior to this command.\n"
						DEV_NODEID_HELP "\n"
						LOAD_NODEID_HELP "\n"
						"Uses the SDO segmented transfer to load the binary. Note that this is more "
						"unsafe method compared to \"loadbinwfr\".",
				.args = ARG_OPTIONAL,
				.callback = &cmd_segloadwfr
		},
		{
				.cmd_long = "uvloadbin",
				.str = "Loads firmware to UV device with an UV compatible bootloader. "
						"Accepts a raw binary, a .uvdev or .uvsys package, or no argument to flash "
						"the devices loaded earlier with --dev / --sys, or the binary given with "
						"'firmware' when no devices were given. "
						"The device node id should be selected with 'node' option prior to this command.\n"
						DEV_NODEID_HELP "\n"
						LOAD_NODEID_HELP,
				.args = ARG_OPTIONAL,
				.callback = &cmd_uvload
		},
		{
				.cmd_long = "uvloadbinwfr",
				.str = "Loads firmware to UV device with an UV compatible bootloader "
						"by waiting for NMT boot up message. "
						"Accepts a raw binary, a .uvdev or .uvsys package, or no argument to flash "
						"the devices loaded earlier with --dev / --sys, or the binary given with "
						"'firmware' when no devices were given. "
						"The device node id should be selected with 'node' option prior to this command.\n"
						DEV_NODEID_HELP "\n"
						LOAD_NODEID_HELP,
				.args = ARG_OPTIONAL,
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
				.cmd_long = "sim",
				.str = "Runs the Linux simulator of every device in the loaded system "
						"as a separate process (give --sys or --dev first), each "
						"connected to the selected CAN device (--can) with the "
						"device's node id. uvcan keeps running to monitor the "
						"simulators and kills them when it exits. Linux only. This is "
						"the same action as the UI's \"Run simulator\" button.\n"
						"The commands given after this one are run once the simulators "
						"are up and answering on the bus, so they address the "
						"simulated devices: '--sim --loadparam params.yml' starts the "
						"simulators and loads the parameters onto them. Only after the "
						"last command has run does uvcan settle into monitoring them, "
						"until Ctrl-C.\n"
						DEV_NODEID_HELP,
				.args = ARG_NONE,
				.callback = &cmd_sim
		},
		{
				.cmd_long = "find",
				.cmd_short = 'f',
				.str = "Searches the CAN bus for Usevolt devices. Clears any existing "
						"devices, listens for CANopen heartbeats for 2 seconds and, for "
						"every OPERATIONAL node whose vendor id is Usevolt's, reads its "
						"product code and adds it as a new device (with an empty "
						"configuration-file path).",
				.args = ARG_NONE,
				.callback = &cmd_find
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
				.str = "Loads media with the UV media download protocol. The argument may be a media\n"
						"file, a directory of media files (all recognized files in it are loaded;\n"
						"subdirectories are not), a .uvdev package (its bundled media is loaded), or a\n"
						".uvsys package (each device's bundled media is loaded). If the argument is\n"
						"omitted, the bundled media of the devices loaded earlier with --dev / --sys is\n"
						"loaded. Devices whose package bundles no media are reported with a warning.\n"
						DEV_NODEID_HELP "\n"
						LOAD_NODEID_HELP,
				.args = ARG_OPTIONAL,
				.callback = &cmd_loadmedia
		},
		{
				.cmd_long = "firmware",
				.str = "Sets the firmware binary which *makeuvdev* packages into the .uvdev file.\n"
						"Mandatory for *makeuvdev*. The *loadbin* command family flashes this\n"
						"binary as well when it is given no file of its own and no devices were\n"
						"loaded with --dev / --sys.",
				.args = ARG_REQUIRE,
				.callback = &cmd_firmware
		},
		{
				.cmd_long = "linuxbin",
				.str = "Sets the Linux simulator executable which *makeuvdev* packages into the\n"
						".uvdev file. Optional: a package without it simply cannot be simulated.",
				.args = ARG_REQUIRE,
				.callback = &cmd_linuxbin
		},
		{
				.cmd_long = "bootloader",
				.str = "Sets the bootloader binary which *makeuvdev* packages into the .uvdev file.\n"
						"Optional.",
				.args = ARG_REQUIRE,
				.callback = &cmd_bootloader
		},
		{
				.cmd_long = "media",
				.str = "Adds a media file, or a directory of media files, into the media which\n"
						"*makeuvdev* packages into the .uvdev file. Of a directory the files in it\n"
						"are packaged, not its subdirectories. Optional, and can be given more than\n"
						"once; all of them end up in the same media directory of the package, which\n"
						"*loadmedia* then loads onto the device.",
				.args = ARG_REQUIRE,
				.callback = &cmd_media
		},
		{
				.cmd_long = "mediadir",
				.str = "Sets the name which the media directory gets in the .uvdev package which\n"
						"*makeuvdev* writes, i.e. the value of the manifest's MEDIA key. Optional,\n"
						"defaults to 'media'. The device stores its media under names relative to\n"
						"the package root, so this is the directory prefix which the firmware asks\n"
						"its media by: a build asking for 'media_hd/icon_hd.png' needs its package\n"
						"made with '--mediadir media_hd'.",
				.args = ARG_REQUIRE,
				.callback = &cmd_mediadir
		},
		{
				.cmd_long = "fwversion",
				.str = "Sets the firmware version string which *makeuvdev* stores in the .uvdev\n"
						"package. Usually the same git describe based version which the firmware\n"
						"itself was built with. Optional.",
				.args = ARG_REQUIRE,
				.callback = &cmd_fwversion
		},
		{
				.cmd_long = "makeuvdev",
				.str = "Writes a .uvdev device package to the file given as the argument. The package\n"
						"bundles the device database given with *db* (and every file which the\n"
						"database pulls in with a \"content\" reference) and the firmware binary given\n"
						"with *firmware*; both of them are mandatory and have to be given before this\n"
						"command. The Linux simulator (*linuxbin*), the bootloader binary\n"
						"(*bootloader*), the media files (*media*) and the firmware version\n"
						"(*fwversion*) are optional and are left out of the package when not given.\n"
						"The directory of the output file is created if it doesn't exist, and the\n"
						"'.uvdev' extension is added to the file name when it is missing.",
				.args = ARG_REQUIRE,
				.callback = &cmd_makeuvdev
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
				.cmd_long = "sdoreadstr",
				.str = "Reads string from a device with CANOpen SDO request",
				.args = ARG_NONE,
				.callback = &cmd_sdoreadstr
		},
		{
				.cmd_long = "sdowrite",
				.str = "Writes data to a device with CANOpen SDO request",
				.args = ARG_REQUIRE,
				.callback = &cmd_sdowrite
		},
		{
				.cmd_long = "sdowritestr",
				.str = "Writes string to a device with CANOpen SDO request",
				.args = ARG_REQUIRE,
				.callback = &cmd_sdowrite
		},
		{
				.cmd_long = "loadparam",
				.str = "Writes parameters to UVCan device(s). The argument may be a parameter file, or\n"
						"a .uvsys package (each device's bundled parameters are written to it). If the\n"
						"argument is omitted, the bundled parameters of the devices loaded earlier with\n"
						"--sys / --dev are written. .uvdev packages are not accepted (they carry no\n"
						"parameters).\n"
						"EMCY messages are suppressed on every target device before the first parameter\n"
						"is written, so no device warns about another one being configured. A device\n"
						"which does not have the parameter is loaded anyway, with a warning. For a\n"
						"whole system the devices are stored and reset together once all of them have\n"
						"been written.\n"
						DEV_NODEID_HELP "\n"
						LOAD_NODEID_HELP,
				.args = ARG_OPTIONAL,
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
		},
		{
				.cmd_long = "user",
				.str = "Sets the account username stored on this computer and shared by every "
						"uvcan install (the same value shown in the UI's Account panel). It is "
						"saved in plain text and reused by later runs when --user is not given. "
						"Equivalent to typing the username in the UI Account panel.",
				.args = ARG_REQUIRE,
				.callback = &cmd_user
		},
		{
				.cmd_long = "pwd",
				.str = "Sets the account password stored on this computer and shared by every "
						"uvcan install (the same value shown in the UI's Account panel). It is "
						"saved in plain text and reused by later runs when --pwd is not given. "
						"Equivalent to typing the password in the UI Account panel.",
				.args = ARG_REQUIRE,
				.callback = &cmd_pwd
		},
		{
				.cmd_long = "silent",
				.cmd_short = 's',
				.str = "Excecutes uvcan silent, without logging process information",
				.args = ARG_NONE,
				.callback = &cmd_silent
		}
};

unsigned int commands_count(void) {
	return sizeof(commands) / sizeof(commands[0]);
}



// Set once --can / -c is given on the command line, so the UI start-up skips the
// CAN-device configuration window (the user already chose the device explicitly).
static bool can_given;

bool cmd_can(const char *arg) {
	can_given = true;
	PRINT("selecting '%s' as CAN dev\n", arg);
	strcpy(this->can_channel, arg);

	PRINT("Setting CAN dev name and baudrate: %u\n", arg, this->baudrate);
	uv_can_set_baudrate(this->can_channel, this->baudrate);

	return true;
}

bool cmd_baud(const char *arg) {
	bool ret = false;
	if (!arg) {
		PRINT("Bad baudrate\n");
	}
	else {
		unsigned int baudrate = strtol(arg, NULL, 0);
		PRINT("Setting CAN baudrate: %u\n", baudrate);
		this->baudrate = baudrate;
		uv_can_set_baudrate(this->can_channel, baudrate);
		// force bus state up
		uv_can_set_up(true);
		ret = true;
	}

	return ret;
}

// Assigns *nodeid* to the device given last with --dev, so that a node id given
// after a device file means the same as the file's own ":<nodeid>" suffix.
// Does nothing when no device file has been given on the command line.
static void select_cmdline_device(uint8_t nodeid) {
	device_st *d = system_set_cmdline_dev_nodeid(&dev.system, nodeid);
	if (d != NULL) {
		PRINT("Node ID 0x%x assigned to device '%s'\n", nodeid, d->name);
	}
}

void commands_select_nodeid(uint8_t nodeid, bool force) {
	db_set_nodeid_force(&dev.db, nodeid);
	dev.system.forced_nodeid_set = true;
	dev.system.forced_nodeid = nodeid;
	// *nodeid* selects the device only, it never assigns a new node id; only
	// *forcenodeid* overrides the node id a device package, a system file or a
	// parameter file carries
	dev.system.forcenodeid = force;
}

bool cmd_node(const char *arg) {
	bool ret = true;
	if (!arg) {
		PRINT("Give Node ID.\n");
		ret = false;
	}
	else {
		uint8_t nodeid = strtol(arg, NULL, 0);
		PRINT("Selected Node ID 0x%x\n", nodeid);
		commands_select_nodeid(nodeid, false);
		select_cmdline_device(nodeid);
	}

	return ret;
}

bool cmd_forcenode(const char *arg) {
	bool ret = true;
	if (!arg) {
		PRINT("Give Node ID.\n");
		ret = false;
	}
	else {
		uint8_t nodeid = strtol(arg, NULL, 0);
		PRINT("Selected Node ID 0x%x. The node id from the parameter file will "
				"be assigned to this device.\n", nodeid);
		commands_select_nodeid(nodeid, true);
		select_cmdline_device(nodeid);
	}

	return ret;
}

bool cmd_srcdest(const char *arg) {
	strcpy(dev.srcdest, arg);
	PRINT("Source destination file path set to '%s'\n", arg);
	return true;
}

bool cmd_incdest(const char *arg) {
	strcpy(dev.incdest, arg);
	PRINT("Include destination file path set to '%s'\n", arg);
	return true;
}


bool silent = true;
bool cmd_silent(const char *arg) {
	silent = false;

	return true;
}

bool cmd_user(const char *arg) {
	// store (and persist) the shared account username, exactly as editing the
	// Username field in the UI Account panel does
	credentials_set_username(arg);
	PRINT("Account username set to '%s'\n", arg);

	return true;
}

bool cmd_pwd(const char *arg) {
	// store (and persist) the shared account password. Do not echo it back
	credentials_set_password(arg);
	PRINT("Account password set\n");

	return true;
}

/// @brief: Task body for the --ui command. Runs under the FreeRTOS scheduler so
/// the HAL task pumps CAN/CANopen while the UI is open; this is what lets the UI
/// monitor heartbeats and perform SDO reads (e.g. the "Search devices" button).
static void ui_task(void *ptr) {
	// A node id selected with *nodeid* / *forcenodeid* (or with a load command's
	// ':<nodeid>' postfix) belongs to the commands which follow it on the command
	// line. It stops here: in the UI every device is addressed by its own node id,
	// so nothing the user does there is silently redirected to the node id which
	// happened to be on the command line.
	system_clear_forced_nodeid(&dev.system);

	// Open the HAL graphical configuration window for selecting the CAN device
	// and baudrate, unless --can / -c was already given on the command line (the
	// user explicitly chose the device, so there is nothing to ask). This blocks
	// until the user closes it.
	if (!can_given) {
		uv_ui_confwindow_exec(&uv_uistyles[0]);
	}

	// Then open uvcan's own main display showing the system and its devices.
	// Blocks until that window is closed.
	uvui_exec();
}

#if !CONFIG_TARGET_WIN
/// @brief: Seconds since some fixed point, for timing the steps below.
static double sf_now(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double) ts.tv_sec + (double) ts.tv_nsec / 1e9;
}


/// @brief: Task body for --serverfiles. A task rather than the command callback
/// itself for the same reason --update is one: the callbacks run before the
/// scheduler starts.
static void serverfiles_task(void *ptr) {
	(void) ptr;
	char err[256] = "";
	const char *url = credentials_get_url();
	const char *user = credentials_get_username();

	printf("Logging in to %s as '%s'...\n", url, user);
	fflush(stdout);
	double t0 = sf_now();
	if (!remotefiles_login(url, user, credentials_get_password(),
			err, sizeof(err))) {
		printf("%s\n", err);
		return;
	}
	double t1 = sf_now();
	uint8_t fleets = remotefiles_get_fleet_count();
	printf("  logged in in %.2f s; %u fleet(s)\n", t1 - t0,
			(unsigned int) fleets);

	printf("Listing...\n");
	fflush(stdout);
	if (!remotefiles_list(err, sizeof(err))) {
		printf("%s\n", err);
		return;
	}
	double t2 = sf_now();

	uint16_t products = remotefiles_get_product_count();
	uint16_t files = 0;
	for (uint16_t p = 0; p < products; p++) {
		const remotefiles_product_st *prod = remotefiles_get_product(p);
		if (prod != NULL) {
			files = (uint16_t) (files + prod->version_count);
		}
	}
	printf("  listed %u director(ies) holding %u file(s) in %.2f s\n\n",
			(unsigned int) products, (unsigned int) files, t2 - t1);

	// one block per fleet, in the order the panel puts its tabs in
	for (uint8_t f = 0; f < fleets; f++) {
		printf("%s\n", remotefiles_get_fleet(f));
		for (uint16_t p = 0; p < products; p++) {
			const remotefiles_product_st *prod = remotefiles_get_product(p);
			if ((prod == NULL) || (prod->fleet != f)) {
				continue;
			}
			printf("  %s  (%u)\n", prod->name,
					(unsigned int) prod->version_count);
			for (uint16_t v = 0; v < prod->version_count; v++) {
				const remotefiles_version_st *ver = &prod->versions[v];
				printf("    %-44s %-12s %8llu KB\n", ver->version,
						(ver->released[0] != '\0') ? ver->released : "-",
						(unsigned long long) (ver->size / 1024u));
			}
		}
	}
	printf("\ntotal %.2f s\n", t2 - t0);
	fflush(stdout);
}
#endif


bool cmd_serverfiles(const char *arg) {
	(void) arg;
#if !CONFIG_TARGET_WIN
	add_task(&serverfiles_task);
#else
	printf("Listing the server files is not wired up on the Windows build.\n");
#endif
	return true;
}


bool cmd_version(const char *arg) {
	(void) arg;
	printf("uvcan %s (build %u)\n",
			selfupdate_this_name(), (unsigned int) selfupdate_this_version());
	return true;
}


bool cmd_checkupdate(const char *arg) {
	(void) arg;
	selfupdate_info_st info;
	bool newer = false;
	char err[256] = "";
	if (!selfupdate_check(&info, &newer, err, sizeof(err))) {
		printf("%s\n", err);
	}
	else if (!newer) {
		printf("uvcan %s (build %u) is the newest published version.\n",
				selfupdate_this_name(),
				(unsigned int) selfupdate_this_version());
	}
	else {
		printf("uvcan %s (build %u) is available; this is %s (build %u).\n",
				info.name, (unsigned int) info.version,
				selfupdate_this_name(),
				(unsigned int) selfupdate_this_version());
		if (info.released[0] != '\0') {
			printf("  released %s\n", info.released);
		}
		if (info.notes[0] != '\0') {
			printf("  %s\n", info.notes);
		}
		printf("Install it with 'uvcan --update'.\n");
	}
	return true;
}


/// @brief: Task body for --update.
///
/// A task rather than the command callback itself, because the download logs
/// its progress and sleeps between the lines, and a command callback runs
/// BEFORE the scheduler is started -- where uv_rtos_task_delay() has no
/// scheduler to yield to and takes the process down with it. Everything else
/// here that waits on something (--ui, --sim) is a task for the same reason.
static void update_task(void *ptr) {
	(void) ptr;
	selfupdate_info_st info;
	bool newer = false;
	char err[256] = "";
	if (!selfupdate_check(&info, &newer, err, sizeof(err))) {
		printf("%s\n", err);
	}
	else if (!newer) {
		printf("uvcan %s (build %u) is already the newest published version.\n",
				selfupdate_this_name(),
				(unsigned int) selfupdate_this_version());
	}
	else if (!selfupdate_apply(&info, err, sizeof(err))) {
		printf("%s\n", err);
	}
	else {
		printf("Updated to uvcan %s (build %u). "
				"Restart uvcan to use it.\n",
				info.name, (unsigned int) info.version);
	}
	fflush(stdout);
}


bool cmd_update(const char *arg) {
	(void) arg;
	add_task(&update_task);
	return true;
}


// Where --ui stands in the task list, and whether it was given at all. A UI
// opened after a --sim is what the run then lives for, which changes what
// happens to its simulators when the window is closed (see sim_monitor).
static bool ui_given;
static int ui_task_index;

bool cmd_ui(const char *arg) {
	ui_given = true;
	ui_task_index = uv_vector_size(&dev.tasks);

	// Enable verbose PRINT output (as if -s/--silent were given) so the UI's log
	// label shows the extra debug information. `silent` defaults to true, which
	// suppresses PRINT; clearing it turns that output on.
	silent = false;

	// The UI has no controlling terminal to type the root password into, so ask
	// for it with a native graphical dialog (pkexec) when the CAN netdev needs
	// the privileged "ip link" bring-up commands.
	uv_can_use_gui_password(true);

	// Register the UI as a task instead of running it inline: command callbacks
	// run before the scheduler starts, but the UI needs the scheduler running so
	// CAN traffic is processed (heartbeat monitor, SDO reads).
	add_task(&ui_task);
	return true;
}


/// @brief: Index of the --sim task in the task list, so sim_task() can tell
/// whether any command follows it on the command line (see below).
static int sim_task_index;

/// @brief: True when a command follows --sim on the command line, i.e. when the
/// simulators have to be ready to answer before the launch is done.
static volatile bool sim_commands_follow;

/// @brief: True once --sim has launched at least one simulator, i.e. once there
/// is something for sim_monitor() to keep uvcan alive for.
static volatile bool sim_started;

/// @brief: Set by the keeper task once the simulators are up (or none started),
/// which is what --sim waits for before the next command is run.
static volatile bool sim_ready;

/// @brief: Set by the keeper task when the last simulator has stopped.
static volatile bool sim_keeper_done;


/// @brief: Launches the simulators and stays alive until the last of them has
/// stopped. This is the thread they are forked from, and it has to be: they are
/// launched with PR_SET_PDEATHSIG, which the kernel raises when the thread that
/// forked the child exits - not when the process does, which is the tempting
/// reading of it. sim_task() cannot do the launching itself, because it returns
/// as soon as the simulators are up so that the rest of the command line runs,
/// and every simulator would be killed with it.
static void sim_keeper_task(void *ptr) {
	// use the CAN device actually active in the HAL (kept in sync by --can and the
	// config window) rather than dev.can_channel, which the config window does not
	// update
	const char *chn = uv_can_get_dev();
	// On a real CAN device the simulators' frames need another node to acknowledge
	// them; with no real device on the bus they never reach uvcan, the devices
	// never come online and no parameters load. Warn before running on a non-virtual
	// device (non-interactive, so this is a heads-up, not a prompt).
	if (!simrun_can_is_virtual(chn)) {
		PRINT("WARNING: running simulators on the real CAN device '%s'. At least "
				"one real device must be present on the CAN network to acknowledge "
				"the bus messages, otherwise the simulators cannot communicate and "
				"their parameters will not load. Use a virtual device (e.g. vcan0) "
				"to simulate a standalone system.\n", chn);
	}
	uint8_t started = simrun_start_system(&dev.system, chn);
	if (started == 0) {
		PRINT("No simulators were started. Load a system with --sys (or add "
				"devices with --dev) first, and make sure each device package "
				"bundles a Linux simulator.\n");
	}
	else {
		sim_started = true;
		PRINT("Started %u simulator(s) on '%s'.\n",
				(unsigned int) started, dev.can_channel);
		// once started, load the system's bundled parameters onto the simulators
		// (waits for them to come online, then suppresses EMCY / writes / stores /
		// resets); the simulators move Started -> Loading params -> Running. The
		// --sim command simulates every device (it does not manage already-online
		// real devices the way the UI does), so no restore list is passed.
		simrun_load_params_async(&dev.system, NULL, 0);
		// wait for that load before giving the turn to the next command: it owns
		// the SDO client, which a following --loadparam would otherwise fight over
		while (!simrun_load_params_is_finished()) {
			simrun_step();
			uv_rtos_task_delay(200);
		}
		// The commands after --sim (--loadparam, --sdowrite, ...) talk to the
		// simulators over CAN, so let them answer first. Only then: with no command
		// to serve, the wait would only delay the monitoring, and a simulator which
		// moved itself to another node id (it may, see simrun_start_system) would
		// stall the start until the timeout for nothing.
		if (sim_commands_follow) {
			simrun_wait_online();
		}
		else {
			// nothing follows --sim; go straight to monitoring the simulators
		}
	}
	// the simulators are up (or there are none): the next command can run
	sim_ready = true;

	// keep monitoring until every simulator has stopped (exited or been killed);
	// reaping updates each one's state as it goes. sim_monitor() is what waits for
	// this, once every command of the command line has had its turn.
	while (simrun_any_running()) {
		simrun_step();
		uv_rtos_task_delay(200);
	}
	if (sim_started) {
		PRINT("All simulators have stopped.\n");
	}
	else {
		// nothing was started, so nothing stopped either
	}
	sim_keeper_done = true;
	uv_rtos_task_delete(NULL);
}


/// @brief: Task body for --sim. Starts the keeper task above and waits until the
/// simulators are up, so that the commands given after --sim on the command line
/// run against them. Keeping uvcan alive afterwards is sim_monitor()'s job.
static void sim_task(void *ptr) {
	sim_commands_follow = (uv_vector_size(&dev.tasks) > (sim_task_index + 1));
	uv_rtos_task_create(&sim_keeper_task, "simkeeper",
			UV_RTOS_MIN_STACK_SIZE * 5, NULL, UV_RTOS_IDLE_PRIORITY + 1, NULL);
	while (!sim_ready) {
		uv_rtos_task_delay(100);
	}
}


void sim_monitor(void) {
	// a UI opened after the simulators were started is what watches them; closing
	// its window ends the run. A UI which was already closed before --sim started
	// them (it stood earlier on the command line) is not.
	bool watched_from_ui = ui_given && (ui_task_index > sim_task_index);
	if (sim_started && !watched_from_ui) {
		PRINT("The simulators are running. Press Ctrl-C to stop them.\n");
		fflush(stdout);
		while (!sim_keeper_done) {
			uv_rtos_task_delay(200);
		}
	}
	else {
		// Nothing to keep uvcan alive for: --sim was not given, it started
		// nothing, or the simulators were watched from a UI window which the user
		// has now closed. Exiting kills them, as it does for the simulators
		// started from the UI's own "Run simulator" button.
	}
}


bool cmd_sim(const char *arg) {
	(void) arg;
	// verbose output so the launch / monitor messages are shown
	silent = false;

	// run under the scheduler (like --ui): the simulators are child processes we
	// keep monitoring while uvcan stays alive.
	// Remember where this command stands in the task list: the tasks added after
	// it are the commands given after --sim, which the simulators have to be
	// ready for.
	sim_task_index = uv_vector_size(&dev.tasks);
	add_task(&sim_task);
	return true;
}

