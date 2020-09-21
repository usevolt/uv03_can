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


#ifndef SAVEPARAM_H_
#define SAVEPARAM_H_


#include <uv_utilities.h>
#include <stdbool.h>



typedef struct {
	char file[256];
	uint8_t nodeid;
	uint8_t progress;
	// true if the saving has finished
	bool finished;
} saveparam_st;



/// @brief: Returns true if the saving has finished. Note that this doesn't separate
/// successful and failure saving from each other
static inline bool saveparam_is_finished(saveparam_st *this) {
	return this->finished;
}

/// @brief: Returns the progress percent while uploading
static inline uint8_t saveparam_get_progress(saveparam_st *this) {
	return this->progress;
}



/// @brief: Loads uvcan parameters with the name of **arg** to device selected
/// previously with command *nodeid*.
bool cmd_saveparam(const char *arg);



#endif /* SAVE_H_ */



