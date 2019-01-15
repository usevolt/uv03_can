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


#include "load_firmware.h"
#include "main.h"
#include <gtk/gtk.h>
#include <fcntl.h>
#include "ui.h"

static void flash(GtkButton *button, gpointer user_data);
static void flashwfr(GtkButton *button, gpointer user_data);
static void bin_set (GtkFileChooserButton *widget, gpointer user_data);
static gboolean update(gpointer data);



void load_firmware_init(load_firmware_st *this, GtkBuilder *builder) {
	GObject *obj = gtk_builder_get_object(builder, "firmware");
	this->filechooser = (GtkWidget *) obj;
	g_signal_connect(obj, "file-set", G_CALLBACK(bin_set), NULL);

	obj = gtk_builder_get_object(builder, "firmwarenodeid");
	this->nodeid = GTK_WIDGET(obj);

	obj = gtk_builder_get_object(builder, "firmware_log");
	this->firmwarelog = (GtkWidget *) obj;

	obj = gtk_builder_get_object(builder, "flash");
	g_signal_connect(obj, "clicked", G_CALLBACK(flash), NULL);
	gtk_widget_set_sensitive(GTK_WIDGET(obj), false);
	this->flash = GTK_WIDGET(obj);

	obj = gtk_builder_get_object(builder, "flashwfr");
	g_signal_connect(obj, "clicked", G_CALLBACK(flashwfr), NULL);
	gtk_widget_set_sensitive(GTK_WIDGET(obj), false);
	this->flashwfr = GTK_WIDGET(obj);

	memset(this->buffer, '\0', sizeof(this->buffer));

	this->fp = NULL;
	this->update_id = -1;
	this->nodeid_count = 0;
}


void load_firmware_step(load_firmware_st *this, uint16_t step_ms) {
	while (this->nodeid_count < ui_get_nodeid_count(&dev.ui)) {
		uint16_t i = ui_get_nodeid(&dev.ui, this->nodeid_count);
		char id[64];
		sprintf(id, "0x%x", i);
		gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(this->nodeid), id);
		if ((db_is_loaded(&dev.db) && i == db_get_nodeid(&dev.db)) ||
				this->nodeid_count == 0) {
			gtk_combo_box_set_active(GTK_COMBO_BOX(this->nodeid), this->nodeid_count);
		}
		this->nodeid_count++;
	}
}



#define this (&dev.ui.load_firmware)

static void flash(GtkButton *button, gpointer user_data) {
	if (this->update_id == -1) {
		gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(this->firmwarelog)), "", -1);
		char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(this->filechooser));
		if (this->fp == NULL && filename != NULL) {
			gtk_widget_set_sensitive(this->flash, false);
			gtk_widget_set_sensitive(this->flashwfr, false);
			char cmd[256];
			sprintf(cmd, "uvcan --nodeid %u --loadbin %s", db_get_nodeid(&dev.db), filename);
			printf("executing: %s\n", cmd);
			// on windows only 1 connection to the PEAK CAN-USB adapter
			// is permitted. Thus close our connection to let the flashing open it
			uv_can_close();
			this->fp = popen(cmd, "r");
			if (this->fp == NULL) {
				gtk_widget_set_sensitive(this->flash, true);
				gtk_widget_set_sensitive(this->flashwfr, true);
				uv_can_set_up();
				printf("Failed to run command %s\n", cmd);
			}
			else {
				this->update_id = g_timeout_add(20, update, NULL);
			}
		}
		g_free(filename);
	}
}


static void flashwfr(GtkButton *button, gpointer user_data) {
	if (this->update_id == -1) {
		gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(this->firmwarelog)), "", -1);
		char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(this->filechooser));
		if (this->fp == NULL && filename != NULL) {
			gtk_widget_set_sensitive(this->flash, false);
			gtk_widget_set_sensitive(this->flashwfr, false);
			char cmd[256];
			sprintf(cmd, "uvcan --nodeid %u --loadbinwfr %s", db_get_nodeid(&dev.db), filename);
			printf("executing: %s\n", cmd);

			// on windows only 1 connection to the PEAK CAN-USB adapter
			// is permitted. Thus close our connection to let the flashing open it
			uv_can_close();
			this->fp = popen(cmd, "r");
			if (this->fp == NULL) {
				gtk_widget_set_sensitive(this->flash, true);
				gtk_widget_set_sensitive(this->flashwfr, true);
				uv_can_set_up();
				printf("Failed to run command %s\n", cmd);
			}
			else {
				this->update_id = g_timeout_add(20, update, NULL);
			}
		}
		g_free(filename);
	}
}


void bin_set (GtkFileChooserButton *widget, gpointer user_data) {
	char *name = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(this->filechooser));
	gtk_widget_set_sensitive(this->flash, !!name);
	gtk_widget_set_sensitive(this->flashwfr, !!name);
	if (name != NULL && !strstr(name, ".bin")) {
		gtk_widget_set_sensitive(this->flash, false);
		gtk_widget_set_sensitive(this->flashwfr, false);
		GtkWidget *d;
		d = gtk_message_dialog_new(GTK_WINDOW(dev.ui.window), 0,
				GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, "The selected file has to be a firmware binary.");
		gtk_dialog_run(GTK_DIALOG(d));
		gtk_widget_destroy(GTK_WIDGET(d));
	}
	g_free(name);
}


static gboolean update(gpointer data) {
	if (this->fp != NULL) {
		char line[256];
		if (fgets(line, sizeof(line), this->fp) != NULL) {
			gtk_text_buffer_insert_at_cursor(
					gtk_text_view_get_buffer(GTK_TEXT_VIEW(this->firmwarelog)), line, -1);

			GtkWidget *scrollwindow = gtk_widget_get_parent(GTK_WIDGET(this->firmwarelog));
			GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scrollwindow));
			gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj));
			gtk_scrolled_window_set_vadjustment(GTK_SCROLLED_WINDOW(scrollwindow), adj);
		}
		else {
			pclose(this->fp);
			uv_can_set_up();
			gtk_widget_set_sensitive(this->flash, true);
			gtk_widget_set_sensitive(this->flashwfr, true);
			this->fp = NULL;
			g_source_remove(this->update_id);
			this->update_id = -1;
		}
	}
	return TRUE;
}
