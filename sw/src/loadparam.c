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



#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <uv_json.h>
#include "loadparam.h"
#include "main.h"
#include "db.h"
#include "uvstdin.h"
#include "uvdev.h"
#include "find.h"
#include "ui/uvui.h"

#define this (&dev.loadparam)


void loadparam_step(void *dev);



// <windows.h> (via the FreeRTOS Win32 port) defines ERROR as 0; undef it so
// this colored-print macro can be defined without a redefinition warning.
#undef ERROR
#define ERROR(str, ...) printf(PRINT_BOLDRED str PRINT_RESET, __VA_ARGS__)
#define ERRORSTR(str) printf(PRINT_BOLDRED str PRINT_RESET)
#define WARNING(str, ...) printf(PRINT_BOLDYELLOW str PRINT_RESET, __VA_ARGS__)
#define WARNINGSTR(str) printf(PRINT_BOLDYELLOW str PRINT_RESET)

bool loadparam_load(const char *path) {
	bool ret = false;

	this->current_file = 0;
	memset(this->files, 0, sizeof(this->files));

	uv_vector_init(&this->queries, this->queries_buffer,
			QUERY_COUNT, sizeof(this->queries_buffer[0]));

	this->dev_count = 0;
	this->sys_load_mode = false;

	if (!path) {
		ERRORSTR("Give parameter file as a file path to binary file.\n");
	}
	else {
		strcpy(this->files[0], path);
		add_task(loadparam_step);
		uv_can_set_up(false);
		ret = true;
	}

	return ret;
}


bool cmd_loadparam(const char *arg) {
	return loadparam_load(arg);
}


/// @brief: Loads the object dictionary of *device*'s .uvdev package into dev.db
/// and forces *device*'s actual node id as the target, so the load writes to the
/// right node regardless of the NODEID stored in the parameter file. Returns true
/// on success; the caller must db_deinit() afterwards.
static bool load_device_db(device_st *device) {
	bool ret = false;
	uvdev_st pkg;
	if (uvdev_open(&pkg, device->filepath)) {
		char cwd[1024] = {};
		if ((strlen(pkg.database) != 0) &&
				(getcwd(cwd, sizeof(cwd)) != NULL) &&
				(chdir(pkg.dir) == 0)) {
			ret = cmd_db(pkg.database);
			if (chdir(cwd)) {
				// best effort; the original cwd should always be restorable
			}
			if (ret && (device->nodeid != 0)) {
				db_set_nodeid_force(&dev.db, device->nodeid);
			}
		}
		uvdev_close(&pkg);
	}
	return ret;
}


// Arguments for the asynchronous single-device load task.
static device_st *async_load_device;
static char async_load_file[1024];

/// @brief: Task body running loadparam_load_device() off the UI thread.
static void loadparam_device_task(void *ptr) {
	loadparam_load_device(async_load_device, async_load_file);
	this->finished = true;
	uv_rtos_task_delete(NULL);
}

void loadparam_load_device_async(device_st *device, const char *file) {
	async_load_device = device;
	strncpy(async_load_file, (file != NULL) ? file : "",
			sizeof(async_load_file) - 1);
	async_load_file[sizeof(async_load_file) - 1] = '\0';
	// mark in-progress before the task starts so a caller can poll immediately
	this->finished = false;
	uv_rtos_task_create(&loadparam_device_task, "loadparam_task",
			UV_RTOS_MIN_STACK_SIZE * 5, NULL, UV_RTOS_IDLE_PRIORITY + 1, NULL);
}


// Target list for the asynchronous multi-device parameter load (each device's
// own param_file is used as the source).
static device_st *async_params_devs[SYSTEM_DEV_MAX_COUNT];
static uint8_t async_params_count;
static volatile bool async_params_finished = true;


bool loadparam_load_params_is_finished(void) {
	return async_params_finished;
}


/// @brief: Task body loading saved parameters onto each target device in turn.
static void loadparam_params_task(void *ptr) {
	for (uint8_t i = 0; i < async_params_count; i++) {
		device_st *d = async_params_devs[i];
		if ((d != NULL) && (strlen(d->param_file) != 0)) {
			printf("Loading parameters to node 0x%x from '%s'\n",
					(unsigned int) d->nodeid, d->param_file);
			fflush(stdout);
			if (!loadparam_load_device(d, d->param_file)) {
				// a CANopen transfer to this device failed: stop the whole load
				ERROR("Parameter loading stopped: writing to node 0x%x failed.\n",
						(unsigned int) d->nodeid);
				fflush(stdout);
				break;
			}
		}
	}
	async_params_finished = true;
	uv_rtos_task_delete(NULL);
}


void loadparam_load_params_async(device_st **devices, uint8_t count) {
	if (count > SYSTEM_DEV_MAX_COUNT) {
		count = SYSTEM_DEV_MAX_COUNT;
	}
	for (uint8_t i = 0; i < count; i++) {
		async_params_devs[i] = devices[i];
	}
	async_params_count = count;
	// mark in-progress before the task starts so a caller can poll immediately
	async_params_finished = false;
	uv_rtos_task_create(&loadparam_params_task, "loadparams_task",
			UV_RTOS_MIN_STACK_SIZE * 5, NULL, UV_RTOS_IDLE_PRIORITY + 1, NULL);
}


// Implementation behind loadparam_load_device(). *sys_mode* selects whether the
// per-device EMCY suppress / store / reset steps run here (false, the default) or
// are left to the caller (true, the system load).
static bool loadparam_load_device_ex(device_st *device, const char *file,
		bool sys_mode) {
	bool ret = false;
	if ((device == NULL) || (file == NULL) || (strlen(file) == 0)) {
		ERRORSTR("ERROR: loadparam_load_device: no device or file given.\n");
	}
	else if (strlen(device->filepath) == 0) {
		ERRORSTR("ERROR: the device has no configuration package; assign a .uvdev\n"
				"file before loading parameters.\n");
	}
	else {
		// prepare the loadparam state for a single file
		this->current_file = 0;
		this->dev_count = 0;
		this->sys_load_mode = sys_mode;
		memset(this->files, 0, sizeof(this->files));
		uv_vector_init(&this->queries, this->queries_buffer,
				QUERY_COUNT, sizeof(this->queries_buffer[0]));
		strcpy(this->files[0], file);

		bool db_loaded = load_device_db(device);
		if (!db_loaded) {
			ERROR("Failed to load the database of '%s'.\n", device->filepath);
		}
		else {
			uv_can_set_up(false);
			loadparam_step(NULL);
			db_deinit();
			ret = this->success;
		}
		this->sys_load_mode = false;
	}
	return ret;
}


