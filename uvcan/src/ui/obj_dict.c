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



#include "obj_dict.h"
#include "main.h"
#include <gtk/gtk.h>


static void obj_dict_scale_value_changed(GtkRange *range, gpointer user_data);
static void obj_dict_spin_button_value_changed(GtkSpinButton *spin_button, gpointer user_data);
static void obj_dict_text_entry_activated(GtkEntry *entry, gpointer user_data);
static void obj_dict_text_button_clicked(GtkButton *button, gpointer user_data);
static void par_init(obj_dict_par_st *this, db_obj_st *db_obj, GtkWidget *parent);
static void par_expand(obj_dict_par_st *this);
static void par_compress(obj_dict_par_st *this);
static void obj_dict_par_selected (GtkListBox *box, GtkListBoxRow *row, gpointer user_data);
static void reset(GtkButton *button, gpointer user_data);
static void revert(GtkButton *button, gpointer user_data);
static void save(GtkButton *button, gpointer user_data);




void obj_dict_update_view(obj_dict_st *this) {
	GList *children, *iter;

	children = gtk_container_get_children(GTK_CONTAINER(this->obj_dict));
	for (iter = children; iter != NULL; iter = g_list_next(iter)) {
		gtk_widget_destroy(GTK_WIDGET(iter->data));
	}
	g_list_free(children);

	for (uint8_t i = 0; i < db_get_object_count(&dev.db); i++) {
		par_init(&this->obj_dict_params[i], db_get_obj(&dev.db, i), GTK_WIDGET(this->obj_dict));
	}
	gtk_widget_show_all(GTK_WIDGET(this->obj_dict));
}


void obj_dict_show(obj_dict_st *this, GObject *container, struct GtkBuilder *builder) {
	this->obj_dict = (GObject*) container;
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(container), GTK_SELECTION_NONE);

	obj_dict_update_view(this);

	g_signal_connect(container, "row-activated", G_CALLBACK(obj_dict_par_selected), NULL);
	this->selected_par = -1;

	GObject *obj = gtk_builder_get_object(GTK_BUILDER(builder), "reset");
	g_signal_connect(obj, "clicked", G_CALLBACK(reset), NULL);

	obj = gtk_builder_get_object(GTK_BUILDER(builder), "revert");
	g_signal_connect(obj, "clicked", G_CALLBACK(revert), NULL);

	obj = gtk_builder_get_object(GTK_BUILDER(builder), "save");
	g_signal_connect(obj, "clicked", G_CALLBACK(save), NULL);
}




static void par_init(obj_dict_par_st *this, db_obj_st *db_obj, GtkWidget *parent) {
	this->obj = db_obj;
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
	GtkWidget *headerbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 50);
	this->gobject = box;
	gtk_widget_set_margin_top(box, 20);
	gtk_widget_set_margin_bottom(box, gtk_widget_get_margin_top(box));
	gtk_container_add(GTK_CONTAINER(parent), box);
	gtk_container_add(GTK_CONTAINER(box), headerbox);

	snprintf(this->main_index_str, sizeof(this->main_index_str),
			"0x%x", db_obj->obj.main_index);
	GtkWidget *label = gtk_label_new(this->main_index_str);
	gtk_container_add(GTK_CONTAINER(headerbox), label);

	label = gtk_label_new(db_obj->name);
	g_object_set(label, "expand", TRUE, NULL);
	gtk_container_add(GTK_CONTAINER(headerbox), label);

	db_permission_to_longstr(db_obj->obj.permissions, this->permissions_str);
	strcat(this->permissions_str, "\n");
	char str[32];
	db_type_to_str(db_obj->obj.type, str);
	strcat(this->permissions_str, str);
	label = gtk_label_new(this->permissions_str);
	gtk_label_set_xalign(GTK_LABEL(label), 0.5f);
	gtk_container_add(GTK_CONTAINER(headerbox), label);
}



