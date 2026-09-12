/*
 * This file is part of the uvcan distribution (www.usevolt.fi).
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

#include "http.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#if !CONFIG_TARGET_WIN
// fork()/waitpid() and the scheduler delay, for the progress-logged download
// only. Everything else here is written against what mingw has too, because
// the Windows build compiles this file even though nothing on it fetches
// anything yet.
#include <sys/wait.h>
#include <uv_rtos.h>
#endif


// How often a running download's progress is logged, and how many percentage
// points it has to have advanced since the last line for another one to be
// worth printing. The log is read line by line (in the terminal and in the UI's
// log view), so a line per quarter second would bury everything else in it.
#define HTTP_PROGRESS_MS		250
#define HTTP_PROGRESS_STEP		5


void uvhttp_tmp_path(char *out, size_t len, const char *tag,
		const char *suffix) {
	snprintf(out, len, "/tmp/uvcan_%s_%d_%s", tag, (int) getpid(), suffix);
}


bool uvhttp_write_file(const char *path, const char *content) {
	bool ret = false;
	FILE *f = fopen(path, "w");
	if (f != NULL) {
		fputs(content, f);
		fclose(f);
		chmod(path, 0600);
		ret = true;
	}
	return ret;
}


char *uvhttp_read_file(const char *path) {
	char *ret = NULL;
	FILE *f = fopen(path, "rb");
	if (f != NULL) {
		fseek(f, 0, SEEK_END);
		long size = ftell(f);
		rewind(f);
		if (size >= 0) {
			ret = malloc((size_t) size + 1);
			if (ret != NULL) {
				size_t rd = fread(ret, 1, (size_t) size, f);
				ret[rd] = '\0';
			}
		}
		fclose(f);
	}
	return ret;
}


void uvhttp_cfg_sanitize(char *dst, size_t dstlen, const char *src) {
	size_t d = 0;
	if ((dst != NULL) && (dstlen > 0)) {
		if (src != NULL) {
			for (size_t i = 0; (src[i] != '\0') && (d + 1 < dstlen); i++) {
				char c = src[i];
				if ((c != '"') && (c != '\r') && (c != '\n')) {
					dst[d++] = c;
				}
				else {
				}
			}
		}
		else {
		}
		dst[d] = '\0';
	}
	else {
	}
}


int uvhttp_cfg_common(char *dst, size_t dstlen, int timeout_s,
		const char *user, const char *pass) {
	int n = snprintf(dst, dstlen,
			"silent\nshow-error\n"
			// No `location`, so a redirect is never followed: a request may
			// carry credentials, and following one would hand them to wherever
			// the answer happens to point. A caller that knows its URL is
			// https can pin that too, by appending its own `proto` line.
			"connect-timeout = 15\nmax-time = %d\n",
			timeout_s);
	if ((user != NULL) && (pass != NULL) && ((size_t) n < dstlen)) {
		char u[512];
		char p[512];
		uvhttp_cfg_sanitize(u, sizeof(u), user);
		uvhttp_cfg_sanitize(p, sizeof(p), pass);
		n += snprintf(&dst[n], dstlen - (size_t) n, "user = \"%s:%s\"\n", u, p);
	}
	else {
		// an unauthenticated request: the public part of the server wants no
		// credentials, and sending them anyway would be handing them to a path
		// that never asked
	}
	return n;
}


// Runs `curl -K <cfg_path>` and returns the HTTP status code curl reports via
// its write-out, or 0 when curl could not be run. The command line contains
// only our fixed temp paths, so nothing user- or server-supplied reaches the
// shell.
static long http_run_curl(const char *cfg_path, const char *tag) {
	long code = 0;
	char code_path[256];
	uvhttp_tmp_path(code_path, sizeof(code_path), tag, "code");

	char cmd[1024];
	snprintf(cmd, sizeof(cmd), "curl -K '%s' > '%s' 2>/dev/null",
			cfg_path, code_path);
	int rc = system(cmd);
	if (rc != -1) {
		char *body = uvhttp_read_file(code_path);
		if (body != NULL) {
			code = strtol(body, NULL, 10);
			free(body);
		}
	}
	remove(code_path);
	return code;
}


#if !CONFIG_TARGET_WIN

// Logs one progress line for a transfer that has *got* of *expected* bytes
// (*expected* 0 when the size is not known in advance).
static void http_log_progress(uint64_t got, uint64_t expected) {
	if (expected > 0) {
		printf("  %3u %%   %llu / %llu KB\n",
				(unsigned int) ((got * 100u) / expected),
				(unsigned long long) (got / 1024u),
				(unsigned long long) (expected / 1024u));
	}
	else {
		printf("  %llu KB\n", (unsigned long long) (got / 1024u));
	}
	fflush(stdout);
}


// As http_run_curl(), logging the transfer's progress to stdout while it runs.
//
// curl is started with fork() and execvp() rather than through the shell, so
// that its stdout (which carries the write-out status code) can be sent to the
// code file while its stderr stays where the program's own output goes. Its own
// progress meter is left off on purpose: it redraws one line with carriage
// returns, which the line-based log capture would turn into one enormous line.
// The progress below is measured from the size the destination file has reached
// instead, which is what the user is waiting for anyway.
static long http_run_curl_logged(const char *cfg_path, const char *tag,
		const char *dest_path, uint64_t expected) {
	long code = 0;
	char code_path[256];
	uvhttp_tmp_path(code_path, sizeof(code_path), tag, "code");
	remove(code_path);

	pid_t pid = fork();
	if (pid == 0) {
		// curl's stdout is the status code, and nothing else: the parent reads
		// this file once curl is done
		if (freopen(code_path, "w", stdout) == NULL) {
			_exit(127);
		}
		char *argv[] = { "curl", "-K", (char*) cfg_path, NULL };
		execvp("curl", argv);
		// no curl on this machine; the parent sees the empty code file and
		// reports it as "could not reach the server"
		_exit(127);
	}
	else if (pid > 0) {
		unsigned int logged_pct = 0;
		uint64_t logged_got = 0;
		int rc;
		do {
			// the FreeRTOS scheduler tick interrupts the wait; keep waiting
			// rather than leaving a zombie behind
			rc = waitpid(pid, NULL, WNOHANG);
			if (rc == 0) {
				uv_rtos_task_delay(HTTP_PROGRESS_MS);
				struct stat st;
				if (stat(dest_path, &st) == 0) {
					uint64_t got = (uint64_t) st.st_size;
					if (expected > 0) {
						unsigned int pct = (unsigned int) ((got * 100u) / expected);
						if (pct >= (logged_pct + HTTP_PROGRESS_STEP)) {
							http_log_progress(got, expected);
							logged_pct = pct;
						}
					}
					else if (got >= (logged_got + 1024u * 1024u)) {
						// nothing to measure against: a line per megabyte then
						http_log_progress(got, 0);
						logged_got = got;
					}
					else {
					}
				}
			}
			else {
			}
		} while ((rc == 0) || ((rc == -1) && (errno == EINTR)));

		char *body = uvhttp_read_file(code_path);
		if (body != NULL) {
			code = strtol(body, NULL, 10);
			free(body);
		}
	}
	else {
		// fork failed; nothing was transferred
	}
	remove(code_path);
	return code;
}


#endif /* !CONFIG_TARGET_WIN -- the progress-logged download's machinery */


