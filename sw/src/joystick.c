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


#include "joystick.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <uv_rtos.h>
#include <uv_canopen.h>
#include "main.h"
#include "db.h"
#include "find.h"
#include "parser.h"
#include "archive.h"


// The bridge task's cycle
#define STEP_MS				10
// A value whose PDO has not been seen for this long no longer drives the
// controller: the axis goes back to the middle and the button is let go
#define VALUE_TIMEOUT_MS	1000
// How long joystick_stop() waits for the bridge task to see that it should end
#define STOP_TIMEOUT_MS		2000


const char *const JOYSTICK_AXIS_NAMES[JOYSTICK_AXIS_COUNT] = {
		"X", "Y", "Z", "RX", "RY", "RZ", "SLIDER1", "SLIDER2"
};


// A slot of the running controller, resolved from the configuration
typedef struct {
	bool used;
	uint8_t size;
	bool is_signed;
	uint16_t pdo_cobid;
	uint8_t pdo_offset;
	int32_t min;
	int32_t max;
	int32_t threshold;
	bool invert;
	// written by the CAN rx hook: the latest value and the tick it was got at
	// (0 while none has been)
	volatile int32_t value;
	volatile uint32_t value_tick;
	// what the controller was last given
	volatile int32_t out;
	volatile bool out_valid;
	bool sent;
} rt_slot_st;


// the configuration in use, and a count of the changes made to it, which is how
// joystick_step() notices that the driver has to be restarted
static joystick_conf_st conf = { .name = JOYSTICK_DEFAULT_NAME };
static uint32_t conf_gen;
static uint32_t running_gen;

static rt_slot_st rt[JOYSTICK_SLOT_COUNT];
// true while the controller exists; the rx hook only looks at *rt* then
static volatile bool running;
// the bridge task runs while *task_run* is set and clears *task_alive* on exit
static volatile bool task_run;
static volatile bool task_alive;

static char status[256] = "Joystick driver is not configured";


// ---- configuration --------------------------------------------------------


void joystick_conf_reset(joystick_conf_st *c) {
	memset(c, 0, sizeof(*c));
	snprintf(c->name, sizeof(c->name), "%s", JOYSTICK_DEFAULT_NAME);
}


bool joystick_conf_is_used(const joystick_conf_st *c) {
	bool ret = false;
	for (uint8_t i = 0; (i < JOYSTICK_SLOT_COUNT) && !ret; i++) {
		ret = joystick_slot_is_used(&c->slots[i]);
	}
	return ret;
}


/// @brief: The type name a value is written into the file with
static const char *type_str(uint8_t size, bool is_signed) {
	const char *ret;
	if (size == 1) {
		ret = is_signed ? "CANOPEN_SIGNED8" : "CANOPEN_UNSIGNED8";
	}
	else if (size == 2) {
		ret = is_signed ? "CANOPEN_SIGNED16" : "CANOPEN_UNSIGNED16";
	}
	else {
		ret = is_signed ? "CANOPEN_SIGNED32" : "CANOPEN_UNSIGNED32";
	}
	return ret;
}


/// @brief: Reads the size and signedness out of a type name such as
/// "CANOPEN_SIGNED16" or an array type "CANOPEN_ARRAYUNSIGNED8"
static void parse_type_str(const char *str, uint8_t *size, bool *is_signed) {
	*is_signed = (strstr(str, "SIGNED") != NULL) &&
			(strstr(str, "UNSIGNED") == NULL);
	if (strstr(str, "32") != NULL) {
		*size = 4;
	}
	else if (strstr(str, "16") != NULL) {
		*size = 2;
	}
	else {
		*size = 1;
	}
}


/// @brief: An integer field which may be written either as a number or as a
/// (hexadecimal) string
static int32_t node_int(parser_node_st node, int32_t def) {
	int32_t ret = def;
	if (parser_node_is_valid(node)) {
		parser_types_e t = parser_get_type(node);
		if (t == PARSER_INT) {
			ret = parser_get_int(node);
		}
		else if (t == PARSER_STRING) {
			char buf[32] = "";
			parser_get_string(node, buf, sizeof(buf));
			ret = (int32_t) strtol(buf, NULL, 0);
		}
		else if (t == PARSER_BOOL) {
			ret = parser_get_bool(node) ? 1 : 0;
		}
		else {
		}
	}
	return ret;
}


