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


static gboolean update(gpointer data);
static void can_callb(void *ptr, uv_can_message_st *msg);
static void add_nodeid(uint8_t nodeid);


#define this (&dev.ui)



static void can_callb(void *ptr, uv_can_message_st *msg) {
	uv_mutex_lock(&this->mutex);
	if ((msg->id & ~0xFF) == CANOPEN_SDO_RESPONSE_ID ||
			(msg->id & ~0xFF) == CANOPEN_SDO_REQUEST_ID) {
		uint8_t nodeid = (msg->id & 0xFF);
		add_nodeid(nodeid);
	}
	terminal_can_rx(&this->terminal, msg);
	cantrace_rx(&this->cantrace, msg);
	uv_mutex_unlock(&this->mutex);
}

static void add_nodeid(uint8_t nodeid) {
	bool found = false;
	for (uint8_t i = 0; i < uv_vector_size(&this->nodeids); i++) {
		uint8_t *nid = uv_vector_at(&this->nodeids, i);
		if (nodeid == *nid) {
			found = true;
			break;
		}
	}
	if (!found) {
		uv_vector_push_back(&this->nodeids, &nodeid);
	}
}


bool cmd_ui(const char *arg) {
	bool ret = false;

	uv_rtos_add_idle_task(&ui_main);

	ret = true;

	return ret;
}

void window_closed(GtkApplication *application, GtkWindow *window, gpointer user_data) {
	uv_deinit();
	cantrace_deinit(&this->cantrace);
}


static void can_dev_changed(GtkComboBox *box) {
	strcpy(dev.can_channel, gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(box)));
	gtk_switch_set_state(GTK_SWITCH(this->can_switch), FALSE);
}


static gboolean can_switch (GtkSwitch *widget, gboolean state, gpointer user_data) {
	if (state) {
		uv_can_set_baudrate(dev.can_channel,
				gtk_spin_button_get_value(GTK_SPIN_BUTTON(this->can_baudrate)));
		char *err = uv_can_set_up();
		if (err != NULL) {
			GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(this->window), 0,
					GTK_MESSAGE_WARNING, GTK_BUTTONS_OK,
					"Connecting to CAN device returned an error: '%s'", err);
			gtk_dialog_run(GTK_DIALOG(d));
			gtk_widget_destroy(GTK_WIDGET(d));

			gtk_switch_set_state(GTK_SWITCH(widget), false);
			gtk_switch_set_active(GTK_SWITCH(widget), false);
		}
		else {
			gtk_switch_set_state(GTK_SWITCH(widget), true);
			gtk_switch_set_active(GTK_SWITCH(widget), true);
		}
	}
	else {
		uv_can_close();
		gtk_switch_set_state(GTK_SWITCH(widget), false);
		gtk_switch_set_active(GTK_SWITCH(widget), false);
	}
	gtk_widget_set_sensitive(GTK_WIDGET(this->stackswitcher), gtk_switch_get_state(GTK_SWITCH(widget)));
	gtk_widget_set_sensitive(GTK_WIDGET(this->stack), gtk_switch_get_state(GTK_SWITCH(widget)));

	return TRUE;
}

void db_file_set (GtkFileChooserButton *widget, gpointer user_data) {
	char *name = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(this->db));
	if (!cmd_db(name)) {
		GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(this->window), 0,
				GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, "Selected database file '%s' is invalid.", name);
		gtk_dialog_run(GTK_DIALOG(d));
		gtk_widget_destroy(GTK_WIDGET(d));
	}
	else {
		obj_dict_update_view(&this->obj_dict);
	}
	g_free(name);
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

			// stack switcher
			obj = gtk_builder_get_object(builder, "stackswitcher");
			this->stackswitcher = obj;
			gtk_widget_set_sensitive(GTK_WIDGET(obj), false);

			// stack
			obj = gtk_builder_get_object(builder, "stack1");
			this->stack = obj;
			gtk_widget_set_sensitive(GTK_WIDGET(obj), false);

			// CAN dev
			obj = gtk_builder_get_object(builder, "can_dev");
			this->can_dev = obj;
			uint8_t selected_index = 0;
			// add all found devices to CAN dev selection combo box
			for (uint8_t i = 0; i < uv_can_get_device_count(); i++) {
				gtk_combo_box_text_insert_text(GTK_COMBO_BOX_TEXT(obj), i,
						uv_can_get_device_name(i));
				if (strcmp(dev.can_channel, uv_can_get_device_name(i)) == 0) {
					selected_index = i;
				}
			}
			gtk_combo_box_set_active(GTK_COMBO_BOX(obj), selected_index);
			g_signal_connect(obj, "changed", G_CALLBACK(can_dev_changed), NULL);
