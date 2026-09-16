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
#ifndef UVCAN_VERSIONNOTES_H_
#define UVCAN_VERSIONNOTES_H_

#include <stddef.h>
#include <stdint.h>


/// @file: What is new in this build, compiled into it.
///
/// The notes are the subject line of every commit made after the last published
/// package: uvcan is published as a plain binary from a shelf that holds no
/// release notes, so the only place a running uvcan can get them from is
/// itself. packaging/gen-version-notes.sh writes them into a header at build
/// time -- the range is read from the manifest of the last published package,
/// ../prod/latest.json -- and this module is the whole of what the rest of
/// uvcan sees of them. The UI shows them in its "Version notes" window (see
/// ui/versionnotes_win.h).
///
/// A build made where that manifest is not (a fresh clone; prod/ is not
/// tracked) cannot know what was last published. It carries the newest handful
/// of commits instead and says so: versionnotes_since_build() is then 0, and a
/// caller which tells the user where the notes start has to say "recent
/// changes" rather than name a build.


/// @brief: One note: a commit, as one line.
typedef struct {
	/// @brief: The day the commit was made, as "YYYY-MM-DD".
	const char *date;
	/// @brief: The commit's subject line.
	const char *text;
} versionnote_st;


/// @brief: How many notes this build carries. 0 when it is the published build
/// itself, i.e. nothing has changed since.
uint32_t versionnotes_count(void);


/// @brief: The note at *index*, newest first, or NULL when there is none.
const versionnote_st *versionnotes_get(uint32_t index);


/// @brief: How many commits were left out of the list, over the cap it is kept
/// under. 0 in every ordinary case.
uint32_t versionnotes_omitted(void);


/// @brief: The published build the notes start after: its build number, and its
/// version name ("304-ge126").
///
/// The number is 0, and the name "", when this build does not know what was
/// last published -- see the note on the file above.
uint32_t versionnotes_since_build(void);
const char *versionnotes_since_name(void);


#endif /* UVCAN_VERSIONNOTES_H_ */
