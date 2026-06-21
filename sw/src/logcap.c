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


#include "logcap.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>


// Number of log lines retained, and the maximum stored length of each.
#define LOGCAP_MAX_LINES	1000
#define LOGCAP_LINE_LEN		256


// Ring buffer of completed log lines.
static char lines[LOGCAP_MAX_LINES][LOGCAP_LINE_LEN];
// Index of the next slot to write, and how many lines are currently stored.
static int line_head;
static int line_count;
// The line currently being assembled (no terminator seen yet).
static char curline[LOGCAP_LINE_LEN];
static int curlen;

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static bool started;
// The original stdout file descriptor, kept so the captured output is still
// echoed to the terminal.
static int orig_stdout_fd = -1;


/// @brief: Appends the assembled current line to the ring buffer (called with
/// the mutex held). Empty lines are skipped so a CR/LF pair or a stray carriage
/// return does not produce blank entries.
static void push_curline(void) {
	if (curlen > 0) {
		curline[curlen] = '\0';
		memcpy(lines[line_head], curline, curlen + 1);
		line_head = (line_head + 1) % LOGCAP_MAX_LINES;
		if (line_count < LOGCAP_MAX_LINES) {
			line_count++;
		}
		curlen = 0;
		curline[0] = '\0';
	}
}


/// @brief: Feeds raw captured bytes into the line assembler. ANSI escape
/// sequences (the colored PRINT macros) are kept verbatim so the UI labels can
/// interpret them and render the log in colour, just like the terminal does.
static void consume(const char *buf, ssize_t n) {
	pthread_mutex_lock(&mutex);
	for (ssize_t i = 0; i < n; i++) {
		char c = buf[i];
		if ((c == '\n') || (c == '\r')) {
			push_curline();
		}
		else if (curlen >= LOGCAP_LINE_LEN - 1) {
			// the line is full: store it and start a new one with this byte
			push_curline();
			curline[curlen++] = c;
		}
		else {
			curline[curlen++] = c;
		}
	}
	pthread_mutex_unlock(&mutex);
}


/// @brief: Reader thread: drains the capture pipe, echoes the bytes to the
/// original terminal, and feeds them into the line assembler.
static void *reader_thread(void *arg) {
	int fd = (int) (intptr_t) arg;

	// This is a plain pthread, not a FreeRTOS task. The FreeRTOS POSIX port drives
	// scheduling with process signals (SIGALRM tick, SIGUSR1 resume). If one is
	// delivered to this thread it both interrupts the blocking read() below (EINTR)
	// and runs the port's handler on a thread it does not manage. Block all signals
	// here so they are always delivered to the FreeRTOS task threads instead.
	sigset_t all;
	sigfillset(&all);
	pthread_sigmask(SIG_BLOCK, &all, NULL);

	char buf[4096];
	for (;;) {
		ssize_t n = read(fd, buf, sizeof(buf));
		if (n > 0) {
			if (orig_stdout_fd >= 0) {
				if (write(orig_stdout_fd, buf, n)) {
					// best effort echo to the terminal
				}
			}
			consume(buf, n);
		}
		else if ((n < 0) && (errno == EINTR)) {
			// interrupted by a signal before any data: just retry the read
			continue;
		}
		else {
			// end of file on the pipe, or an unrecoverable error
			break;
		}
	}
	return NULL;
}


void logcap_init(void) {
	if (started) {
		return;
	}

	int pipe_fd[2];
	if (pipe(pipe_fd) != 0) {
		return;
	}

	// keep a copy of the real stdout so the reader can echo to the terminal
	orig_stdout_fd = dup(STDOUT_FILENO);

	// redirect both stdout and stderr into the pipe's write end
	fflush(stdout);
	fflush(stderr);
	dup2(pipe_fd[1], STDOUT_FILENO);
	dup2(pipe_fd[1], STDERR_FILENO);
	close(pipe_fd[1]);

	// stdout would otherwise be fully buffered now that it is a pipe; make it
	// unbuffered so log lines appear promptly (stderr is unbuffered already)
	setvbuf(stdout, NULL, _IONBF, 0);

	pthread_t thread;
	if (pthread_create(&thread, NULL, &reader_thread,
			(void*) (intptr_t) pipe_fd[0]) == 0) {
		pthread_detach(thread);
		started = true;
	}
}


void logcap_get_last_line(char *out, size_t out_len) {
	if ((out == NULL) || (out_len == 0)) {
		return;
	}
	pthread_mutex_lock(&mutex);
	const char *src = "";
	if (curlen > 0) {
		// show the line being assembled for liveness (e.g. progress prints)
		curline[curlen] = '\0';
		src = curline;
	}
	else if (line_count > 0) {
		src = lines[(line_head - 1 + LOGCAP_MAX_LINES) % LOGCAP_MAX_LINES];
	}
	else {
		// nothing logged yet
	}
	strncpy(out, src, out_len - 1);
	out[out_len - 1] = '\0';
	pthread_mutex_unlock(&mutex);
}


int logcap_get_line_count(void) {
	pthread_mutex_lock(&mutex);
	int ret = line_count;
	pthread_mutex_unlock(&mutex);
	return ret;
}


void logcap_get_range(char *out, size_t out_len, int skip_newest, int max_lines) {
	if ((out == NULL) || (out_len == 0)) {
		return;
	}
	pthread_mutex_lock(&mutex);

	if (skip_newest < 0) {
		skip_newest = 0;
	}
	// lines available up to (newest - skip_newest)
	int avail = line_count - skip_newest;
	if (avail < 0) {
		avail = 0;
	}
	int count = avail;
	if ((max_lines > 0) && (count > max_lines)) {
		count = max_lines;
	}
	// one-past-the-last line to emit, and the oldest line to start from
	int end = (line_head - skip_newest + 2 * LOGCAP_MAX_LINES) % LOGCAP_MAX_LINES;
	int start = (end - count + 2 * LOGCAP_MAX_LINES) % LOGCAP_MAX_LINES;

	size_t used = 0;
	out[0] = '\0';
	for (int i = 0; i < count; i++) {
		int idx = (start + i) % LOGCAP_MAX_LINES;
		const char *line = lines[idx];
		size_t llen = strlen(line);
		if (llen + 2 >= out_len) {
			continue;
		}
		if (used + llen + 2 >= out_len) {
			// not enough room: drop the oldest accumulated lines, keep newest
			used = 0;
			out[0] = '\0';
		}
		memcpy(out + used, line, llen);
		used += llen;
		out[used++] = '\n';
		out[used] = '\0';
	}
	pthread_mutex_unlock(&mutex);
}


void logcap_get_all(char *out, size_t out_len, int max_lines) {
	// the most recent lines: a range that skips nothing from the newest end
	logcap_get_range(out, out_len, 0, max_lines);
}
