#############################################
#
#	USEVOLT MAKEFILE
# Builds a project adding a git hash to preprocessor symbols
#
# * Project name is taken from the name of the git repository
#
# Build targets:
#   make            # same as 'make linux'
#   make linux      # native Linux build, produces ./uvcan
#   make linux-package # build linux + tar the install package into ../prod
#   make win        # Windows cross-compile (x86_64 mingw-w64), produces ./uvcan.exe
#   make windows    # alias for 'make win'
#   make win-package # build win + zip a double-clickable package under release/
#   make publish    # build linux tarball + windows zip into ../prod
#   make clean      # remove build artifacts of both targets
#
##############################################



### APP CONFIG ###


# Target operating system, selected by the build target below. Either "linux"
# (default) or "win". Can also be overridden manually: make OS=win image
OS ?= linux


#Sepcify the optimizations used. Os == optimize for size, Og = optimize for debug, O3, O2, O1
O := Og

# Name of the directory where build files will be saved
BUILDDIR := release

# Directory for build-time generated sources (e.g. images embedded into the
# binary as C arrays). Lives under BUILDDIR so 'make clean' removes it.
GENERATEDDIR := $(BUILDDIR)/generated
# Files compiled into the binary as C byte arrays (via xxd -i) and #included by
# the code that uses them: the UI's PNG images and the MQTT broker's CA
# certificate.
EMBEDDED_MEDIA := $(GENERATEDDIR)/media_minus_hd.h \
				$(GENERATEDDIR)/media_usevolt_bg.h \
				$(GENERATEDDIR)/cert_uvca.h


### Platform specific configuration ###############################
ifeq ($(OS),win)

# Windows is cross-compiled from Linux with the mingw-w64 toolchain.
CC := x86_64-w64-mingw32-gcc
# Additional CPU define symbol, used for FreeRTOS compile
CORE := CORE_WIN
TARGET := CONFIG_TARGET_WIN
# Name of the produced binary firmware
BINARY := uvcan.exe

# HAL platform sources (PCANBasic CAN backend, Win32 memory/rtos/gpio)
PLATFORM_SRCDIR := hal/uv_hal/src_win
# FreeRTOS native Windows (Win32) port. The Posix port cannot be used on
# native Windows because it implements the scheduler with POSIX signals.
PLATFORM_PORTDIR := hal/uv_hal/freertos/portable/MSVC-MingW
PLATFORM_PORTSRCDIRS := $(PLATFORM_PORTDIR)
PLATFORM_INCLUDEDIRS := -I"$(PLATFORM_PORTDIR)"
# CFLAGS: no ncurses, no pthreads (FreeRTOS uses native Win32 threads).
# UI: OpenGL backend using the mingw-built glfw3/freetype + GLEW (compiled in).
# The third-party libs/headers are built by thirdparty_win/build.sh.
PLATFORM_CFLAGS := -I"thirdparty_win/include" -DCONFIG_UI_OPENGL=1 -DGLEW_STATIC
# Link against the PCAN-Basic driver DLL, the Windows multimedia timer (winmm,
# used by the FreeRTOS Win32 port for its tick) and the OpenGL UI stack.
PLATFORM_LIBS := $(PLATFORM_SRCDIR)/PCANBasic.dll -lwinmm \
				-L"thirdparty_win/lib" -lglfw3 -lfreetype -lopengl32 -lgdi32 -lcomdlg32
