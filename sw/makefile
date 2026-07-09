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
# PNG images compiled into the binary. Each is turned into a C header holding a
# byte array (via xxd -i) and #included by the UI code that draws it.
EMBEDDED_MEDIA := $(GENERATEDDIR)/media_minus_hd.h


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
PLATFORM_CFLAGS := $(shell pkg-config --cflags ncurses glfw3 glew gl freetype2) -pthread -DCONFIG_UI_OPENGL=1
PLATFORM_LIBS := -pthread -lncurses -lreadline $(shell pkg-config --libs glfw3 glew gl freetype2) -lm
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



CFLAGS := $(PLATFORM_CFLAGS) -D$(TARGET)=1 -D__UV_PROJECT_NAME=$(FULL_NAME) -D__UV_PROGRAM_VERSION=$(GIT_REV_ID) $(INCLUDEDIRS) -D$(CORE) -$(O) -g3 -Wall -fmessage-length=0 -fno-builtin -ffunction-sections -fdata-sections

LDFLAGS := -D__UV_APP_VERSION=\"$(GIT_VERSION)\"



# Get list of object files, with paths
OBJECTS := $(addprefix $(BUILDDIR)/,$(SOURCES:%.c=%.o))
# List of object dependencies
DEPS := $(OBJECTS:.o=.d)



### Build targets #################################################

# Name of the double-clickable Windows package (folder + .zip).
PKGNAME := $(FULL_NAME)-win
PKGDIR  := $(BUILDDIR)/$(PKGNAME)

# Production output directory (sibling of sw/, at the repo root) collecting the
# shippable artifacts built by 'make publish': the Linux install tarball (binary
# + install.sh + packaging/) and the double-clickable Windows package (.zip).
PRODDIR     := ../prod
PUBLISH_TAR := $(PRODDIR)/$(FULL_NAME)-linux.tar.gz

.PHONY: linux win windows win-package win-package-stage image post-build clean install uninstall publish

# Bare 'make' builds the Linux target.
linux:
	@$(MAKE) --no-print-directory OS=linux image

# Per-user desktop install: builds (if needed) and registers the binary, the
# .uvsys file association, icon and launcher. Use 'sudo make install ARGS=--system'
# for a machine-wide install. See install.sh --help for all options.
install: $(BINARY)
	@./install.sh $(ARGS)

uninstall:
	@./install.sh --uninstall $(ARGS)

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
# zip in prod before the second clean removes it). PKGNAME/PKGDIR/FULL_NAME are
# OS-independent, so the paths resolve the same in either sub-make.
publish:
	@mkdir -p "$(PRODDIR)"
	@$(MAKE) --no-print-directory clean
	@$(MAKE) --no-print-directory OS=win win-package
	@cp "$(PKGDIR).zip" "$(PRODDIR)/"
	@$(MAKE) --no-print-directory clean
	@$(MAKE) --no-print-directory OS=linux image
	@echo ""
	@echo "Publishing to $(PRODDIR)"
	@rm -f "$(PUBLISH_TAR)"
	tar czf "$(PUBLISH_TAR)" uvcan install.sh packaging
	@echo "Created:"
	@echo "    $(PUBLISH_TAR)"
	@echo "    $(PRODDIR)/$(PKGNAME).zip"

# Internal target that performs the actual build for the selected OS.
image: $(BINARY)
	@$(MAKE) --no-print-directory OS=$(OS) post-build


# Turn a PNG into a C header holding its bytes as a byte array (minus_hd_png /
# minus_hd_png_len). The array name is forced with -n so it is independent of
# the source path.
$(GENERATEDDIR)/media_minus_hd.h: media/minus_hd.png
	@mkdir -p $(@D)
	xxd -i -n minus_hd_png $< > $@

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