static void node_str(parser_node_st parent, const char *name, char *dest,
		size_t len) {
	parser_node_st n = parser_find_child(parent, name);
	if (parser_node_is_valid(n) && (parser_get_type(n) == PARSER_STRING)) {
		parser_get_string(n, dest, len);
	}
	else {
		dest[0] = '\0';
	}
}


/// @brief: Reads the binding common to an axis and a button
static void read_slot(parser_node_st obj, joystick_slot_st *slot) {
	slot->nodeid = (uint8_t) node_int(parser_find_child(obj, "NODEID"), 0);
	node_str(obj, "DEVICE", slot->device, sizeof(slot->device));
	node_str(obj, "OBJECT", slot->object, sizeof(slot->object));
	slot->mindex = (uint16_t) node_int(parser_find_child(obj, "MAININDEX"), 0);
	slot->sindex = (uint8_t) node_int(parser_find_child(obj, "SUBINDEX"), 0);
	char type[48] = "";
	node_str(obj, "TYPE", type, sizeof(type));
	parse_type_str((strlen(type) != 0) ? type : "CANOPEN_SIGNED32",
			&slot->size, &slot->is_signed);
	slot->cobid = (uint16_t) node_int(parser_find_child(obj, "COBID"), 0);
	slot->offset = (uint8_t) node_int(parser_find_child(obj, "OFFSET"), 0);
	slot->invert = (node_int(parser_find_child(obj, "INVERT"), 0) != 0);
	if ((slot->nodeid > 127) || (slot->cobid == 0) || (slot->cobid > 0x7ff) ||
			((slot->offset + slot->size) > 8)) {
		slot->nodeid = 0;
	}
}


bool joystick_conf_read(joystick_conf_st *c, const char *path) {
	bool ret = false;
	char *data = NULL;
	joystick_conf_reset(c);
	parser_node_st root = parser_read_file(path, &data);
	if (!parser_node_is_valid(root)) {
		const char *why = parser_last_error();
		printf("Failed to read the joystick configuration '%s'%s%s\n", path,
				(why != NULL) ? ": " : ".", (why != NULL) ? why : "");
	}
	else {
		ret = true;
		char name[64] = "";
		node_str(root, "NAME", name, sizeof(name));
		if (strlen(name) != 0) {
			snprintf(c->name, sizeof(c->name), "%s", name);
		}

		parser_node_st axes = parser_find_child(root, "AXES");
		if (parser_node_is_valid(axes) && (parser_get_type(axes) == PARSER_ARRAY)) {
			for (unsigned int i = 0; i < parser_array_get_size(axes); i++) {
				parser_node_st obj = parser_array_at(axes, i);
				char aname[16] = "";
				node_str(obj, "AXIS", aname, sizeof(aname));
				int a = -1;
				for (uint8_t k = 0; k < JOYSTICK_AXIS_COUNT; k++) {
					if (strcasecmp(aname, JOYSTICK_AXIS_NAMES[k]) == 0) {
						a = k;
					}
				}
				if (a < 0) {
					printf("Joystick configuration: unknown axis '%s' skipped.\n",
							aname);
				}
				else {
					joystick_slot_st *s = &c->slots[a];
					read_slot(obj, s);
					s->min = node_int(parser_find_child(obj, "MIN"), 0);
					s->max = node_int(parser_find_child(obj, "MAX"), 1000);
				}
			}
		}

		parser_node_st btns = parser_find_child(root, "BUTTONS");
		if (parser_node_is_valid(btns) && (parser_get_type(btns) == PARSER_ARRAY)) {
			for (unsigned int i = 0; i < parser_array_get_size(btns); i++) {
				parser_node_st obj = parser_array_at(btns, i);
				int32_t b = node_int(parser_find_child(obj, "BUTTON"), 0);
				if ((b < 1) || (b > JOYSTICK_BUTTON_COUNT)) {
					printf("Joystick configuration: button %d is out of the range "
							"1 ... %d, skipped.\n", (int) b, JOYSTICK_BUTTON_COUNT);
				}
				else {
					joystick_slot_st *s = &c->slots[JOYSTICK_AXIS_COUNT + b - 1];
					read_slot(obj, s);
					s->threshold = node_int(parser_find_child(obj, "THRESHOLD"), 1);
				}
			}
		}
	}
	free(data);
	return ret;
}