// As http_run_curl(), reading back every status line curl wrote rather than
// only the first. Returns how many were read.
static int http_run_curl_codes(const char *cfg_path, const char *tag,
		long *codes, int max_codes) {
	int n = 0;
	char code_path[256];
	uvhttp_tmp_path(code_path, sizeof(code_path), tag, "code");

	char cmd[1024];
	snprintf(cmd, sizeof(cmd), "curl -K '%s' > '%s' 2>/dev/null",
			cfg_path, code_path);
	int rc = system(cmd);
	if (rc != -1) {
		char *body = uvhttp_read_file(code_path);
		if (body != NULL) {
			char *line = body;
			while ((line != NULL) && (*line != '\0') && (n < max_codes)) {
				char *nl = strchr(line, '\n');
				if (nl != NULL) {
					*nl = '\0';
				}
				else {
				}
				if (line[0] != '\0') {
					codes[n++] = strtol(line, NULL, 10);
				}
				else {
				}
				line = (nl != NULL) ? (nl + 1) : NULL;
			}
			free(body);
		}
		else {
		}
	}
	else {
	}
	remove(code_path);
	return n;
}


int uvhttp_curl_multi(const char *cfg, long *codes, int max_codes) {
	char cfg_path[256];
	uvhttp_tmp_path(cfg_path, sizeof(cfg_path), "http", "cfg");
	int n = 0;
	if (uvhttp_write_file(cfg_path, cfg)) {
		n = http_run_curl_codes(cfg_path, "http", codes, max_codes);
	}
	else {
	}
	remove(cfg_path);
	return n;
}


