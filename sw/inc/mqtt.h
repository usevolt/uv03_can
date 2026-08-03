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
#include <uv_can.h>
// the CAN forwarding API below speaks the REMOTE protocol's own types
#include <uv_remote_proto.h>


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


/// @brief: Starts connecting to *host* as *username* / *password*. Returns
/// immediately - the connection completes (or fails) over the following
/// mqtt_step() calls; watch mqtt_get_state(). Any existing connection is
/// dropped first, which also forgets the fleets discovered so far.
///
/// Returns false when the client could not even be started (no host given, or
/// libmosquitto refused the parameters), in which case the state is
/// MQTT_STATE_ERROR and mqtt_get_error() has the reason.
bool mqtt_connect(const char *host, const char *username, const char *password);


/// @brief: Lists *name* as a fleet without waiting to hear from it, so its tab
/// exists before any of its devices has published anything. Called with the
/// fleets the file server said this account holds; the broker's ACL confines
/// the subscriptions to the same set, so nothing is shown that could not be
/// listened to anyway. Harmless to repeat.
void mqtt_add_fleet(const char *name);


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


/// @brief: Drops device *dev_index* of fleet *fleet_index* from the view.
///
/// The devices after it move down a slot, so whatever indexes them has to be
/// rebuilt. The device is also ignored for the rest of this broker session:
/// nothing here can stop it publishing, so without that it would be back on its
/// next heartbeat and the removal would look like it had not worked. Connecting
/// again starts from a clean slate, which is this view's equivalent of the
/// System tab's manual re-search.
///
/// @return: false when the indices name no device.
bool mqtt_remove_dev(uint8_t fleet_index, uint8_t dev_index);


// --- what a device tells us about itself ------------------------------------
//
// Devices publish a JSON heartbeat on their fleet's announce topic every few
// seconds, and REMOTE protocol messages on their own to_admin topic. The
// heartbeat is what makes an idle device show up at all: it names itself there,
// whereas the announce *topic* does not identify who published.

/// @brief: The device's own name for itself ("UV0D Display"), or "" until it
/// has been heard from. Distinct from mqtt_get_dev_name(), which is the MQTT
/// client id the broker knows it by.
const char *mqtt_get_dev_devname(uint8_t fleet_index, uint8_t dev_index);

/// @brief: Seconds since the device booted, as of its last heartbeat.
uint32_t mqtt_get_dev_uptime_s(uint8_t fleet_index, uint8_t dev_index);

/// @brief: Mask of REMOTE_IOT_FEATURE_* the device currently has switched on.
uint8_t mqtt_get_dev_features(uint8_t fleet_index, uint8_t dev_index);

/// @brief: The device's remote_iot_state_e.
uint8_t mqtt_get_dev_state(uint8_t fleet_index, uint8_t dev_index);

/// @brief: Size of the device's display, as reported when UI mirroring was
/// switched on. False until it has told us, which is why the UI window can only
/// be opened once the device has answered.
bool mqtt_get_dev_ui_size(uint8_t fleet_index, uint8_t dev_index,
		uint16_t *width, uint16_t *height);


// --- driving a device -------------------------------------------------------

/// @brief: Asks the device to switch the given REMOTE_IOT_FEATURE_* mask on.
/// The mask is absolute, not a set/clear, so resending the same one is
/// harmless. What actually took effect comes back in the heartbeat and in the
/// device's status message — the device may veto a feature locally.
///
/// @return: false when not connected or the publish was refused.
bool mqtt_dev_set_features(uint8_t fleet_index, uint8_t dev_index,
		uint8_t features);


/// @brief: Called with one complete mirrored UI frame, i.e. the compact command
/// stream between FRAME_BEGIN and FRAME_END. Runs inside mqtt_step(), on the
/// caller's thread. The buffer is owned by the client and only valid for the
/// duration of the call.
typedef void (*mqtt_ui_frame_callb_t)(uint8_t fleet_index, uint8_t dev_index,
		const uint8_t *cmds, uint32_t len, void *user);

/// @brief: Registers the sink for mirrored UI frames. Pass NULL to clear.
void mqtt_set_ui_frame_callb(mqtt_ui_frame_callb_t callb, void *user);


/// @brief: Called with a complete asset - a font, or an image - that a device
/// sent in answer to mqtt_dev_request_asset(). *data* is owned by the client
/// and only valid for the duration of the call.
///
/// A *len* of zero means the device cannot serve that asset: it is reported
/// rather than swallowed, so the caller stops asking for it.
typedef void (*mqtt_asset_callb_t)(uint8_t fleet_index, uint8_t dev_index,
		uint8_t kind, uint32_t id, const uint8_t *data, uint32_t len,
		void *user);