# MQTT (the Fleet tab) needs a mingw-built libmosquitto plus its TLS backend
# (OpenSSL) staged into thirdparty_win/{include,lib} - the apt mingw toolchain
# ships neither. Until they are staged the Fleet tab's broker connection is
# stubbed out (src/mqtt.c). Once the libs are in place, opt in with
#   make win MOSQUITTO=1
# which defines MOSQUITTO_WIN (compiling in the real client) and links it. The
# extra Windows system libs are what static OpenSSL + winsock pull in; drop
# -lssl -lcrypto and add the OpenSSL DLLs beside uvcan.exe instead if linking a
# DLL-import libmosquitto (see the Windows MQTT notes).
ifeq ($(MOSQUITTO),1)
PLATFORM_CFLAGS += -DMOSQUITTO_WIN=1
PLATFORM_LIBS += -lmosquitto -lssl -lcrypto -lws2_32 -lcrypt32 -lbcrypt
endif
# The OpenGL UI backend + PNG decoder live in src_linux (they are portable,
# pure GLFW/GLEW/FreeType); GLEW is compiled straight from its amalgamated
# source. These are added individually so the rest of src_linux is excluded.
PLATFORM_EXTRA_SOURCES := hal/uv_hal/src_linux/uv_ui_opengl.c \
				hal/uv_hal/src_linux/lodepng.c \
				thirdparty_win/glew.c

else

# Builds on this hardware (native Linux).
CC := gcc
CORE := CORE_LINUX
TARGET := CONFIG_TARGET_LINUX
BINARY := uvcan

PLATFORM_SRCDIR := hal/uv_hal/src_linux
PLATFORM_PORTDIR := hal/uv_hal/freertos/portable/ThirdParty/GCC/Posix
PLATFORM_PORTSRCDIRS := $(PLATFORM_PORTDIR) $(PLATFORM_PORTDIR)/utils
PLATFORM_INCLUDEDIRS := -I"$(PLATFORM_PORTDIR)" -I"$(PLATFORM_PORTDIR)/utils"
# libmosquitto is the MQTT client used by the Fleet tab (src/mqtt.c). It has no
# mingw build here, so the Fleet tab's broker connection is Linux-only.
PLATFORM_CFLAGS := $(shell pkg-config --cflags ncurses glfw3 glew gl freetype2 libmosquitto) -pthread -DCONFIG_UI_OPENGL=1
PLATFORM_LIBS := -pthread -lncurses -lreadline $(shell pkg-config --libs glfw3 glew gl freetype2 libmosquitto) -lm
# On Linux the UI backend + PNG decoder are compiled as part of src_linux.
PLATFORM_EXTRA_SOURCES :=

endif
###################################################################


# List of include directories
INCLUDEDIRS := 	-I"inc/" \
				-I"inc/ui" \
				-I"$(GENERATEDDIR)" \
				-I"uw_can/" \
				-I"hal/uv_hal/inc" \
				-I"hal/uv_hal/inc/ui" \
				-I"hal/uv_hal/inc/canopen" \
				-I"hal/uv_hal/freertos/include" \
				$(PLATFORM_INCLUDEDIRS)


#Source directories
SOURCEDIRS := src \
				src/ui \
				hal/uv_hal/src \
				hal/uv_hal/src/ui \
				hal/uv_hal/src/canopen \
				hal/uv_hal/freertos \
				$(PLATFORM_PORTSRCDIRS)

SOURCEDIRS := $(SOURCEDIRS) \
				$(PLATFORM_SRCDIR)



### END OF APP CONFIG ###





SOURCES := $(shell find $(SOURCEDIRS) -maxdepth 1 -name '*.c') $(PLATFORM_EXTRA_SOURCES)
GIT_VERSION := $(shell git describe --abbrev=4 --dirty --always --tags)
GIT_REV_ID := $(shell git rev-list --count HEAD)
APP_NAME := $(shell basename `git rev-parse --show-toplevel`)
FULL_NAME := $(APP_NAME)_$(GIT_VERSION)

# Version used for the names of the shippable packages only; the strings
# compiled into the binary keep $(GIT_VERSION) above. git describe counts the
# commits since the nearest tag, which restarts at zero on every new tag, so
# package names built from it do not increase over the project's life. The
# rev-list count does: it is the number of commits in HEAD's history, and it
# grows with every commit, tag or no tag. The abbreviated hash keeps the name
# pointing at one exact commit (the count alone would not distinguish branches),
# and -dirty marks a build made from a modified tree, as git describe does.
GIT_HASH := $(shell git rev-parse --short=4 HEAD)
GIT_DIRTY := $(shell git update-index -q --refresh >/dev/null 2>&1; \
	git diff-index --quiet HEAD -- 2>/dev/null || echo -dirty)
