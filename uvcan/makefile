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
				-I"hal/uv_han/freertos" \
				-I"hal/uv_hal/freertos/include" \
				-I"hal/uv_hal/freertos/portable/GCC/Posix"

#List of directories where to search libraries
LIB_DIRS := 	

#List of libraries
LIBS :=			

#Source directories
SOURCEDIRS := src \
				hal/uv_hal/src \
				hal/uv_hal/freertos \
				hal/uv_hal/freertos/portable/GCC/Posix \
				hal/uv_hal/src_linux


### END OF APP CONFIG ###





### Builds on this hardware (Linux only supported)
CC := gcc
SOURCES := $(shell find $(SOURCEDIRS) -maxdepth 1 -name '*.c')
GIT_VERSION := $(shell git describe --abbrev=4 --dirty --always --tags)
APP_NAME := $(shell basename `git rev-parse --show-toplevel`)
FULL_NAME := $(APP_NAME)_$(GIT_VERSION)


CFLAGS := -D__UV_PROJECT_NAME=$(FULL_NAME) $(INCLUDEDIRS) -D$(CORE) -$(O) -g3 -Wall -fmessage-length=0 -fno-builtin -ffunction-sections -fdata-sections


LDFLAGS := -D__UV_APP_VERSION=\"$(GIT_VERSION)\" -static $(LIB_DIRS) $(LIBS) -pthread


# Get list of object files, with paths
OBJECTS := $(addprefix $(BUILDDIR)/,$(SOURCES:%.c=%.o))
# List of object dependencies
DEPS := $(OBJECTS:.o=.d)



all: $(BINARY)
	$(MAKE) post-build

$(BINARY): $(OBJECTS)
	@echo ""
	@echo "Linking target"
	@echo ""
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJECTS) -o "$(BINARY)"

$(BUILDDIR)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -I$(HEADERDIR) -I$(dir $<) -MMD -MT $@ -c $< -o $@


post-build:
	-@echo ""
	-@echo 'Performing post-build steps'
	-@echo ""

clean:
	@rm -rf $(BUILDDIR)
	@rm -rf $(BINARY)

	
-include $(DEPS)