bool loadparam_load_device(device_st *device, const char *file) {
	return loadparam_load_device_ex(device, file, false);
}


/// @brief: Resolve the readable answer-keyed query form: return the child of
/// *qref* whose key matches the query's chosen answer text, or NULL (with a
/// warning) if no such key exists.
///
/// The returned pointer is a named member, so it is usable with all the generic
/// uv_jsonreader accessors (get_type/get_int/get_string/find_child) and with
/// array_get_size/array_at, unlike a bare array element from array_at.
static char *query_keyed_select(char *qref, query_st *q) {
	char *answer = q->answers[q->correct_answer];
	char *sel = uv_jsonreader_find_child(qref, answer);
	if (sel == NULL) {
		WARNING("Query '%s': chosen answer \"%s\" is not a key in its value "
				"object. Skipping this value.\n", q->name, answer);
	}
	return sel;
}


/// @brief: Gets and returns the query value. Integer values are returned,
/// string values are copied to *dest_str*.
///
/// @param dest_str: If *type* is JSON_STRING, the resulting string is copied to this
/// @param array_obj: If *type* is JSON_ARRAY, the resulting array object is copied to this
static int query_get(char *json_obj, char *dest_str, int dest_len, char **array_obj) {
	int ret = 0;
	switch (uv_jsonreader_get_type(json_obj)) {
	case JSON_OBJECT:
	{
		char name[32];
		uv_jsonreader_get_obj_name(json_obj, name, sizeof(name));
		// query object
		// For object types, search all object's children and if any child's
		// name matches with query name, fetch that value
		for (uint8_t i = 0; i < uv_vector_size(&this->queries); i++) {
			query_st *q = uv_vector_at(&this->queries, i);
			char *qref = uv_jsonreader_find_child(json_obj, q->name);
			if (qref != NULL) {
				printf("Query %s answered: (%u) %s\n",
						q->name,
						q->correct_answer + 1,
						q->answers[q->correct_answer]);
				if (uv_jsonreader_get_type(qref) == JSON_OBJECT) {
					// readable answer-keyed form: { "<answer>": <value>, ... }.
					// Resolve the chosen answer's value and parse it recursively;
					// the generic query_get handles int/string/array/nested query.
					char *sel = query_keyed_select(qref, q);
					if (sel != NULL) {
						ret = query_get(sel, dest_str, dest_len, array_obj);
					}
				}
				else {
					// legacy positional-array form: indexed by the answer number
					switch(uv_jsonreader_array_get_type(qref, q->correct_answer)) {
					case JSON_OBJECT:
					{
						char *newquery = uv_jsonreader_array_at(
								qref, q->correct_answer);
						// found new query inside query, parse it recursively
						ret = query_get(newquery, dest_str, dest_len, array_obj);
						break;
					}
					case JSON_INT:
						ret = uv_jsonreader_array_get_int(qref, q->correct_answer);
						break;
					case JSON_STRING:
						if (uv_jsonreader_array_get_size(qref) > q->correct_answer) {
							uv_jsonreader_array_get_string(qref,
									q->correct_answer,
									dest_str,
									dest_len);
						}
						break;
					case JSON_ARRAY:
						*array_obj = uv_jsonreader_array_at(qref, q->correct_answer);
						break;
					default:
						break;
					}
				}
				break;
			}
		}
		break;
	}
	case JSON_ARRAY:
		*array_obj = json_obj;
		break;
	case JSON_INT:
		ret = uv_jsonreader_get_int(json_obj);
		break;
	case JSON_STRING:
		if (dest_str && dest_len) {
			uv_jsonreader_get_string(json_obj, dest_str, dest_len);
		}
		break;
	default:
		break;
	}
	return ret;
}


