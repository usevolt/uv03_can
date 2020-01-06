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
#include "load.h"
#include "main.h"
#include <gtk/gtk.h>
#include <fcntl.h>
#include "ui.h"

static void flash(GtkButton *button, gpointer user_data);
static void bin_set (GtkFileChooserButton *widget, gpointer user_data);
static gboolean update(gpointer data);



void load_firmware_init(load_firmware_st *this, GtkBuilder *builder) {
	GObject *obj = gtk_builder_get_object(builder, "firmware");
	this->filechooser = (GtkWidget *) obj;
	g_signal_connect(obj, "file-set", G_CALLBACK(bin_set), NULL);

	obj = gtk_builder_get_object(builder, "firmwarenodeid");
	this->nodeid = GTK_WIDGET(obj);

	obj = gtk_builder_get_object(builder, "progressbar");
	this->progressbar = (GtkWidget *) obj;
	gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(this->progressbar), 0);

	obj = gtk_builder_get_object(builder, "flash");
	g_signal_connect(obj, "clicked", G_CALLBACK(flash), NULL);
	gtk_widget_set_sensitive(GTK_WIDGET(obj), false);
	this->flash = GTK_WIDGET(obj);

	obj = gtk_builder_get_object(builder, "wfr");
	this->wfr = GTK_WIDGET(obj);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(this->wfr), false);

	obj = gtk_builder_get_object(builder, "blocktransfer");
	this->blocktransfer = GTK_WIDGET(obj);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(this->blocktransfer), true);

	obj = gtk_builder_get_object(builder, "infolabel");
	this->infolabel = GTK_WIDGET(obj);

	obj = gtk_builder_get_object(builder, "olduv");
	this->olduvprotocol = GTK_WIDGET(obj);

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
		char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(this->filechooser));
		if (filename != NULL) {
			gtk_widget_set_sensitive(this->flash, false);

			uint8_t nodeid = strtol(gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(this->nodeid)), NULL, 0);
			loadbin(filename,
					nodeid,
					gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(this->wfr)),
					gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(this->olduvprotocol)),
					gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(this->blocktransfer)));

			this->update_id = g_timeout_add(20, update, NULL);

		}
		g_free(filename);
	}
}


void bin_set (GtkFileChooserButton *widget, gpointer user_data) {
	char *name = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(this->filechooser));
	gtk_widget_set_sensitive(this->flash, !!name);
	if (name != NULL && !strstr(name, ".bin")) {
		gtk_widget_set_sensitive(this->flash, false);
		GtkWidget *d;
		d = gtk_message_dialog_new(GTK_WINDOW(dev.ui.window), 0,
				GTK_MESSAGE_WARNING, GTK_BUTTONS_OK, "The selected file has to be a firmware binary.");
		gtk_dialog_run(GTK_DIALOG(d));
		gtk_widget_destroy(GTK_WIDGET(d));
	}
	char *n = strstr(name, "0x");
	if (n != NULL) {
		uint8_t nid = strtol(n, NULL, 0);
		ui_add_nodeid(nid);
		if (this->nodeid_count == ui_get_nodeid_count(&dev.ui)) {
			// no new nodeid added. Cycle through them and find the active one
			for (uint8_t i = 0; i < ui_get_nodeid_count(&dev.ui); i++) {
				if (nid == ui_get_nodeid(&dev.ui, i)) {
					gtk_combo_box_set_active(GTK_COMBO_BOX(this->nodeid), i);
					break;
				}
			}
		}
		else {
			// new nodeid added
			char id[64];
			sprintf(id, "0x%x", nid);
			gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(this->nodeid), id);
			gtk_combo_box_set_active(GTK_COMBO_BOX(this->nodeid), this->nodeid_count);
			this->nodeid_count++;
		}
	}
	g_free(name);
}


static gboolean update(gpointer data) {
	if (loadbin_is_finished(&dev.load)) {
		g_source_remove(this->update_id);
		this->update_id = -1;
		gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(this->progressbar), 1);
		gtk_widget_set_sensitive(this->flash, true);
		// set ui can callback again, since loadbin has set it to its own callback
		uv_canopen_set_can_callback(&uican_callb);

	}
	else {
		gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(this->progressbar), loadbin_get_progress(&dev.load) / 100.0f);
	}
	return TRUE;
}
