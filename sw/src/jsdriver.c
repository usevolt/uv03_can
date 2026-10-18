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


// The virtual game controller of the operating system, behind the jsdriver_*
// functions of joystick.h: a uinput device on Linux, a vJoy device on Windows.


#include "joystick.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>


#if CONFIG_TARGET_WEB

// a web page cannot create a game controller

bool jsdriver_open(const char *name, char *err, size_t err_len) {
	(void) name;
	snprintf(err, err_len, "not available in a web browser");
	return false;
}

const char *jsdriver_name(void) {
	return "none";
}

void jsdriver_set_axis(uint8_t axis, int32_t value) {
	(void) axis;
	(void) value;
}

void jsdriver_set_button(uint8_t index, bool pressed) {
	(void) index;
	(void) pressed;
}

void jsdriver_sync(void) {
}

void jsdriver_close(void) {
}


#elif CONFIG_TARGET_LINUX

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/uinput.h>


#define UINPUT_PATH		"/dev/uinput"

// The axes in the order of JOYSTICK_AXIS_NAMES
static const uint16_t axis_codes[JOYSTICK_AXIS_COUNT] = {
		ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ, ABS_THROTTLE, ABS_RUDDER
};

static int fd = -1;


/// @brief: The key code of button *index*: the 16 joystick buttons
/// BTN_TRIGGER ... BTN_DEAD first, then BTN_TRIGGER_HAPPY1 onwards, which is
/// how the kernel's joydev numbers them too
static uint16_t button_code(uint8_t index) {
	return (index < 16) ? (uint16_t) (BTN_TRIGGER + index) :
			(uint16_t) (BTN_TRIGGER_HAPPY1 + index - 16);
}


static void emit(uint16_t type, uint16_t code, int32_t value) {
	if (fd >= 0) {
		struct input_event ev;
		memset(&ev, 0, sizeof(ev));
		ev.type = type;
		ev.code = code;
		ev.value = value;
		if (write(fd, &ev, sizeof(ev)) != sizeof(ev)) {
			// the event queue is full; the next change sends the state again
		}
	}
}


bool jsdriver_open(const char *name, char *err, size_t err_len) {
	bool ret = false;
	fd = open(UINPUT_PATH, O_WRONLY | O_NONBLOCK);
	if (fd < 0) {
		int e = errno;
		if ((e == EACCES) || (e == EPERM)) {
			snprintf(err, err_len, "no permission to open " UINPUT_PATH ". "
					"Run uvcan's install.sh, which installs the udev rule that "
					"gives it to the user logged in.");
		}
		else {
			snprintf(err, err_len, "cannot open " UINPUT_PATH ": %s "
					"(is the uinput kernel module loaded?)", strerror(e));
		}
	}
	else {
		bool ok = (ioctl(fd, UI_SET_EVBIT, EV_KEY) == 0) &&
				(ioctl(fd, UI_SET_EVBIT, EV_ABS) == 0) &&
				(ioctl(fd, UI_SET_EVBIT, EV_SYN) == 0);
		for (uint8_t i = 0; ok && (i < JOYSTICK_BUTTON_COUNT); i++) {
			ok = (ioctl(fd, UI_SET_KEYBIT, button_code(i)) == 0);
		}
		for (uint8_t i = 0; ok && (i < JOYSTICK_AXIS_COUNT); i++) {
			struct uinput_abs_setup abs;
			memset(&abs, 0, sizeof(abs));
			abs.code = axis_codes[i];
			abs.absinfo.minimum = -JOYSTICK_AXIS_MAX;
			abs.absinfo.maximum = JOYSTICK_AXIS_MAX;
			ok = (ioctl(fd, UI_SET_ABSBIT, axis_codes[i]) == 0) &&
					(ioctl(fd, UI_ABS_SETUP, &abs) == 0);
		}
		if (ok) {
			struct uinput_setup setup;
			memset(&setup, 0, sizeof(setup));
			setup.id.bustype = BUS_VIRTUAL;
			setup.id.version = 1;
			snprintf(setup.name, sizeof(setup.name), "%s", name);
			ok = (ioctl(fd, UI_DEV_SETUP, &setup) == 0) &&
					(ioctl(fd, UI_DEV_CREATE) == 0);
		}
		if (!ok) {
			snprintf(err, err_len, "creating the uinput device failed: %s",
					strerror(errno));
			close(fd);
			fd = -1;
		}
		else {
			ret = true;
		}
	}
	return ret;
}


const char *jsdriver_name(void) {
	return "uinput";
}


void jsdriver_set_axis(uint8_t axis, int32_t value) {
	if (axis < JOYSTICK_AXIS_COUNT) {
		emit(EV_ABS, axis_codes[axis], value);
	}
}


void jsdriver_set_button(uint8_t index, bool pressed) {
	if (index < JOYSTICK_BUTTON_COUNT) {
		emit(EV_KEY, button_code(index), pressed ? 1 : 0);
	}
}


void jsdriver_sync(void) {
	emit(EV_SYN, SYN_REPORT, 0);
}