/// @brief: Writes the binding common to an axis and a button
static uv_errors_e write_slot(parser_writer_st *w, const joystick_slot_st *s) {
	uv_errors_e e = ERR_NONE;
	char buf[16];
	snprintf(buf, sizeof(buf), "0x%x", (unsigned int) s->nodeid);
	e |= parser_writer_add_string(w, "NODEID", buf);
	e |= parser_writer_add_string(w, "DEVICE", s->device);
	e |= parser_writer_add_string(w, "OBJECT", s->object);
	snprintf(buf, sizeof(buf), "0x%x", (unsigned int) s->mindex);
	e |= parser_writer_add_string(w, "MAININDEX", buf);
	e |= parser_writer_add_int(w, "SUBINDEX", s->sindex);
	e |= parser_writer_add_string(w, "TYPE", type_str(s->size, s->is_signed));
	snprintf(buf, sizeof(buf), "0x%x", (unsigned int) s->cobid);
	e |= parser_writer_add_string(w, "COBID", buf);
	e |= parser_writer_add_int(w, "OFFSET", s->offset);
	return e;
}


bool joystick_conf_write(const joystick_conf_st *c, const char *path) {
	static char buffer[32768];
	parser_formats_e format = parser_format_from_filename(path);
	parser_writer_st w;
	uv_errors_e e = ERR_NONE;
	e |= parser_writer_init(&w, buffer, sizeof(buffer), format);
	e |= parser_writer_add_int(&w, "VERSION", 1);
	e |= parser_writer_add_string(&w, "NAME", c->name);

	e |= parser_writer_begin_array(&w, "AXES");
	for (uint8_t i = 0; i < JOYSTICK_AXIS_COUNT; i++) {
		const joystick_slot_st *s = &c->slots[i];
		if (joystick_slot_is_used(s)) {
			e |= parser_writer_begin_object(&w, NULL);
			e |= parser_writer_add_string(&w, "AXIS", JOYSTICK_AXIS_NAMES[i]);
			e |= write_slot(&w, s);
			e |= parser_writer_add_int(&w, "MIN", s->min);
			e |= parser_writer_add_int(&w, "MAX", s->max);
			e |= parser_writer_add_bool(&w, "INVERT", s->invert);
			e |= parser_writer_end_object(&w);
		}
	}
	e |= parser_writer_end_array(&w);

	e |= parser_writer_begin_array(&w, "BUTTONS");
	for (uint8_t i = 0; i < JOYSTICK_BUTTON_COUNT; i++) {
		const joystick_slot_st *s = &c->slots[JOYSTICK_AXIS_COUNT + i];
		if (joystick_slot_is_used(s)) {
			e |= parser_writer_begin_object(&w, NULL);
			e |= parser_writer_add_int(&w, "BUTTON", i + 1);
			e |= write_slot(&w, s);
			e |= parser_writer_add_int(&w, "THRESHOLD", s->threshold);
			e |= parser_writer_add_bool(&w, "INVERT", s->invert);
			e |= parser_writer_end_object(&w);
		}
	}
	e |= parser_writer_end_array(&w);
	e |= parser_writer_end(&w);

	bool ret = false;
	if (e != ERR_NONE) {
		printf("The joystick configuration does not fit in the write buffer.\n");
	}
	else if (!parser_write_file(path, buffer, format)) {
		printf("Failed to write the joystick configuration '%s'.\n", path);
	}
	else {
		ret = true;
	}
	return ret;
}


/// @brief: The full range of a value of *size* bytes
static void type_range(uint8_t size, bool is_signed, int32_t *min, int32_t *max) {
	if (size == 1) {
		*min = is_signed ? INT8_MIN : 0;
		*max = is_signed ? INT8_MAX : UINT8_MAX;
	}
	else if (size == 2) {
		*min = is_signed ? INT16_MIN : 0;
		*max = is_signed ? INT16_MAX : UINT16_MAX;
	}
	else {
		*min = is_signed ? INT32_MIN : 0;
		*max = INT32_MAX;
	}
}


