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


#ifndef UI_VERSIONNOTES_WIN_H_
#define UI_VERSIONNOTES_WIN_H_


#include <uv_hal_config.h>

#if CONFIG_UI

#include <uv_ui.h>


/// @brief: Opens the modal "Version notes" window: what this uvcan adds to the
/// build that was last published, one line per commit, newest first. Blocks
/// until the user closes it.
///
/// The notes are compiled into the binary (see versionnotes.h), so the window
/// works on a machine that has neither the repository nor a network.
void versionnotes_win_exec(const uv_uistyle_st *style);


#endif

#endif /* UI_VERSIONNOTES_WIN_H_ */
