/*
 * uv_win_compat.c
 *
 * Implementations of the Windows portability shims declared in
 * uv_win_compat.h. Active only for CONFIG_TARGET_WIN.
 */

#include "uv_hal_config.h"

/* Keep the translation unit non-empty for non-Windows targets. */
typedef int uv_win_compat_dummy_t;

#if CONFIG_TARGET_WIN

#include "uv_win_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* basename()/dirname() must not be macro-expanded inside their own
 * definitions. */
#undef basename
#undef dirname
#undef readline


static bool is_sep(char c) {
	return (c == '/' || c == '\\');
}


char *uv_win_basename(char *path) {
	static char dot[] = ".";
	if (path == NULL || path[0] == '\0') {
		return dot;
	}
	// strip trailing separators
	size_t len = strlen(path);
	while (len > 1 && is_sep(path[len - 1])) {
		path[--len] = '\0';
	}
	// find last separator
	char *base = path;
	for (char *p = path; *p != '\0'; p++) {
		if (is_sep(*p)) {
			base = p + 1;
		}
	}
	return (*base != '\0') ? base : path;
}


char *uv_win_dirname(char *path) {
	static char dot[] = ".";
	if (path == NULL || path[0] == '\0') {
		return dot;
	}
	// strip trailing separators
	size_t len = strlen(path);
	while (len > 1 && is_sep(path[len - 1])) {
		path[--len] = '\0';
	}
	// find last separator
	char *last = NULL;
	for (char *p = path; *p != '\0'; p++) {
		if (is_sep(*p)) {
			last = p;
		}
	}
	if (last == NULL) {
		return dot;
	}
	// trim separators from the directory part
	while (last > path && is_sep(*(last - 1))) {
		last--;
	}
	if (last == path) {
		// path was like "/foo" -> dir is "/"
		path[1] = '\0';
	}
	else {
		*last = '\0';
	}
	return path;
}


char *uv_win_readline(const char *prompt) {
	if (prompt != NULL) {
		fputs(prompt, stdout);
		fflush(stdout);
	}
	char buf[1024];
	if (fgets(buf, sizeof(buf), stdin) == NULL) {
		return NULL;
	}
	size_t len = strlen(buf);
	while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
		buf[--len] = '\0';
	}
	char *ret = malloc(len + 1);
	if (ret != NULL) {
		memcpy(ret, buf, len + 1);
	}
	return ret;
}

#endif /* CONFIG_TARGET_WIN */
