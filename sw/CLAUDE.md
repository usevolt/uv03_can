# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**uvcan** is a Linux command-line tool for CANopen device management built by Usevolt Oy. It handles firmware loading, parameter read/write, CAN bus monitoring, SDO operations, and C code generation from device database files. Licensed under GPLv3.

## Build Commands

```bash
# Install dependencies (first time)
sudo apt-get install pkg-config libncurses-dev libglfw3-dev libglew-dev \
    libgl-dev libfreetype-dev libreadline-dev libmosquitto-dev

# Build
make

# Clean and rebuild
make clean && make

# Build both shippable packages (Windows .zip + Linux tarball) plus the
# self-update binary and manifest, all into ../prod
make package

# Push what ../prod holds to the file server's public shelf
make publish
```

The binary is output as `./uvcan`. Build artifacts go to `release/`. Version is derived from git tags/commits automatically.

`./install.sh` does the same from scratch: it installs those dependencies (adding
the -dev packages only when the run actually builds), builds uvcan if no binary
is there yet, and installs it into `~/.local` along with the `.uvsys` / `.uvdev`
desktop integration (MIME types, file icons and the handler entry). `--system`
installs machine-wide, `--build` forces a rebuild and `--no-deps` skips the apt
step.

## Versions and self-update

uvcan's version is `<build id>-g<hash>` (`300-g1905`), the same shape the
firmware projects use; a build from a modified tree gets a `-dirty` suffix.
`--version` prints it together with the bare build id, which is
`__UV_PROGRAM_VERSION`, i.e. `git rev-list --count HEAD`. That id is what is
compared when looking for an update, because it grows with every commit for the
life of the project, tag or no tag. Tags are not part of the version: a
`git describe` name counts commits since the nearest tag and restarts at every
release, so it cannot be compared at all.

uvcan is published on the file server's **public** shelf,
`https://files.usevolt.fi/pub/uvcan/` — served to anyone with no credentials,
because uvcan is free software and a freshly downloaded one has no account to
log in with. `pub` is a reserved fleet name: the Caddyfile answers `/pub/`
itself, ahead of the generated per-fleet blocks, and `uvfleetctl` refuses to
grant it to an account (see the uv3b_iotbrkr checkout). Nothing secret may go
there.

- `--checkupdate` reads `latest.json` from that path and reports what it finds.
  The UI makes the same check once in the background when it opens, when the
  Settings tab's "Check updates on start up" is ticked (stored in
  `account.conf` as `check_updates`, on by default). A newer build is logged in
  yellow and offered in a dialog; accepting it installs it on a task of its own.
  A failed check is silent.
- `--update` downloads the published binary, verifies it against the size and
  SHA-256 in the manifest, and `rename()`s it over the running one — a running
  executable cannot be written to (`ETXTBSY`) but can be renamed away from. The
  binary it replaces is kept as `uvcan.old`. The running process keeps running
  from the file it started with, so it has to be restarted.
- There is no signature on the manifest. The checksum and the binary come from
  the same origin, so it guards against a corrupt download and not against a
  compromised server.

`make package` builds the packages, copies the binary under its published name
and writes `latest.json`; `make publish` uploads them with `uvupload`. A
`-dirty` build is refused: what is published has to be a commit anybody can
check out again.

A machine that has no uvcan yet installs one with:

```bash
curl -fsSL https://files.usevolt.fi/pub/uvcan/get-uvcan.sh | sh
```

`packaging/get-uvcan.sh` reads `latest.json`, downloads the Linux package it
names, and runs the `install.sh` inside it. Arguments reach install.sh, so
`| sh -s -- --system` installs machine-wide. This is why the manifest carries
`package` and `package_sha256` as well as the bare binary the updater uses: the
shelf keeps every release, and the newest by filename is not the newest by build
number.

`src/http.c` holds the curl plumbing shared by `remotefiles.c` (the per-fleet,
authenticated file panel) and `selfupdate.c` (the public, unauthenticated
updater). Every request is made through a curl **config file** rather than a
command line, so nothing user- or server-supplied reaches a shell and no
credentials appear in the process arguments. Redirects are never followed.

