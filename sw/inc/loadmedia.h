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


#ifndef LOADMEDIA_H_
#define LOADMEDIA_H_


#include <uv_utilities.h>
#include <stdbool.h>



typedef struct {
	// path to the media file or directory given as the command argument
	char file[1024];
} loadmedia_st;



/// @brief: Single entry point for loading media from a file path with the UV
/// media protocol. If *path* is a directory, all recognized media files in it
/// are loaded. Used by the loadmedia command and intended for reuse from within
/// uvcan (e.g. the UI). Returns false if *path* is NULL.
bool loadmedia_load(const char *path);


/// @brief: Loads the media file with a uv media protocol
bool cmd_loadmedia(const char *arg);


/// @brief: Asynchronously loads the media bundled in the .uvdev package at
/// *uvdev_path* onto the device at *nodeid*. Extracts the package, loads every
/// media file from its MEDIA directory with the UV media protocol, then releases
/// the package. Runs on its own task; poll loadmedia_load_device_is_finished().
/// Intended for the UI (the device tab's "Load media files" button).
void loadmedia_load_device_async(const char *uvdev_path, uint8_t nodeid);


/// @brief: Returns true when no asynchronous device media load is running (i.e.
/// the last loadmedia_load_device_async() has completed). True before any start.
bool loadmedia_load_device_is_finished(void);


#endif /* LOADMEDIA_H_ */