static void par_expand(obj_dict_par_st *this) {
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
	gtk_container_add(GTK_CONTAINER(this->gobject), row);

	if (CANOPEN_IS_INTEGER(this->obj->obj.type)) {
		// read the object value from the device
		int32_t value = 0;
		if (CANOPEN_IS_READABLE(this->obj->obj.permissions)) {
			if (uv_canopen_sdo_read(db_get_nodeid(&dev.db), this->obj->obj.main_index,
					this->obj->obj.sub_index, CANOPEN_TYPE_LEN(this->obj->obj.type), &value) != ERR_NONE) {
				// reading the value from the device failed
				GtkWidget *d;
				if (gtk_switch_get_state(GTK_SWITCH(dev.ui.can_switch))) {
					d = gtk_message_dialog_new(GTK_WINDOW(dev.ui.window), 0,
							GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, "Couldn't read the object value from the device. "
									"Check that the device is connected and powered properly.");
				}
				else {
					d = gtk_message_dialog_new(GTK_WINDOW(dev.ui.window), 0,
							GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, "Couldn't read the object value from the device "
									"because the CAN-USB adapter is not connected.");
				}
				gtk_dialog_run(GTK_DIALOG(d));
				gtk_widget_destroy(GTK_WIDGET(d));
			}
		}

		if (CANOPEN_IS_WRITABLE(this->obj->obj.permissions)) {
			GtkWidget *obj = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,
					this->obj->min.value_int, this->obj->max.value_int, 1);
			this->scale = obj;
			gtk_widget_set_hexpand(obj, TRUE);
			gtk_scale_set_draw_value(GTK_SCALE(obj), FALSE);
			gtk_range_set_value(GTK_RANGE(obj), value);
				g_signal_connect(obj, "value-changed", G_CALLBACK(obj_dict_scale_value_changed), NULL);
			gtk_container_add(GTK_CONTAINER(row), obj);

			obj = gtk_spin_button_new_with_range(this->obj->min.value_int, this->obj->max.value_int, 1);
			this->spin_button = obj;
			gtk_spin_button_set_value(GTK_SPIN_BUTTON(obj), value);
			g_signal_connect(obj, "value-changed", G_CALLBACK(obj_dict_spin_button_value_changed), NULL);
			gtk_container_add(GTK_CONTAINER(row), obj);
		}
		else {
			// read only objects
			char str[1024];
			sprintf(str, "Value: %i", value);
			GtkWidget *obj = gtk_label_new(str);
			gtk_widget_set_hexpand(obj, true);
			gtk_container_add(GTK_CONTAINER(row), obj);
			this->label = obj;
		}
	}
	else if (CANOPEN_IS_ARRAY(this->obj->obj.type)) {

		db_array_child_st *child = this->obj->child_ptr;
		bool errmessage_shown = false;
		for (int i = 0; i < this->obj->obj.array_max_size; i++) {
			GtkWidget *obj = gtk_label_new(child->name);
			g_object_set(obj, "width-request", 300, NULL);
			gtk_container_add(GTK_CONTAINER(row), obj);

			int32_t value = 0;
			if (CANOPEN_IS_READABLE(this->obj->obj.permissions) &&
					!errmessage_shown &&
					uv_canopen_sdo_read(db_get_nodeid(&dev.db), this->obj->obj.main_index,
							i + 1, CANOPEN_TYPE_LEN(this->obj->obj.type), &value) != ERR_NONE) {
				// reading the value from the device failed
				GtkWidget *d;
				if (gtk_switch_get_state(GTK_SWITCH(dev.ui.can_switch))) {
					d = gtk_message_dialog_new(GTK_WINDOW(dev.ui.window), 0,
							GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, "Couldn't read the object value from the device. "
									"Check that the device is connected and powered properly.");
				}
				else {
					d = gtk_message_dialog_new(GTK_WINDOW(dev.ui.window), 0,
							GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, "Couldn't read the object value from the device "
									"because the CAN-USB adapter is not connected.");
				}
				gtk_dialog_run(GTK_DIALOG(d));
				gtk_widget_destroy(GTK_WIDGET(d));
				errmessage_shown = true;
			}


			obj = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,
					child->min.value_int, child->max.value_int, 1);
			gtk_widget_set_hexpand(obj, TRUE);
			gtk_scale_set_draw_value(GTK_SCALE(obj), FALSE);
			gtk_range_set_value(GTK_RANGE(obj), value);
			gtk_widget_set_sensitive(obj, CANOPEN_IS_WRITABLE(this->obj->obj.permissions));
			g_signal_connect(obj, "value-changed", G_CALLBACK(obj_dict_scale_value_changed), NULL);
			gtk_container_add(GTK_CONTAINER(row), obj);

			obj = gtk_spin_button_new_with_range(child->min.value_int, child->max.value_int, 1);
			gtk_widget_set_sensitive(obj, CANOPEN_IS_WRITABLE(this->obj->obj.permissions));
			gtk_spin_button_set_value(GTK_SPIN_BUTTON(obj), value);
			g_signal_connect(obj, "value-changed", G_CALLBACK(obj_dict_spin_button_value_changed), NULL);
			gtk_container_add(GTK_CONTAINER(row), obj);

			child = child->next_sibling;
			if (child == NULL) {
				break;
			}
			row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
			gtk_container_add(GTK_CONTAINER(this->gobject), row);

		}
	}
	else if (CANOPEN_IS_STRING(this->obj->obj.type)) {
		char str[1024] = { 0 };
		// read the value from device

		if (CANOPEN_IS_READABLE(this->obj->obj.permissions) &&
				uv_canopen_sdo_read(db_get_nodeid(&dev.db), this->obj->obj.main_index,
						this->obj->obj.sub_index, this->obj->array_max_size.value_int, str)) {
			// reading failed, show the error message
			GtkWidget *d;
			if (gtk_switch_get_state(GTK_SWITCH(dev.ui.can_switch))) {
				d = gtk_message_dialog_new(GTK_WINDOW(dev.ui.window), 0,
						GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, "Couldn't read the object value from the device. "
								"Check that the device is connected and powered properly.");
			}
			else {
				d = gtk_message_dialog_new(GTK_WINDOW(dev.ui.window), 0,
						GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, "Couldn't read the object value from the device "
								"because the CAN-USB adapter is not connected.");
			}
			gtk_dialog_run(GTK_DIALOG(d));
			gtk_widget_destroy(GTK_WIDGET(d));
		}
		GtkWidget *obj = gtk_entry_new();
		this->text_entry = GTK_WIDGET(obj);
		gtk_entry_set_text(GTK_ENTRY(obj), str);
		g_signal_connect(obj, "activate", G_CALLBACK(obj_dict_text_entry_activated), this);
		gtk_container_add(GTK_CONTAINER(row), obj);

		obj = gtk_button_new_with_label("Update");
		this->text_button = obj;
		g_signal_connect(obj, "clicked", G_CALLBACK(obj_dict_text_button_clicked), this);
		gtk_container_add(GTK_CONTAINER(row), obj);
	}
	else {
		// unknown data type
		GtkWidget *obj = gtk_label_new("Unknown parameter type");
		gtk_container_add(GTK_CONTAINER(row), obj);
	}

	gtk_widget_show_all(this->gobject);
}



