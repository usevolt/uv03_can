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


#ifndef FLEETCMD_H_
#define FLEETCMD_H_


#include <stdbool.h>


/// @file: Command line access to the remote fleet: the --login, --remotelist,
/// --remoteui and --remotecan options. A thin layer over fleet.h, which the
/// UI's Account panel and Fleet tab use as well.
///
/// Every one of them logs in with the stored account (see --user / --pwd) if
/// no broker session is open yet, so `uvcan --remotelist` works on its own;
/// --login is there to log in explicitly, or just to check the account.


bool cmd_login(const char *arg);
bool cmd_remotelist(const char *arg);
bool cmd_remoteui(const char *arg);
bool cmd_remotecan(const char *arg);


#endif /* FLEETCMD_H_ */
