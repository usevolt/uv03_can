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


#include "uvdev.h"
#include "main.h"
#include "archive.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv_json.h>


// Reads the string value of *key* from the manifest object into *dest*. Leaves
// *dest* an empty string when the key is missing or not a string.
static void manifest_get_str(char *manifest, const char *key,
		char *dest, size_t dest_len) {
	dest[0] = '\0';
	char *child = uv_jsonreader_find_child(manifest, (char*) key);
	if (child != NULL && uv_jsonreader_get_type(child) == JSON_STRING) {
		uv_jsonreader_get_string(child, dest, dest_len);
	}
}


bool uvdev_open(uvdev_st *this, const char *uvdev_path) {
	bool ret = false;
	memset(this, 0, sizeof(*this));

	// create a fresh temporary directory to extract into
	if (!archive_mktempdir("uvcan_uvdev", this->dir, sizeof(this->dir))) {
		PRINT("Failed to create a temporary directory for '%s'.\n", uvdev_path);
	}
	else {
		// .uvdev is a plain zip archive; extract it quietly, overwriting
		if (!archive_extract(uvdev_path, this->dir)) {
			PRINT("Failed to extract '%s'. Is it a valid .uvdev package and is "
					"the extraction tool (unzip / tar) available?\n", uvdev_path);
			uvdev_close(this);
		}
		else {
			// read the uvdev.json manifest
			char manifest_path[1100];
			snprintf(manifest_path, sizeof(manifest_path), "%s/uvdev.json",
					this->dir);
			FILE *fptr = fopen(manifest_path, "r");
			if (fptr == NULL) {
				PRINT("Package '%s' does not contain a uvdev.json manifest.\n",
						uvdev_path);
				uvdev_close(this);
			}
			else {
				fseek(fptr, 0, SEEK_END);
				long size = ftell(fptr);
				rewind(fptr);
				char *data = malloc(size + 1);
				if (data != NULL && fread(data, 1, size, fptr) == (size_t) size) {
					data[size] = '\0';
					if (uv_jsonreader_init(data, size) == ERR_NONE) {
						manifest_get_str(data, "DATABASE",
								this->database, sizeof(this->database));
						manifest_get_str(data, "FIRMWARE",
								this->firmware, sizeof(this->firmware));
						manifest_get_str(data, "LINUX_BIN",
								this->linux_bin, sizeof(this->linux_bin));
						manifest_get_str(data, "VERSION",
								this->version, sizeof(this->version));
						ret = true;
					}
					else {
						PRINT("Failed to parse the uvdev.json manifest of '%s'.\n",
								uvdev_path);
						uvdev_close(this);
					}
				}
				else {
					PRINT("Failed to read the uvdev.json manifest of '%s'.\n",
							uvdev_path);
					uvdev_close(this);
				}
				free(data);
				fclose(fptr);
			}
		}
	}

	return ret;
}


void uvdev_close(uvdev_st *this) {
	archive_rmtree(this->dir);
	this->dir[0] = '\0';
}
