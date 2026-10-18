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


#ifndef JOYSTICK_H_
#define JOYSTICK_H_


#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <uv_can.h>
#include "system.h"


/// @brief: The joystick driver: a virtual game controller on the computer whose
/// axes and buttons follow values of the system's CAN-bus devices.
///
/// Every axis and button of the virtual controller can be bound to one object of
/// one device's object dictionary -- an input such as "AIN1 Value" or an output
/// such as a valve's current -- which the device sends in one of its TXPDOs. The
/// values are picked out of the PDOs as they go by on the bus; nothing is ever
/// requested from the devices, so the driver adds no traffic of its own.
///
/// The controller is a uinput device on Linux and a vJoy device on Windows (the
/// vJoy driver has to be installed; uvcan loads its vJoyInterface.dll at run
/// time). There is none on the web build.
///
/// The configuration is kept in the system package, as joystick.json (or
/// joystick.yaml) next to the uvsys manifest:
///
///   {
///     "VERSION": 1,
///     "NAME": "uvcan joystick",
///     "AXES": [
///       { "AXIS": "X", "NODEID": "0x11", "DEVICE": "UV1F HHC",
///         "OBJECT": "AIN1 Value", "MAININDEX": "0x2100", "SUBINDEX": 0,
///         "TYPE": "CANOPEN_SIGNED16", "COBID": "0x191", "OFFSET": 0,
///         "MIN": -1000, "MAX": 1000, "INVERT": false }
///     ],
///     "BUTTONS": [
///       { "BUTTON": 1, "NODEID": "0x11", "DEVICE": "UV1F HHC",
///         "OBJECT": "DIN1 Value", "MAININDEX": "0x2110", "SUBINDEX": 0,
///         "TYPE": "CANOPEN_UNSIGNED8", "COBID": "0x191", "OFFSET": 12,
///         "THRESHOLD": 1, "INVERT": false }
///     ]
///   }
///
/// See JOYSTICK_AXIS_NAMES for the axis names. An axis maps MIN..MAX onto its
/// full travel; a button is pressed while the value is THRESHOLD or above
/// (below, with INVERT). COBID is the TXPDO the value comes in, OFFSET its first
/// byte there and TYPE its size and signedness: those are what the value is
/// decoded with. NODEID, MAININDEX and SUBINDEX name the object, DEVICE and
/// OBJECT tell a reader what is bound.


/// @brief: Number of axes and buttons of the virtual controller
#define JOYSTICK_AXIS_COUNT			8
#define JOYSTICK_BUTTON_COUNT		32
#define JOYSTICK_SLOT_COUNT			(JOYSTICK_AXIS_COUNT + JOYSTICK_BUTTON_COUNT)

/// @brief: Axis names in the configuration file, in axis order: X, Y, Z, RX,
/// RY, RZ, SLIDER1 and SLIDER2 (the throttle and rudder on Linux, the two
/// sliders on vJoy)
extern const char *const JOYSTICK_AXIS_NAMES[JOYSTICK_AXIS_COUNT];

/// @brief: The range an axis value is given to the backend in
#define JOYSTICK_AXIS_MAX			32767

/// @brief: Name of the configuration file inside a .uvsys package, without its
/// extension (JSON is written; either format is read)
#define JOYSTICK_FILE_NAME			"joystick"

/// @brief: The controller's name when the file does not give one
#define JOYSTICK_DEFAULT_NAME		"uvcan joystick"


/// @brief: One axis or button of the controller and the object it follows
typedef struct {
	/// @brief: Node id of the device, 0 when the slot is not used
	uint8_t nodeid;
	/// @brief: The device's name, for the reader of the file only
	char device[64];
	/// @brief: The object's name, for the reader of the file only
	char object[96];
	uint16_t mindex;
	uint8_t sindex;
	/// @brief: Size of the value in bytes (1, 2 or 4) and its signedness
	uint8_t size;
	bool is_signed;
	/// @brief: COB-ID of the TXPDO the value comes in, and its byte offset there
	uint16_t cobid;
	uint8_t offset;
	/// @brief: Axis: the device values at the two ends of the axis
	int32_t min;
	int32_t max;
	/// @brief: Button: the value from which on the button is pressed
	int32_t threshold;
	/// @brief: Axis: reverse the direction. Button: pressed below the threshold.
	bool invert;
} joystick_slot_st;


/// @brief: The whole controller configuration. Slots 0 ... JOYSTICK_AXIS_COUNT - 1
/// are the axes, the rest the buttons.
typedef struct {
	char name[64];
	joystick_slot_st slots[JOYSTICK_SLOT_COUNT];
} joystick_conf_st;


/// @brief: One object of a device's object dictionary which can drive an axis
/// or a button: an integer value, or one entry of an integer array, which the
/// device sends in a TXPDO. Built from the device's .uvdev package by
/// joystick_catalog_build().
typedef struct {
	char name[96];
	uint16_t mindex;
	uint8_t sindex;
	uint8_t size;
	bool is_signed;
	int32_t min;
	int32_t max;
	/// @brief: COB-ID of the TXPDO the device sends the value in, and its byte
	/// offset in that PDO
	uint16_t pdo_cobid;
	uint8_t pdo_offset;
} joystick_obj_st;


