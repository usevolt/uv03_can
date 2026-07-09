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


/// @brief: Account credentials (username + password) persisted in a per-user file
/// that is shared by every uvcan install on this computer, so entering them once -
/// in the UI "Account" panel or with the --user / --pwd command-line options -
/// makes them available to all of them and to later runs.
///
/// The credentials are stored in PLAIN TEXT; they are NOT encrypted. The file lives
/// at a fixed per-user location every uvcan looks in:
///   Windows: %APPDATA%\uvcan\account.conf   (or %USERPROFILE%\uvcan\...)
///   others:  $XDG_CONFIG_HOME/uvcan/account.conf, else $HOME/.config/uvcan/...


/// @brief: Maximum stored length, including the null terminator, of the username
/// and of the password.
#define CREDENTIALS_MAX		256


/// @brief: Loads the stored credentials from the shared file into memory. Call once
/// at startup, before the command-line options are processed (so --user / --pwd can
/// override them). A missing or unreadable file leaves both empty.
void credentials_init(void);


/// @brief: The current in-memory username / password. Never NULL; "" when unset.
const char *credentials_get_username(void);
const char *credentials_get_password(void);


/// @brief: Sets the username / password and persists it to the shared file. Used by
/// both the UI "Account" panel and the --user / --pwd command-line options, so the
/// two are equivalent. A NULL argument is treated as an empty string.
void credentials_set_username(const char *username);
void credentials_set_password(const char *password);


#endif /* CREDENTIALS_H_ */
