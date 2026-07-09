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


#ifndef SERVERFILES_WIN_H_
#define SERVERFILES_WIN_H_


#include <uv_ui.h>


/// @brief: Opens the modal "Server files" window: logs in to the Usevolt file
/// server with the stored account credentials, lists the account's files as a tree
/// (product -> versions with metadata) and lets the user download a version. Blocks
/// until the window is closed. On a login/list failure it shows an error dialog and
/// returns. The caller must ensure the username, password and server URL are set
/// (see credentials.h) before calling.
void serverfiles_win_exec(const uv_uistyle_st *style);


#endif /* SERVERFILES_WIN_H_ */
