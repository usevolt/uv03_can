#############################################
#
#	USEVOLT MAKEFILE
# Builds a project adding a git hash to preprocessor symbols
# 
# * Project name is taken from the name of the git repository
# 
##############################################



### APP CONFIG ###

UNAME := $(shell uname)

# Additional CPU define symbol, used for FreeRTOS compile
ifeq ($(UNAME), MINGW64_NT-6.1)
CORE := CORE_WIN
TARGET := CONFIG_TARGET_WIN
else ifeq ($(UNAME), MINGW32_NT-6.1-7601)
CORE := CORE_WIN
TARGET := CONFIG_TARGET_WIN
else
CORE := CORE_LINUX
TARGET := CONFIG_TARGET_LINUX
endif

#Sepcify the optimizations used. Os == optimize for size, Og = optimize for debug, O3, O2, O1
O := Og

# Name of the directory where build files will be saved
BUILDDIR := release

# Name of the produced binary firmware
BINARY := uvcan


# List of include directories
INCLUDEDIRS := 	-I"inc/" \
				-I"inc/ui" \
				-I"uw_can/" \
				-I"hal/uv_hal/inc" \
				-I"hal/uv_hal/inc/canopen" \
				-I"hal/uv_hal/freertos/include" \
				-I"hal/uv_hal/freertos/portable/ThirdParty/GCC/Posix" \
				-I"hal/uv_hal/freertos/portable/ThirdParty/GCC/Posix/utils"

ifeq ($(CORE), CORE_WIN)
INCLUDEDIRS := $(INCLUDEDIRS) \
	-IC:/msys64/mingw64/include/ncurses
endif

#List of directories where to search libraries
LIB_DIRS := 	-L./hal/uv_hal/src_win

#List of libraries
ifeq ($(CORE), CORE_WIN)
LIBS := -lPCANBasic
endif


#Source directories
SOURCEDIRS := src \
				src/ui \
				hal/uv_hal/src \
				hal/uv_hal/src/canopen \
				hal/uv_hal/freertos \
				hal/uv_hal/freertos/portable/ThirdParty/GCC/Posix \
				hal/uv_hal/freertos/portable/ThirdParty/GCC/Posix/utils
				
ifeq ($(CORE), CORE_LINUX)
SOURCEDIRS := $(SOURCEDIRS) \
				hal/uv_hal/src_linux
else
SOURCEDIRS := $(SOURCEDIRS) \
				hal/uv_hal/src_win
endif

# Path and name of GResource file for GTK UI
# Path has to end in /
GRESOURCE_DIR := src/ui/
#eName of the GResources xml file. This same name is used for generated .c and .h files
GRESOURCE_FILE := gresources



### END OF APP CONFIG ###





### Builds on this hardware (Linux only supported)
CC := gcc
SOURCES := $(shell find $(SOURCEDIRS) -maxdepth 1 -name '*.c')
GIT_VERSION := $(shell git describe --abbrev=4 --dirty --always --tags)
GIT_REV_ID := $(shell git rev-list --count HEAD)
APP_NAME := $(shell basename `git rev-parse --show-toplevel`)
FULL_NAME := $(APP_NAME)_$(GIT_VERSION)

UIFILES := $(shell find $(GRESOURCE_DIR) -maxdepth 1 -name '*.ui')
GRESOURCE := $(GRESOURCE_DIR)$(GRESOURCE_FILE).xml


CFLAGS := $(shell pkg-config --cflags gtk+-3.0 ncurses) -D$(TARGET)=1 -D__UV_PROJECT_NAME=$(FULL_NAME) -D__UV_PROGRAM_VERSION=$(GIT_REV_ID) $(INCLUDEDIRS) -D$(CORE) -$(O) -g3 -Wall -fmessage-length=0 -fno-builtin -ffunction-sections -fdata-sections

LDFLAGS := -D__UV_APP_VERSION=\"$(GIT_VERSION)\"

ifeq ($(CORE), CORE_WIN)
CFLAGS := $(CFLAGS) -mwindows
LDFLAGS := $(LDFLAGS) -mwindows
endif


# Get list of object files, with paths
OBJECTS := $(addprefix $(BUILDDIR)/,$(SOURCES:%.c=%.o))
# List of object dependencies
DEPS := $(OBJECTS:.o=.d)


all: $(GRESOURCE) $(BINARY)
	$(MAKE) post-build


$(GRESOURCE): $(UIFILES)
	glib-compile-resources $(GRESOURCE) --generate-header --target $(GRESOURCE_DIR)$(GRESOURCE_FILE).h
	glib-compile-resources $(GRESOURCE) --generate-source --target $(GRESOURCE_DIR)$(GRESOURCE_FILE).c
	@touch $(GRESOURCE)

$(BUILDDIR)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -I$(HEADERDIR) -I$(dir $<) -MMD -MT $@ -c $< -o $@


$(BINARY): $(OBJECTS)
	@echo ""
	@echo "Linking target"
	@echo ""
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJECTS) -pthread -o "$(BINARY)" -lncurses $(shell pkg-config --libs gtk+-3.0) $(LIB_DIRS) $(LIBS)


ifeq ($(CORE), CORE_WIN)
post-build:
	-@echo ""
	-@echo 'Performing post-build steps'
	-@echo 'Copying uvcan.exe to install directory'
	cp uvcan.exe install/
else
post-build:
	-@echo ""
	-@echo 'Performing post-build steps'
	-@echo ""
endif

clean:
	@rm -rf $(BUILDDIR)
	@rm -rf $(BINARY)
	@touch $(UIFILES)

	
-include $(DEPS)

