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


#ifndef CREDENTIALS_H_
#define CREDENTIALS_H_


#include <stdbool.h>
#include <stddef.h>


/// @brief: Account credentials (username + password) persisted in a per-user file
/// that is shared by every uvcan install on this computer, so entering them once -
/// in the UI "Account" panel or with the --user / --pwd command-line options -
/// makes them available to all of them and to later runs.
///
/// There are two completely separate accounts, stored side by side in the same
/// file and never mixed:
///  - the *file server* account (credentials_get_* / credentials_set_*), used by
///    the system tab's Account panel and the "Server files" browser;
///  - the *fleet* account (credentials_fleet_*), used by the Fleet tab to log in
///    to the MQTT broker.
///
/// The credentials are stored in PLAIN TEXT; they are NOT encrypted. The file lives
/// at a fixed per-user location every uvcan looks in:
///   Windows: %APPDATA%\uvcan\account.conf   (or %USERPROFILE%\uvcan\...)
///   others:  $XDG_CONFIG_HOME/uvcan/account.conf, else $HOME/.config/uvcan/...


/// @brief: Maximum stored length, including the null terminator, of the username
/// and of the password.
#define CREDENTIALS_MAX		256


/// @brief: Server addresses the two accounts default to when nothing is stored
/// yet. The user can overwrite them in the UI; an emptied field falls back to
/// these again on the next start.
#define CREDENTIALS_URL_DEFAULT			"files.usevolt.fi"
#define CREDENTIALS_FLEET_URL_DEFAULT	"mqtt.usevolt.fi"


/// @brief: Loads the stored credentials from the shared file into memory. Call once
/// at startup, before the command-line options are processed (so --user / --pwd can
/// override them). A missing or unreadable file leaves both empty.
void credentials_init(void);


/// @brief: Builds the path of *filename* inside uvcan's per-user configuration
/// directory - the one holding account.conf, see above - into *out* (size *len*),
/// creating the directory if it does not exist. Returns false when no home
/// directory is known from the environment, in which case *out* is untouched.
/// Used for the other small per-user files uvcan keeps beside the account, e.g.
/// the MQTT broker's CA certificate.
bool credentials_config_path(char *out, size_t len, const char *filename);


/// @brief: The current in-memory username / password / server URL. Never NULL;
/// "" when unset. The URL is the base address of the Usevolt file server the UI's
/// "Server files" browser connects to (e.g. "https://files.usevolt.fi").
const char *credentials_get_username(void);
const char *credentials_get_password(void);
const char *credentials_get_url(void);


/// @brief: Sets the username / password / server URL and persists it to the shared
/// file. Used by both the UI (the system tab's Account panel and URL field) and the
/// --user / --pwd command-line options, so the two are equivalent. A NULL argument
/// is treated as an empty string.
void credentials_set_username(const char *username);
void credentials_set_password(const char *password);
void credentials_set_url(const char *url);


/// @brief: The fleet account: the MQTT broker address and the username /
/// password used on it. Never NULL; the URL falls back to
/// CREDENTIALS_FLEET_URL_DEFAULT, the rest to "".
///
/// There is deliberately no fleet name here. Which fleets an account may see is
/// the server's decision, asked for at login (remotefiles_get_fleet()) and
/// enforced by the broker's ACL - so there is nothing local to keep in step
/// with it, and nothing for the user to get wrong.
const char *credentials_fleet_get_url(void);
const char *credentials_fleet_get_username(void);
const char *credentials_fleet_get_password(void);


/// @brief: Sets a fleet account field and persists it to the shared file, so it
/// is still there the next time uvcan is started on this computer. A NULL
/// argument is treated as an empty string.
void credentials_fleet_set_url(const char *url);
void credentials_fleet_set_username(const char *username);
void credentials_fleet_set_password(const char *password);


#endif /* CREDENTIALS_H_ */