/// @brief: Sends a press, release, drag or scroll to a device, as if its own
/// screen had been touched. The coordinates are on the device's display.
///
/// The device is given raw presses and releases and derives clicks and drags
/// from them itself, so a drag is a press repeated at new coordinates and a
/// click is a press followed by a release.
///
/// @return: false when not connected or the publish was refused.
bool mqtt_dev_send_input(uint8_t fleet_index, uint8_t dev_index,
		uint8_t action, int16_t x, int16_t y, int16_t scroll, char key);


/// @brief: Registers the sink for assets. Pass NULL to clear.
void mqtt_set_asset_callb(mqtt_asset_callb_t callb, void *user);


/// @brief: Called when a device ends the remote session from its own end, i.e.
/// when the machine operator closed it on the device's screen. Runs inside
/// mqtt_step().
///
/// The device has already switched its features off by the time this arrives,
/// so it is not a request to be granted or refused: it is what makes the
/// difference between a session that was closed and one that merely went quiet.
typedef void (*mqtt_close_callb_t)(uint8_t fleet_index, uint8_t dev_index,
		void *user);

/// @brief: Registers the sink for device-initiated closes. Pass NULL to clear.
void mqtt_set_close_callb(mqtt_close_callb_t callb, void *user);


// --- CAN forwarding ---------------------------------------------------------
//
// A device forwards the traffic on its own CAN bus once the CAN feature is
// switched on and it has been told which messages are wanted. What arrives is
// whole CAN frames; what goes the other way is injected onto that bus.

/// @brief: Called with one CAN frame received from a device. Runs inside
/// mqtt_step(), on the caller's thread.
typedef void (*mqtt_can_callb_t)(uint8_t fleet_index, uint8_t dev_index,
		const uv_can_msg_st *msg, void *user);

/// @brief: Registers the sink for forwarded CAN frames. Pass NULL to clear.
void mqtt_set_can_callb(mqtt_can_callb_t callb, void *user);


/// @brief: Starts or stops CAN forwarding on a device. Leaves the UI feature
/// alone: the two are switched independently and neither owns the other.
bool mqtt_dev_set_can_active(uint8_t fleet_index, uint8_t dev_index,
		bool active);

/// @brief: True while the CAN feature is in effect on a device, as the device
/// itself last reported it.
bool mqtt_dev_get_can_active(uint8_t fleet_index, uint8_t dev_index);


/// @brief: The rxconf negotiation: what the device should forward.
///
/// Sent as a round — clear, then one message per filter, then done — because
/// the device applies each filter as it arrives and has no other way of knowing
/// when the set is complete. A mask of zero means every id of that type, which
/// is the only way to ask for a whole bus.
bool mqtt_dev_send_rxclear(uint8_t fleet_index, uint8_t dev_index);
bool mqtt_dev_send_rxconf(uint8_t fleet_index, uint8_t dev_index,
		uint32_t id, uint32_t mask, uv_can_msg_types_e type);
bool mqtt_dev_send_rxdone(uint8_t fleet_index, uint8_t dev_index);


/// @brief: Sends one CAN frame to a device, which puts it on its own bus.
bool mqtt_dev_send_can(uint8_t fleet_index, uint8_t dev_index,
		const uv_can_msg_st *msg);


/// @brief: The device's own account of what it has forwarded and dropped, per
/// traffic class. Only the device can know this: a frame it dropped looks from
/// here exactly like one that was never sent.
///
/// @return: false when the device has not reported any yet.
bool mqtt_get_dev_can_stats(uint8_t fleet_index, uint8_t dev_index,
		remote_can_stats_st *dest);

/// @brief: Asks a device for the asset (*kind*, *id*) it is drawing with but
/// this end does not have. The answer arrives at the callback above.
///
/// @return: false when not connected or the publish was refused.
bool mqtt_dev_request_asset(uint8_t fleet_index, uint8_t dev_index,
		uint8_t kind, uint32_t id);

/// @brief: Starts or stops collecting mirrored UI frames from a device. While
/// collecting, complete frames go to the callback above. Enabling also asks the
/// device to start mirroring, and disabling asks it to stop.
bool mqtt_dev_set_ui_active(uint8_t fleet_index, uint8_t dev_index,
		bool active);

/// @brief: True while frames from this device are being collected.
bool mqtt_dev_get_ui_active(uint8_t fleet_index, uint8_t dev_index);


#endif /* MQTT_H_ */
