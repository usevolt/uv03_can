/*
 * uv_win_compat.h
 *
 * Portability shims that let the (Linux-oriented) uvcan application sources
 * build with the native Windows mingw-w64 toolchain. mingw lacks libgen.h,
 * wordexp.h, GNU readline and realpath(); this header provides drop-in
 * replacements for the small subset that uvcan uses.
 *
 * The whole file is a no-op for every target except CONFIG_TARGET_WIN, so the
 * Linux build keeps using the real libgen/wordexp/readline/realpath.
 */

#ifndef UV_WIN_COMPAT_H_
#define UV_WIN_COMPAT_H_

#include "uv_hal_config.h"

#if CONFIG_TARGET_WIN

#include <stdlib.h>
#include <stddef.h>

#ifndef PATH_MAX
#define PATH_MAX 260   /* == Windows MAX_PATH */
#endif


/* --- libgen.h (basename/dirname) ---------------------------------------
 * Both take a modifiable path and, like the POSIX versions, may overwrite it.
 * They understand both '/' and '\\' separators. */
char *uv_win_basename(char *path);
char *uv_win_dirname(char *path);
#define basename(p)	uv_win_basename(p)
#define dirname(p)	uv_win_dirname(p)


/* --- realpath() --------------------------------------------------------
 * _fullpath() resolves to an absolute path, returning resolved on success. */
#define realpath(path, resolved)	_fullpath((resolved), (path), PATH_MAX)


/* --- pipe() ------------------------------------------------------------
 * mingw exposes the POSIX pipe as _pipe(fds, size, textmode). Wrap it so
 * uvstdin's GUI input-feed pipe builds; binary mode keeps the byte stream
 * untranslated. read()/write()/close()/dup2()/STDIN_FILENO already come from
 * mingw's <io.h>/<unistd.h>. */
#include <io.h>
#include <fcntl.h>
#define pipe(fds)	_pipe((fds), 4096, _O_BINARY)


/* --- GNU readline ------------------------------------------------------
 * Prints prompt, reads one line from stdin into a malloc'd, newline-stripped
 * buffer (caller frees). Returns NULL on EOF. Tab-completion is unsupported. */
char *uv_win_readline(const char *prompt);
#define readline(prompt)	uv_win_readline(prompt)
#define rl_bind_key(key, func)	((void) 0)
#define rl_complete		NULL


/* --- wordexp.h ---------------------------------------------------------
 * Minimal replacement that performs no shell expansion and passes the input
 * through as a single token. Enough for resolving a typed-in file path. */
typedef struct {
	size_t we_wordc;
	char **we_wordv;
} wordexp_t;
#define WRDE_NOCMD	0

static inline int wordexp(const char *words, wordexp_t *we, int flags) {
	(void) flags;
	static char *vec[1];
	vec[0] = (char *) words;
	we->we_wordc = 1;
	we->we_wordv = vec;
	return 0;
}
static inline void wordfree(wordexp_t *we) {
	(void) we;
}

#endif /* CONFIG_TARGET_WIN */

#endif /* UV_WIN_COMPAT_H_ */
