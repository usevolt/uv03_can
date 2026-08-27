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


#include "archive.h"
#include "uv_hal_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if CONFIG_TARGET_WIN
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
#endif


bool archive_mktempdir(const char *prefix, char *dest, size_t dest_len) {
	bool ret = false;
#if CONFIG_TARGET_WIN
	// %TEMP% (always ends with a backslash)
	char base[MAX_PATH];
	DWORD n = GetTempPathA(sizeof(base), base);
	if ((n != 0) && (n < sizeof(base))) {
		// CreateDirectoryA fails if the name already exists, so try a few
		// candidates built from the process id and tick count until one is free
		DWORD pid = GetCurrentProcessId();
		for (unsigned int i = 0; (i < 1000) && !ret; i++) {
			snprintf(dest, dest_len, "%s%s.%lu.%lu", base, prefix,
					(unsigned long) pid, (unsigned long) (GetTickCount() + i));
			if (CreateDirectoryA(dest, NULL)) {
				ret = true;
			}
		}
	}
#else
	char tmpl[1024];
	snprintf(tmpl, sizeof(tmpl), "/tmp/%s.XXXXXX", prefix);
	if (mkdtemp(tmpl) != NULL) {
		strncpy(dest, tmpl, dest_len - 1);
		dest[dest_len - 1] = '\0';
		ret = true;
	}
#endif
	return ret;
}


bool archive_extract(const char *archive, const char *destdir) {
	char cmd[2304];
#if CONFIG_TARGET_WIN
	// bsdtar (tar.exe, in System32 on Windows 10 1803+) extracts zip archives by
	// content sniffing, so the .uvsys/.uvdev extension is irrelevant.
	snprintf(cmd, sizeof(cmd), "tar.exe -xf \"%s\" -C \"%s\"", archive, destdir);
#else
	snprintf(cmd, sizeof(cmd), "unzip -q -o \"%s\" -d \"%s\"", archive, destdir);
#endif
	return (system(cmd) == 0);
}


void archive_rmtree(const char *dir) {
	if ((dir != NULL) && (strlen(dir) != 0)) {
#if CONFIG_TARGET_WIN
		char cmd[1100];
		snprintf(cmd, sizeof(cmd), "rmdir /s /q \"%s\"", dir);
		if (system(cmd)) {
			// ignore failure; the OS reaps the temp area eventually
		}
#else
		// Run rm directly instead of through system(): this also runs from the
		// signal handler that cleans up on Ctrl-C and on the UI window closing,
		// and system() is not async-signal-safe - it can deadlock on the glibc
		// lock held by whichever thread the signal interrupted, leaving a program
		// that prints "cleaning up" and never ends. fork(), execv() and waitpid()
		// are all async-signal-safe, and with no shell in between there is no
		// quoting to get wrong either.
		pid_t pid = fork();
		if (pid == 0) {
			char *argv[] = { "rm", "-rf", "--", (char*) dir, NULL };
			execv("/bin/rm", argv);
			// no /bin/rm (a merged-/usr system without the compatibility link)
			execv("/usr/bin/rm", argv);
			_exit(127);
		}
		else if (pid > 0) {
			// the FreeRTOS scheduler tick interrupts the wait every millisecond;
			// keep waiting rather than leaving a zombie behind
			while ((waitpid(pid, NULL, 0) == -1) && (errno == EINTR)) {
			}
		}
		else {
			// fork failed; the OS reaps the temp area eventually
		}
#endif
	}
}


void archive_sweep_stale_tmpdirs(void) {
#if !CONFIG_TARGET_WIN
	// A hard crash (SIGSEGV / SIGKILL) and the HAL's SIGINT handler (which uses
	// _exit()) skip atexit, so an extraction directory can be left behind. Sweep
	// such leftovers from earlier runs. The age guard (older than a day) keeps
	// this from disturbing the fresh directories of another uvcan instance that
	// may be running concurrently (CLI alongside an open UI).
	if (system("find /tmp -maxdepth 1 -type d "
			"\\( -name 'uvcan_uvsys.*' -o -name 'uvcan_uvdev.*' \\) "
			"-mmin +1440 -exec rm -rf {} + 2>/dev/null")) {
		// best effort; nothing to do if the sweep fails
	}
#else
	// No equally safe one-liner on Windows; rely on the atexit cleanup for normal
	// exits and on Windows' own %TEMP% housekeeping for crash leftovers.
#endif
}
