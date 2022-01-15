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


#ifndef LOADPARAM_H_
#define LOADPARAM_H_


#include <uv_utilities.h>
#include <stdbool.h>


#define QUERY_NAME_LEN	64
#define QUERY_QUESTION_LEN	128
#define QUERY_ANSWER_COUNT	6
#define QUERY_ANSWER_LEN	64

typedef struct {
	char name[QUERY_NAME_LEN];
	char question[QUERY_QUESTION_LEN];
	char answers[QUERY_ANSWER_COUNT][QUERY_ANSWER_LEN];
	uint8_t answer_count;
	uint8_t correct_answer;
} query_st;


#define QUERY_COUNT			20

typedef struct {
	char files[64][256];
	unsigned int current_file;

	query_st queries_buffer[QUERY_COUNT];
	uv_vector_st queries;

	uint8_t modified_dev_nodeids[64];
	uint8_t dev_count;

	// true if the loading has finished
	bool finished;
} loadparam_st;



/// @brief: Returns true if the loading has finished. Note that this doesn't separate
/// successful and failure loading from each other
static inline bool loadparam_is_finished(loadparam_st *this) {
	return this->finished;
}



/// @brief: Loads uvcan parameters with the name of **arg** to device selected
/// previously with command *nodeid*.
bool cmd_loadparam(const char *arg);



#endif /* LOAD_H_ */



