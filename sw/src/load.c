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
#include <string.h>
#include "load.h"
#include "main.h"
#include "uvdev.h"

#define this (&dev.load)


void load_step(void *dev);


#define BLOCK_SIZE	256
#define RESPONSE_DELAY_MS	2000
#define BOOTLOADER_INDEX	0x1F50
#define BOOTLOADER_SUBINDEX	1


static void update(void *ptr) {
	int32_t data_index = 0;
	while (true) {
		if (_canopen.sdo.client.data_index != data_index) {
			int32_t percent = (_canopen.sdo.client.data_index * 100 +
					_canopen.sdo.client.data_count / 2) /
							_canopen.sdo.client.data_count;
			printf("downloaded %u / %u bytes (%u %%)\n",
					_canopen.sdo.client.data_index,
					_canopen.sdo.client.data_count,
					percent);
			fflush(stdout);
			this->progress = percent;
			if (percent == 100) {
				printf("Loading done!\n");
				uv_rtos_task_delete(NULL);
				break;
			}
		}
		data_index = _canopen.sdo.client.data_index;
		uv_rtos_task_delay(20);
	}
	uv_rtos_task_delete(NULL);
}


/// @brief: Configures the load module's state from a firmware file path and the
/// bootloader options shared by all load variants. Does not start the transfer.
static void load_configure(const char *path, bool wfr, bool uv, bool block_transfer) {
	strcpy(this->firmware, path);
	this->response = false;
	this->wfr = wfr;
	this->uv = uv;
	this->block_transfer = block_transfer;
	this->nodeid = db_get_nodeid(&dev.db);
	this->cancel = false;
	this->waiting = false;
	// the CLI wfr variants keep their bounded wait; the UI's loadbin() opts into
	// an indefinite wait explicitly after this call
	this->wait_forever = false;
	uv_delay_init(&this->delay, wfr ? LOADWFR_WAIT_TIME_MS : RESPONSE_DELAY_MS);
}


bool load_firmware(const char *path, bool wfr, bool uv, bool block_transfer) {
	bool ret = false;

	if (!path) {
		printf("ERROR: Give firmware as a file path to binary file.\n");
	}
	else {
		printf("Firmware %s selected\n", path);
		load_configure(path, wfr, uv, block_transfer);
		add_task(load_step);
		uv_can_set_up(false);
		ret = true;
	}

	return ret;
}


/// @brief: FreeRTOS task entry for the in-software loadbin path. load_step()
/// runs the whole transfer synchronously and then returns; a FreeRTOS task
/// function must delete itself instead of returning (the CLI path wraps it in
/// task_step() which does this), so do it here for the direct-task path.
static void loadbin_task(void *ptr) {
	load_step(ptr);
	uv_rtos_task_delete(NULL);
}


void load_cancel(void) {
	this->cancel = true;
}


void loadbin(char *filepath, uint8_t nodeid, bool wfr, bool uv, bool block_transfer) {
	load_configure(filepath, wfr, uv, block_transfer);
	// the in-software entry targets an explicitly given node instead of the
	// one from the loaded database
	this->nodeid = nodeid;
	// mark as in-progress synchronously so a caller that polls right away does
	// not see a stale "finished" from a previous flash before the task starts
	this->finished = false;
	this->cancel = false;
	this->waiting = false;
	// the in-software (UI) entry waits indefinitely for the device when flashing
	// with wfr, so an offline device can be powered on and then flashed
	this->wait_forever = wfr;

	uv_rtos_task_create(&loadbin_task, "loadbin_task",
			UV_RTOS_MIN_STACK_SIZE, NULL, UV_RTOS_IDLE_PRIORITY + 1, NULL);
}


