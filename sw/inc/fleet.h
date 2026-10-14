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


#ifndef FLEET_H_
#define FLEET_H_


#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


/// @file: The remote fleet session: logging in to the Usevolt account, finding
/// a device of the fleet, and the remote UI and remote CAN sessions opened with
/// it.
///
/// One implementation serves both ends of uvcan: the command line options
/// (--login, --remotelist, --remoteui, --remotecan, see fleetcmd.c) and the UI
/// (the Settings tab's Account panel and the Fleet tab). The broker client
/// itself is mqtt.c and the CAN bridge remotecan.c; this is what sits on top
/// of the two and is common to every user of them.
///
/// Devices are addressed by their (fleet, device) index into the broker
/// client's view, as everywhere else. Those indices move when a device is
/// removed from the view, which is why removal goes through fleet_remove_dev().


/// @brief: The name of an account's main fleet. The UI lists it before the
/// others: it is the first tab of both the Fleet tab and the server files window.
#define FLEET_MAIN_NAME		"main"


/// @brief: Outcome of fleet_login().
typedef struct {
	// the file server accepted the account and said which fleets it holds
	bool files_ok;
	// the broker connect was started. It completes over the following pump
	// cycles: mqtt_get_state() or fleet_wait_connected() tells how it went
	bool broker_ok;
	// why the file server login failed, "" when it did not
	char err[256];
} fleet_login_st;


/// @brief: Logs in to the file server and starts the broker connection with
/// the same account. Blocks for as long as the file server login takes, which
/// is seconds against a slow server, so the UI calls it from a task of its own.
///
/// Logs what it does to stdout. Any existing broker session is dropped first.
/// The broker client has to be pumped (mqtt_start_pump()) for the connection
/// to complete.
void fleet_login(const char *files_url, const char *broker_url,
		const char *username, const char *password, fleet_login_st *result);


/// @brief: Takes a finished fleet_login() over: lists the fleets the file
/// server said the account holds, so they show before any of their devices has
/// published. Called on the thread that reads the fleet list (the UI thread).
/// When the broker could not be started and the file server gave no reason,
/// the broker's reason is copied into *result*.
void fleet_login_finish(fleet_login_st *result);


/// @brief: Waits for the broker connection started by fleet_login() to
/// complete. For the command line: the UI watches mqtt_get_state() instead.
///
/// @return: true once connected. false on a failure or after *timeout_ms*, with
/// *err* (size *err_len*) saying which.
bool fleet_wait_connected(uint32_t timeout_ms, char *err, size_t err_len);


/// @brief: Finds a device of the fleet view by *spec*, which may be
///  - "<fleet>/<client id>", exact,
///  - a client id,
///  - the device's own name for itself (as in its heartbeat), or
///  - the identification name its operator gave it,
/// the latter three matched case-insensitively across every fleet.
///
/// @return: true when exactly one device matches. Otherwise *err* (size
/// *err_len*) says that none or several did.
bool fleet_find_dev(const char *spec, uint8_t *fleet_index,
		uint8_t *dev_index, char *err, size_t err_len);


/// @brief: Writes the full, multi-line description of a device into *buf*:
/// name, fleet, state, uptime, what is switched on, and the CAN forwarding
/// statistics while its bus is bridged. What the Fleet tab shows on a device's
/// tab.
void fleet_dev_info(uint8_t fleet_index, uint8_t dev_index,
		char *buf, size_t len);


/// @brief: Writes a one-line summary of a device into *buf*, for listings.
/// Columns line up with fleet_dev_summary_header().
void fleet_dev_summary(uint8_t fleet_index, uint8_t dev_index,
		char *buf, size_t len);
const char *fleet_dev_summary_header(void);


/// @brief: Takes a device out of the view (see mqtt_remove_dev()), stopping
/// the remote UI and CAN sessions held with it and keeping the remote UI
/// session pointed at the right device when the ones after it move down a slot.
///
/// @return: false when the indices name no device.
bool fleet_remove_dev(uint8_t fleet_index, uint8_t dev_index);


/// @brief: Starts mirroring a device's display. The window can only be sized
/// once the device has said how big its display is, so this asks it to start
/// and fleet_step() opens the window when the answer arrives. Stops any other
/// device being mirrored: there is one window.
///
/// @return: false when the request could not be sent.
bool fleet_ui_start(uint8_t fleet_index, uint8_t dev_index);


/// @brief: Stops mirroring, closing the window and telling the device to stop
/// sending. Safe to call when nothing is mirrored.
void fleet_ui_stop(void);


/// @brief: True while a remote UI session is held with any device, i.e. from
/// fleet_ui_start() until it is stopped: by fleet_ui_stop(), by the user
/// closing the window, or by the device ending the session.
bool fleet_ui_is_active(void);


/// @brief: True while the mirror window is open on this particular device.
bool fleet_ui_shows(uint8_t fleet_index, uint8_t dev_index);


/// @brief: Pumps the sessions. Call it regularly, from one thread (the UI
/// thread, or the command line task holding a session):
///  - runs what the broker client left for this thread (mqtt_ui_step()): UI
///    frames, assets, and a device's request to end the session,
///  - ends the CAN bridge when the broker connection is gone,
///  - opens the mirror window once the device's display size is known, and
///    notices the user closing it.
///
/// @return: true when a session started or ended during the call, so that a
/// view of them knows to refresh.
bool fleet_step(void);


#endif /* FLEET_H_ */
