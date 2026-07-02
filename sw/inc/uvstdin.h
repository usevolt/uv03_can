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


#ifndef UVSTDIN_H_
#define UVSTDIN_H_


#include <stddef.h>
#include <stdbool.h>


/// @brief: Cooperative, interrupt-safe replacement for fgets(stdin) / fgetc(stdin)
/// used by uvcan's interactive prompts.
///
/// The FreeRTOS POSIX port drives context switches with signals, so a plain
/// fgets/fgetc on stdin is interrupted (EINTR) by the scheduler tick and returns
/// early. The prompts used to guard against this by disabling interrupts around
/// the read — but that also halts the scheduler, which freezes the whole GUI if
/// the read blocks with no input available. These helpers instead read stdin one
/// byte at a time with an EINTR retry loop and NO interrupt masking, so a blocking
/// read keeps the scheduler (and the UI) running. The GUI can then feed the read
/// live via uv_stdin_feed() from its log-view command line.


/// @brief: Reads a line from stdin into *buf* (at most *bufsize*-1 chars plus the
/// null terminator), keeping the trailing newline like fgets. Blocks until a line
/// is available or EOF. Returns *buf*, or NULL on EOF with nothing read. EINTR is
/// retried transparently.
char *uv_stdin_getline(char *buf, size_t bufsize);


/// @brief: Reads a single byte from stdin, EINTR-safe. Returns the byte (0..255)
/// or -1 on EOF / error.
int uv_stdin_getchar(void);


/// @brief: Redirects stdin to the read end of an internal pipe, so uv_stdin_feed()
/// can supply input to the prompts. Called by the GUI in place of the previous
/// /dev/null redirect. Safe to call once; a second call is a no-op.
void uv_stdin_use_pipe(void);


/// @brief: Feeds *line* to stdin (as if typed at the command line), for the next
/// stdin read to consume. A trailing newline is appended when missing. No-op when
/// the pipe is not installed (uv_stdin_use_pipe() was not called). Safe to call
/// from the UI thread.
void uv_stdin_feed(const char *line);


/// @brief: Returns true while a stdin read is blocked waiting for input, i.e. an
/// interactive prompt is on-screen expecting an answer. The GUI polls this to open
/// its log command line automatically. Safe to call from any thread.
bool uv_stdin_is_waiting(void);


#endif /* UVSTDIN_H_ */
