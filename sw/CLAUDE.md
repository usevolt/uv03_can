# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**uvcan** is a Linux command-line tool for CANopen device management built by Usevolt Oy. It handles firmware loading, parameter read/write, CAN bus monitoring, SDO operations, and C code generation from device database files. Licensed under GPLv3.

## Build Commands

```bash
# Install dependencies (first time)
sudo apt-get install libncurses-dev pkg-config libmosquitto-dev

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
| parser | `src/parser.c` | Common JSON/YAML interface; every file uvcan reads or writes goes through it |
| db | `src/db.c` (largest, ~1700 LOC) | CANopen device database parsing, object dictionary management, up to 512 objects |
| loadparam | `src/loadparam.c` | Write parameters to devices via SDO, multi-device support with query/answer flow |
| saveparam | `src/saveparam.c` | Read parameters from devices and save to files |
| export | `src/export.c` | Generate C header/source files from device database |
| load | `src/load.c` | Firmware flashing via multiple bootloader protocols (standard, segmented, legacy UV) |
| listen | `src/listen.c` | CAN bus message monitoring |
| terminal | `src/terminal.c` | Interactive terminal via SDO reply protocol |
| sdo | `src/sdo.c` | Direct SDO read/write operations |
| loadmedia | `src/loadmedia.c` | Media file upload via UV media protocol |
| makeuvdev | `src/makeuvdev.c` | Assembles a .uvdev device package out of a build's artifacts |

### File formats: JSON and YAML

Every file uvcan reads or writes -- device databases (`--db`, including their
`content` includes), parameter files (`--loadparam` / `--saveparam`) and the
manifests inside `.uvdev` / `.uvsys` packages -- can be written in either JSON
or YAML. The format is chosen by the file's extension: `.yaml` and `.yml` are
parsed and written as YAML, everything else as JSON.

Modules never call `uv_json*` or `uv_yaml*` directly; they use the common
interface in `inc/parser.h`:

- `parser_read_file(path, &buffer)` reads a file, picks the format from the
  extension and returns the root `parser_node_st`. The caller `free()`s the buffer.
- `parser_find_child` / `_get_child` / `_get_type` / `_get_int` / `_get_string` /
  `_array_at` / `_array_get_size` ... mirror the `uv_jsonreader_*` API. A node is
  a small by-value struct; check it with `parser_node_is_valid()` instead of `!= NULL`.
- `parser_writer_*` mirrors `uv_jsonwriter_*`. `parser_write_file()` writes the
  result and pretty-prints JSON output with `jq`.
- `parser_find_file(dir, "uvdev", ...)` locates a package manifest written in
  any of the supported formats.

Both readers check the document's structure before handing the root out: a JSON
document has to be an object at the root with every brace and bracket closed and
nothing but whitespace after it. `uv_jsonreader` itself validates nothing, so a
truncated file would otherwise be "read" without complaint and then answer every
lookup with nonsense — a parameter file whose last brace is missing would load an
arbitrary subset of the parameters onto a device. A rejected document gives an
invalid node; `parser_last_error()` then tells why, as a sentence fit for an
error message.

Notes on the two formats: in both of them uvcan stores hexadecimal values as
quoted strings (`"MAININDEX": "0x2100"` / `MAININDEX: "0x2100"`) and the string
values are quoted as well; only the YAML keys are left unquoted. `parser_get_type`
reports a string which is a valid hexadecimal value as `PARSER_INT`, so a value
like `"0x2100"` reads back as an integer in both formats. (In JSON the reader
does the same for any quoted number.) A hand-written YAML file may also use
native unquoted hex (`MAININDEX: 0x2100`) — that is read as an integer too.

### Parameter file query value formats

`loadparam` files may declare a `QUERIES` array of interactive prompts; the chosen
answer then selects which value a parameter receives. A query is referenced inside
a value by its `NAME`, in one of two interchangeable forms (detected by value type):

- **Positional array** (legacy), indexed by the answer number — must stay aligned with `ANSWERS`:
  `"DATA": { "valve": [2000, 3500] }`
- **Answer-keyed object** (readable), keyed by the answer text — self-documenting and order-independent:
  `"DATA": { "valve": { "Danfoss": 2000, "Sauer": 3500 } }`

Prefer the keyed form for new files. If the chosen answer's key is missing from a
keyed object, that value is skipped with a warning. Both forms work anywhere a
query is referenced (`DATA`, `MAININDEX`, `TYPE`, `SUBINDEX`, `NODEID`, and
device-selecting queries).

### Creating .uvdev packages

`--makeuvdev <file.uvdev>` writes a device package out of a project's build
artifacts, i.e. it is what a project's `make publish` calls. The package is a
plain zip archive holding a `uvdev.json` manifest plus the files it names:

| Manifest key | Option | |
|---|---|---|
| `DATABASE` | `--db` | mandatory; the object dictionary file **and** every file it pulls in with a `"content"` reference |
| `FIRMWARE` | `--firmware` | mandatory |
| `LINUX_BIN` | `--linuxbin` | optional, the desktop simulator executable |
| `BOOTLOADER` | `--bootloader` | optional |
| `MEDIA` | `--media` | optional, a media file or a directory of them; can be given more than once |
| `VERSION` | `--fwversion` | optional, usually the `git describe` version the firmware was built with |

The `"content"` files are not searched for again: `db` records each of them as
it reads the database, so exactly the files the database was parsed from end up
in the package. They keep the paths they have relative to the project root (the
parent directory of the database's own directory, which is what the top level
`"content"` references are resolved against), so the references still resolve
when the database is read back from the package. A file outside that root
cannot be packaged and is reported as an error.

### Configuration

`inc/uv_hal_config.h` contains all HAL feature flags and the `PRINT()` macro (stderr output, suppressed with `--silent`). CAN defaults: channel `can0`, baudrate 250000.

### Conventions

- C11 with GCC on Linux
- Headers in `inc/`, sources in `src/`
- Module pattern: each module has a `_st` struct in its header, instance stored in `dev`, init/step functions
- `PRINT(...)` macro for user-facing output (respects silent mode)
- No test framework; manual testing via CLI
