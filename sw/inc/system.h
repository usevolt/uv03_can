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


#ifndef SYSTEM_H_
#define SYSTEM_H_


#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


/// @brief: Maximum number of devices a single system can hold. This is also
/// the maximum number of device tabs shown in the UI.
#define SYSTEM_DEV_MAX_COUNT		20


/// @brief: Connection state of a device on the CAN bus. The non-OFFLINE values
/// mirror the device's CANopen NMT state, as read from its heartbeat. They are
/// ordered by "liveness" so a simple comparison (state != DEV_STATE_OFFLINE)
/// tells whether the device is reachable on the bus.
typedef enum {
	/// @brief: The device has not (yet) been seen on the CAN bus. This is the
	/// default for devices added from a configuration file (--dev / --sys).
	DEV_STATE_OFFLINE = 0,
	/// @brief: The device is sending a BOOT_UP heartbeat (NMT boot-up): it has
	/// appeared on the bus but is not yet pre-operational.
	DEV_STATE_BOOTUP,
	/// @brief: The device is sending a PRE-OPERATIONAL (or STOPPED) heartbeat: it
	/// is on the bus and answers SDO requests, but does not exchange PDOs.
	DEV_STATE_PREOP,
	/// @brief: The device is sending an OPERATIONAL heartbeat: fully running.
	DEV_STATE_OP,
} dev_state_e;


/// @brief: A single device belonging to a system. A device is described by
/// a configuration file given on the command line with --device, or created
/// empty from the UI's "Add device" tab.
typedef struct {
	/// @brief: Path to the device configuration file. Empty string when the
	/// device was created from the UI and has not been saved yet.
	char filepath[1024];
	/// @brief: Device name derived from the configuration file name. Used as the
	/// tab title when the DATABASE does not provide a DEV_NAME default.
	char name[128];
	/// @brief: The device's friendly name, read from the DATABASE's DEV_NAME
	/// object default value (with defines resolved). Empty when not available.
	char devname[64];
	/// @brief: The device's CANopen node id. Either given explicitly when the
	/// device was added, or read from the configuration file's DATABASE.
	uint8_t nodeid;
	/// @brief: The default node id defined in the configuration file's DATABASE.
	/// Shown as "Default Node-ID" in the device tab. Unlike *nodeid* this is never
	/// overwritten by the device's live node id, so it always reflects the file.
	/// 0 when unknown (no file, or the database defines none).
	uint8_t default_nodeid;
	/// @brief: The CAN object dictionary revision number (CAN IF VERSION), read
	/// from the configuration file's DATABASE. 0 when unknown.
	uint32_t revision;
	/// @brief: The device's own CAN IF VERSION, read from the CAN IF VERSION
	/// object on the device over the bus. 0 when unknown (e.g. offline). Compared
	/// against *revision* to detect a configuration/firmware mismatch.
	uint32_t dev_revision;
	/// @brief: Object dictionary location and size of the device's CAN IF VERSION
	/// object, captured from the configuration file's DATABASE so *dev_revision*
	/// can be read over the bus. *if_version_mindex* is 0 when the database has no
	/// such object.
	uint16_t if_version_mindex;
	uint8_t if_version_sindex;
	uint8_t if_version_size;
	/// @brief: Firmware software version string from the .uvdev package manifest's
	/// VERSION field. Empty when not available. Shown next to the device's own
	/// software version so the two can be compared.
	char conf_version[64];
	/// @brief: CANopen vendor id, read from the DATABASE. 0 when unknown.
	uint32_t vendor_id;
	/// @brief: CANopen product code, read from the DATABASE. 0 when unknown.
	uint32_t product_code;
	/// @brief: Device software version, read from the device's Identity object
	/// (0x1018 sub 3, "Revision number") over the CAN bus while it is online.
	/// 0 when unknown (e.g. the device has never been seen on the bus).
	uint32_t sw_version;
	/// @brief: Latches that a software-version read has been attempted while the
	/// device is online, so a read that returns nothing (e.g. a device whose
	/// Identity object does not answer with a plain 4-byte value) is not retried
	/// every cycle. Cleared when the device leaves OP/PRE-OP so a fresh read is
	/// taken after a reboot/reconnect.
	bool sw_version_tried;
	/// @brief: Whether (and how) the device is currently seen on the CAN bus.
	/// Devices added from a file start OFFLINE; --find and the --ui monitor set it
	/// to the device's live NMT state (BOOTUP / PREOP / OP).
	dev_state_e state;
	/// @brief: True once a configuration file has been parsed into this device.
	bool loaded;
	/// @brief: True when the device's .uvdev package bundles a media directory
	/// (the MEDIA manifest entry). Lets the device tab offer "Load media files".
	bool has_media;
	/// @brief: True when the device was discovered on the bus but is not a Usevolt
	/// device (its Identity vendor id differs from Usevolt's). Such devices have no
	/// configuration package: the UI shows only their CAN-bus identity.
	bool thirdparty;
	/// @brief: Path to a saved parameter file (.json) for this device, set when the
	/// device was loaded from a .uvsys system package that bundled one (the PARAM
	/// entry, extracted into the package's temp dir). Empty otherwise. Used to
	/// offer loading those parameters onto the device once it is online.
	char param_file[1024];
} device_st;