void joystick_slot_set_obj(joystick_slot_st *slot, uint8_t index,
		const device_st *device, const joystick_obj_st *obj) {
	memset(slot, 0, sizeof(*slot));
	slot->nodeid = device->nodeid;
	const char *dname = (strlen(device->devname) != 0) ?
			device->devname : device->name;
	snprintf(slot->device, sizeof(slot->device), "%.63s", dname);
	snprintf(slot->object, sizeof(slot->object), "%s", obj->name);
	slot->mindex = obj->mindex;
	slot->sindex = obj->sindex;
	slot->size = obj->size;
	slot->is_signed = obj->is_signed;
	slot->cobid = obj->pdo_cobid;
	slot->offset = obj->pdo_offset;
	slot->min = obj->min;
	slot->max = obj->max;
	// a digital value (0 / 1) or any value above zero presses the button
	slot->threshold = ((obj->min <= 0) && (obj->max >= 1)) ? 1 : obj->max;
	(void) index;
}


// ---- object catalog -------------------------------------------------------


/// @brief: Resolves a TXPDO's COB-ID as the database writes it, e.g.
/// "CANOPEN_TXPDO1_ID + UV1F2_NODEID", for the device at *nodeid*. 0 for a
/// disabled PDO or one this cannot make sense of.
static uint16_t resolve_cobid(const char *str, uint8_t nodeid) {
	uint16_t ret = 0;
	const char *p = strstr(str, "TXPDO");
	char *end = NULL;
	if (strstr(str, "DISABLED") != NULL) {
		ret = 0;
	}
	else if ((p != NULL) && (p[5] >= '1') && (p[5] <= '4')) {
		ret = (uint16_t) (CANOPEN_TXPDO1_ID + (p[5] - '1') * 0x100);
		if (strstr(str, "NODEID") != NULL) {
			ret += nodeid;
		}
	}
	else {
		unsigned long v = strtoul(str, &end, 0);
		if ((end != str) && (v != 0) && (v <= 0x7ff)) {
			ret = (uint16_t) v;
			if (strstr(str, "NODEID") != NULL) {
				ret += nodeid;
			}
		}
	}
	return ret;
}


/// @brief: A limit of the database, or *def* when it is not a plain number (it
/// names a define such as INT32_MIN)
static int32_t db_limit(dbvalue_st *v, int32_t def) {
	return (dbvalue_get_type(v) == DBVALUE_INT) ? dbvalue_get_int(v) : def;
}


static bool catalog_push(joystick_obj_st **arr, uint16_t *count,
		uint16_t *cap, const joystick_obj_st *obj) {
	bool ret = true;
	if (*count >= *cap) {
		uint16_t ncap = (*cap == 0) ? 256 : (uint16_t) (*cap * 2);
		joystick_obj_st *n = (ncap > *cap) ?
				realloc(*arr, ncap * sizeof(joystick_obj_st)) : NULL;
		if (n == NULL) {
			ret = false;
		}
		else {
			*arr = n;
			*cap = ncap;
		}
	}
	if (ret) {
		(*arr)[*count] = *obj;
		(*count)++;
	}
	return ret;
}


