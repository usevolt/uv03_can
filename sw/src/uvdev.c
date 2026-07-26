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
#include "parser.h"


// Reads the string value of *key* from the manifest object into *dest*. Leaves
// *dest* an empty string when the key is missing or not a string.
static void manifest_get_str(parser_node_st manifest, const char *key,
		char *dest, size_t dest_len) {
	dest[0] = '\0';
	parser_node_st child = parser_find_child(manifest, key);
	if (parser_node_is_valid(child) && parser_get_type(child) == PARSER_STRING) {
		parser_get_string(child, dest, dest_len);
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
			// read the uvdev manifest. It can be written either in JSON or
			// in YAML, i.e. as uvdev.json, uvdev.yaml or uvdev.yml
			char manifest_path[1100];
			if (!parser_find_file(this->dir, "uvdev",
					manifest_path, sizeof(manifest_path))) {
				PRINT("Package '%s' does not contain a uvdev.json manifest.\n",
						uvdev_path);
				uvdev_close(this);
			}
			else {
				char *data = NULL;
				parser_node_st manifest = parser_read_file(manifest_path, &data);
				if (parser_node_is_valid(manifest)) {
					manifest_get_str(manifest, "DATABASE",
							this->database, sizeof(this->database));
					manifest_get_str(manifest, "FIRMWARE",
							this->firmware, sizeof(this->firmware));
					manifest_get_str(manifest, "LINUX_BIN",
							this->linux_bin, sizeof(this->linux_bin));
					manifest_get_str(manifest, "VERSION",
							this->version, sizeof(this->version));
					manifest_get_str(manifest, "MEDIA",
							this->media, sizeof(this->media));
					ret = true;
				}
				else {
					PRINT("Failed to read the manifest '%s' of '%s'.\n",
							manifest_path, uvdev_path);
					uvdev_close(this);
				}
				free(data);
			}
		}
	}

	return ret;
}


void uvdev_close(uvdev_st *this) {
	archive_rmtree(this->dir);
	this->dir[0] = '\0';
}