PKG_FULL_NAME := $(APP_NAME)_$(GIT_REV_ID)-g$(GIT_HASH)$(GIT_DIRTY)



CFLAGS := $(PLATFORM_CFLAGS) -D$(TARGET)=1 -D__UV_PROJECT_NAME=$(FULL_NAME) -D__UV_PROGRAM_VERSION=$(GIT_REV_ID) $(INCLUDEDIRS) -D$(CORE) -$(O) -g3 -Wall -fmessage-length=0 -fno-builtin -ffunction-sections -fdata-sections

LDFLAGS := -D__UV_APP_VERSION=\"$(GIT_VERSION)\"



# Get list of object files, with paths
OBJECTS := $(addprefix $(BUILDDIR)/,$(SOURCES:%.c=%.o))
# List of object dependencies
DEPS := $(OBJECTS:.o=.d)



### Build targets #################################################

# Name of the double-clickable Windows package (folder + .zip).
PKGNAME := $(PKG_FULL_NAME)-win
PKGDIR  := $(BUILDDIR)/$(PKGNAME)

# Production output directory (sibling of sw/, at the repo root) collecting the
# shippable artifacts built by 'make publish': the Linux install tarball (binary
# + install.sh + packaging/) and the double-clickable Windows package (.zip).
PRODDIR     := ../prod
PUBLISH_TAR := $(PRODDIR)/$(PKG_FULL_NAME)-linux.tar.gz

.PHONY: linux linux-package linux-package-stage win windows win-package win-package-stage image post-build clean install uninstall publish

# Bare 'make' builds the Linux target.
linux:
	@$(MAKE) --no-print-directory OS=linux image

# Per-user desktop install: builds (if needed) and registers the binary, the
# .uvsys and .uvdev file associations, icons and launcher. Use 'sudo make install ARGS=--system'
# for a machine-wide install. See install.sh --help for all options.
install: $(BINARY)
	@./install.sh $(ARGS)

uninstall:
	@./install.sh --uninstall $(ARGS)

# Builds the Linux target and packs the install tarball (the binary, install.sh
# and packaging/) into $(PRODDIR). This is the whole of what a Linux user needs:
# unpack it anywhere and run ./install.sh, which installs the runtime
# dependencies and registers the .uvsys / .uvdev associations. Needs no mingw toolchain,
# unlike 'publish', which builds the Windows package as well.
linux-package:
	@$(MAKE) --no-print-directory OS=linux image
	@$(MAKE) --no-print-directory linux-package-stage

linux-package-stage:
	@mkdir -p "$(PRODDIR)"
	@rm -f "$(PUBLISH_TAR)"
	tar czf "$(PUBLISH_TAR)" uvcan install.sh packaging
	@echo "Created $(PUBLISH_TAR)"

win windows:
	@$(MAKE) --no-print-directory OS=win image

# Builds the Windows target and assembles a double-clickable, zipped package
# (uvcan.exe + PCANBasic.dll + fonts + launcher + README) under release/.
win-package:
	@$(MAKE) --no-print-directory OS=win image
	@$(MAKE) --no-print-directory OS=win win-package-stage