joystick_obj_st *joystick_catalog_build(const device_st *device, uint16_t *count) {
	joystick_obj_st *ret = NULL;
	uint16_t cap = 0;
	*count = 0;
	if (system_load_device_database(device)) {
		// every integer value of the database, from which the ones mapped to
		// the TXPDOs are picked below
		joystick_obj_st *all = NULL;
		uint16_t all_count = 0;
		uint16_t all_cap = 0;
		bool ok = true;
		for (int32_t i = 0; ok && (i < db_get_object_count(&dev.db)); i++) {
			db_obj_st *o = db_get_obj(&dev.db, i);
			canopen_object_type_e type = o->obj.type;
			uint8_t size = CANOPEN_TYPE_LEN(type);
			if (CANOPEN_IS_STRING(type) || ((size != 1) && (size != 2) &&
					(size != 4))) {
				continue;
			}
			joystick_obj_st e = { };
			uint8_t sz;
			parse_type_str(o->type_str, &sz, &e.is_signed);
			e.size = size;
			e.mindex = o->obj.main_index;
			int32_t tmin, tmax;
			type_range(size, e.is_signed, &tmin, &tmax);
			if (CANOPEN_IS_ARRAY(type)) {
				uint8_t sindex = 1;
				for (db_array_child_st *c = o->child_ptr; ok && (c != NULL);
						c = c->next_sibling) {
					snprintf(e.name, sizeof(e.name), "%s: %s",
							dbvalue_get_string(&o->name),
							dbvalue_get_string(&c->name));
					e.sindex = sindex++;
					e.min = db_limit(&c->min, tmin);
					e.max = db_limit(&c->max, tmax);
					ok = catalog_push(&all, &all_count, &all_cap, &e);
				}
			}
			else {
				snprintf(e.name, sizeof(e.name), "%s",
						dbvalue_get_string(&o->name));
				e.sindex = o->obj.sub_index;
				e.min = db_limit(&o->min, tmin);
				e.max = db_limit(&o->max, tmax);
				ok = catalog_push(&all, &all_count, &all_cap, &e);
			}
		}

		// the values the device sends in its TXPDOs, in the order it sends them
		for (uint16_t p = 0; ok && (p < db_get_txpdo_count(&dev.db)); p++) {
			db_txpdo_st *pdo = db_get_txpdo(&dev.db, p);
			uint16_t cobid = resolve_cobid(pdo->cobid, device->nodeid);
			uint8_t offset = 0;
			for (uint8_t m = 0; ok && (cobid != 0) &&
					(m < CONFIG_CANOPEN_PDO_MAPPING_COUNT); m++) {
				canopen_pdo_mapping_st *map = &pdo->mappings.mappings[m];
				if ((map->length == 0) || ((offset + map->length) > 8)) {
					break;
				}
				for (uint16_t k = 0; k < all_count; k++) {
					if ((all[k].mindex == map->main_index) &&
							(all[k].sindex == map->sub_index) &&
							(all[k].size == map->length)) {
						joystick_obj_st e = all[k];
						e.pdo_cobid = cobid;
						e.pdo_offset = offset;
						ok = catalog_push(&ret, count, &cap, &e);
						break;
					}
				}
				offset += map->length;
			}
		}
		free(all);
		db_deinit();
	}
	return ret;
}


// ---- configuration in use -------------------------------------------------


const joystick_conf_st *joystick_get_conf(void) {
	return &conf;
}


bool joystick_has_conf(void) {
	return joystick_conf_is_used(&conf);
}


void joystick_set_conf(const joystick_conf_st *c) {
	conf = *c;
	conf_gen++;
}


bool joystick_load_from_dir(const char *dir) {
	bool ret = false;
	char path[1200];
	if (parser_find_file(dir, JOYSTICK_FILE_NAME, path, sizeof(path))) {
		joystick_conf_st c;
		if (joystick_conf_read(&c, path)) {
			joystick_set_conf(&c);
			ret = true;
		}
	}
	return ret;
}


bool joystick_write_to_dir(const char *dir, char *name_out, size_t name_len) {
	bool ret = false;
	if (joystick_has_conf()) {
		char path[1200];
		// a hand-written joystick.yaml would be found ahead of the new file
		const char *other[] = { "yaml", "yml" };
		for (uint8_t i = 0; i < 2; i++) {
			snprintf(path, sizeof(path), "%s/%s.%s", dir, JOYSTICK_FILE_NAME,
					other[i]);
			remove(path);
		}
		snprintf(path, sizeof(path), "%s/%s.json", dir, JOYSTICK_FILE_NAME);
		ret = joystick_conf_write(&conf, path);
		if (ret && (name_out != NULL)) {
			snprintf(name_out, name_len, "%s.json", JOYSTICK_FILE_NAME);
		}
	}
	return ret;
}


