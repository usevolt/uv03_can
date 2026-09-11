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
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <fcntl.h>
#include <time.h>
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
	// The owner's process id goes into the name (as it does on Windows), so that
	// a later run can tell a directory belonging to a uvcan which is still
	// running from one a killed run left behind. See
	// archive_sweep_stale_tmpdirs().
	char tmpl[1024];
	snprintf(tmpl, sizeof(tmpl), "/tmp/%s.%ld.XXXXXX", prefix, (long) getpid());
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


#if !CONFIG_TARGET_WIN
// How old a temporary directory whose name does not say who made it has to be
// before it is swept: long enough that no run still going on can own it.
#define TMPDIR_STALE_AGE_S		(24 * 60 * 60)

// The temporary directories uvcan creates, named "<prefix><pid>.XXXXXX" under
// /tmp. Nothing outside this list is ever swept.
static const char *const tmpdir_prefixes[] = {
		"uvcan_uvsys.", "uvcan_uvdev.", "uvcan_pkg." };


// Recognises one of our temporary directory names. Returns the process id part
// of *name* -- the digits which follow the prefix -- or "" when the name carries
// no process id (a directory from a uvcan older than this naming), or NULL when
// the name is not one of ours at all.
static const char *tmpdir_pid_part(const char *name) {
	const char *ret = NULL;
	for (size_t i = 0; (i < (sizeof(tmpdir_prefixes) /
			sizeof(tmpdir_prefixes[0]))) && (ret == NULL); i++) {
		size_t len = strlen(tmpdir_prefixes[i]);
		if (strncmp(name, tmpdir_prefixes[i], len) == 0) {
			// only digits followed by the mkdtemp part count as a process id;
			// the random part of an old-style name can start with digits too
			const char *p = &name[len];
			size_t digits = strspn(p, "0123456789");
			ret = ((digits != 0u) && (p[digits] == '.')) ? p : "";
		}
		else {
		}
	}
	return ret;
}


// Reads /proc/<pid>/comm (with *pid* NULL for our own) into *dest*. Plain
// open()/read() rather than stdio: this also runs before the scheduler is up,
// and there is no need to pull a FILE buffer in for 16 bytes.
static bool proc_comm(const char *pid, char *dest, size_t dest_len) {
	bool ret = false;
	char path[64];
	snprintf(path, sizeof(path), "/proc/%s/comm",
			(pid != NULL) ? pid : "self");
	int fd = open(path, O_RDONLY);
	if (fd >= 0) {
		ssize_t n = read(fd, dest, dest_len - 1);
		while ((n < 0) && (errno == EINTR)) {
			n = read(fd, dest, dest_len - 1);
		}
		if (n > 0) {
			dest[n] = '\0';
			dest[strcspn(dest, "\n")] = '\0';
			ret = true;
		}
		else {
		}
		close(fd);
	}
	else {
	}
	return ret;
}


// True when the process id at the front of *pidpart* is a uvcan which is still
// running. The id alone does not say that: ids are reused, and the one in a
// leftover directory's name may by now belong to anything at all. The program's
// name in /proc tells uvcan from a stranger, and comparing it against our own
// rather than against a literal keeps working for a renamed binary.
static bool tmpdir_owner_is_alive(const char *pidpart) {
	bool ret = false;
	char pid[16];
	size_t digits = strspn(pidpart, "0123456789");
	if ((digits != 0u) && (digits < sizeof(pid))) {
		memcpy(pid, pidpart, digits);
		pid[digits] = '\0';
		char self[64];
		char other[64];
		if (proc_comm(NULL, self, sizeof(self)) &&
				proc_comm(pid, other, sizeof(other))) {
			ret = (strcmp(self, other) == 0);
		}
		else {
		}
	}
	else {
	}
	return ret;
}
#endif


void archive_sweep_stale_tmpdirs(void) {
#if !CONFIG_TARGET_WIN
	// A hard crash (SIGSEGV / SIGKILL) skips both atexit and the SIGINT/SIGTERM
	// cleanup, so an extraction directory - or the directory the server files
	// window downloads its packages into - can be left behind. Sweep such
	// leftovers from earlier runs.
	DIR *dir = opendir("/tmp");
	if (dir != NULL) {
		struct dirent *entry = readdir(dir);
		while (entry != NULL) {
			const char *pidpart = tmpdir_pid_part(entry->d_name);
			char path[1100];
			struct stat st;
			if (pidpart == NULL) {
				// not one of ours
			}
			else if (snprintf(path, sizeof(path), "/tmp/%s", entry->d_name) < 0) {
			}
			else if ((stat(path, &st) != 0) || !S_ISDIR(st.st_mode)) {
			}
			else if (tmpdir_owner_is_alive(pidpart)) {
				// another uvcan is running and this is its directory
			}
			else if ((*pidpart == '\0') &&
					((time(NULL) - st.st_mtime) < TMPDIR_STALE_AGE_S)) {
				// No owner in the name: a directory from a uvcan older than this
				// naming, or the staging directory savesys assembles a package
				// in. Nothing says whether it is in use, so the age guard is all
				// that keeps this from pulling the ground from under a run that
				// is going on right now.
			}
			else {
				archive_rmtree(path);
			}
			entry = readdir(dir);
		}
		closedir(dir);
	}
	else {
		// no /tmp to sweep; nothing to do
	}
#else
	// No equally safe sweep on Windows; rely on the atexit cleanup for normal
	// exits and on Windows' own %TEMP% housekeeping for crash leftovers.
#endif
}