/// @brief: A system groups together up to SYSTEM_DEV_MAX_COUNT devices that
/// share a CAN bus. The system itself is described by a configuration file
/// given on the command line with --system.
typedef struct {
	/// @brief: Path to the system configuration file. Empty string when no
	/// --system file was given.
	char filepath[1024];
	/// @brief: Human readable system name, shown on the "System" tab.
	char name[128];
	/// @brief: True once a system configuration file has been parsed.
	bool loaded;

	device_st devs[SYSTEM_DEV_MAX_COUNT];
	uint8_t dev_count;
} system_st;


/// @brief: Resets the system to an empty, unconfigured state.
void system_reset(system_st *this);

/// @brief: Installs temporary-directory cleanup. Call once at startup. Registers
/// an atexit handler that removes the active .uvsys extraction directory on a
/// clean exit, and sweeps stale extraction directories left behind by earlier
/// runs that crashed or were killed (which bypass the atexit handler).
void system_init_tmp_cleanup(void);

/// @brief: Removes every .uvsys extraction directory created during this run.
/// Same work as the registered atexit handler, but callable directly from a
/// signal handler (the HAL's SIGINT handler _exit()s and so skips atexit).
/// Idempotent: a second call has nothing left to remove.
void system_remove_tmpdirs(void);

/// @brief: Assigns a system configuration file.
///
/// STUB: currently only stores the path and derives the system name from the
/// file name. Parsing the file contents is left as a TODO.
bool system_set_file(system_st *this, const char *filepath);

/// @brief: Appends a device described by *filepath* to the system.
///
/// *nodeid* sets the device's CANopen node id; pass 0 to read it from the
/// configuration file's DATABASE instead. The DATABASE is also consulted for the
/// CAN revision number. Returns false if the system already holds
/// SYSTEM_DEV_MAX_COUNT devices.
bool system_add_device(system_st *this, const char *filepath, uint8_t nodeid);

/// @brief: Appends an empty (unconfigured) device, e.g. one created from the
/// UI's "Add device" tab. Returns a pointer to the new device, or NULL if the
/// system is already full.
device_st *system_add_empty_device(system_st *this);

/// @brief: Adds a device discovered on the CAN bus (e.g. by the --find command
/// or the UI's "Search devices" button). The device has no configuration file
/// yet (its path is left empty); only its node id, vendor id and product code
/// are known. Returns a pointer to the new device, or NULL if the system is
/// already full.
device_st *system_add_found_device(system_st *this, uint8_t nodeid,
		uint32_t vendor_id, uint32_t product_code);

/// @brief: Removes all devices from the system, leaving any loaded system-file
/// information intact.
void system_clear_devices(system_st *this);

/// @brief: Removes *device* from the system, shifting the remaining devices
/// down to keep the list contiguous. Returns true if the device was found and
/// removed. Used by the UI's per-device "Remove" button.
bool system_remove_device(system_st *this, device_st *device);

/// @brief: Assigns (or replaces) the configuration file of an already existing
/// device, deriving its name from the path. Used by the UI's file picker.
///
/// STUB: the file contents are not parsed yet, see system_set_file().
void system_set_device_file(device_st *device, const char *filepath);

/// @brief: Returns true once a system configuration (.uvsys) file has been
/// loaded. While true, individual device files must not be added, because the
/// system file already contains the system's devices.
static inline bool system_is_sysfile_loaded(system_st *this) {
	return this->loaded;
}

static inline uint8_t system_get_dev_count(system_st *this) {
	return this->dev_count;
}

/// @brief: Returns the device at *index*, or NULL if out of range.
static inline device_st *system_get_dev(system_st *this, uint8_t index) {
	return (index < this->dev_count) ? &this->devs[index] : NULL;
}

/// @brief: Returns true when no more devices can be added.
static inline bool system_is_full(system_st *this) {
	return this->dev_count >= SYSTEM_DEV_MAX_COUNT;
}

/// @brief: --system command callback. Sets the active system configuration file.
bool cmd_system(const char *arg);

/// @brief: --device command callback. Adds a device configuration file.
bool cmd_device(const char *arg);


/// @brief: Returns true when *path* ends (case-insensitively) with *suffix*.
/// Both NULL-safe (returns false when either is NULL).
bool path_ends_with(const char *path, const char *suffix);

/// @brief: True when *path* names a .uvdev device package.
static inline bool path_is_uvdev(const char *path) {
	return path_ends_with(path, ".uvdev");
}

/// @brief: True when *path* names a .uvsys system package.
static inline bool path_is_uvsys(const char *path) {
	return path_ends_with(path, ".uvsys");
}

/// @brief: Resolves the effective file argument for the optional-argument load
/// commands (--loadbin / --loadmedia / --loadparam). *stored* is the argument
/// attached to the option (an empty string when the option was given no =value).
/// When *stored* is empty this falls back to the first non-option token on the
/// command line, so the historical space-separated form (e.g. "--loadbin fw.bin")
/// keeps working. Returns NULL when no file was given at all, meaning the command
/// should operate on the devices already loaded with --dev / --sys.
///
/// Must be called once the scheduler is running (i.e. from a task), since the
/// non-option arguments are only known after the option parsing loop finishes.
const char *cmdline_load_arg(const char *stored);


#endif /* SYSTEM_H_ */
