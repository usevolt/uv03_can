################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../src/can.c \
../src/can_msg.c \
../src/commands.c \
../src/main.c \
../src/ui.c 

OBJS += \
./src/can.o \
./src/can_msg.o \
./src/commands.o \
./src/main.o \
./src/ui.o 

C_DEPS += \
./src/can.d \
./src/can_msg.d \
./src/commands.d \
./src/main.d \
./src/ui.d 


# Each subdirectory must supply rules for building sources it contributes
src/%.o: ../src/%.c
	@echo 'Building file: $<'
	@echo 'Invoking: GCC C Compiler'
	gcc -I"/home/usevolt/uv/projects/uv03_can/uv_can/inc" -O0 -g3 -Wall -c -fmessage-length=0 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@:%.o=%.d)" -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '


