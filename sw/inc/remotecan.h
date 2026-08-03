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


#ifndef REMOTECAN_H_
#define REMOTECAN_H_


#include <stdbool.h>
#include <stdint.h>
#include <uv_can.h>


/// @file: Puts a remote machine's CAN bus on this computer as a network
/// interface of its own.
///
/// The bus of a device in the fleet is carried over the broker as whole CAN
/// frames. Rather than mixing those into the bus uvcan itself is connected to —
/// which would put a machine in the field on the same wire as whatever is on
/// the bench, node ids and all — the bridge creates a virtual CAN interface and
/// puts them there. Everything that speaks SocketCAN then reaches the remote
/// bus: `candump uvremote0`, `cansend`, or a second uvcan started with
/// `-c uvremote0`, which gets its whole device view of a machine on the other
/// side of the country.
///
/// The interface exists only while the bridge is up. It is created when
/// bridging starts and removed when it stops, when the device closes the
/// session, or when uvcan exits — an interface left behind would look like a
/// bus that is still there.
///
/// Linux only: it is SocketCAN that makes this possible at all. Every call is a
/// no-op elsewhere.


/// @brief: Name of the interface the remote bus appears as.
#define REMOTECAN_IFNAME		"uvremote0"

/// @brief: How many filter entries the table holds.
#define REMOTECAN_FILTER_MAX	8


/// @brief: One filter entry: the ids whose bits under *mask* equal *id*, of
/// this frame type. A mask of zero matches every id of that type.
typedef struct {
	uint32_t id;
	uint32_t mask;
	uv_can_msg_types_e type;
} remotecan_filter_st;


/// @brief: Starts bridging the CAN bus of one device.
///
/// Creates the interface, asks the device to start forwarding, and tells it
/// which messages are wanted (the filter table below). Any bridge already
/// running is stopped first — one remote bus at a time, since there is one
/// interface.
///
/// @return: false if the interface could not be created or opened, in which
/// case remotecan_get_error() says why and nothing was started.
bool remotecan_start(uint8_t fleet_index, uint8_t dev_index);


/// @brief: Stops bridging: tells the device to stop forwarding, closes the
/// socket and removes the interface. Safe to call when nothing is running.
void remotecan_stop(void);


/// @brief: Removes the interface and drops the socket without talking to the
/// broker, for the path where the process is about to die anyway.
///
/// The signal handler cannot publish: it is not the place to be waiting on a
/// network client, and the device switches everything off by itself as soon as
/// the broker session goes. What must not be left behind is the interface.
void remotecan_shutdown(void);


/// @brief: True while a bridge is up.
bool remotecan_is_active(void);

/// @brief: True while the bridge is up and showing this particular device.
bool remotecan_shows(uint8_t fleet_index, uint8_t dev_index);


/// @brief: Pumps the bridge: moves whatever has been written to the interface
/// on to the device. Call once per UI cycle. Frames coming the other way arrive
/// through the MQTT client's callback and need no pumping.
void remotecan_step(void);


/// @brief: Name of the interface in use, or "" when nothing is bridged.
const char *remotecan_get_ifname(void);

/// @brief: Why the last start failed. "" when it did not.
const char *remotecan_get_error(void);


/// @brief: Frames carried each way since the bridge started, and how many were
/// refused by the filter on this end.
uint32_t remotecan_get_rx_count(void);
uint32_t remotecan_get_tx_count(void);
uint32_t remotecan_get_filtered_count(void);
/// @brief: Frames that could not be written to the interface or published.
uint32_t remotecan_get_error_count(void);


/// @brief: Which kinds of frame are carried, in both directions.
///
/// Standard (11 bit) frames are on by default and extended (29 bit) ones are
/// not: a J1939 bus is mostly extended frames at a rate no remote link is going
/// to keep up with, and on most machines none of it is what somebody connecting
/// from far away came to look at. Turning both off carries nothing, which is a
/// legitimate way to hold a bridge open and quiet.
///
/// Changing either while a bridge is up re-negotiates with the device, so it
/// takes effect on what the device sends as well as on what is accepted here.
bool remotecan_get_allow_std(void);
void remotecan_set_allow_std(bool value);
bool remotecan_get_allow_ext(void);
void remotecan_set_allow_ext(bool value);


/// @brief: The filter table, which decides what is carried in both directions.
///
/// Sent to the device as the rxconf round, so it also decides what the device
/// bothers to put on the link — filtering here rather than only on arrival is
/// the point: the link is the scarce thing.
uint8_t remotecan_get_filter_count(void);
const remotecan_filter_st *remotecan_get_filter(uint8_t index);

/// @brief: Replaces the whole filter table, re-negotiating with the device if
/// a bridge is up. Passing a count of zero restores the default, which is every
/// frame of whichever types are allowed above.
void remotecan_set_filters(const remotecan_filter_st *filters, uint8_t count);


#endif /* REMOTECAN_H_ */
