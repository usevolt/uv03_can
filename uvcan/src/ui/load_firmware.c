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

static void flash(GtkButton *button, gpointer user_data);
static void flashwfr(GtkButton *button, gpointer user_data);
static void bin_set (GtkFileChooserButton *widget, gpointer user_data);
static gboolean update(gpointer data);



void load_firmware_init(load_firmware_st *this, GtkBuilder *builder) {
	GObject *obj = gtk_builder_get_object(builder, "firmware");
	this->filechooser = (GtkWidget *) obj;
	g_signal_connect(obj, "file-set", G_CALLBACK(bin_set), NULL);

	obj = gtk_builder_get_object(builder, "firmware_log");
	this->firmwarelog = (GtkWidget *) obj;

	obj = gtk_builder_get_object(builder, "flash");
	g_signal_connect(obj, "clicked", G_CALLBACK(flash), NULL);

	obj = gtk_builder_get_object(builder, "flashwfr");
	g_signal_connect(obj, "clicked", G_CALLBACK(flashwfr), NULL);

	memset(this->buffer, '\0', sizeof(this->buffer));

	this->fp = NULL;
	this->update_id = -1;
}



#define this (&dev.ui.load_firmware)

static void flash(GtkButton *button, gpointer user_data) {
	if (this->update_id == -1) {
		gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(this->firmwarelog)), "", -1);
		char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(this->filechooser));
		if (this->fp == NULL && filename != NULL) {
			char cmd[256];
			sprintf(cmd, "uvcan --nodeid %u --loadbin %s", dev.nodeid, filename);
			printf("executing: %s\n", cmd);
			this->fp = popen(cmd, "r");
			if (this->fp == NULL) {
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
			char cmd[256];
			sprintf(cmd, "uvcan --nodeid %u --loadbinwfr %s", dev.nodeid, filename);
			printf("executing: %s\n", cmd);
			this->fp = popen(cmd, "r");
			if (this->fp == NULL) {
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
	if (name != NULL && !strstr(name, ".bin")) {
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
			this->fp = NULL;
			g_source_remove(this->update_id);
			this->update_id = -1;
		}
	}
	return TRUE;
}

