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

#include "versionnotes.h"

// The notes themselves, written by packaging/gen-version-notes.sh into
// release/generated/ as the makefile builds. This is the only file that
// includes them: everything else goes through the functions below, so a change
// to how they are generated stays here.
#include "versionnotes_gen.h"


uint32_t versionnotes_count(void) {
	return (uint32_t) VERSION_NOTES_COUNT;
}


const versionnote_st *versionnotes_get(uint32_t index) {
	const versionnote_st *ret = NULL;
	if (index < (uint32_t) VERSION_NOTES_COUNT) {
		ret = &version_notes[index];
	}
	else {
	}
	return ret;
}


uint32_t versionnotes_omitted(void) {
	return (uint32_t) VERSION_NOTES_OMITTED;
}


uint32_t versionnotes_since_build(void) {
	return (uint32_t) VERSION_NOTES_SINCE_BUILD;
}


const char *versionnotes_since_name(void) {
	return VERSION_NOTES_SINCE_NAME;
}
