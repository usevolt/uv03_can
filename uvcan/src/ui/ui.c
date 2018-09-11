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



#include <ui.h>
#include <stdio.h>
#include <string.h>
#include <uv_terminal.h>
#include <gtk/gtk.h>
#include "main.h"
#include "gresources.h"
#include "db.h"


static void ui_main(void *ptr);
static void obj_dict_init(void);
static void obj_dict_scale_value_changed(GtkRange *range, gpointer user_data);
static void obj_dict_spin_button_value_changed(GtkSpinButton *spin_button, gpointer user_data);


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
	gtk_container_add(GTK_CONTAINER(headerbox), label);
}



static void par_expand(obj_dict_par_st *this) {
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
	gtk_container_add(GTK_CONTAINER(this->gobject), row);

	if (CANOPEN_IS_INTEGER(this->obj->obj.type)) {
		GtkWidget *obj = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,
				this->obj->min, this->obj->max, 1);
		this->scale = obj;
		gtk_widget_set_hexpand(obj, TRUE);
		gtk_scale_set_draw_value(GTK_SCALE(obj), FALSE);
		g_signal_connect(obj, "value-changed", G_CALLBACK(obj_dict_scale_value_changed), NULL);
		gtk_container_add(GTK_CONTAINER(row), obj);

		obj = gtk_spin_button_new_with_range(this->obj->min, this->obj->max, 1);
		this->spin_button = obj;
		g_signal_connect(obj, "value-changed", G_CALLBACK(obj_dict_spin_button_value_changed), NULL);
		gtk_container_add(GTK_CONTAINER(row), obj);
	}

	gtk_widget_show_all(this->gobject);
}

static void par_compress(obj_dict_par_st *this) {
	GList *children = gtk_container_get_children(GTK_CONTAINER(this->gobject));
	if (g_list_length(children) >= 2) {
		GtkWidget *child = (GtkWidget*) g_list_nth_data(children, 1);
		gtk_container_remove(GTK_CONTAINER(this->gobject), child);
	}
}


#define this (&dev.ui)

static void obj_dict_scale_value_changed(GtkRange *range, gpointer user_data) {
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(this->obj_dict_params[this->selected_par].spin_button),
			gtk_range_get_value(GTK_RANGE(this->obj_dict_params[this->selected_par].scale)));
}

static void obj_dict_spin_button_value_changed(GtkSpinButton *spin_button, gpointer user_data) {
	gtk_range_set_value(GTK_RANGE(this->obj_dict_params[this->selected_par].scale),
			gtk_spin_button_get_value(GTK_SPIN_BUTTON(this->obj_dict_params[this->selected_par].spin_button)));
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



bool cmd_ui(const char *arg) {
	bool ret = false;

	uv_rtos_add_idle_task(&ui_main);

	ret = true;

	return ret;
}


static void can_dev_changed(GtkComboBox *box) {
	strcpy(dev.can_channel, gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(box)));
	gtk_switch_set_state(GTK_SWITCH(this->can_switch), FALSE);
}


static gboolean can_switch (GtkSwitch *widget, gboolean state, gpointer user_data) {
	if (state) {
		uv_can_set_baudrate(dev.can_channel,
				gtk_spin_button_get_value(GTK_SPIN_BUTTON(this->can_baudrate)));
	}
	return FALSE;
}

void db_file_set (GtkFileChooserButton *widget, gpointer user_data) {
	if (!cmd_db(gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(this->db)))) {
		GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(this->window), 0,
				GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, "Selected database file '%s' is invalid.",
				gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(this->db)));
		gtk_dialog_run(GTK_DIALOG(d));
		gtk_widget_destroy(GTK_WIDGET(d));
	}
	else {
		obj_dict_init();
	}
}


static void obj_dict_init(void) {
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

static void activate (GtkApplication* app, gpointer user_data)
{
	GtkBuilder *builder = gtk_builder_new();
	GResource *resource = gresources_get_resource();
	GBytes *strbytes = g_resource_lookup_data(resource, "/org/gtk/uvcan/src/ui/mainwindow.ui",
			G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
	GByteArray *str;
	if (strbytes == NULL) {
		printf("Error parsing main window data\n");
	}
	else {
		str = g_bytes_unref_to_array(strbytes);
		if (gtk_builder_add_from_string(builder,
				&g_array_index(str, gchar, 0), strlen(&g_array_index(str, char, 0)), NULL)) {

			this->window = GTK_WIDGET(gtk_builder_get_object(builder, "mainwindow"));

			GObject *obj;

			// CAN dev
			obj = gtk_builder_get_object(builder, "can_dev");
			this->can_dev = obj;
			// add all found devices to CAN dev selection combo box
			for (uint8_t i = 0; i < uv_can_get_device_count(); i++) {
				gtk_combo_box_text_insert_text(GTK_COMBO_BOX_TEXT(obj), i,
						uv_can_get_device_name(i));
			}
			gtk_combo_box_set_active(GTK_COMBO_BOX(obj), 0);
			g_signal_connect(obj, "changed", G_CALLBACK(can_dev_changed), NULL);

			// Baudrate
			obj = gtk_builder_get_object(builder, "can_baudrate");
			this->can_baudrate = obj;
			gtk_spin_button_set_range(GTK_SPIN_BUTTON(obj),0, 2000000);
			gtk_spin_button_set_value(GTK_SPIN_BUTTON(obj), 250000);
			gtk_spin_button_set_increments(GTK_SPIN_BUTTON(obj), 50000, 50000);

			// can switch
			obj = gtk_builder_get_object(builder, "can_switch");
			this->can_switch = obj;
			g_signal_connect(obj, "state-set", G_CALLBACK(can_switch), NULL);

			// database
			obj = gtk_builder_get_object(builder, "database");
			this->db = obj;
			g_signal_connect(obj, "file-set", G_CALLBACK(db_file_set), NULL);
			gtk_file_chooser_button_set_title(GTK_FILE_CHOOSER_BUTTON(obj),
					"Select database file (*.json)");
			if (db_is_loaded(&dev.db)) {
				gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(this->db), db_get_file(&dev.db));
			}

			// object dictionary
			obj = gtk_builder_get_object(builder, "obj_dict");
			this->obj_dict = obj;
			gtk_list_box_set_selection_mode(GTK_LIST_BOX(obj),GTK_SELECTION_NONE);
			obj_dict_init();
			g_signal_connect(obj, "row-activated", G_CALLBACK(obj_dict_par_selected), NULL);
			this->selected_par = -1;




			g_object_unref(builder);
			gtk_window_set_application(GTK_WINDOW(this->window), app);
			gtk_widget_show_all(GTK_WIDGET(this->window));
		}
		else {
			printf("error!\n");
		}
	}
}

static void ui_main(void *ptr) {
	GtkApplication *app;

	app = gtk_application_new("org.gtk.uvcan", G_APPLICATION_FLAGS_NONE);
	g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
	g_application_run(G_APPLICATION(app), 0, NULL);
	g_object_unref(app);

	exit(0);
}