long uvhttp_curl(const char *cfg) {
	char cfg_path[256];
	uvhttp_tmp_path(cfg_path, sizeof(cfg_path), "http", "cfg");
	long code = 0;
	if (uvhttp_write_file(cfg_path, cfg)) {
		code = http_run_curl(cfg_path, "http");
	}
	remove(cfg_path);
	return code;
}


#if !CONFIG_TARGET_WIN

long uvhttp_curl_logged(const char *cfg, const char *dest_path,
		uint64_t expected) {
	char cfg_path[256];
	uvhttp_tmp_path(cfg_path, sizeof(cfg_path), "http", "cfg");
	long code = 0;
	if (uvhttp_write_file(cfg_path, cfg)) {
		code = http_run_curl_logged(cfg_path, "http", dest_path, expected);
	}
	remove(cfg_path);
	return code;
}

#else /* CONFIG_TARGET_WIN */

// No fork() to watch the destination file grow with, so the transfer runs
// silently. Nothing on the Windows build downloads anything yet; when
// something does, the progress is what it loses, not the download.
long uvhttp_curl_logged(const char *cfg, const char *dest_path,
		uint64_t expected) {
	(void) dest_path;
	(void) expected;
	return uvhttp_curl(cfg);
}

#endif


void uvhttp_err(char *err, unsigned int err_len, const char *msg) {
	if ((err != NULL) && (err_len > 0)) {
		strncpy(err, msg, err_len - 1);
		err[err_len - 1] = '\0';
	}
}


void uvhttp_status_err(long code, const char *what, char *err,
		unsigned int err_len) {
	if (code == 0) {
		uvhttp_err(err, err_len,
				"Could not reach the server (is curl installed and the URL "
				"correct?).");
	}
	else if (code == 401) {
		uvhttp_err(err, err_len, "Invalid username or password.");
	}
	else if (code == 403) {
		uvhttp_err(err, err_len, "This account may not read that fleet's files.");
	}
	else if (code == 404) {
		uvhttp_err(err, err_len,
				"No such fleet on the file server, or it has no files area yet.");
	}
	else {
		char m[128];
		snprintf(m, sizeof(m), "Server returned HTTP %ld %s.", code, what);
		uvhttp_err(err, err_len, m);
	}
}


bool uvhttp_sha256_file(const char *path, char *out, size_t out_len) {
	bool ret = false;
	// sha256sum rather than a hash implementation of our own: it is in
	// coreutils, so it is on every machine this runs on, and the one thing it
	// is asked to do is read a file we have just written ourselves. The path is
	// always one of ours, never anything a server said.
	//
	// Through system() and a temp file rather than popen(): the FreeRTOS POSIX
	// port drives its scheduler with signals, so a read from a popen() pipe is
	// interrupted (EINTR) and returns nothing, which read as "this file does
	// not match its checksum" -- an update that had downloaded perfectly would
	// be thrown away. glibc's system() retries the wait itself, which is why
	// the curl calls above have always been reliable.
	char out_path[256];
	uvhttp_tmp_path(out_path, sizeof(out_path), "http", "sha");
	char cmd[1024];
	snprintf(cmd, sizeof(cmd), "sha256sum '%s' > '%s' 2>/dev/null",
			path, out_path);
	int rc = system(cmd);
	if (rc != -1) {
		char *body = uvhttp_read_file(out_path);
		if (body != NULL) {
			// "<64 hex>  <path>"; the hash is everything up to the first space
			char *sp = strchr(body, ' ');
			if (sp != NULL) {
				*sp = '\0';
			}
			else {
			}
			if ((strlen(body) == 64) && (out_len > 64)) {
				snprintf(out, out_len, "%s", body);
				ret = true;
			}
			else {
			}
			free(body);
		}
		else {
		}
	}
	else {
	}
	remove(out_path);
	return ret;
}