bool joystick_save_to_system(system_st *sys) {
	bool ret = false;
	char name[64];
	// realpath() writes up to PATH_MAX bytes
	char path[PATH_MAX];
	bool used = joystick_has_conf();
	if (!system_is_sysfile_loaded(sys) || (strlen(sys->extract_dir) == 0)) {
		// nothing to save into: the next system file saved carries it
	}
#if CONFIG_TARGET_WIN
	else if (_fullpath(path, sys->filepath, sizeof(path)) == NULL) {
#else
	else if (realpath(sys->filepath, path) == NULL) {
#endif
		printf("The system file '%s' is gone; the joystick configuration was "
				"not saved into it.\n", sys->filepath);
	}
	else if (!used) {
		// everything was unbound: the package loses its joystick file
		char found[1200];
		while (parser_find_file(sys->extract_dir, JOYSTICK_FILE_NAME, found,
				sizeof(found))) {
			const char *base = strrchr(found, '/');
			base = (base != NULL) ? (base + 1) : found;
			snprintf(name, sizeof(name), "%.63s", base);
			remove(found);
			if (!archive_remove_file(path, sys->extract_dir, name)) {
				printf("Failed to remove '%s' from '%s'.\n", name, path);
				break;
			}
		}
		printf("Removed the joystick configuration from '%s'.\n", path);
		ret = true;
	}
	else if (!joystick_write_to_dir(sys->extract_dir, name, sizeof(name))) {
		// the reason is printed already
	}
	else if (!archive_put_file(path, sys->extract_dir, name)) {
		printf("Failed to add the joystick configuration to '%s'.\n", path);
	}
	else {
		printf("Saved the joystick configuration into '%s'.\n", path);
		ret = true;
	}
	fflush(stdout);
	return ret;
}


// ---- the bridge -----------------------------------------------------------


static int32_t sign_extend(uint32_t raw, uint8_t size, bool is_signed) {
	int32_t ret;
	if (!is_signed) {
		ret = (int32_t) raw;
	}
	else if (size == 1) {
		ret = (int8_t) raw;
	}
	else if (size == 2) {
		ret = (int16_t) raw;
	}
	else {
		ret = (int32_t) raw;
	}
	return ret;
}


void joystick_can_rx(const uv_can_msg_st *msg) {
	if (running && (msg->type == CAN_STD)) {
		uint32_t now = uv_rtos_get_tick_count();
		for (uint8_t i = 0; i < JOYSTICK_SLOT_COUNT; i++) {
			rt_slot_st *s = &rt[i];
			if (s->used && (s->pdo_cobid == msg->id) &&
					((s->pdo_offset + s->size) <= msg->data_length)) {
				uint32_t raw = 0;
				for (uint8_t b = 0; b < s->size; b++) {
					raw |= ((uint32_t) msg->data_8bit[s->pdo_offset + b]) << (8 * b);
				}
				s->value = sign_extend(raw, s->size, s->is_signed);
				s->value_tick = (now != 0) ? now : 1;
			}
		}
	}
}


/// @brief: What slot *i* gives the controller now; false when it has no value
static bool slot_output(uint8_t i, uint32_t now, int32_t *out) {
	rt_slot_st *s = &rt[i];
	bool ret = s->used && (s->value_tick != 0) &&
			((now - s->value_tick) < VALUE_TIMEOUT_MS);
	int32_t v = s->value;
	if (!ret) {
		*out = 0;
	}
	else if (joystick_slot_is_axis(i)) {
		int64_t span = (int64_t) s->max - s->min;
		int64_t o = 0;
		if (span != 0) {
			o = (((int64_t) v - s->min) * 2 * JOYSTICK_AXIS_MAX) / span -
					JOYSTICK_AXIS_MAX;
		}
		if (o > JOYSTICK_AXIS_MAX) {
			o = JOYSTICK_AXIS_MAX;
		}
		else if (o < -JOYSTICK_AXIS_MAX) {
			o = -JOYSTICK_AXIS_MAX;
		}
		else {
		}
		*out = (int32_t) (s->invert ? -o : o);
	}
	else {
		bool pressed = (v >= s->threshold);
		*out = (pressed != s->invert) ? 1 : 0;
	}
	return ret;
}


static void bridge_task(void *ptr) {
	(void) ptr;
	while (task_run) {
		uint32_t now = uv_rtos_get_tick_count();
		bool changed = false;
		for (uint8_t i = 0; i < JOYSTICK_SLOT_COUNT; i++) {
			rt_slot_st *s = &rt[i];
			int32_t out = 0;
			s->out_valid = slot_output(i, now, &out);
			if (s->used && (!s->sent || (out != s->out))) {
				s->out = out;
				s->sent = true;
				changed = true;
				if (joystick_slot_is_axis(i)) {
					jsdriver_set_axis(i, out);
				}
				else {
					jsdriver_set_button(i - JOYSTICK_AXIS_COUNT, out != 0);
				}
			}
		}
		if (changed) {
			jsdriver_sync();
		}
		uv_rtos_task_delay(STEP_MS);
	}
	task_alive = false;
	uv_rtos_task_delete(NULL);
}


/// @brief: Fills the running slots from the configuration in use
static void resolve(void) {
	memset(rt, 0, sizeof(rt));
	for (uint8_t i = 0; i < JOYSTICK_SLOT_COUNT; i++) {
		const joystick_slot_st *c = &conf.slots[i];
		rt_slot_st *s = &rt[i];
		if (joystick_slot_is_used(c)) {
			s->size = c->size;
			s->is_signed = c->is_signed;
			s->pdo_cobid = c->cobid;
			s->pdo_offset = c->offset;
			s->min = c->min;
			s->max = c->max;
			s->threshold = c->threshold;
			s->invert = c->invert;
			s->used = true;
		}
	}
}


void joystick_stop(void) {
	if (task_alive) {
		task_run = false;
		for (uint32_t t = 0; task_alive && (t < STOP_TIMEOUT_MS); t += STEP_MS) {
			uv_rtos_task_delay(STEP_MS);
		}
	}
	if (running) {
		running = false;
		jsdriver_close();
		snprintf(status, sizeof(status), "Joystick driver stopped");
		printf("%s.\n", status);
		fflush(stdout);
	}
}


/// @brief: Creates the controller and the bridge task for the configuration
/// in use
static void start(void) {
	resolve();
	char err[160] = "";
	if (!jsdriver_open(conf.name, err, sizeof(err))) {
		snprintf(status, sizeof(status), "Joystick driver could not start: %s",
				err);
		printf("%s\n", status);
	}
	else {
		uint8_t axes = 0;
		uint8_t buttons = 0;
		for (uint8_t i = 0; i < JOYSTICK_SLOT_COUNT; i++) {
			if (rt[i].used) {
				if (joystick_slot_is_axis(i)) {
					axes++;
				}
				else {
					buttons++;
				}
			}
		}
		snprintf(status, sizeof(status),
				"Joystick driver running (%s): %u axes, %u buttons",
				jsdriver_name(), (unsigned int) axes, (unsigned int) buttons);
		printf("%s.\n", status);
		running = true;
		task_run = true;
		task_alive = true;
		uv_rtos_task_create(&bridge_task, "joystick", UV_RTOS_MIN_STACK_SIZE * 4,
				NULL, UV_RTOS_IDLE_PRIORITY + 1, NULL);
	}
	fflush(stdout);
}


void joystick_step(void) {
	if (running_gen != conf_gen) {
		running_gen = conf_gen;
		joystick_stop();
		if (joystick_has_conf()) {
			start();
		}
		else {
			snprintf(status, sizeof(status),
					"Joystick driver is not configured");
		}
	}
}


bool joystick_is_running(void) {
	return running;
}


const char *joystick_get_status(void) {
	return status;
}


bool joystick_get_live(uint8_t index, int32_t *raw, int32_t *out) {
	bool ret = false;
	if (running && (index < JOYSTICK_SLOT_COUNT) && rt[index].used &&
			rt[index].out_valid) {
		*raw = rt[index].value;
		*out = rt[index].out;
		ret = true;
	}
	return ret;
}


// ---- --joystick -------------------------------------------------------------


static char cmd_file[1024];


static void joystick_cmd_task(void *ptr) {
	(void) ptr;
	if (strlen(cmd_file) != 0) {
		joystick_conf_st c;
		if (joystick_conf_read(&c, cmd_file)) {
			joystick_set_conf(&c);
		}
	}
	if (!joystick_has_conf()) {
		printf("No joystick configuration: give a system file with one (--sys) "
				"or a joystick configuration file.\n");
	}
	else {
		find_start_monitor();
		joystick_step();
		if (joystick_is_running()) {
			printf("Press Ctrl-C to stop.\n");
			fflush(stdout);
			while (true) {
				uv_rtos_task_delay(100);
			}
		}
	}
	fflush(stdout);
}


bool cmd_joystick(const char *arg) {
	snprintf(cmd_file, sizeof(cmd_file), "%s", (arg != NULL) ? arg : "");
	add_task(&joystick_cmd_task);
	return true;
}
