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


#include "uvstdin.h"

#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdbool.h>
// Windows shim: provides pipe() (via mingw's _pipe); no-op on Linux.
#include "uv_win_compat.h"


// Write end of the stdin pipe when uv_stdin_use_pipe() is active, else -1.
static int feed_fd = -1;

// True while a read() is blocked waiting for input, i.e. an interactive prompt is
// on-screen expecting the user to type an answer. Polled by the GUI (uvui) so it
// can pop the log's command line open automatically. Volatile: set on the reading
// task, read on the UI task.
static volatile bool input_waiting = false;


bool uv_stdin_is_waiting(void) {
	return input_waiting;
}


/// @brief: Reads a single byte from stdin, retrying on EINTR (the FreeRTOS POSIX
/// port's tick signal). Returns 1 on success, 0 on EOF, -1 on error.
static int read_byte(char *out) {
	input_waiting = true;
	while (1) {
		ssize_t r = read(STDIN_FILENO, out, 1);
		if (r == 1) {
			input_waiting = false;
			return 1;
		}
		if (r == 0) {
			input_waiting = false;
			return 0;
		}
		if ((r < 0) && (errno == EINTR)) {
			// interrupted by a scheduler signal: retry
			continue;
		}
		input_waiting = false;
		return -1;
	}
}


char *uv_stdin_getline(char *buf, size_t bufsize) {
	if ((buf == NULL) || (bufsize == 0)) {
		return NULL;
	}
	size_t i = 0;
	bool any = false;
	while (i + 1 < bufsize) {
		char c;
		int r = read_byte(&c);
		if (r <= 0) {
			// EOF / error: return what we have, or NULL if nothing was read
			break;
		}
		any = true;
		buf[i++] = c;
		if (c == '\n') {
			break;
		}
	}
	buf[i] = '\0';
	return any ? buf : NULL;
}


int uv_stdin_getchar(void) {
	char c;
	int r = read_byte(&c);
	return (r == 1) ? (int) (unsigned char) c : -1;
}


void uv_stdin_use_pipe(void) {
	if (feed_fd >= 0) {
		// already installed
		return;
	}
	int fds[2];
	if (pipe(fds) != 0) {
		// non-fatal: prompts simply keep reading from the current stdin
		return;
	}
	// point stdin at the read end, keep the write end for uv_stdin_feed()
	if (dup2(fds[0], STDIN_FILENO) < 0) {
		close(fds[0]);
		close(fds[1]);
		return;
	}
	close(fds[0]);
	feed_fd = fds[1];
}


void uv_stdin_feed(const char *line) {
	if ((feed_fd < 0) || (line == NULL)) {
		return;
	}
	size_t len = strlen(line);
	if (len > 0) {
		if (write(feed_fd, line, len) < 0) {
			// best effort; nothing to do if the write fails
		}
	}
	// ensure the reading side sees a complete line
	if ((len == 0) || (line[len - 1] != '\n')) {
		if (write(feed_fd, "\n", 1) < 0) {
			// best effort
		}
	}
}
