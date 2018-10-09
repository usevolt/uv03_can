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


#ifndef UI_OBJ_DICT_H_
#define UI_OBJ_DICT_H_


#include <stdbool.h>
#include <uv_utilities.h>
#include <uv_canopen.h>
#include "db.h"



struct _GObject;
typedef struct _GObject GObject;

struct _GtkWidget;
typedef struct _GtkWidget GtkWidget;

struct GtkBuilder;



/// @brief: GTK object structure defining a single object dictionary parameter
typedef struct {
	GtkWidget *gobject;
	char main_index_str[16];
	char permissions_str[32];
	db_obj_st *obj;
	GtkWidget *scale;
	GtkWidget *spin_button;
} obj_dict_par_st;


/// @brief: Active object dictionary array pointer structure
typedef struct {
	GtkWidget *scale;
	GtkWidget *spin_button;
} obj_dict_arr_par_st;

typedef struct {
	GObject *obj_dict;
	obj_dict_par_st obj_dict_params[128];
	int8_t selected_par;
	obj_dict_arr_par_st active_arr_param[128];
} obj_dict_st;


/// @brief: Shows the object dictionary. Should be called when initializing the window where
/// object dictionary is.
void obj_dict_show(obj_dict_st *this, GObject *container, struct GtkBuilder *builder);

/// @brief: Loads the object dictionary entries from the database
void obj_dict_update_view(obj_dict_st *this);


#endif /* UI_OBJ_DICT_H_ */