bool load_flash_uvdev_to_node(const char *uvdev_path, uint8_t nodeid, bool wfr) {
	bool ret = false;
	if ((uvdev_path == NULL) || (strlen(uvdev_path) == 0)) {
		printf(PRINT_BOLDRED
				"ERROR: no firmware package given to flash.\n" PRINT_RESET);
	}
	else {
		uvdev_st pkg;
		if (!uvdev_open(&pkg, uvdev_path)) {
			printf(PRINT_BOLDRED "ERROR: failed to open the device package '%s'.\n"
					PRINT_RESET, uvdev_path);
		}
		else {
			if (strlen(pkg.firmware) == 0) {
				printf(PRINT_BOLDRED "ERROR: package '%s' has no FIRMWARE entry in "
						"its manifest.\n" PRINT_RESET, uvdev_path);
			}
			else {
				// copy the firmware out of the (soon to be removed) package temp
				// dir to a stable path the async flash task can read
				char src[2048];
				static char dst[] = "/tmp/uvcan_flash.bin";
				char cmd[4200];
				snprintf(src, sizeof(src), "%s/%s", pkg.dir, pkg.firmware);
				snprintf(cmd, sizeof(cmd), "cp '%s' '%s'", src, dst);
				if (system(cmd) == 0) {
					printf("Flashing firmware '%s' to node 0x%x%s\n",
							pkg.firmware, nodeid,
							wfr ? " once it boots up" : "");
					loadbin(dst, nodeid, wfr, false, true);
					ret = true;
				}
				else {
					printf(PRINT_BOLDRED "ERROR: failed to extract the firmware "
							"binary from the package.\n" PRINT_RESET);
				}
			}
			uvdev_close(&pkg);
		}
	}
	return ret;
}


bool load_flash_device_to_node(device_st *device, uint8_t nodeid, bool wfr) {
	bool ret = false;
	if ((device == NULL) || (strlen(device->filepath) == 0)) {
		printf(PRINT_BOLDRED
				"ERROR: the device has no configuration package; assign a .uvdev\n"
				"file before flashing firmware.\n" PRINT_RESET);
	}
	else {
		ret = load_flash_uvdev_to_node(device->filepath, nodeid, wfr);
	}
	return ret;
}


bool load_flash_device(device_st *device, bool wfr) {
	// flash the device's own package to its own node id
	return load_flash_device_to_node(device,
			(device != NULL) ? device->nodeid : 0, wfr);
}


// Synchronously flashes the binary at *bin_path* to *nodeid* using the shared
// load state and bootloader options, running the whole transfer in the calling
// task (blocks until done). Returns true on a completed, successful flash. Used
// to flash several devices in turn from the load dispatch task.
static bool flash_bin_sync(const char *bin_path, uint8_t nodeid,
		bool wfr, bool uv, bool block_transfer) {
	load_configure(bin_path, wfr, uv, block_transfer);
	// load_configure sets the node from the loaded database; override it with the
	// explicit target node of this flash
	this->nodeid = nodeid;
	this->finished = false;
	load_step(NULL);
	return this->success;
}


// Extracts the FIRMWARE binary from the .uvdev package at *uvdev_path* and flashes
// it to *nodeid* synchronously. The transfer runs while the package's temporary
// directory is still open, so the binary is read straight from it. Returns true on
// a successful flash; warns and returns false when the package is unreadable or
// has no FIRMWARE entry.
static bool flash_uvdev_sync(const char *uvdev_path, uint8_t nodeid,
		bool wfr, bool uv, bool block_transfer) {
	bool ret = false;
	uvdev_st pkg;
	if (!uvdev_open(&pkg, uvdev_path)) {
		printf(PRINT_BOLDRED "ERROR: failed to open the device package '%s'.\n"
				PRINT_RESET, uvdev_path);
	}
	else {
		if (strlen(pkg.firmware) == 0) {
			printf(PRINT_BOLDRED "ERROR: package '%s' has no FIRMWARE entry in "
					"its manifest.\n" PRINT_RESET, uvdev_path);
		}
		else {
			char src[1600];
			snprintf(src, sizeof(src), "%s/%s", pkg.dir, pkg.firmware);
			printf("Flashing firmware '%s' to node 0x%x%s\n", pkg.firmware,
					nodeid, wfr ? " once it boots up" : "");
			ret = flash_bin_sync(src, nodeid, wfr, uv, block_transfer);
		}
		uvdev_close(&pkg);
	}
	return ret;
}