static uv_errors_e load_param(char *json_obj,
							  uint16_t mindex,
							  canopen_object_type_e objtype,
							  const char *parent_info) {
	uv_errors_e ret = ERR_NONE;
	uint8_t sindex = 0;
	uint8_t sindex_offset = 0;

	char *val = uv_jsonreader_find_child(json_obj, "INFO");
	char info[128] = {};
	if (parent_info != NULL) {
		snprintf(info, sizeof(info), "%s: ", parent_info);
	}
	if (val != NULL) {
		size_t len = strlen(info);
		uv_jsonreader_get_string(val, info + len, sizeof(info) - len);
	}


	val = uv_jsonreader_find_child(json_obj, "MAININDEX");
	if (val) {
		mindex = query_get(val, NULL, 0, NULL);
	}


	val = uv_jsonreader_find_child(json_obj, "TYPE");
	if (val) {
		char typestr[64];
		query_get(val, typestr, sizeof(typestr), NULL);
		objtype = db_str_to_type(typestr);
	}

	val = uv_jsonreader_find_child(json_obj, "SUBINDEX");
	if (val != NULL) {
		sindex = query_get(val, NULL, 0, NULL);
	}
	// SUBINDEX_OFFSET is used for array objects to not start writing the data
	// to the first element in array.
	val = uv_jsonreader_find_child(json_obj, "SUBINDEX_OFFSET");
	if (val != NULL) {
		sindex_offset = query_get(val, NULL, 0, NULL);
	}

	char *data = uv_jsonreader_find_child(json_obj, "DATA");

	char *query_array = NULL;
	query_st *query = NULL;
	bool skip = false;
	if (data == NULL) {
		// DATA object not found. Check if any queries are defined
		for (uint16_t i = 0; i < uv_vector_size(&this->queries); i++) {
			query = uv_vector_at(&this->queries, i);
			char *obj = uv_jsonreader_find_child(json_obj, query->name);
			if (obj != NULL) {
				// matching query found
				query_array = obj;
				break;
			}
		}
		if (query_array == NULL) {
			char name[64] = {};
			uv_jsonreader_get_obj_name(json_obj, name, sizeof(name));
			ERROR("no \"DATA\" or queries found in object '%s'\n",
				  name);
			ret = ERR_ABORTED;
		}
	}

	// If the query value uses the readable answer-keyed object form, resolve the
	// chosen answer's value into *data* so the regular DATA code paths below
	// handle it uniformly (the resolved pointer is a named member). A missing
	// answer key warns and skips writing this parameter.
	if (query_array != NULL &&
			uv_jsonreader_get_type(query_array) == JSON_OBJECT) {
		data = query_keyed_select(query_array, query);
		query_array = NULL;
		if (data == NULL) {
			skip = true;
		}
	}

	// at this point either *data* or *query_array* should contain
	// object to load

	uv_json_types_e type = JSON_UNSUPPORTED;
	if (data) {
		type = uv_jsonreader_get_type(data);
	}
	else if (!skip) {
		type = uv_jsonreader_array_get_type(
				query_array, query->correct_answer);
		if (type == JSON_OBJECT) {
			// set data to point to selected object. *load_param* is
			// then called for this object recursively.
			data = uv_jsonreader_array_at(query_array, query->correct_answer);
		}
		else {

		}
	}
	else {
		// answer key missing from keyed query value: nothing to load
	}


	if (ret == ERR_NONE && !skip) {
		if (type == JSON_ARRAY) {
			char *array = NULL;
			if (data != NULL) {
				query_get(data, NULL, 0, &array);
			}
			else if (query_array != NULL) {
				array = uv_jsonreader_array_at(query_array, query->correct_answer);
			}
			else {

			}
			// *array* holds pointer to JSON array object
			for (uint32_t i = 0; i < uv_jsonreader_array_get_size(array); i++) {
				switch (uv_jsonreader_array_get_type(array, i)) {
					case JSON_INT:
						uint32_t d = uv_jsonreader_array_get_int(array, i);
						LOG("Writing '%s' (0x%x) [%u] = 0x%x",
								info, mindex, i + 1 + sindex_offset, d);
						ret |= uv_canopen_sdo_write(db_get_nodeid(&dev.db),
													mindex,
													i + 1 + sindex_offset,
													CANOPEN_SIZEOF(objtype), &d);
						break;
						case JSON_OBJECT: {
							char *obj = uv_jsonreader_array_at(array, i);
							// child objects are loaded recursively
							ret |= load_param(
									obj,
									mindex,
									objtype,
									info);
						break;
						}
					default:
						LOG_END();
						ERROR("array of object type '%s' not supported\n",
								uv_json_type_to_str(
										uv_jsonreader_array_get_type(array, i)));
						fflush(stdout);
						break;
				}
				LOG_END();
				if (ret != ERR_NONE) {
					ERROR("Array loading failed for subindex %u\n",
						  i + 1 + sindex_offset);
					LOG_SDO_ERROR();
					// stop the whole array on the first failed transfer
					break;
				}
			}
		}
		else if (type == JSON_OBJECT) {
			// DATA was OBJECT type, which means it need to be parsed
			// recursively
			ret |= load_param(data, mindex, objtype, info);
		}
		else if (type == JSON_STRING) {
			char str[1024] = {};
			if (data != NULL) {
				query_get(data, str, sizeof(str), NULL);
			}
			else if (query_array != NULL) {
				char s[128] = {};
				uv_jsonreader_array_get_string(query_array,
						query->correct_answer, s, sizeof(s));
				strcpy(str, s);
			}
			else {

			}
			LOG("Writing '%s' (0x%x) = \"%s\"",
					info, mindex, str);
			ret |= uv_canopen_sdo_write(db_get_nodeid(&dev.db),
					mindex, sindex + sindex_offset, strlen(str) + 1, str);
			if (ret != ERR_NONE) {
				LOG_END();
				ERROR("Loading string '%s' failed.\n", str);
				LOG_SDO_ERROR();
			}
		}
		else {
			// data is integer data
			uint32_t d;
			if (data != NULL) {
				d = query_get(data, NULL, 0, NULL);
			}
			else if (query_array != NULL) {
				d = uv_jsonreader_array_get_int(query_array, query->correct_answer);
			}
			else {

			}
			LOG("Writing '%s' (0x%x) = 0x%x",
					info, mindex, d);
			ret |= uv_canopen_sdo_write(db_get_nodeid(&dev.db),
					mindex, sindex + sindex_offset, CANOPEN_SIZEOF(objtype), &d);
			if (ret != ERR_NONE) {
				LOG_END();
				ERROR("Parameter loading failed for subindex %u\n", sindex);
				LOG_SDO_ERROR();
			}
		}
	}
	else if (ret != ERR_NONE) {
		LOG_END();
		ERRORSTR("Parameter in a wrong format\n");
		if (strlen(info) != 0) {
			ERROR("Parameter info: '%s'\n\n", info);
		}
		else {
			ERRORSTR("'INFO' value not found from the parameter.\n\n");
		}
		fflush(stderr);
	}
	else {
		// skipped (e.g. missing answer key); already warned, nothing to do
	}


	return ret;
}