static void par_compress(obj_dict_par_st *this) {
	GList *children = gtk_container_get_children(GTK_CONTAINER(this->gobject));
	while (g_list_length(children) >= 2) {
		GtkWidget *child = (GtkWidget*) g_list_nth_data(children, 1);
		if (child) {
			gtk_container_remove(GTK_CONTAINER(this->gobject), child);
		}
		else {
			break;
		}
		children = gtk_container_get_children(GTK_CONTAINER(this->gobject));
	}
}



#define this (&dev.ui.obj_dict)


static void obj_dict_scale_value_changed(GtkRange *range, gpointer user_data) {
	db_obj_st *obj = db_get_obj(&dev.db, this->selected_par);
	if (CANOPEN_IS_ARRAY(obj->obj.type)) {
		GtkWidget *p = gtk_widget_get_parent(GTK_WIDGET(range));
		// note: number 2 refers to the spin button. If the order of UI elements is modified,
		// it should be modified accordingly
		GtkWidget *spinbutton = g_list_nth_data(gtk_container_get_children(GTK_CONTAINER(p)), 2);
		gtk_spin_button_set_value(GTK_SPIN_BUTTON(spinbutton), gtk_range_get_value(range));
	}
	else {
		gtk_spin_button_set_value(GTK_SPIN_BUTTON(this->obj_dict_params[this->selected_par].spin_button),
				gtk_range_get_value(range));
	}
}

