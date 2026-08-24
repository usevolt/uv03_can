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


#include "makeuvdev.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "main.h"
#include "db.h"
#include "archive.h"
#include "parser.h"


// <windows.h> defines ERROR as 0; undef so the colored-print macro can be defined.
#undef ERROR
#define ERROR(str, ...) printf(PRINT_BOLDRED str PRINT_RESET, __VA_ARGS__)
#define ERRORSTR(str) printf(PRINT_BOLDRED str PRINT_RESET)
#define WARNING(str, ...) printf(PRINT_BOLDYELLOW str PRINT_RESET, __VA_ARGS__)


/// @brief: Maximum count of --media options which can be given
#define MEDIA_MAX_COUNT		32
/// @brief: The name of the directory which the media files are packaged into,
/// i.e. the value of the manifest's MEDIA key
#define MEDIA_DIR			"media"


// The files which the package is assembled from, as given with the options of
// this module. Only *firmware* is mandatory; the database comes from --db.
static char firmware[1024];
static char linuxbin[1024];
static char bootloader[1024];
static char version[128];
static char media[MEDIA_MAX_COUNT][1024];
static unsigned int media_count;



// Stores the argument of the option *opt* into *dest*.
static bool set_arg(const char *arg, char *dest, size_t dest_len,
		const char *opt) {
	bool ret = false;
	if ((arg == NULL) || (strlen(arg) == 0)) {
		ERROR("Give a value for the '%s' option.\n", opt);
	}
	else if (strlen(arg) >= dest_len) {
		ERROR("The value given for the '%s' option is too long.\n", opt);
	}
	else {
		strcpy(dest, arg);
		ret = true;
	}
	return ret;
}


bool cmd_firmware(const char *arg) {
	bool ret = set_arg(arg, firmware, sizeof(firmware), "firmware");
	if (ret) {
		PRINT("Firmware binary set to '%s'\n", firmware);
	}
	return ret;
}


const char *makeuvdev_get_firmware(void) {
	return firmware;
}


bool cmd_linuxbin(const char *arg) {
	bool ret = set_arg(arg, linuxbin, sizeof(linuxbin), "linuxbin");
	if (ret) {
		PRINT("Linux simulator executable set to '%s'\n", linuxbin);
	}
	return ret;
}


bool cmd_bootloader(const char *arg) {
	bool ret = set_arg(arg, bootloader, sizeof(bootloader), "bootloader");
	if (ret) {
		PRINT("Bootloader binary set to '%s'\n", bootloader);
	}
	return ret;
}


bool cmd_fwversion(const char *arg) {
	bool ret = set_arg(arg, version, sizeof(version), "fwversion");
	if (ret) {
		PRINT("Firmware version set to '%s'\n", version);
	}
	return ret;
}


bool cmd_media(const char *arg) {
	bool ret = false;
	if (media_count >= MEDIA_MAX_COUNT) {
		ERROR("At most %u 'media' options can be given.\n",
				(unsigned int) MEDIA_MAX_COUNT);
	}
	else {
		ret = set_arg(arg, media[media_count], sizeof(media[0]), "media");
		if (ret) {
			PRINT("Added '%s' to the media of the package\n",
					media[media_count]);
			media_count++;
		}
	}
	return ret;
}



#if !CONFIG_TARGET_WIN


// The file name part of *path*, i.e. the name which the file gets at the root
// of the package. Unlike basename() this does not modify the path it is given.
static const char *path_basename(const char *path) {
	const char *sep = strrchr(path, '/');
	return (sep != NULL) ? (sep + 1) : path;
}


// True when *path* names an existing regular file
static bool is_file(const char *path) {
	struct stat st;
	return (stat(path, &st) == 0) && S_ISREG(st.st_mode);
}


// True when *path* names an existing directory
static bool is_dir(const char *path) {
	struct stat st;
	return (stat(path, &st) == 0) && S_ISDIR(st.st_mode);
}


// Runs *cmd* in a shell, returning true when it succeeded
static bool run(const char *cmd) {
	return (system(cmd) == 0);
}


