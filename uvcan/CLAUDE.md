# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**uvcan** is a Linux command-line tool for CANopen device management built by Usevolt Oy. It handles firmware loading, parameter read/write, CAN bus monitoring, SDO operations, and C code generation from device database files. Licensed under GPLv3.

## Build Commands

```bash
# Install dependencies (first time)
sudo apt-get install libncurses-dev pkg-config

# Build
make

# Clean and rebuild
make clean && make
```

The binary is output as `./uvcan`. Build artifacts go to `release/`. Version is derived from git tags/commits automatically.

## Architecture

### HAL Submodule

`hal/` is a git submodule containing the Usevolt Hardware Abstraction Layer (`uv_hal`). It provides the CANopen stack, FreeRTOS (POSIX port), JSON parsing, CAN interface, and memory management. The HAL is shared across embedded targets (LPC1549, LPC4078) and host platforms (Linux, Windows). **Do not modify HAL code for uvcan-specific changes.**

### Application Structure

The application uses a global `struct _dev_st dev` instance (defined via `CONFIG_APP_ST` in `uv_hal_config.h`). Modules access it through `#define this (&dev)`. FreeRTOS runs on a POSIX port with a 50 MB heap.

**Command system** (`src/commands.c`): GNU getopt-based. Each command is a `commands_st` struct with long/short options and a callback. Commands are processed sequentially from the command line. Commands that need ongoing execution register tasks via `add_task()`.

**Task system** (`src/main.c`): Up to 5 concurrent tasks with mutex-based round-robin execution. Commands register step callbacks that run each cycle.

### Key Modules

| Module | File | Purpose |
|--------|------|---------|
| db | `src/db.c` (largest, ~1700 LOC) | CANopen device database parsing from JSON, object dictionary management, up to 512 objects |
| loadparam | `src/loadparam.c` | Write parameters to devices via SDO, multi-device support with query/answer flow |
| saveparam | `src/saveparam.c` | Read parameters from devices and save to files |
| export | `src/export.c` | Generate C header/source files from device database |
| load | `src/load.c` | Firmware flashing via multiple bootloader protocols (standard, segmented, legacy UV) |
| listen | `src/listen.c` | CAN bus message monitoring |
| terminal | `src/terminal.c` | Interactive terminal via SDO reply protocol |
| sdo | `src/sdo.c` | Direct SDO read/write operations |
| loadmedia | `src/loadmedia.c` | Media file upload via UV media protocol |

### Configuration

`inc/uv_hal_config.h` contains all HAL feature flags and the `PRINT()` macro (stderr output, suppressed with `--silent`). CAN defaults: channel `can0`, baudrate 250000.

### Conventions

- C11 with GCC on Linux
- Headers in `inc/`, sources in `src/`
- Module pattern: each module has a `_st` struct in its header, instance stored in `dev`, init/step functions
- `PRINT(...)` macro for user-facing output (respects silent mode)
- No test framework; manual testing via CLI