## Architecture

### HAL Submodule

`hal/` is a git submodule containing the Usevolt Hardware Abstraction Layer (`uv_hal`). It provides the CANopen stack, FreeRTOS (POSIX port), JSON parsing, CAN interface, and memory management. The HAL is shared across embedded targets (LPC1549, LPC4078) and host platforms (Linux, Windows). **Do not modify HAL code for uvcan-specific changes.**

### Application Structure

The application uses a global `struct _dev_st dev` instance (defined via `CONFIG_APP_ST` in `uv_hal_config.h`). Modules access it through `#define this (&dev)`. FreeRTOS runs on a POSIX port with a 50 MB heap.

**Command system** (`src/commands.c`): GNU getopt-based. Each command is a `commands_st` struct with long/short options and a callback. Commands are processed sequentially from the command line. Commands that need ongoing execution register tasks via `add_task()`.

**Task system** (`src/main.c`): Up to 5 concurrent tasks with mutex-based round-robin execution. Commands register step callbacks that run each cycle.

**Help** (`src/help.c`): `--help` lists every command; given the name of one
(`--help loadparam`, `--help=loadparam`, `-h loadparam`, `--help --loadparam`,
case-insensitive, short names accepted) it prints only that command's entry.
getopt does not attach a space-separated value to an optional-argument option,
so the name is taken from the next unconsumed token -- but only when it really
names a command, which is what keeps `--loadbin fw.bin --help` printing the full
listing instead of erroring on the file name. The listing is marked up: the
option names are printed bold, and so are the `*name*` / `**name**` command
references the descriptions are written with (the asterisks themselves are
dropped). Write new descriptions with those markers.

### Node id selection

Every command which talks to a device gets its node id from one of three places,
in this order: the `<file>:<nodeid>` postfix of the argument, a `--nodeid` /
`--forcenodeid` option, or the device's own node id (from its `.uvdev` package or
the `.uvsys` system file).

- `--dev`, `--loadbin` (and the whole `loadbin` family), `--loadmedia` and
  `--loadparam` all take the postfix, split by the shared
  `cmdline_parse_nodeid_arg()`. For the load commands
  `cmdline_load_arg_nodeid()` resolves the argument and its postfix together, in
  both the attached and the space-separated form, for raw files as well as for
  `.uvdev` / `.uvsys` packages. `--loadparam params.json:0xd` means the same as
  `--forcenodeid 0xd --loadparam params.json`; an argument holding nothing but
  the postfix (`--loadbin :0x22`) selects the node and operates on the devices
  already loaded with `--dev` / `--sys`.
- The selection lives in `system_st` (`forced_nodeid_set` / `forced_nodeid` /
  `forcenodeid`) and is captured into `system_nodeids_st` by
  `system_nodeids_save()`. The command callbacks all run while the command line
  is parsed but the work runs later from their tasks, so each load command saves
  the selection in effect at its own place on the command line and restores it
  for the duration of its dispatch. That is what makes the node ids positional.
- `--nodeid` only selects the node to talk to. `--forcenodeid` in addition
  overrides the node id a device package or system file carries:
  `system_apply_forced_nodeid()` writes it into the devices which `loadbin`,
  `loadmedia` and `loadparam` are about to operate on. A single node id cannot
  name one of several devices, so an operation spanning more than one device
  reports that the forced node id is ignored and drops the selection rather than
  writing every device to that one node.
- `--forcenodeid` also lets a raw `--loadparam` file reprogram the device's node
  id from the file's `NODEID` (after a confirmation prompt). A device's *bundled*
  parameters never do: `load_device_db()` targets the device's own node id and
  clears the flag.
- The selection stops at the command line. `ui_task()` calls
  `system_clear_forced_nodeid()` before the UI opens, so a device the user picks
  there is always addressed by its own node id.

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
| | `--mediadir` | optional, the name the media directory gets in the package (default `media`); the device stores its media under package-relative names, so this is the prefix the firmware asks its media by (e.g. `media_hd` for a high-resolution build) |
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