void jsdriver_close(void) {
	if (fd >= 0) {
		ioctl(fd, UI_DEV_DESTROY);
		close(fd);
		fd = -1;
	}
}


#elif CONFIG_TARGET_WIN

// vJoy (https://github.com/BrunnerInnovation/vJoy, or the original
// jshafer817/vJoy) is a virtual joystick driver for Windows. Its
// vJoyInterface.dll is loaded at run time, so uvcan neither needs its SDK to
// build nor fails to start where it is not installed.

#include <windows.h>


// the vJoy device uvcan drives; the vJoy configuration tool creates device 1
#define VJOY_ID				1
// HID usages of the axes, in the order of JOYSTICK_AXIS_NAMES: X Y Z RX RY RZ
// and the two sliders
static const UINT axis_usages[JOYSTICK_AXIS_COUNT] = {
		0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37
};
// GetVJDStatus()
#define VJD_STAT_OWN		0
#define VJD_STAT_FREE		1

typedef BOOL (__cdecl *vjoy_enabled_t)(void);
typedef int (__cdecl *vjoy_status_t)(UINT);
typedef BOOL (__cdecl *vjoy_acquire_t)(UINT);
typedef void (__cdecl *vjoy_relinquish_t)(UINT);
typedef BOOL (__cdecl *vjoy_reset_t)(UINT);
typedef BOOL (__cdecl *vjoy_axis_t)(LONG, UINT, UINT);
typedef BOOL (__cdecl *vjoy_btn_t)(BOOL, UINT, UCHAR);

static HMODULE dll;
static vjoy_relinquish_t vjoy_relinquish;
static vjoy_axis_t vjoy_axis;
static vjoy_btn_t vjoy_btn;
static bool acquired;


bool jsdriver_open(const char *name, char *err, size_t err_len) {
	(void) name;
	bool ret = false;
	if (dll == NULL) {
		dll = LoadLibraryA("vJoyInterface.dll");
	}
	if (dll == NULL) {
		dll = LoadLibraryA("C:\\Program Files\\vJoy\\x64\\vJoyInterface.dll");
	}
	if (dll == NULL) {
		snprintf(err, err_len, "the vJoy driver is not installed "
				"(vJoyInterface.dll was not found). Install vJoy and configure "
				"its device 1.");
	}
	else {
		vjoy_enabled_t enabled = (vjoy_enabled_t) GetProcAddress(dll, "vJoyEnabled");
		vjoy_status_t status = (vjoy_status_t) GetProcAddress(dll, "GetVJDStatus");
		vjoy_acquire_t acquire = (vjoy_acquire_t) GetProcAddress(dll, "AcquireVJD");
		vjoy_reset_t reset = (vjoy_reset_t) GetProcAddress(dll, "ResetVJD");
		vjoy_relinquish = (vjoy_relinquish_t) GetProcAddress(dll, "RelinquishVJD");
		vjoy_axis = (vjoy_axis_t) GetProcAddress(dll, "SetAxis");
		vjoy_btn = (vjoy_btn_t) GetProcAddress(dll, "SetBtn");
		if ((enabled == NULL) || (status == NULL) || (acquire == NULL) ||
				(vjoy_relinquish == NULL) || (vjoy_axis == NULL) ||
				(vjoy_btn == NULL)) {
			snprintf(err, err_len, "vJoyInterface.dll is not a vJoy interface "
					"library this uvcan knows");
		}
		else if (!enabled()) {
			snprintf(err, err_len, "the vJoy driver is not enabled");
		}
		else {
			int st = status(VJOY_ID);
			if ((st != VJD_STAT_OWN) && (st != VJD_STAT_FREE)) {
				snprintf(err, err_len, "vJoy device %d is missing or used by "
						"another program", VJOY_ID);
			}
			else if (!acquire(VJOY_ID)) {
				snprintf(err, err_len, "vJoy device %d could not be acquired",
						VJOY_ID);
			}
			else {
				if (reset != NULL) {
					reset(VJOY_ID);
				}
				acquired = true;
				ret = true;
			}
		}
	}
	return ret;
}


const char *jsdriver_name(void) {
	return "vJoy";
}


void jsdriver_set_axis(uint8_t axis, int32_t value) {
	if (acquired && (axis < JOYSTICK_AXIS_COUNT)) {
		// vJoy's axes run from 0 to 0x8000
		LONG v = (LONG) (((int64_t) value + JOYSTICK_AXIS_MAX) * 0x8000 /
				(2 * JOYSTICK_AXIS_MAX));
		vjoy_axis(v, VJOY_ID, axis_usages[axis]);
	}
}


void jsdriver_set_button(uint8_t index, bool pressed) {
	if (acquired && (index < JOYSTICK_BUTTON_COUNT)) {
		vjoy_btn(pressed ? TRUE : FALSE, VJOY_ID, (UCHAR) (index + 1));
	}
}


void jsdriver_sync(void) {
	// vJoy updates the device on every call
}


void jsdriver_close(void) {
	if (acquired) {
		vjoy_relinquish(VJOY_ID);
		acquired = false;
	}
}


#endif