static uv_errors_e parse_dev(char *json) {
	uv_errors_e ret = ERR_NONE;
	char *obj = uv_jsonreader_find_child(json, "NODEID");
	char *query_array = NULL;
	query_st *query = NULL;
	if (obj != NULL) {
		uint8_t nodeid = 0;
		nodeid = query_get(obj, NULL, 0, NULL);
		printf("The NODEID set to 0x%x from the param file\n", nodeid);
		db_set_nodeid(&dev.db, nodeid);
		if (db_get_nodeid(&dev.db) != nodeid) {
			// setting nodeid failed, means that it was force set manually.
			// write new nodeid to device
			printf("Writing new NODEID 0x%x to device 0x%x\n",
					nodeid,
					db_get_nodeid(&dev.db));
			uv_canopen_sdo_write(db_get_nodeid(&dev.db),
					CONFIG_CANOPEN_NODEID_INDEX,
					0,
					1,
					&nodeid);
		}
	}
	else {
		// NODEID not found, try to find queries
		// DATA object not found. Check if any queries are defined
		for (uint16_t i = 0; i < uv_vector_size(&this->queries); i++) {
			query = uv_vector_at(&this->queries, i);
			char *obj = uv_jsonreader_find_child(json, query->name);
			if (obj != NULL) {
				// matching query found
				query_array = obj;
				break;
			}
		}
		if (query_array == NULL) {
			ERRORSTR("no \"DATA\" or queries found in device\n");
			ret = ERR_ABORTED;
		}

		if (db_get_nodeid(&dev.db) == 0) {
			ERRORSTR("NODEID not set. Set it either from the param file "
					"or with --nodeid or --db commands.\n");
			ret = ERR_CANOPEN_NODE_ID_ENTRY_INVALID;
		}
	}

	// parse recursively queried object
	if (query_array != NULL) {
		if (uv_jsonreader_get_type(query_array) == JSON_OBJECT) {
			// readable answer-keyed form: select the device sub-object by the
			// chosen answer's text (query_keyed_select warns if the key is missing)
			json = query_keyed_select(query_array, query);
		}
		else {
			// legacy positional-array form
			json = uv_jsonreader_array_at(query_array, query->correct_answer);
		}
		if (json != NULL) {
			ret = parse_dev(json);
		}
		else if (uv_jsonreader_get_type(query_array) != JSON_OBJECT) {
			WARNING("Query '%s' didn't contain selected answer index %i,\n"
					"Skipping this device.\n",
					query->name,
					query->correct_answer);
		}
		else {
			// keyed form already warned via query_keyed_select
		}
	}
	else {
		obj = uv_jsonreader_find_child(json, "CAN IF VERSION");
		if (obj != NULL) {
			uint16_t can_if = query_get(obj, NULL, 0, NULL);
			uint16_t dev_if = 0;
			char *mindex = uv_jsonreader_find_child(json, "CAN IF MINDEX");
			char *sindex = uv_jsonreader_find_child(json, "CAN IF SINDEX");
			if (mindex != NULL &&
					sindex != NULL) {
				LOG("Reading CAN IF from 0x%x\n", db_get_nodeid(&dev.db));
				if (uv_canopen_sdo_read(db_get_nodeid(&dev.db), uv_jsonreader_get_int(mindex),
						uv_jsonreader_get_int(sindex), CANOPEN_SIZEOF(CANOPEN_UNSIGNED16),
						&dev_if) == ERR_NONE) {
					ret = db_check_can_if_version(&dev.db,
							can_if, dev_if,
							"parameter file", "device");
				}
				else {
					LOG_SDO_ERROR();
					PROMPTSTR(
						   "Failed to read CAN interface from the device.\n"
							"The CAN IF VERSION object dictionary entry might not be defined.\n"
							"Press anything to continue or 'skip' to skip this device.\n\n");
					char str[128] = {};
					uv_stdin_getline(str, sizeof(str) - 1);
					if (strstr(str, "skip")) {
						printf("User selected: skip device\n");
						ret = ERR_SKIPPED;
					}
					else {
						printf("User selected: continue (CAN IF read failed)\n");
					}
				}
			}
			else {
				PROMPTSTR(
					   "\"CAN IF MINDEX\" or \"CAN IF SINDEX\" not found in the parameter file.\n\n"
					   "Press anything to continue or type 'skip' to skip this device.\n\n");
				char str[128] = {};
				uv_stdin_getline(str, sizeof(str) - 1);
				if (strstr(str, "skip")) {
					printf("User selected: skip device\n");
					ret = ERR_SKIPPED;
				}
				else {
					printf("User selected: continue (CAN IF MINDEX/SINDEX missing)\n");
				}
			}

		}
		else {
			PROMPT(
				   "Parameter file didn't contain CAN interface version number for device 0x%x.\n"
					"Undefined behaviour might occur while loading the parameters.\n\n"
					"Press anything to continue or type 'skip' to skip this device.\n\n",
					db_get_nodeid(&dev.db));
			char str[128] = {};
			uv_stdin_getline(str, sizeof(str) - 1);
			if (strstr(str, "skip")) {
				printf("User selected: skip device 0x%x\n", db_get_nodeid(&dev.db));
				ret = ERR_SKIPPED;
			}
			else {
				printf("User selected: continue (no CAN IF version for device 0x%x)\n",
						db_get_nodeid(&dev.db));
			}
		}

		if (ret == ERR_NONE) {
			// add this dev to modified dev list
			bool match = false;
			for (uint8_t i = 0; i < this->dev_count; i++) {
				if (this->modified_dev_nodeids[i] == db_get_nodeid(&dev.db)) {
					match = true;
					break;
				}
			}
			if (!match) {
				this->modified_dev_nodeids[this->dev_count++] = db_get_nodeid(&dev.db);
			}

			obj = uv_jsonreader_find_child(json, "PARAMS");
			if (obj != NULL && uv_jsonreader_get_type(obj) == JSON_ARRAY) {
				char *arr = obj;
				unsigned int count = uv_jsonreader_array_get_size(arr);
				if (count != 0) {
					printf("Found %u params. Downloading them to the target device 0x%x.\n",
							uv_jsonreader_array_get_size(arr),
							db_get_nodeid(&dev.db));
					fflush(stdout);
					obj = uv_jsonreader_array_at(arr, 0);
					for (int32_t i = 0; i < uv_jsonreader_array_get_size(arr); i++) {
						if (uv_jsonreader_get_type(obj) == JSON_OBJECT) {
							ret |= load_param(obj, 0, CANOPEN_UNDEFINED, NULL);
						}
						else {
							LOG_END();
							ERROR("PARAMS array contained something else\n"
									"than objects at index %i\n", i + 1);
							ret |= ERR_ABORTED;
						}

						if (ret != ERR_NONE) {
							// a transfer failed: stop downloading further params
							break;
						}

						uv_jsonreader_get_next_sibling(obj, &obj);
					}
					LOG_END();
				}
				else {
					printf("PARAMS array empty, moving to operator specific parameters.\n");
				}
			}
			else {
				ERRORSTR("Couldn't find array type object 'PARAMS' from the json file.\n");
				fflush(stdout);
				ret = ERR_ABORTED;
			}

			obj = uv_jsonreader_find_child(json, "OPERATORS");
			char *opdb_mindex_json = uv_jsonreader_find_child(json, "OPDB_MAININDEX");
			char *current_op_json = uv_jsonreader_find_child(json, "CURRENT_OP");
			char *opdb_type_json = uv_jsonreader_find_child(json, "OPDB_TYPE");

			if (obj != NULL && uv_jsonreader_get_type(obj) == JSON_ARRAY &&
					opdb_mindex_json != NULL && uv_jsonreader_get_type(opdb_mindex_json) == JSON_INT &&
					current_op_json != NULL && uv_jsonreader_get_type(current_op_json) == JSON_INT &&
					opdb_type_json != NULL && uv_jsonreader_get_type(opdb_type_json) == JSON_STRING) {

				char *operators = obj;
				uint32_t opdb_mindex = uv_jsonreader_get_int(opdb_mindex_json);
				canopen_object_type_e opdb_type = db_jsonval_to_type(opdb_type_json);

				// check how many ops dev has. Skipped (leaving ret set) when the
				// PARAMS above already failed, so no further bus traffic is sent.
				uint16_t devopcount = 1;
				if (ret == ERR_NONE) {
					ret |= uv_canopen_sdo_read(db_get_nodeid(&dev.db),
							opdb_mindex + 1, 0, sizeof(devopcount), &devopcount);
				}

				// copy as many times as necessary to have all the operators
				uint8_t op_count = uv_jsonreader_array_get_size(operators);
				for (uint32_t i = devopcount; (i < op_count) && (ret == ERR_NONE); i++) {
					printf("Creating new operator by copying operator %u\n", 1);
					fflush(stdout);
					uint32_t data = 1;
					ret |= uv_canopen_sdo_write(db_get_nodeid(&dev.db),
							opdb_mindex, 3, CANOPEN_SIZEOF(opdb_type), &data);
					// wait for the device to copy the operators
					uv_rtos_task_delay(300);
				}

				// cycle through all the operators
				for (uint32_t i = 0; (i < op_count) && (ret == ERR_NONE); i++) {
					uint32_t data = i + 1;
					LOG("Changing active operator to op %u...", data);
					ret |= uv_canopen_sdo_write(db_get_nodeid(&dev.db), opdb_mindex,
							1, CANOPEN_SIZEOF(opdb_type), &data);
					// wait for the device to switch operator before loading params
					uv_rtos_task_delay(500);
					LOG("Loading parameters for operator %u", data);

					char *op = uv_jsonreader_array_at(operators, i);
					obj = uv_jsonreader_array_at(op, 0);
					// cycle through all this operators parameters
					for (uint32_t j = 0; (j < uv_jsonreader_array_get_size(op)) &&
							(ret == ERR_NONE); j++) {
						if (uv_jsonreader_get_type(obj) == JSON_OBJECT) {
							ret |= load_param(obj, 0, CANOPEN_UNDEFINED, NULL);
						}
						else {
							LOG_END();
							ERROR("OPERATORS array contained something else\n"
									"than object at operator %u, parameter index %u\n",
									i + 1, j + 1);
						}

						uv_jsonreader_get_next_sibling(obj, &obj);
					}
					LOG_END();
					// only store if every parameter of this operator was written
					if (ret == ERR_NONE) {
						printf("Saving the parameters for op %u...\n", i + 1);
						fflush(stdout);
						ret |= uv_canopen_sdo_store_params(db_get_nodeid(&dev.db),
								MEMORY_ALL_PARAMS);
						if (ret != ERR_NONE) {
							ERROR("Error encountered when storing the parameters for op %u\n", i + 1);
							LOG_SDO_ERROR();
						}
						// wait for the parameters to be saved
						uv_rtos_task_delay(100);
					}
				}

				// restore the current operator only if all operators loaded ok
				if (ret == ERR_NONE) {
					// todo: load the current op
					uint32_t data = uv_jsonreader_get_int(current_op_json) + 1;
					printf("Setting the current operator to op %i\n", data);
					fflush(stdout);
					ret |= uv_canopen_sdo_write(db_get_nodeid(&dev.db), opdb_mindex, 1,
							 CANOPEN_SIZEOF(opdb_type), &data);
					uv_rtos_task_delay(300);
				}
			}
			else {
				if (opdb_mindex_json == NULL) {
					ERRORSTR("OPDB_MAININDEX filed not found\n");
				}
				if (opdb_type_json)
				ERRORSTR("OPERATORS array not found from the JSON.\n\n");
				fflush(stdout);
			}
		}

	}


	return ret;
}


