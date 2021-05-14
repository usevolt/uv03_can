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
#include <sys/time.h>

static void adj_changed(GtkAdjustment *adjustment, gpointer user_data);

static void adj_value_changed(GtkAdjustment *adjustment, gpointer user_data);




void cantrace_msg_init(cantrace_msg_st *this, uv_can_msg_st *msg) {
	memset(this, 0, sizeof(cantrace_msg_st));
	switch (msg->type) {
	case CAN_STD:
		strcpy(this->type_str, "STD");
		break;
	case CAN_EXT:
		strcpy(this->type_str, "EXT");
		break;
	case CAN_ERR:
		strcpy(this->type_str, "ERR");
		break;
	default:
		strcpy(this->type_str, "?");
		break;
	}
	snprintf(this->id_str, sizeof(this->id_str) - 1, "0x%x", msg->id);
	struct tm *time;
	struct timeval timev = uv_can_get_rx_time();
	time = localtime(&timev.tv_sec);
	snprintf(this->time_str, sizeof(this->time_str) - 1, "%02u:%02u:%02u:%03u ",
			time->tm_hour, time->tm_min, time->tm_sec, (unsigned int) timev.tv_usec / 1000);
	snprintf(this->dlc_str, sizeof(this->dlc_str) - 1, "%u", msg->data_length);
	this->data_str[0] = '\0';
	for (int i = 0; i < msg->data_length; i++) {
		char str[64] = { 0 };
		snprintf(str, sizeof(str) - 1, "%02x ", msg->data_8bit[i]);
		int len = sizeof(this->data_str) - 1 - strnlen(this->data_str, sizeof(this->data_str));
		if (len > 0) {
			strncat(this->data_str, str, len);
		}
	}
}



void cantrace_init(cantrace_st *this, GtkBuilder *builder) {
	GObject *obj = gtk_builder_get_object(builder, "cantrace");
	this->traceview = GTK_WIDGET(obj);
	GtkViewport *viewport = (void*) gtk_widget_get_parent(GTK_WIDGET(this->traceview));
	GtkAdjustment *adj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(viewport));
	g_signal_connect(G_OBJECT(adj), "changed", G_CALLBACK(adj_changed), NULL);
	g_signal_connect(G_OBJECT(adj), "value-changed", G_CALLBACK(adj_value_changed), NULL);

	this->children_count = 0;
	uv_ring_buffer_init(&this->msgs, this->msg_buffer,
			sizeof(this->msg_buffer) / sizeof(this->msg_buffer[0]),
			sizeof(this->msg_buffer[0]));
	uv_mutex_init(&this->mutex);
	uv_mutex_unlock(&this->mutex);
}



void cantrace_step(cantrace_st *this, uint16_t step_ms) {
	// update the view
	uv_mutex_lock(&this->mutex);
	uv_can_msg_st msg;
	while (uv_ring_buffer_pop(&this->msgs, &msg) == ERR_NONE) {
		cantrace_msg_st tracemsg;
		cantrace_msg_init(&tracemsg, &msg);
		// free the oldest child
		this->children_count++;
		if (this->children_count > CANTRACE_CHILDREN_COUNT) {
			gtk_container_remove(GTK_CONTAINER(this->traceview),
					GTK_WIDGET(gtk_list_box_get_row_at_index(GTK_LIST_BOX(this->traceview), 0)));
		}
		GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
		gtk_widget_set_hexpand(GTK_WIDGET(row), true);
		gtk_box_set_homogeneous(GTK_BOX(row), true);
		GtkWidget *obj;
		obj = gtk_label_new(tracemsg.id_str);
		gtk_container_add(GTK_CONTAINER(row), GTK_WIDGET(obj));
		obj = gtk_label_new(tracemsg.dlc_str);
		gtk_container_add(GTK_CONTAINER(row), GTK_WIDGET(obj));
		obj = gtk_label_new(tracemsg.data_str);
		gtk_container_add(GTK_CONTAINER(row), GTK_WIDGET(obj));
		obj = gtk_label_new(tracemsg.type_str);
		gtk_container_add(GTK_CONTAINER(row), GTK_WIDGET(obj));
		obj = gtk_label_new(tracemsg.time_str);
		gtk_container_add(GTK_CONTAINER(row), GTK_WIDGET(obj));

		gtk_container_add(GTK_CONTAINER(this->traceview), GTK_WIDGET(row));
		gtk_widget_show_all(GTK_WIDGET(this->traceview));
	}

	uv_mutex_unlock(&this->mutex);
}

void cantrace_rx(cantrace_st *this, uv_can_msg_st *msg) {
	// add message to the buffer
	uv_mutex_lock(&this->mutex);
	uv_ring_buffer_push(&this->msgs, msg);
	uv_mutex_unlock(&this->mutex);
}


void cantrace_deinit(cantrace_st *this) {
}


#define this (&dev.ui.cantrace)


static void adj_changed(GtkAdjustment *adjustment, gpointer user_data) {
	gtk_adjustment_set_value(adjustment, gtk_adjustment_get_upper(adjustment));
	GtkViewport *viewport = (void*) gtk_widget_get_parent(GTK_WIDGET(this->traceview));
	gtk_scrollable_set_vadjustment(GTK_SCROLLABLE(viewport), adjustment);

}

static void adj_value_changed(GtkAdjustment *adjustment, gpointer user_data) {
//	printf("value: %f\n", gtk_adjustment_get_value(adjustment));
}





