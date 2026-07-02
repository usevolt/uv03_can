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


#ifndef UI_TERMINALTAB_H_
#define UI_TERMINALTAB_H_


#include <uv_hal_config.h>

#if CONFIG_UI

#include <uv_ui.h>
#include <stdint.h>


/// @brief: Builds the "Terminal" sub-tab view — a scrollable window that shows
/// the characters the device sends, and a permanently-focused command line under
/// it — into the content area of *parent* (the sub-tab's tab window), targeting
/// the device at *nodeid*. The received-text history is kept in a persistent
/// buffer, so leaving and re-entering the tab keeps what was already printed.
///
/// Communicates over the Usevolt SDO reply terminal protocol (object 0x5FFF),
/// the same one the `uvcan -n <nodeid> -t` command uses. The first call also
/// installs the CAN receive sniffer and starts the background transmit task.
void terminaltab_build(void *parent, uint8_t nodeid);


/// @brief: Must be called every UI cycle while the terminal view is built. Pumps
/// the characters received from the device into the scrollable view. When
/// *focused* is true the command line owns the keyboard: typed characters are
/// captured and Enter sends the line to the device (focus is kept). Pass false
/// when the terminal is not the visible, front-most view (e.g. the log view is
/// expanded over it) so it does not steal keyboard input.
void terminaltab_step(bool focused);


#endif

#endif /* UI_TERMINALTAB_H_ */
