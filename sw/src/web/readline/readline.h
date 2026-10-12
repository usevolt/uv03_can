/*
 * This file is part of the uv_hal distribution (www.usevolt.fi).
 * Copyright (c) 2026 Usevolt Oy.
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

#ifndef UVCAN_WEB_READLINE_H_
#define UVCAN_WEB_READLINE_H_

/// @file: GNU readline on the web browser build, where there is no terminal
/// to read a line from. readline() answers as if the user had pressed Enter
/// at once, which every prompt that uses it takes as its default.

#include <stddef.h>

typedef int rl_command_func_t(int, int);

static inline int rl_complete(int count, int key) {
	(void) count;
	(void) key;
	return 0;
}

static inline int rl_bind_key(int key, rl_command_func_t *function) {
	(void) key;
	(void) function;
	return 0;
}

static inline char *readline(const char *prompt) {
	(void) prompt;
	return NULL;
}

#endif /* UVCAN_WEB_READLINE_H_ */