win-package-stage:
	@echo ""
	@echo "Packaging $(PKGNAME)"
	@rm -rf "$(PKGDIR)" "$(PKGDIR).zip"
	@mkdir -p "$(PKGDIR)/fonts"
	cp "$(BINARY)" "$(PKGDIR)/"
	cp "$(PLATFORM_SRCDIR)/PCANBasic.dll" "$(PKGDIR)/"
	cp fonts/LiberationSans-Regular.ttf "$(PKGDIR)/fonts/"
	cp fonts/LiberationMono-Regular.ttf "$(PKGDIR)/fonts/"
	cp packaging/uvcan-ui.bat "$(PKGDIR)/"
	cp packaging/uvcan-open.bat "$(PKGDIR)/"
	cp packaging/uvsys.ico "$(PKGDIR)/"
	cp packaging/uvdev.ico "$(PKGDIR)/"
	cp packaging/install-association.bat "$(PKGDIR)/"
	cp packaging/uninstall-association.bat "$(PKGDIR)/"
	cp packaging/README.txt "$(PKGDIR)/"
	cd "$(BUILDDIR)" && zip -r -q "$(PKGNAME).zip" "$(PKGNAME)"
	@echo "Created $(PKGDIR).zip"

# Builds both shippable artifacts and drops them in $(PRODDIR):
#   * Windows package: the double-clickable .zip produced by win-package
#   * Linux install tarball: uvcan + install.sh + packaging/ (unpack on the
#     target Ubuntu box and run ./install.sh — see install.sh --help)
# The linux and win builds share $(BUILDDIR) for their object files, so the two
# OS targets cannot coexist there — we 'clean' between them (and stash the win
# zip in prod before the second clean removes it). PKGNAME/PKGDIR/PKG_FULL_NAME are
# OS-independent, so the paths resolve the same in either sub-make.
publish:
	@mkdir -p "$(PRODDIR)"
	@$(MAKE) --no-print-directory clean
	@$(MAKE) --no-print-directory OS=win win-package
	@cp "$(PKGDIR).zip" "$(PRODDIR)/"
	@$(MAKE) --no-print-directory clean
	@echo ""
	@echo "Publishing to $(PRODDIR)"
	@$(MAKE) --no-print-directory linux-package
	@echo "Created:"
	@echo "    $(PUBLISH_TAR)"
	@echo "    $(PRODDIR)/$(PKGNAME).zip"

# Internal target that performs the actual build for the selected OS.
image: $(BINARY)
	@$(MAKE) --no-print-directory OS=$(OS) post-build


# Turn a PNG into a C header holding its bytes as a byte array (minus_hd_png /
# minus_hd_png_len). xxd derives the array name from its input; running it from
# the media/ dir with the bare filename yields exactly "minus_hd_png", which
# also keeps this working on older xxd versions that lack the -n flag.
$(GENERATEDDIR)/media_minus_hd.h: media/minus_hd.png
	@mkdir -p $(@D)
	cd $(<D) && xxd -i $(<F) > $(abspath $@)

# The Usevolt logo, embedded the same way (usevolt_bg_png / usevolt_bg_png_len).
# Drawn as a faint background watermark by the UI (see uvui.c).
$(GENERATEDDIR)/media_usevolt_bg.h: media/usevolt_bg.png
	@mkdir -p $(@D)
	cd $(<D) && xxd -i $(<F) > $(abspath $@)

# The MQTT broker's private CA certificate, embedded the same way (uvca_crt /
# uvca_crt_len). libmosquitto wants a CA *file*, so mqtt.c writes these bytes out
# next to the account file on first connect - see mqtt.c.
$(GENERATEDDIR)/cert_uvca.h: certs/uvca.crt
	@mkdir -p $(@D)
	cd $(<D) && xxd -i $(<F) > $(abspath $@)

# Every object is built only after the embedded media headers exist (order-only:
# regenerating the image doesn't force a full rebuild, only a missing header does).
$(OBJECTS): | $(EMBEDDED_MEDIA)

$(BUILDDIR)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -I$(HEADERDIR) -I$(dir $<) -MMD -MT $@ -c $< -o $@


$(BINARY): $(OBJECTS)
	@echo ""
	@echo "Linking target"
	@echo ""
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJECTS) -o "$(BINARY)" $(PLATFORM_LIBS) $(LIBS)


post-build:
	-@echo ""
	-@echo 'Performing post-build steps'
	-@echo ""

clean:
	@rm -rf $(BUILDDIR)
	@rm -rf uvcan uvcan.exe


-include $(DEPS)
