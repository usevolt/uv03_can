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


#ifndef LOGCAP_H_
#define LOGCAP_H_


#include <stdbool.h>
#include <stddef.h>


/// @brief: Starts capturing everything the program writes to stdout and stderr
/// into an in-memory ring buffer, while still echoing it to the original
/// terminal. Idempotent: a second call does nothing. Used by the UI to show the
/// most recent log line and a full log view.
void logcap_init(void);


/// @brief: Copies the most recently logged line into *out* (always
/// null-terminated). Empty string when nothing has been logged yet.
void logcap_get_last_line(char *out, size_t out_len);


/// @brief: Copies the most recent *max_lines* captured log lines (oldest first,
/// newest last) into *out*, joined by newlines and always null-terminated. Pass
/// max_lines <= 0 for as many as fit. If the result does not fit, the oldest
/// lines are dropped so the newest ones are kept.
void logcap_get_all(char *out, size_t out_len, int max_lines);


/// @brief: Like logcap_get_all(), but the emitted window ends *skip_newest* lines
/// before the newest captured line, so the caller can page back through history.
/// With skip_newest == 0 this is identical to logcap_get_all(). Up to *max_lines*
/// lines (oldest first) are written, joined by newlines and null-terminated.
void logcap_get_range(char *out, size_t out_len, int skip_newest, int max_lines);


/// @brief: Returns the number of complete lines currently held in the capture
/// ring buffer. Used by the UI to clamp how far the log view can be scrolled.
int logcap_get_line_count(void);


#endif /* LOGCAP_H_ */