// Gives the path which *path* should have inside the package, i.e. its path
// relative to *root*. Both are resolved with realpath() first, so that the './'
// and '../' forms which the database's "content" references use do not end up
// in the package.
//
// Returns false when *path* is not inside *root*: such a file could only be
// packaged outside of the package's own directory, where the loader would never
// find it again.
static bool package_path(const char *root, const char *path,
		char *dest, size_t dest_len) {
	bool ret = false;
	// realpath() allocates the resolved paths, so that they are not limited to
	// a buffer size of our own
	char *rootreal = realpath(root, NULL);
	char *pathreal = realpath(path, NULL);

	if (rootreal == NULL) {
		ERROR("Failed to resolve the directory '%s'.\n", root);
	}
	else if (pathreal == NULL) {
		ERROR("Failed to resolve the file '%s'.\n", path);
	}
	else {
		// a trailing '/' of the root is not a part of the comparison, but the
		// path has to continue with one so that '/a/bc' is not taken to be
		// inside '/a/b'
		size_t rootlen = strlen(rootreal);
		while ((rootlen > 1) && (rootreal[rootlen - 1] == '/')) {
			rootlen--;
		}
		const char *rel = NULL;
		if (strncmp(pathreal, rootreal, rootlen) == 0) {
			if (pathreal[rootlen] == '/') {
				rel = pathreal + rootlen + 1;
			}
			else if ((rootlen == 1) && (rootreal[0] == '/')) {
				// the root is the file system root, which the separator is
				// already a part of
				rel = pathreal + 1;
			}
			else {
				// the paths merely share a prefix
			}
		}
		if (rel == NULL) {
			ERROR("The file '%s' is not inside the project directory '%s', so "
					"it cannot be packaged.\n", path, rootreal);
		}
		else if (strlen(rel) >= dest_len) {
			ERROR("The path of the file '%s' is too long.\n", path);
		}
		else {
			strcpy(dest, rel);
			ret = true;
		}
	}
	free(rootreal);
	free(pathreal);

	return ret;
}


// Copies *src* into the staging directory *stage*, to the package relative path
// *relpath*, creating the directories which it needs.
static bool stage_file(const char *stage, const char *src, const char *relpath) {
	bool ret = false;
	char cmd[4096];

	// the directory part of the destination, if it has one
	char dir[1024];
	strncpy(dir, relpath, sizeof(dir) - 1);
	dir[sizeof(dir) - 1] = '\0';
	char *sep = strrchr(dir, '/');
	if (sep != NULL) {
		*sep = '\0';
		snprintf(cmd, sizeof(cmd), "mkdir -p \"%s/%s\"", stage, dir);
		ret = run(cmd);
	}
	else {
		ret = true;
	}

	if (ret) {
		snprintf(cmd, sizeof(cmd), "cp \"%s\" \"%s/%s\"", src, stage, relpath);
		ret = run(cmd);
	}
	if (!ret) {
		ERROR("Failed to add the file '%s' to the package.\n", src);
	}

	return ret;
}


// Copies the media file or directory *src* into the package's media directory.
// Of a directory only the files in it are copied, not its subdirectories, which
// is also how *loadmedia* reads a directory of media.
static bool stage_media(const char *mediadir, const char *src) {
	bool ret;
	char cmd[4096];

	if (is_dir(src)) {
		snprintf(cmd, sizeof(cmd),
				"find \"%s\" -maxdepth 1 -type f -exec cp {} \"%s/\" \\;",
				src, mediadir);
		ret = run(cmd);
	}
	else {
		snprintf(cmd, sizeof(cmd), "cp \"%s\" \"%s/\"", src, mediadir);
		ret = run(cmd);
	}
	if (!ret) {
		ERROR("Failed to add the media '%s' to the package.\n", src);
	}

	return ret;
}