#if CORE_WIN
			gtk_widget_set_sensitive(GTK_WIDGET(obj), false);
#endif

			// Baudrate
			obj = gtk_builder_get_object(builder, "can_baudrate");
			this->can_baudrate = obj;
			gtk_spin_button_set_range(GTK_SPIN_BUTTON(obj),0, 2000000);
			gtk_spin_button_set_value(GTK_SPIN_BUTTON(obj), dev.baudrate);
			gtk_spin_button_set_increments(GTK_SPIN_BUTTON(obj), 50000, 50000);

			// can switch
			obj = gtk_builder_get_object(builder, "can_switch");
			this->can_switch = obj;
			g_signal_connect(obj, "state-set", G_CALLBACK(can_switch), NULL);
			gtk_switch_set_state(GTK_SWITCH(this->can_switch), uv_can_is_connected());

			// database
			obj = gtk_builder_get_object(builder, "database");
			this->db = obj;
			g_signal_connect(obj, "file-set", G_CALLBACK(db_file_set), NULL);
			gtk_file_chooser_button_set_title(GTK_FILE_CHOOSER_BUTTON(obj),
					"Select database file (*.json)");
			if (db_is_loaded(&dev.db)) {
				gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(this->db), db_get_file(&dev.db));
			}

			// cantrace
			obj = gtk_builder_get_object(builder, "cantrace");
			cantrace_init(&this->cantrace, builder);

			// object dictionary
			obj = gtk_builder_get_object(builder, "obj_dict");
			obj_dict_show(&this->obj_dict, obj, (struct GtkBuilder *) builder);

			// load firmware
			load_firmware_init(&this->load_firmware, builder);

			// terminal
			terminal_init(&this->terminal, builder);


			g_object_unref(builder);
			gtk_window_set_application(GTK_WINDOW(this->window), app);
			gtk_widget_show_all(GTK_WIDGET(this->window));

			uv_mutex_init(&this->mutex);
			uv_mutex_unlock(&this->mutex);
			uv_vector_init(&this->nodeids, this->nodeid_buffer,
					sizeof(this->nodeid_buffer[0]) / sizeof(this->nodeid_buffer[0]),
					sizeof(this->nodeid_buffer[0]));
			uv_canopen_set_can_callback(&can_callb);
			g_timeout_add(20, update, NULL);
		}
		else {
			printf("error!\n");
		}
	}
}

static gboolean update(gpointer data) {
	uint16_t step_ms = 20;
	uv_mutex_lock(&this->mutex);

	add_nodeid(db_get_nodeid(&dev.db));

	terminal_step(&this->terminal, step_ms);
	cantrace_step(&this->cantrace, step_ms);
	load_firmware_step(&this->load_firmware, step_ms);
	uv_mutex_unlock(&this->mutex);

	return true;
}


static void ui_main(void *ptr) {
	GtkApplication *app;

	app = gtk_application_new("org.gtk.uvcan", G_APPLICATION_FLAGS_NONE);
	g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
	g_signal_connect (app, "window-removed", G_CALLBACK (window_closed), NULL);
	g_application_run(G_APPLICATION(app), 0, NULL);
	g_object_unref(app);

	exit(0);
}