static void obj_dict_spin_button_value_changed(GtkSpinButton *spin_button, gpointer user_data) {
	db_obj_st *obj = db_get_obj(&dev.db, this->selected_par);
	int16_t sindex = obj->obj.sub_index;
	if (CANOPEN_IS_ARRAY(obj->obj.type)) {
		// find out which index was modified
		GtkWidget *p = gtk_widget_get_parent(GTK_WIDGET(spin_button));
		GtkWidget *pp = gtk_widget_get_parent(p);
		GList *children = gtk_container_get_children(GTK_CONTAINER(pp));
		int i;
		for (i = 0; i < g_list_length(children); i++) {
			GtkWidget *c = (GtkWidget*) g_list_nth_data(children, i);
			// found the children number
			if (c == p) {
				sindex = i;
				break;
			}
		}
		// note: number 1 refers to the range. If the order of UI elements is modified,
		// it should be modified accordingly
		GtkWidget *range = g_list_nth_data(gtk_container_get_children(GTK_CONTAINER(p)), 1);
		gtk_range_set_value(GTK_RANGE(range), gtk_spin_button_get_value(spin_button));
	}
	else {
		gtk_range_set_value(GTK_RANGE(this->obj_dict_params[this->selected_par].scale),
				gtk_spin_button_get_value(GTK_SPIN_BUTTON(this->obj_dict_params[this->selected_par].spin_button)));
	}
	int32_t value = gtk_spin_button_get_value(spin_button);
	uv_canopen_sdo_write(db_get_nodeid(&dev.db), obj->obj.main_index, sindex,
			CANOPEN_TYPE_LEN(obj->obj.type), &value);
}


static void obj_dict_text_entry_activated(GtkEntry *entry, gpointer user_data) {
	obj_dict_par_st *par = &this->obj_dict_params[this->selected_par];
	obj_dict_text_button_clicked(GTK_BUTTON(par->text_button), par);
}

static void obj_dict_text_button_clicked(GtkButton *button, gpointer user_data) {
	db_obj_st *obj = db_get_obj(&dev.db, this->selected_par);
	obj_dict_par_st *par = &this->obj_dict_params[this->selected_par];

	const char *value = gtk_entry_get_text(GTK_ENTRY(par->text_entry));
	uv_canopen_sdo_write(db_get_nodeid(&dev.db), obj->obj.main_index, obj->obj.sub_index,
			uv_mini(obj->array_max_size.value_int, strlen(value) + 1), (void*) value);
}


static void obj_dict_par_selected (GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
	if (this->selected_par == gtk_list_box_row_get_index(row)) {
		par_compress(&this->obj_dict_params[this->selected_par]);
		this->selected_par = -1;
	}
	else {
		if (this->selected_par != -1) {
			par_compress(&this->obj_dict_params[this->selected_par]);
		}
		this->selected_par = gtk_list_box_row_get_index(row);
		par_expand(&this->obj_dict_params[this->selected_par]);
	}
}




static void reset(GtkButton *button, gpointer user_data) {
	uv_canopen_nmt_master_send_cmd(db_get_nodeid(&dev.db),
			CANOPEN_NMT_CMD_RESET_NODE);
}


static void revert(GtkButton *button, gpointer user_data) {
	GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(dev.ui.window), 0,
			GTK_MESSAGE_WARNING, GTK_BUTTONS_YES_NO,
			"Clear all modifications and restore device to factory settings?");
	int result = gtk_dialog_run(GTK_DIALOG(d));
	gtk_widget_destroy(GTK_WIDGET(d));

	if (result == GTK_RESPONSE_YES) {
		uv_canopen_sdo_write(db_get_nodeid(&dev.db), CONFIG_CANOPEN_RESTORE_PARAMS_INDEX, 1, 4, "load");

		uv_canopen_nmt_master_send_cmd(db_get_nodeid(&dev.db),
				CANOPEN_NMT_CMD_RESET_NODE);

		if (this->selected_par >= 0) {
			par_compress(&this->obj_dict_params[this->selected_par]);
		}
	}
}


static void save(GtkButton *button, gpointer user_data) {
	uv_canopen_sdo_write(db_get_nodeid(&dev.db), CONFIG_CANOPEN_STORE_PARAMS_INDEX, 1, 4, "save");
}