// Copies the database given with --db, and every file which it pulled in with a
// "content" reference, into the staging directory. The files keep the paths
// which they have relative to the project's root directory, so that the
// references still resolve when the database is read from the package.
//
// @param dbrel: The database's own path inside the package is stored here; it
// is the value of the manifest's DATABASE key.
static bool stage_database(const char *stage, char *dbrel, size_t dbrel_len) {
	bool ret = false;
	char *root = db_get_basepath(&dev.db);
	char rel[1024];

	if (!package_path(root, db_get_file(&dev.db), dbrel, dbrel_len)) {
		// the reason was already reported
	}
	else if (!stage_file(stage, db_get_file(&dev.db), dbrel)) {
	}
	else {
		ret = true;
		printf("Adding database '%s'", dbrel);
		if (db_get_include_count(&dev.db) != 0) {
			printf(" and %u file(s) included by it",
					(unsigned int) db_get_include_count(&dev.db));
		}
		printf("\n");
		fflush(stdout);

		for (uint16_t i = 0; (i < db_get_include_count(&dev.db)) && ret; i++) {
			char *inc = db_get_include(&dev.db, i);
			ret = package_path(root, inc, rel, sizeof(rel)) &&
					stage_file(stage, inc, rel);
		}
	}

	return ret;
}


// Writes the uvdev.json manifest naming each file of the package.
static bool write_manifest(const char *stage, const char *dbrel) {
	static char buffer[4096];
	parser_writer_st writer;
	uv_errors_e e = ERR_NONE;

	e |= parser_writer_init(&writer, buffer, sizeof(buffer),
			PARSER_FORMAT_JSON);
	if (strlen(version) != 0) {
		e |= parser_writer_add_string(&writer, "VERSION", version);
	}
	e |= parser_writer_add_string(&writer, "FIRMWARE", path_basename(firmware));
	if (strlen(linuxbin) != 0) {
		e |= parser_writer_add_string(&writer, "LINUX_BIN",
				path_basename(linuxbin));
	}
	if (strlen(bootloader) != 0) {
		e |= parser_writer_add_string(&writer, "BOOTLOADER",
				path_basename(bootloader));
	}
	if (media_count != 0) {
		e |= parser_writer_add_string(&writer, "MEDIA", MEDIA_DIR);
	}
	e |= parser_writer_add_string(&writer, "DATABASE", dbrel);
	e |= parser_writer_end(&writer);

	bool ret = false;
	if (e != ERR_NONE) {
		ERRORSTR("ERROR: failed to write the uvdev.json manifest.\n");
	}
	else {
		char manifest[1100];
		snprintf(manifest, sizeof(manifest), "%s/uvdev.json", stage);
		ret = parser_write_file(manifest, buffer, PARSER_FORMAT_JSON);
		if (!ret) {
			ERRORSTR("ERROR: failed to write the uvdev.json manifest.\n");
		}
	}

	return ret;
}


// Builds the absolute output path from the argument of the command: adds the
// '.uvdev' extension when it is missing and creates the directory which the
// package is written into.
static bool output_path(const char *arg, char *dest, size_t dest_len) {
	bool ret = false;
	char path[1024];

	strncpy(path, arg, sizeof(path) - 1);
	path[sizeof(path) - 1] = '\0';
	if ((strlen(path) < 6) ||
			(strcmp(path + strlen(path) - 6, ".uvdev") != 0)) {
		strncat(path, ".uvdev", sizeof(path) - strlen(path) - 1);
	}

	if (path[0] == '/') {
		ret = (strlen(path) < dest_len);
		if (ret) {
			strcpy(dest, path);
		}
	}
	else {
		char cwd[1024];
		if (getcwd(cwd, sizeof(cwd)) == NULL) {
			ERRORSTR("ERROR: failed to resolve the working directory.\n");
		}
		else {
			ret = ((size_t) snprintf(dest, dest_len, "%s/%s", cwd, path) <
					dest_len);
		}
	}
	if (!ret) {
		ERROR("ERROR: the output path '%s' is too long.\n", arg);
	}
	else {
		// create the directory the package is written into, so that the output
		// can be given directly into a publishing directory which doesn't exist
		// yet
		char dir[1024];
		strncpy(dir, dest, sizeof(dir) - 1);
		dir[sizeof(dir) - 1] = '\0';
		char *sep = strrchr(dir, '/');
		if ((sep != NULL) && (sep != dir)) {
			*sep = '\0';
			char cmd[1100];
			snprintf(cmd, sizeof(cmd), "mkdir -p \"%s\"", dir);
			ret = run(cmd);
			if (!ret) {
				ERROR("ERROR: failed to create the directory '%s'.\n", dir);
			}
		}
	}

	return ret;
}