// Standard object-dictionary index of the "suppress EMCY messages" object,
// defined in the shared can_common.json. Writing 1 to it stops the device from
// emitting EMCY warning messages while its parameters are being changed.
#define EMCY_SUPPRESS_INDEX		0x2020


// Object dictionary location and type of the "suppress emcy messages" object in
// the loaded database, found by emcy_suppress_find(). When mindex is 0 the
// database has no such object.
typedef struct {
	uint16_t mindex;
	uint8_t sindex;
	canopen_object_type_e type;
} emcy_suppress_st;


// Searches the loaded database (dev.db) for the "suppress emcy messages" object.
// It is matched first by its standard index (EMCY_SUPPRESS_INDEX, from
// can_common.json) and, failing that, loosely by name (case-insensitive, must
// mention both "suppress" and "emcy") so older databases still work. Fills *out*
// and returns true when found.
static bool emcy_suppress_find(emcy_suppress_st *out) {
	bool found = false;
	for (int32_t i = 0; (i < db_get_object_count(&dev.db)) && !found; i++) {
		db_obj_st *o = db_get_obj(&dev.db, i);
		if (o->obj.main_index == EMCY_SUPPRESS_INDEX) {
			out->mindex = o->obj.main_index;
			out->sindex = o->obj.sub_index;
			out->type = o->obj.type;
			found = true;
			break;
		}
		char *name = dbvalue_get_string(&o->name);
		if (name != NULL) {
			char low[128];
			uint16_t k = 0;
			for (; (name[k] != '\0') && (k < sizeof(low) - 1); k++) {
				low[k] = (char) tolower((unsigned char) name[k]);
			}
			low[k] = '\0';
			if ((strstr(low, "suppress") != NULL) &&
					(strstr(low, "emcy") != NULL)) {
				out->mindex = o->obj.main_index;
				out->sindex = o->obj.sub_index;
				out->type = o->obj.type;
				found = true;
			}
		}
	}
	return found;
}