static inline bool joystick_slot_is_axis(uint8_t slot) {
	return slot < JOYSTICK_AXIS_COUNT;
}

static inline bool joystick_slot_is_used(const joystick_slot_st *slot) {
	return slot->nodeid != 0;
}

/// @brief: Empties *conf*: no slot used, the default name
void joystick_conf_reset(joystick_conf_st *conf);

/// @brief: True when any slot of *conf* is used
bool joystick_conf_is_used(const joystick_conf_st *conf);

/// @brief: Reads a configuration file. Returns false, with the reason printed,
/// when the file cannot be read.
bool joystick_conf_read(joystick_conf_st *conf, const char *path);

/// @brief: Writes *conf* to *path*, JSON or YAML by the extension
bool joystick_conf_write(const joystick_conf_st *conf, const char *path);

/// @brief: Fills *slot* with the object *obj* of *device*, with the axis range
/// or the button threshold the object's own limits suggest
void joystick_slot_set_obj(joystick_slot_st *slot, uint8_t index,
		const device_st *device, const joystick_obj_st *obj);


/// @brief: Reads the objects that can drive the controller -- the ones mapped
/// to the device's TXPDOs, in PDO order -- out of *device*'s .uvdev package. Returns a malloc'd array the caller free()s, and the number
/// of its entries in *count*; NULL when the device has no package or it could
/// not be read. Parses the database into dev.db, so it is called from the UI or
/// a command, never from the bridge task.
joystick_obj_st *joystick_catalog_build(const device_st *device, uint16_t *count);


/// @brief: The configuration in use: the one loaded from the system file, or
/// the one last saved from the configuration window
const joystick_conf_st *joystick_get_conf(void);

/// @brief: True when a configuration with something bound is in use
bool joystick_has_conf(void);

/// @brief: Takes *conf* into use, replacing the one in use. The driver is
/// (re)started with it at the next joystick_step(), or stopped when nothing is
/// bound.
void joystick_set_conf(const joystick_conf_st *conf);

/// @brief: Looks for a joystick configuration file in the extracted system
/// package *dir* and takes it into use. Returns true when there was one. Called
/// when a .uvsys file is loaded.
bool joystick_load_from_dir(const char *dir);

/// @brief: Writes the configuration in use into the directory *dir* as
/// joystick.json. Returns false when there is nothing to write or the write
/// fails. *name_out*, when not NULL, receives the file name.
bool joystick_write_to_dir(const char *dir, char *name_out, size_t name_len);


/// @brief: Saves the configuration in use into the system file loaded last,
/// in place: joystick.json is written into the package's extraction directory
/// and put into the .uvsys archive. Returns false when no system file is loaded
/// or the save failed (the reason is printed). Blocks for the archive update,
/// so it is called from the UI or a command.
bool joystick_save_to_system(system_st *sys);

/// @brief: Starts the driver with the configuration in use when it is not
/// running yet or the configuration changed, and stops it when nothing is bound.
/// Called every UI cycle (and by *joystick* on the command line).
void joystick_step(void);

/// @brief: Stops the driver and removes the virtual controller
void joystick_stop(void);

/// @brief: True while the virtual controller exists
bool joystick_is_running(void);

/// @brief: A line telling the state of the driver ("Joystick driver running
/// (uinput): 2 axes, 3 buttons", or why it could not start)
const char *joystick_get_status(void);

/// @brief: The live state of slot *index* while the driver runs: the device's
/// value in *raw* and what the controller is given in *out* (-32767 ... 32767
/// for an axis, 0 / 1 for a button). Returns false when the slot has no value
/// (not used, the driver not running, or the device not heard from).
bool joystick_get_live(uint8_t index, int32_t *raw, int32_t *out);

/// @brief: CAN rx hook, called for every received frame by the heartbeat
/// monitor (find.c). Picks the bound values out of the TXPDOs.
void joystick_can_rx(const uv_can_msg_st *msg);


/// @brief: --joystick: runs the joystick driver with the configuration of the
/// system loaded with *sys* (or of the file given as the argument) until
/// Ctrl-C.
bool cmd_joystick(const char *arg);


/// @brief: The virtual controller of the operating system. Implemented in
/// jsdriver.c for uinput (Linux) and vJoy (Windows).

/// @brief: Creates the controller. *err* receives the reason on failure.
bool jsdriver_open(const char *name, char *err, size_t err_len);
/// @brief: Name of the backend, for the status line
const char *jsdriver_name(void);
/// @brief: Sets an axis, -JOYSTICK_AXIS_MAX ... JOYSTICK_AXIS_MAX
void jsdriver_set_axis(uint8_t axis, int32_t value);
/// @brief: Sets a button, *index* counting from 0
void jsdriver_set_button(uint8_t index, bool pressed);
/// @brief: Sends what was set since the last sync
void jsdriver_sync(void);
/// @brief: Removes the controller
void jsdriver_close(void);


#endif /* JOYSTICK_H_ */