// Checks that everything which the package is assembled from was given and can
// be read.
static bool check_inputs(void) {
	bool ret = false;

	if (!db_is_loaded(&dev.db)) {
		ERRORSTR("ERROR: no database. Give the device database with the 'db' "
				"option before 'makeuvdev'.\n");
	}
	else if (strlen(firmware) == 0) {
		ERRORSTR("ERROR: no firmware binary. Give it with the 'firmware' "
				"option before 'makeuvdev'.\n");
	}
	else if (!is_file(firmware)) {
		ERROR("ERROR: the firmware binary '%s' was not found.\n", firmware);
	}
	else if ((strlen(linuxbin) != 0) && !is_file(linuxbin)) {
		ERROR("ERROR: the Linux simulator executable '%s' was not found.\n",
				linuxbin);
	}
	else if ((strlen(bootloader) != 0) && !is_file(bootloader)) {
		ERROR("ERROR: the bootloader binary '%s' was not found.\n", bootloader);
	}
	else {
		ret = true;
		for (unsigned int i = 0; (i < media_count) && ret; i++) {
			if (!is_file(media[i]) && !is_dir(media[i])) {
				ERROR("ERROR: the media '%s' was not found.\n", media[i]);
				ret = false;
			}
		}
	}

	return ret;
}


#endif



bool cmd_makeuvdev(const char *arg) {
	bool ret = false;
#if CONFIG_TARGET_WIN
	(void) arg;
	ERRORSTR("ERROR: writing .uvdev packages is not supported on Windows.\n");
#else
	if ((arg == NULL) || (strlen(arg) == 0)) {
		ERRORSTR("ERROR: give the .uvdev file to write as the argument.\n");
	}
	else if (!check_inputs()) {
		// the reason was already reported
	}
	else {
		char outpath[1100];
		char stage[1024];
		if (!output_path(arg, outpath, sizeof(outpath))) {
			// the reason was already reported
		}
		else if (!archive_mktempdir("uvcan_uvdev", stage, sizeof(stage))) {
			ERRORSTR("ERROR: failed to create a temporary directory for the "
					"device package.\n");
		}
		else {
			printf("Creating the device package '%s'\n", outpath);
			fflush(stdout);

			// the firmware, the simulator and the bootloader are packaged as
			// they are, at the package's root
			printf("Adding firmware '%s'\n", path_basename(firmware));
			bool ok = stage_file(stage, firmware, path_basename(firmware));
			if (ok && (strlen(linuxbin) != 0)) {
				printf("Adding Linux simulator '%s'\n", path_basename(linuxbin));
				ok = stage_file(stage, linuxbin, path_basename(linuxbin));
			}
			if (ok && (strlen(bootloader) != 0)) {
				printf("Adding bootloader '%s'\n", path_basename(bootloader));
				ok = stage_file(stage, bootloader, path_basename(bootloader));
			}

			// the media assets are collected into a directory of their own
			if (ok && (media_count != 0)) {
				char mediadir[1100];
				char cmd[1200];
				snprintf(mediadir, sizeof(mediadir), "%s/%s", stage, MEDIA_DIR);
				snprintf(cmd, sizeof(cmd), "mkdir -p \"%s\"", mediadir);
				ok = run(cmd);
				for (unsigned int i = 0; (i < media_count) && ok; i++) {
					printf("Adding media '%s'\n", media[i]);
					ok = stage_media(mediadir, media[i]);
				}
			}

			char dbrel[512] = {};
			if (ok) {
				ok = stage_database(stage, dbrel, sizeof(dbrel));
			}
			if (ok) {
				ok = write_manifest(stage, dbrel);
			}

			if (ok) {
				// zip the staging directory into the output path. Remove any
				// existing file first so zip does not append to a stale archive.
				char cmd[4096];
				snprintf(cmd, sizeof(cmd),
						"rm -f \"%s\"; ( cd \"%s\" && zip -qr \"%s\" . )",
						outpath, stage, outpath);
				if (run(cmd)) {
					printf("Device package saved to '%s'\n", outpath);
					ret = true;
				}
				else {
					ERRORSTR("ERROR: failed to create the .uvdev archive. Is "
							"'zip' installed?\n");
				}
			}
			fflush(stdout);

			archive_rmtree(stage);
		}
	}
#endif

	return ret;
}