// Writes *value* (0 or 1) to the "suppress emcy messages" object on *node*.
static void emcy_suppress_write(const emcy_suppress_st *s, uint8_t node,
		uint32_t value) {
	uint32_t v = value;
	uv_errors_e e = uv_canopen_sdo_write(node, s->mindex, s->sindex,
			CANOPEN_SIZEOF(s->type), &v);
	if (e == ERR_NONE) {
		printf("%s EMCY messages on node 0x%x\n",
				value ? "Suppressing" : "Re-enabling", (unsigned int) node);
	}
	else {
		WARNING("Failed to %s EMCY messages on node 0x%x\n",
				value ? "suppress" : "re-enable", (unsigned int) node);
	}
	fflush(stdout);
}


void loadparam_step(void *ptr) {
	this->finished = false;
	this->success = false;

	// scan for additional files given in the parameters
	unsigned int arg_count = 0;
	while (arg_count < dev.argv_count) {
		// argument was given. Write the argument to the device
		strcpy(this->files[arg_count + 1], dev.nonopt_argv[arg_count]);
		arg_count++;
	}

	uv_errors_e e = ERR_NONE;

	// If the database defines a "suppress emcy messages" object, set it on the
	// target device before any parameters are written, so changing parameters
	// does not make the device emit EMCY warning messages. It is cleared again
	// once all parameters have been loaded (below).
	// In system-load mode the caller suppresses (and later clears) EMCY on every
	// device itself, so skip the per-device suppress here.
	emcy_suppress_st emcy_suppress = { };
	bool has_emcy_suppress = !this->sys_load_mode && emcy_suppress_find(&emcy_suppress);
	uint8_t emcy_suppress_node = db_get_nodeid(&dev.db);
	if (has_emcy_suppress && (emcy_suppress_node != 0)) {
		emcy_suppress_write(&emcy_suppress, emcy_suppress_node, 1);
	}

	while (strlen(this->files[this->current_file]) != 0) {
		char *file = this->files[this->current_file];
		FILE *fptr = fopen(file, "rb");

		if (fptr == NULL) {
			// failed to open the file
			ERROR("Failed to open parameter file '%s'.\n", file);
		}
		else {
			int32_t size;
			fseek(fptr, 0, SEEK_END);
			size = ftell(fptr);
			rewind(fptr);

			printf("Opened file '%s'. Size: %i bytes.\n", file, size);
			fflush(stdout);

			char json[size];
			size_t ret = fread(json, size, 1, fptr);
			fclose(fptr);

			if (!ret) {
				ERRORSTR("Reading file failed. "
						"Parameter download cancelled.\n");
				fflush(stderr);
			}
			else {

				uv_jsonreader_init(json, strlen(json));

				// QUERIES array can hold queries that affect the param values.
				// A query is referenced inside a value by its NAME. Two forms
				// are supported for selecting the value by the chosen answer:
				//   legacy positional array, indexed by the answer number:
				//       "DATA": { "valve": [2000, 3500] }
				//   readable answer-keyed object, keyed by the answer text:
				//       "DATA": { "valve": { "Danfoss": 2000, "Sauer": 3500 } }
				// The keyed form is self-documenting and order-independent. If
				// the chosen answer's key is missing, that value is skipped with
				// a warning.
				char *obj = uv_jsonreader_find_child(json, "QUERIES");
				if (obj != NULL &&
						uv_jsonreader_get_type(obj) == JSON_ARRAY) {
					char *queries = obj;
					for (uint16_t i = 0; i < uv_jsonreader_array_get_size(queries); i++) {
						obj = uv_jsonreader_array_at(queries, i);
						query_st q;
						strcpy(q.name, "UNKNOWN");
						bool valid = true;
						char *name = uv_jsonreader_find_child(obj, "NAME");
						if (name != NULL) {
							uv_jsonreader_get_string(name, q.name, sizeof(q.name));
						}
						else {
							valid = false;
						}
						char *question = uv_jsonreader_find_child(obj, "QUESTION");
						if (question != NULL) {
							uv_jsonreader_get_string(question, q.question, sizeof(q.question));
						}
						else {
							valid = false;
						}
						char *answers = uv_jsonreader_find_child(obj, "ANSWERS");
						if (answers != NULL &&
								uv_jsonreader_get_type(answers) == JSON_ARRAY) {
							q.answer_count = uv_jsonreader_array_get_size(answers);
							for (uint16_t i = 0; i < q.answer_count; i++) {
								uv_jsonreader_array_get_string(answers,
										i, q.answers[i], sizeof(q.answers[i]));
							}

						}
						else {
							valid = false;
						}

						if (valid == false) {
							ERROR("ERROR in query '%s'. All values not implemented.\n\n",
									q.name);
						}
						else {
							bool already_answered = false;
							// check if query with this name was already asked
							for (uint32_t i = 0; i < uv_vector_size(&this->queries); i++) {
								query_st *qu = uv_vector_at(&this->queries, i);
								if (strcmp(qu->name, q.name) == 0) {
									already_answered = true;
									printf("Query '%s' was already answered with an answer no %i.\n",
											qu->name,
											qu->correct_answer + 1);
									break;
								}
							}
							if (already_answered == false) {
								// uv_stdin_getline() blocks for real user input
								// without halting the scheduler (so the GUI stays
								// live and can feed the answer via its log command
								// line)
								while (true) {
									PROMPT("\n\n "
											"User input requested: \n"
											"  ** %s **:\n",
											q.question);
									for (uint8_t i = 0; i < q.answer_count; i++) {
										PROMPT("    (%i): %s\n", i + 1, q.answers[i]);
									}

									char ans_str[128] = {};
									int32_t ans = 0;
									uv_stdin_getline(ans_str, sizeof(ans_str) - 1);
									if (sscanf(ans_str, " %d", &ans) < 1) {
										ans = 1;
									}

									if (ans < 1 || ans > q.answer_count) {
										PROMPTSTR("Answer out of bounds. Defaulting to 1.\n");
									}
									else {
										printf("Query '%s': selected (%i) %s\n",
												q.name, ans, q.answers[ans - 1]);
										q.correct_answer = ans - 1;
										break;
									}
								}

								uv_vector_push_back(&this->queries, &q);
							}
						}
					}
				}

				obj = uv_jsonreader_find_child(json, "DEVS");
				if (obj != NULL &&
						uv_jsonreader_get_type(obj) == JSON_ARRAY) {
					// new protocol where each device's settings are stored in a DEVS-array
					for (uint16_t i = 0; i < uv_jsonreader_array_get_size(obj); i++) {
						char *dev = uv_jsonreader_array_at(obj, i);
						if (dev != NULL) {
							e |= parse_dev(dev);
						}
						else {
							ERROR("Parsing DEVS array with index %i resulted in NULL pointer\n", i);
						}
						if (e != ERR_NONE) {
							// a transfer to this device failed: stop the load
							break;
						}
					}
				}
				else {
					// deprecated database protocol, where only one device was
					// supported
					e = parse_dev(json);
				}

				if (e != ERR_NONE) {
					ERRORSTR("Error when fetching operator settings.\n"
							"Loading the parameters might have failed\n");
					fflush(stderr);
				}
			}
		}
		this->current_file++;
		if (e != ERR_NONE) {
			// a CANopen transfer failed: stop processing further files
			break;
		}
	}

	// all parameters have been written: clear the "suppress emcy messages" flag
	// again on every device it could have been set on. Done before the parameters
	// are stored and the devices reset, so the flag is not persisted as suppressed.
	if (has_emcy_suppress) {
		bool cleared[128] = { };
		if (emcy_suppress_node != 0) {
			emcy_suppress_write(&emcy_suppress, emcy_suppress_node, 0);
			cleared[emcy_suppress_node] = true;
		}
		for (uint8_t i = 0; i < this->dev_count; i++) {
			uint8_t nodeid = this->modified_dev_nodeids[i];
			if ((nodeid != 0) && !cleared[nodeid]) {
				emcy_suppress_write(&emcy_suppress, nodeid, 0);
				cleared[nodeid] = true;
			}
		}
	}

	uv_errors_e ret = ERR_NONE;
	if (e != ERR_NONE) {
		// A CANopen read/write failed (after the SDO client's retries). Stop the
		// whole load: the target device(s) are only partially programmed, so do
		// not store the parameters or reset them.
		LOG_END();
		ERRORSTR("\nERROR: Parameter loading stopped due to a CANopen "
				"communication error.\n"
				"The target device(s) may be left partially programmed and were\n"
				"NOT stored or reset.\n\n");
		fflush(stdout);
	}
	// save the params to all devices, then reset them. Skipped in system-load
	// mode: there the caller stores and resets every device together once all
	// devices have been written.
	else if (!this->sys_load_mode) {
		for (uint8_t i = 0; i < this->dev_count; i++) {
			uint8_t nodeid = this->modified_dev_nodeids[i];
			printf("Saving the parameters to dev 0x%x...\n", nodeid);
			fflush(stdout);
			ret |= uv_canopen_sdo_store_params(nodeid,
					MEMORY_ALL_PARAMS);
			uv_rtos_task_delay(300);
			printf("Resetting the device 0x%x...\n", nodeid);
			fflush(stdout);
			if (ret == ERR_NONE) {
				uv_canopen_nmt_master_send_cmd(nodeid,
						CANOPEN_NMT_CMD_RESET_NODE);
			}
			else {
				printf(PRINT_YELLOW
					   "The device 0x%x was not reset due to errors. \n"
						"Manual reset is necessary.\n\n"
					   PRINT_RESET,
						nodeid);
				fflush(stdout);
			}
		}
	}

	// the load succeeded only if no parameter transfer and no store/reset failed
	this->success = (e == ERR_NONE) && (ret == ERR_NONE);
	this->finished = true;
}