// Flashes the firmware of every system device in the index range [start, end)
// to its own node id, in turn. Devices with no configuration package are skipped
// with a warning. Prints a per-run summary.
static void flash_devices(uint8_t start, uint8_t end,
		bool wfr, bool uv, bool block_transfer) {
	uint8_t total = 0;
	uint8_t ok = 0;
	for (uint8_t i = start; i < end; i++) {
		device_st *d = &dev.system.devs[i];
		if (strlen(d->filepath) == 0) {
			printf(PRINT_BOLDYELLOW "Skipping device '%s' (node 0x%x): no "
					"configuration package.\n" PRINT_RESET,
					d->name, (unsigned int) d->nodeid);
			continue;
		}
		total++;
		printf("\n=== Flashing device %u/%u: '%s' (node 0x%x) ===\n",
				(unsigned int) (i - start + 1), (unsigned int) (end - start),
				d->name, (unsigned int) d->nodeid);
		if (flash_uvdev_sync(d->filepath, d->nodeid, wfr, uv, block_transfer)) {
			ok++;
		}
		else {
			printf(PRINT_BOLDRED "Flashing device '%s' (node 0x%x) failed.\n"
					PRINT_RESET, d->name, (unsigned int) d->nodeid);
		}
	}
	printf("\nFlashed %u/%u device(s) successfully.\n",
			(unsigned int) ok, (unsigned int) total);
}


// Task body for the loadbin command family. Runs once the scheduler is up (so the
// non-option arguments are resolvable) and dispatches on the effective argument:
//   - a .uvsys package: load it and flash every device it adds
//   - a .uvdev package:  flash its FIRMWARE to the selected node
//   - any other path:    flash it as a raw firmware binary (legacy behavior)
//   - no argument:       flash every device already loaded with --dev / --sys
static void load_dispatch_step(void *ptr) {
	const char *arg = cmdline_load_arg(this->dispatch_arg);
	bool wfr = this->dispatch_wfr;
	bool uv = this->dispatch_uv;
	bool block = this->dispatch_block;

	if (path_is_uvsys(arg)) {
		uint8_t prev = dev.system.dev_count;
		if (!system_set_file(&dev.system, arg)) {
			printf(PRINT_BOLDRED "ERROR: failed to load system package '%s'.\n"
					PRINT_RESET, arg);
		}
		else {
			flash_devices(prev, dev.system.dev_count, wfr, uv, block);
		}
	}
	else if (path_is_uvdev(arg)) {
		uint8_t prev = dev.system.dev_count;
		// use the node id forced with --nodeid when given (0 -> the package's own)
		if (!system_add_device(&dev.system, arg, db_get_nodeid(&dev.db))) {
			printf(PRINT_BOLDRED "ERROR: failed to add device package '%s'.\n"
					PRINT_RESET, arg);
		}
		else {
			flash_devices(prev, dev.system.dev_count, wfr, uv, block);
		}
	}
	else if (arg != NULL) {
		// raw firmware binary: flash to the node from --nodeid / --db (legacy)
		printf("Firmware %s selected\n", arg);
		flash_bin_sync(arg, db_get_nodeid(&dev.db), wfr, uv, block);
	}
	else if (dev.system.dev_count != 0) {
		// no file given: flash the devices already loaded with --dev / --sys
		flash_devices(0, dev.system.dev_count, wfr, uv, block);
	}
	else {
		printf(PRINT_BOLDRED "ERROR: no firmware given. Provide a firmware binary, "
				"a .uvdev / .uvsys package, or load devices with --dev / --sys "
				"first.\n" PRINT_RESET);
	}
}


// Common entry for the loadbin command variants: captures the dispatch parameters
// and registers the dispatch task.
static bool load_dispatch(const char *arg, bool wfr, bool uv, bool block_transfer) {
	strncpy(this->dispatch_arg, (arg != NULL) ? arg : "",
			sizeof(this->dispatch_arg) - 1);
	this->dispatch_arg[sizeof(this->dispatch_arg) - 1] = '\0';
	this->dispatch_wfr = wfr;
	this->dispatch_uv = uv;
	this->dispatch_block = block_transfer;
	add_task(load_dispatch_step);
	uv_can_set_up(false);
	return true;
}


