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


#ifndef LOADESP32_H_
#define LOADESP32_H_


#include <stdbool.h>
#include <stdint.h>


/// @file: Flashes the ESP32 module firmware bundled in a .uvdev package (the
/// manifest's ESP32 key, see makeuvdev.h) over the module's USB serial port
/// with esptool, the way the device projects' 'make esp' target does:
///
///   1. the device hosting the module is asked over the CAN terminal to leave
///      the module alone ('esp "comm" 0'). A running device resets an ESP32
///      which does not answer, and every reset drops the module's USB port
///      under esptool. No answer (the device is not on the bus, or the module
///      is on the bench) is fine.
///   2. esptool writes every image of the package to its flash offset.
///   3. the device is let to talk to the module again ('esp "comm" 1'), which
///      resets the module so it boots the new firmware.
///
/// esptool v5 or newer has to be installed and on the PATH (pip install
/// esptool).


/// @brief: --esp32port: The serial port of the ESP32 module. Optional. When not
/// given, an Espressif USB port is looked for under /dev/serial/by-id, and when
/// none is found esptool scans the serial ports itself.
bool cmd_esp32port(const char *arg);

/// @brief: --loadesp32: Flashes the ESP32 firmware of a .uvdev package. The
/// argument is the package, optionally with a ':<nodeid>' suffix naming the
/// device which hosts the module. Without an argument the device loaded with
/// --dev / --sys whose package bundles ESP32 firmware is flashed.
bool cmd_loadesp32(const char *arg);


/// @brief: Flashes the ESP32 firmware bundled in the .uvdev package at
/// *uvdev_path* to the module hosted by the device at *nodeid*. Blocks until
/// esptool is done; call from a task. Returns true when esptool succeeded.
bool loadesp32_uvdev(const char *uvdev_path, uint8_t nodeid);


/// @brief: Runs loadesp32_uvdev() on a task of its own. Poll
/// loadesp32_load_device_is_finished(). Intended for the UI (the device tab's
/// "Flash ESP32 firmware" button).
void loadesp32_load_device_async(const char *uvdev_path, uint8_t nodeid);


/// @brief: Returns true when no asynchronous ESP32 flash is running. True
/// before any start.
bool loadesp32_load_device_is_finished(void);


#endif /* LOADESP32_H_ */