bool loadparam_can_if_mismatch(device_st *device,
		uint32_t *file_if, uint32_t *dev_if) {
	uint32_t fif = 0;
	uint32_t did = 0;
	bool file_known = false;

	if ((device != NULL) && (strlen(device->param_file) != 0)) {
		FILE *fptr = fopen(device->param_file, "rb");
		if (fptr != NULL) {
			fseek(fptr, 0, SEEK_END);
			long size = ftell(fptr);
			rewind(fptr);
			// read into a heap buffer (this runs on the UI thread, so avoid a
			// large variable-length array on the stack)
			char *json = (size > 0) ? malloc(size + 1) : NULL;
			if ((json != NULL) && (fread(json, 1, size, fptr) == (size_t) size)) {
				json[size] = '\0';
				uv_jsonreader_init(json, strlen(json));

				// the param file stores its CAN IF VERSION inside the DEVS array
				// (one object per device); fall back to the top level for the
				// deprecated single-device format
				char *devobj = NULL;
				char *devs = uv_jsonreader_find_child(json, "DEVS");
				if ((devs != NULL) &&
						(uv_jsonreader_get_type(devs) == JSON_ARRAY)) {
					int32_t cnt = uv_jsonreader_array_get_size(devs);
					// prefer the entry matching this device's node id
					for (int32_t i = 0; (i < cnt) && (devobj == NULL); i++) {
						char *d = uv_jsonreader_array_at(devs, i);
						char *nid = uv_jsonreader_find_child(d, "NODEID");
						if ((nid != NULL) && ((uint8_t) uv_jsonreader_get_int(nid)
								== device->nodeid)) {
							devobj = d;
						}
					}
					if ((devobj == NULL) && (cnt > 0)) {
						devobj = uv_jsonreader_array_at(devs, 0);
					}
				}
				else {
					devobj = json;
				}

				if (devobj != NULL) {
					char *cif = uv_jsonreader_find_child(devobj, "CAN IF VERSION");
					if ((cif != NULL) &&
							(uv_jsonreader_get_type(cif) == JSON_INT)) {
						fif = uv_jsonreader_get_int(cif);
						file_known = true;
					}
				}
			}
			if (json != NULL) {
				free(json);
			}
			fclose(fptr);
		}
	}

	// read the device's current CAN IF version over the bus
	did = find_read_device_revision(device);

	if (file_if != NULL) {
		*file_if = fif;
	}
	if (dev_if != NULL) {
		*dev_if = did;
	}

	// only a mismatch when both values are known and they differ
	return file_known && (did != 0) && (fif != did);
}


