#############################################
#
#	USEVOLT MAKEFILE
# Builds a project adding a git hash to preprocessor symbols
# 
# * Project name is taken from the name of the git repository
# 
##############################################



### APP CONFIG ###


# Additional CPU define symbol, used for FreeRTOS compile
CORE := CORE_LINUX
TARGET := CONFIG_TARGET_LINUX

#Sepcify the optimizations used. Os == optimize for size, Og = optimize for debug, O3, O2, O1
O := Og

# Name of the directory where build files will be saved
BUILDDIR := release

# Name of the produced binary firmware
BINARY := uvcan


# List of include directories
INCLUDEDIRS := 	-I"inc/" \
				-I"uw_can/" \
				-I"hal/uv_hal/inc" \
				-I"hal/uv_hal/inc/canopen" \
				-I"hal/uv_hal/freertos/include" \
				-I"hal/uv_hal/freertos/portable/ThirdParty/GCC/Posix" \
				-I"hal/uv_hal/freertos/portable/ThirdParty/GCC/Posix/utils"

#List of directories where to search libraries
LIB_DIRS := 	-L./hal/uv_hal/src_win



#Source directories
SOURCEDIRS := src \
				hal/uv_hal/src \
				hal/uv_hal/src/canopen \
				hal/uv_hal/freertos \
				hal/uv_hal/freertos/portable/ThirdParty/GCC/Posix \
				hal/uv_hal/freertos/portable/ThirdParty/GCC/Posix/utils
				
SOURCEDIRS := $(SOURCEDIRS) \
				hal/uv_hal/src_linux



### END OF APP CONFIG ###





### Builds on this hardware (Linux only supported)
CC := gcc
SOURCES := $(shell find $(SOURCEDIRS) -maxdepth 1 -name '*.c')
GIT_VERSION := $(shell git describe --abbrev=4 --dirty --always --tags)
GIT_REV_ID := $(shell git rev-list --count HEAD)
APP_NAME := $(shell basename `git rev-parse --show-toplevel`)
FULL_NAME := $(APP_NAME)_$(GIT_VERSION)



CFLAGS := $(shell pkg-config --cflags ncurses) -pthread -D$(TARGET)=1 -D__UV_PROJECT_NAME=$(FULL_NAME) -D__UV_PROGRAM_VERSION=$(GIT_REV_ID) $(INCLUDEDIRS) -D$(CORE) -$(O) -g3 -Wall -fmessage-length=0 -fno-builtin -ffunction-sections -fdata-sections

LDFLAGS := -D__UV_APP_VERSION=\"$(GIT_VERSION)\"



# Get list of object files, with paths
OBJECTS := $(addprefix $(BUILDDIR)/,$(SOURCES:%.c=%.o))
# List of object dependencies
DEPS := $(OBJECTS:.o=.d)


all: $(BINARY)
	$(MAKE) post-build


$(BUILDDIR)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -I$(HEADERDIR) -I$(dir $<) -MMD -MT $@ -c $< -o $@


$(BINARY): $(OBJECTS)
	@echo ""
	@echo "Linking target"
	@echo ""
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJECTS) -pthread -o "$(BINARY)" -lncurses $(LIB_DIRS) $(LIBS)


post-build:
	-@echo ""
	-@echo 'Performing post-build steps'
	-@echo ""

clean:
	@rm -rf $(BUILDDIR)
	@rm -rf $(BINARY)

	
-include $(DEPS)

