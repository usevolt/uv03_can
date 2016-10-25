/*
 * ui.h
 *
 *  Created on: Jul 7, 2016
 *      Author: usevolt
 */


#ifndef UI_H_
#define UI_H_


#include <string.h>
#include <pthread.h>


extern pthread_mutex_t ui_mutex;

#define FG_RED				"\e[31m"

#define FG_VIOLET			"\e[35m"
#define BG_VIOLET			"\e[45m"

#define FG_WHITE			"\e[37m"
#define BG_WHITE			"\e[40m"


/// @brief: Initializes the UI
void ui_init();


/// @brief: printf is used for logging white text to the command line area
#define ui_printf(...)		pthread_mutex_lock(&ui_mutex); \
	printf(FG_WHITE BG_WHITE __VA_ARGS__); \
	printf(FG_WHITE BG_WHITE); \
	pthread_mutex_unlock(&ui_mutex)

/// @brief: errf is used for informative text logged with red to the command line area
#define ui_errf(...)		pthread_mutex_lock(&ui_mutex); \
	printf(FG_RED BG_WHITE __VA_ARGS__); \
	printf(FG_WHITE BG_WHITE); \
	pthread_mutex_unlock(&ui_mutex)


/// @brief: Logf is used for showing text on the violet background information line
#define ui_logf(...)		pthread_mutex_lock(&ui_mutex); \
	printf(FG_VIOLET BG_WHITE __VA_ARGS__); \
	printf(FG_WHITE BG_WHITE); \
	pthread_mutex_unlock(&ui_mutex)





#endif