// Loads *device*'s database, looks up its EMCY-suppress object and writes *value*
// (1 = suppress, 0 = re-enable) to it on the device. *out*, when not NULL,
// receives the object's location so the caller can write it again later without
// reloading the database. Returns true when the device has the object.
static bool loadparam_emcy_suppress_device(device_st *device, uint32_t value,
		emcy_suppress_st *out) {
	bool ret = false;
	if ((device != NULL) && (strlen(device->filepath) != 0) &&
			(device->nodeid != 0) && load_device_db(device)) {
		emcy_suppress_st s = { };
		if (emcy_suppress_find(&s)) {
			emcy_suppress_write(&s, device->nodeid, value);
			if (out != NULL) {
				*out = s;
			}
			ret = true;
		}
		db_deinit();
	}
	return ret;
}


// Sets the log frame title (no-op when the UI is not compiled in).
static void loadparam_set_title(const char *title) {
#if CONFIG_UI
	uvui_set_log_title(title);
#else
	(void) title;
#endif
}

static void loadparam_reset_title(void) {
#if CONFIG_UI
	uvui_reset_log_title();
#endif
}


// Target list for the asynchronous system parameter load.
static device_st *async_sys_devs[SYSTEM_DEV_MAX_COUNT];
static uint8_t async_sys_count;
static volatile bool async_sys_finished = true;


bool loadparam_load_system_is_finished(void) {
	return async_sys_finished;
}


/// @brief: Task body loading a whole system's parameters in five phases, so the
/// global ordering is: suppress EMCY everywhere, write each device, re-enable
/// EMCY, store, then reset every device together.
static void loadparam_system_task(void *ptr) {
	uint8_t n = async_sys_count;
	emcy_suppress_st emcy[SYSTEM_DEV_MAX_COUNT] = { };
	bool has_emcy[SYSTEM_DEV_MAX_COUNT] = { };
	bool written[SYSTEM_DEV_MAX_COUNT] = { };

	uv_can_set_up(false);

	// Phase 1: suppress EMCY messages on every device before any parameter is
	// written, so no device emits EMCY warnings while another is being changed.
	loadparam_set_title("Loading system: suppressing EMCY messages...");
	printf("Suppressing EMCY messages on all devices...\n");
	fflush(stdout);
	for (uint8_t i = 0; i < n; i++) {
		device_st *d = async_sys_devs[i];
		if ((d != NULL) && (strlen(d->param_file) != 0)) {
			has_emcy[i] = loadparam_emcy_suppress_device(d, 1, &emcy[i]);
		}
	}

	// Phase 2: write each device's parameters in turn (system-load mode skips the
	// per-device EMCY handling, store and reset done in the phases below). A failed
	// device aborts the whole system load: the remaining devices are left untouched
	// and only the devices written so far are stored/reset below.
	for (uint8_t i = 0; i < n; i++) {
		device_st *d = async_sys_devs[i];
		if ((d != NULL) && (strlen(d->param_file) != 0)) {
			char title[256];
			const char *dname = (strlen(d->devname) > 0) ? d->devname : d->name;
			snprintf(title, sizeof(title),
					"Loading system: parameters to device %u/%u (%s)",
					(unsigned int) (i + 1), (unsigned int) n, dname);
			loadparam_set_title(title);
			printf("Loading parameters to node 0x%x from '%s'\n",
					(unsigned int) d->nodeid, d->param_file);
			fflush(stdout);
			written[i] = loadparam_load_device_ex(d, d->param_file, true);
			if (!written[i]) {
				ERROR("System load stopped: writing parameters to node 0x%x "
						"failed.\n", (unsigned int) d->nodeid);
				fflush(stdout);
				break;
			}
		}
	}

	// Phase 3: re-enable EMCY messages now that all parameters are written, so the
	// suppressed state is not stored persistently in the next phase.
	for (uint8_t i = 0; i < n; i++) {
		device_st *d = async_sys_devs[i];
		if ((d != NULL) && has_emcy[i]) {
			emcy_suppress_write(&emcy[i], d->nodeid, 0);
		}
	}

	// Phase 4: store the parameters on every written device.
	loadparam_set_title("Loading system: storing parameters...");
	for (uint8_t i = 0; i < n; i++) {
		device_st *d = async_sys_devs[i];
		if ((d != NULL) && written[i]) {
			printf("Saving the parameters to dev 0x%x...\n",
					(unsigned int) d->nodeid);
			fflush(stdout);
			uv_canopen_sdo_store_params(d->nodeid, MEMORY_ALL_PARAMS);
			uv_rtos_task_delay(300);
		}
	}

	// Phase 5: reset every written device, as close to simultaneously as the bus
	// allows, so they all come back up together.
	loadparam_set_title("Loading system: resetting devices...");
	printf("Resetting all devices...\n");
	fflush(stdout);
	for (uint8_t i = 0; i < n; i++) {
		device_st *d = async_sys_devs[i];
		if ((d != NULL) && written[i]) {
			uv_canopen_nmt_master_send_cmd(d->nodeid, CANOPEN_NMT_CMD_RESET_NODE);
		}
	}

	loadparam_reset_title();
	async_sys_finished = true;
	uv_rtos_task_delete(NULL);
}


void loadparam_load_system_async(device_st **devices, uint8_t count) {
	if (count > SYSTEM_DEV_MAX_COUNT) {
		count = SYSTEM_DEV_MAX_COUNT;
	}
	for (uint8_t i = 0; i < count; i++) {
		async_sys_devs[i] = devices[i];
	}
	async_sys_count = count;
	// mark in-progress before the task starts so a caller can poll immediately
	async_sys_finished = false;
	uv_rtos_task_create(&loadparam_system_task, "loadsys_task",
			UV_RTOS_MIN_STACK_SIZE * 5, NULL, UV_RTOS_IDLE_PRIORITY + 1, NULL);
}
