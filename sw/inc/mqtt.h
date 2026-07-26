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


#ifndef MQTT_H_
#define MQTT_H_


#include <stdbool.h>
#include <stdint.h>


/// @brief: MQTT client for the Usevolt IoT broker (see the uv3b_iotbrkr project),
/// used by the Fleet tab. Connects over TLS to port MQTT_PORT as a fleet admin and
/// listens to the fleet topics to learn which fleets and devices are out there:
///
///   fleets/<fleet>/announce                    - a device joining a fleet
///   fleets/<fleet>/clients/<client>/to_admin   - device -> admin traffic
///   fleets/<fleet>/clients/<client>/to_dev     - admin -> device traffic
///
/// Every fleet name seen in a topic becomes a fleet, and every client id below it
/// a device of that fleet. The broker's ACL confines a fleet admin to their own
/// fleet, so an ordinary account discovers exactly one fleet; the `uvadmin`
/// superuser sees all of them. Neither `announce` nor `to_admin` is retained, so
/// nothing shows up until a device actually publishes - the fleet named in the
/// account settings is therefore always listed, whether it has spoken or not.
///
/// The broker uses a private CA, whose certificate is compiled into uvcan
/// (certs/uvca.crt) and written next to the account file for libmosquitto to read.
///
/// Everything runs on the caller's thread: mqtt_step() must be called regularly
/// (once per UI cycle) to pump the client. This deliberately avoids libmosquitto's
/// own network thread, which would live outside the FreeRTOS POSIX scheduler.
///
/// Linux only - there is no mingw build of libmosquitto here. On Windows every
/// call is a no-op and mqtt_is_supported() returns false.


/// @brief: TLS port the broker listens on (mosquitto.conf "listener 8883").
#define MQTT_PORT				8883

/// @brief: Maximum number of fleets, and of devices per fleet, that are tracked.
#define MQTT_MAX_FLEETS			16
#define MQTT_MAX_DEVS			64

/// @brief: Maximum stored length of a fleet name / client id, including the
/// terminating null.
#define MQTT_NAME_MAX			64


/// @brief: Connection state of the client.
typedef enum {
	// not connected and not trying to
	MQTT_STATE_DISCONNECTED = 0,
	// connect in progress: TCP/TLS handshake and MQTT CONNECT
	MQTT_STATE_CONNECTING,
	// connected and subscribed; the fleet list is being filled in
	MQTT_STATE_CONNECTED,
	// the connection attempt failed or an established connection dropped.
	// mqtt_get_error() tells why
	MQTT_STATE_ERROR,
} mqtt_state_e;


/// @brief: True when this build can talk to a broker at all (false on Windows).
bool mqtt_is_supported(void);


/// @brief: Starts connecting to *host* as fleet admin *username* / *password*.
/// Returns immediately - the connection completes (or fails) over the following
/// mqtt_step() calls; watch mqtt_get_state(). *fleet*, when not empty, is listed
/// as a fleet right away so its tab exists before any device has published.
/// Any existing connection is dropped first.
///
/// Returns false when the client could not even be started (no host given, or
/// libmosquitto refused the parameters), in which case the state is
/// MQTT_STATE_ERROR and mqtt_get_error() has the reason.
bool mqtt_connect(const char *host, const char *username, const char *password,
		const char *fleet);


/// @brief: Closes the connection and forgets the discovered fleets and devices.
/// Safe to call when not connected.
void mqtt_disconnect(void);


/// @brief: Pumps the client: performs the network I/O and runs the callbacks that
/// update the connection state and the fleet list. Call once per UI cycle. Cheap
/// and non-blocking when there is nothing to do.
void mqtt_step(void);


/// @brief: Current connection state.
mqtt_state_e mqtt_get_state(void);

/// @brief: True while the client is connected to the broker.
bool mqtt_is_connected(void);

/// @brief: Short human-readable reason for the last failure. "" when there was
/// none. Meaningful in MQTT_STATE_ERROR.
const char *mqtt_get_error(void);

/// @brief: The host the client is connected (or connecting) to.
const char *mqtt_get_host(void);


/// @brief: Number of fleets discovered so far (plus the one named in the account
/// settings, which is always listed).
uint8_t mqtt_get_fleet_count(void);

/// @brief: Name of the fleet at *index*, or "" when out of range.
const char *mqtt_get_fleet_name(uint8_t index);

/// @brief: Number of devices seen in the fleet at *fleet_index*.
uint8_t mqtt_get_dev_count(uint8_t fleet_index);

/// @brief: Client id of device *dev_index* of the fleet at *fleet_index*, or ""
/// when either index is out of range.
const char *mqtt_get_dev_name(uint8_t fleet_index, uint8_t dev_index);

/// @brief: Number of messages received from device *dev_index* of fleet
/// *fleet_index*, and the wall-clock seconds since the last one (0 when it has
/// only just been seen). Lets the UI show that a device is actually alive.
uint32_t mqtt_get_dev_msg_count(uint8_t fleet_index, uint8_t dev_index);
uint32_t mqtt_get_dev_age_s(uint8_t fleet_index, uint8_t dev_index);


/// @brief: Returns true once when the connection state or the discovered fleet /
/// device tree changed since the last call (the flag is cleared by the read), so
/// the UI knows when to rebuild its tabs.
bool mqtt_poll_changed(void);


#endif /* MQTT_H_ */