bool cmd_load(const char *arg) {
	return load_dispatch(arg, false, false, true);
}


bool cmd_loadwfr(const char *arg) {
	return load_dispatch(arg, true, false, true);
}


bool cmd_segload(const char *arg) {
	return load_dispatch(arg, false, false, false);
}


bool cmd_segloadwfr(const char *arg) {
	return load_dispatch(arg, true, false, false);
}


bool cmd_uvload(const char *arg) {
	return load_dispatch(arg, false, true, true);
}


bool cmd_uvloadwfr(const char *arg) {
	return load_dispatch(arg, true, true, true);
}



static void can_callb(void * ptr, uv_can_msg_st *msg) {
	if ((msg->id == CANOPEN_HEARTBEAT_ID + this->nodeid) &&
			(msg->type == CAN_STD) &&
			(msg->data_length == 1) &&
			((msg->data_8bit[0] == CANOPEN_BOOT_UP))) {
		// canopen boot up message received, node found.
		this->response = true;
		printf("done\n");

		// disable CAN callback, it's not needed anymore.
		uv_canopen_set_can_callback(NULL);
	}
}

void load_step(void *ptr) {
	this->finished = false;
	this->success = false;
	this->progress = 0;

	FILE *fptr = fopen(this->firmware, "rb");

	if (fptr == NULL) {
		// failed to open the file, exit this task
		printf("Failed to open firmware file %s.\n", this->firmware);
		fflush(stdout);
	}
	else {
		int32_t size;
		fseek(fptr, 0, SEEK_END);
		size = ftell(fptr);
		rewind(fptr);
		bool success = false;

		printf("Opened file %s. Size: %i bytes.\n", this->firmware, size);
		fflush(stdout);

		// uv-version of bootloader, i.e. the old version where
		// node had to be reset before downloading
		this->response = !this->uv;
		if (this->uv) {
			// set canopen callback function
			uv_canopen_set_can_callback(&can_callb);

			if (!this->wfr) {
				printf("Resetting node 0x%x\n", this->nodeid);
				fflush(stdout);
				uv_canopen_nmt_master_send_cmd(this->nodeid,
						CANOPEN_NMT_CMD_RESET_NODE);
			}

			// wait for a response to NMT reset command
			printf("Waiting to receive boot up message from node 0x%x...\n", this->nodeid);
			fflush(stdout);
			this->waiting = true;
			while (true) {
				uint16_t step_ms = 1;
				if (this->response) {
					break;
				}
				else if (this->cancel) {
					printf("Flashing cancelled.\n");
					fflush(stdout);
					break;
				}
				else if (!this->wait_forever && uv_delay(&this->delay, step_ms)) {
					printf("Couldn't reset node. No response to NMT Reset Node.\n");
					fflush(stdout);
					break;
				}
				else {
					// keep waiting for the boot-up message
				}
				uv_rtos_task_delay(step_ms);
			}
			this->waiting = false;
			printf("Reset OK. Now downloading...\n");
			fflush(stdout);

			// download with multiple block transfers
			if (this->block_transfer) {
				printf("Downloading the firmware as SDO block transfer\n");
				uint8_t data[BLOCK_SIZE];
				int32_t data_length;
				int32_t index = 0;
				uint32_t block = 0;
				success = true;
				while (index < size) {
					block++;
					if (index + BLOCK_SIZE <= size) {
						data_length = BLOCK_SIZE;
					}
					else {
						data_length = size - index;
					}
					if (this->cancel) {
						printf("Flashing cancelled.\n");
						fflush(stdout);
						success = false;
						break;
					}
					size_t ret = fread(data, data_length, 1, fptr);

					if (!ret) {
						printf("ERROR: Reading file failed at byte %u / %u. "
								"Firmware download cancelled.\n", index, size);
						fflush(stdout);
						break;
					}
					else {
						if (uv_canopen_sdo_block_write(this->nodeid, BOOTLOADER_INDEX, BOOTLOADER_SUBINDEX,
								data_length, data) != ERR_NONE) {
							printf("Error while downloading block %u.\n", block);
							fflush(stdout);
							success = false;
							break;
						}
						else {
							this->progress = (index + data_length) * 100 / size;
							printf("Block %u downloaded, %u / %u bytes (%u %%)\n",
									block,
									index + data_length,
									size,
									this->progress);
							fflush(stdout);
						}
					}
					index += data_length;
				}
			}
		}
		// new CiA 302 compatible protocol download
		else {
			if (this->wfr) {
				this->response = false;
				// set canopen callback function
				uv_canopen_set_can_callback(&can_callb);
				// wait for the device to send its boot-up message. With
				// wait_forever set (the UI's offline flash) this waits until the
				// device is powered on or the flash is cancelled.
				printf("Waiting to receive boot up message from node 0x%x...\n", this->nodeid);
				fflush(stdout);
				this->waiting = true;
				while (true) {
					uint16_t step_ms = 1;
					if (this->response) {
						break;
					}
					else if (this->cancel) {
						printf("Flashing cancelled.\n");
						fflush(stdout);
						break;
					}
					else if (!this->wait_forever && uv_delay(&this->delay, step_ms)) {
						printf("Couldn't reset node. No response to NMT Reset Node.\n");
						fflush(stdout);
						break;
					}
					else {
						// keep waiting for the boot-up message
					}
					uv_rtos_task_delay(step_ms);
				}
				this->waiting = false;
				// the callback is no longer needed once we stop waiting (it is
				// cleared on boot-up too, but not on cancel/timeout)
				uv_canopen_set_can_callback(NULL);
			}

			if (this->response && !this->cancel) {

				printf("Now downloading...\n");
				fflush(stdout);

				// download with block transfer
				if (this->block_transfer) {
					printf("Downloading the firmware as SDO block transfer.\n");
					uint8_t *data = malloc(size);
					size_t ret = fread(data, size, 1, fptr);

					if (!ret) {
						printf("ERROR: Reading file failed. "
								"Firmware download cancelled.\n");
						fflush(stdout);
					}
					else {
						// create task which will update the screen with the loading process
						uv_rtos_task_create(&update, "segload update", UV_RTOS_MIN_STACK_SIZE,
								NULL, UV_RTOS_IDLE_PRIORITY + 100, NULL);

						uv_errors_e e = uv_canopen_sdo_block_write(this->nodeid,
								BOOTLOADER_INDEX, BOOTLOADER_SUBINDEX, size, data);
						if (e != ERR_NONE) {
							printf("Downloading the binary failed. Error code: %u\n", e);
							success = false;
						}
						else {
							success = true;
						}
					}
					free(data);
				}
				// download with segmented transfer
				else {
					printf("Downloading the firmware as SDO segmented transfer\n");
					uint8_t data[size];
					size_t ret = fread(data, size, 1, fptr);

					if (!ret) {
						printf("ERROR: Reading file failed. "
								"Firmware download cancelled.\n");
						fflush(stdout);
					}
					else {
						// create task which will update the screen with the loading process
						uv_rtos_task_create(&update, "segload update", UV_RTOS_MIN_STACK_SIZE,
								NULL, UV_RTOS_IDLE_PRIORITY + 100, NULL);

						uv_errors_e e = uv_canopen_sdo_write(this->nodeid,
								BOOTLOADER_INDEX, BOOTLOADER_SUBINDEX, size, data);
						if (e != ERR_NONE) {
							printf("Downloading the binary failed. Error code: %u\n", e);
							success = false;
						}
						else {
							success = true;
						}
					}
				}
			}
		}

		fclose(fptr);
		if (success) {
			printf("Loading done. Resetting device... OK!\n");
			fflush(stdout);
			uv_canopen_nmt_master_send_cmd(this->nodeid,
					CANOPEN_NMT_CMD_RESET_NODE);
			printf("Binary file closed.\n");
			fflush(stdout);
		}
		this->success = success;
	}

	this->finished = true;
}
