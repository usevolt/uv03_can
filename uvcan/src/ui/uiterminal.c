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


#include "uiterminal.h"
#include "main.h"
#include "db.h"
#include <uv_can.h>
#include <uv_terminal.h>
#include <gtk/gtk.h>


static void send(GtkButton *button, gpointer user_data);
static void clear(GtkButton *button, gpointer user_data);
static void enter(GtkEntry *entry, gpointer user_data);


#define TERMINAL_CHAR_COUNT			65536

static char rx_buffer[65535];
static uv_ring_buffer_st rx;




void terminal_init(terminal_st *this, GtkBuilder *builder) {
	GObject *obj = gtk_builder_get_object(builder, "terminal");
	this->terminal = GTK_WIDGET(obj);

	obj = gtk_builder_get_object(builder, "terminal_input");
	this->entry = GTK_WIDGET(obj);
	gtk_widget_grab_focus(GTK_WIDGET(this->entry));
	g_signal_connect(obj, "activate", G_CALLBACK(enter), NULL);

	obj = gtk_builder_get_object(builder, "terminal_send");
	g_signal_connect(obj, "clicked", G_CALLBACK(send), NULL);

	obj = gtk_builder_get_object(builder, "terminal_clear");
	g_signal_connect(obj, "clicked", G_CALLBACK(clear), NULL);

	uv_mutex_init(&this->mutex);
	uv_mutex_unlock(&this->mutex);

	uv_ring_buffer_init(&rx, rx_buffer, sizeof(rx_buffer), sizeof(rx_buffer[0]));

}

#define this (&dev.ui.terminal)


static void send(GtkButton *button, gpointer user_data) {
	const char *str = gtk_entry_get_text(GTK_ENTRY(this->entry));

	char line[256];
	strcpy(line, str);
	strcat(line, "\n");

	uint8_t len = 0;
	uv_can_msg_st msg;
	msg.type = CAN_STD;
	msg.id = UV_TERMINAL_CAN_RX_ID + db_get_nodeid(&dev.db);
	msg.data_8bit[0] = 0x22;
	msg.data_8bit[1] = UV_TERMINAL_CAN_INDEX % 256;
	msg.data_8bit[2] = UV_TERMINAL_CAN_INDEX / 256;
	msg.data_8bit[3] = UV_TERMINAL_CAN_SUBINDEX;
	for (int i = 0; i < strlen(line); i++) {
		msg.data_8bit[4 + len++] = line[i];
		if (len == 4) {
			msg.data_length = 8;
			uv_can_send(dev.can_channel, &msg);
			len = 0;
		}
	}
	if (len != 0) {
		msg.data_length = 4 + len;
		uv_can_send(dev.can_channel, &msg);
	}

	printf("sent: '%s'\n", str);
	gtk_entry_set_text(GTK_ENTRY(this->entry), "");
}


static void clear(GtkButton *button, gpointer user_data) {
	gtk_entry_set_text(GTK_ENTRY(this->entry), "");
}


static void enter(GtkEntry *entry, gpointer user_data) {
	send(NULL, NULL);
}


void terminal_can_rx(terminal_st *this_ptr, uv_can_msg_st *msg) {
	if ((msg->type == CAN_STD) &&
			(msg->id == (CANOPEN_SDO_RESPONSE_ID + db_get_nodeid(&dev.db))) &&
			(msg->data_8bit[0] == 0x42) &&
			(msg->data_length > 4) &&
			(msg->data_8bit[1] == UV_TERMINAL_CAN_INDEX % 256) &&
			(msg->data_8bit[2] == UV_TERMINAL_CAN_INDEX / 256) &&
			(msg->data_8bit[3] == UV_TERMINAL_CAN_SUBINDEX)) {
		for (int i = 4; i < msg->data_length; i++) {
			uv_mutex_lock(&this->mutex);
			uv_ring_buffer_push(&rx, &msg->data_8bit[i]);
			uv_mutex_unlock(&this->mutex);
		}
	}
}


void terminal_step(terminal_st *this_ptr, uint16_t step_ms) {
	uv_mutex_lock(&this->mutex);

	char c[2];
	c[1] = '\0';
	while (uv_ring_buffer_pop(&rx, &c[0]) == ERR_NONE) {
		GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(this->terminal));
		GtkTextIter end;
		gtk_text_buffer_get_end_iter(buffer, &end);
		gtk_text_buffer_insert(buffer, &end, c, -1);

		int len = gtk_text_buffer_get_char_count(buffer);
		if (len > TERMINAL_CHAR_COUNT) {
			GtkTextIter start;
			gtk_text_buffer_get_start_iter(buffer, &start);
			gtk_text_buffer_get_iter_at_offset(buffer, &end, len - TERMINAL_CHAR_COUNT);
			gtk_text_buffer_delete(buffer, &start, &end);
		}

		GtkWidget *scrollwindow = gtk_widget_get_parent(GTK_WIDGET(this->terminal));
		GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scrollwindow));
		gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj));
		gtk_scrolled_window_set_vadjustment(GTK_SCROLLED_WINDOW(scrollwindow), adj);
	}

	uv_mutex_unlock(&this->mutex);
}
