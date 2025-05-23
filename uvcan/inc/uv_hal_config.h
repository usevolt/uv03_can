/*
 * uw_hal_config.h
 *
 *  Created on: Mar 22, 2016
 *      Author: usevolt
 */

#ifndef UV_HAL_CONFIG_H_
#define UV_HAL_CONFIG_H_


/**** USER CONFIGURATIONS ****/


// note: Target should be specified in the makefile
//#define CONFIG_TARGET_LPC11C14						0
//#define CONFIG_TARGET_LPC1785						0
//#define CONFIG_TARGET_LPC1549						0
//#define CONFIG_TARGET_LINUX							0
//#define CONFIG_TARGET_WIN							1



#define CONFIG_INTERFACE_REVISION					1


#include <stdbool.h>
extern bool silent;
#define PRINT(...)	do {if (!silent) { fprintf(stderr, __VA_ARGS__); }} while (0)


#define CONFIG_NON_VOLATILE_MEMORY					1
#define CONFIG_MAIN_H								"main.h"
#define CONFIG_APP_ST								struct _dev_st dev
#define CONFIG_NON_VOLATILE_START					dev.data_start
#define CONFIG_NON_VOLATILE_END						dev.data_end



#define CONFIG_RTOS									1
#define CONFIG_RTOS_HEAP_SIZE						(50 * 1024 * 1024)
#define CONFIG_UV_BOOTLOADER						1


#define CONFIG_PWM									0
#define CONFIG_PWM0									0
#define CONFIG_PWM0_FREQ							200
#define CONFIG_PWM0_0								1
#define CONFIG_PWM0_0_IO							P0_4
#define CONFIG_PWM0_4								0
#define CONFIG_PWM0_4_IO							0

#define CONFIG_PWM1									0
#define CONFIG_PWM1_FREQ							10000


#define CONFIG_JSON									1



#define CONFIG_TERMINAL								0
#define CONFIG_TERMINAL_BUFFER_SIZE					200
#define CONFIG_TERMINAL_ARG_COUNT					4
#define CONFIG_TERMINAL_INSTRUCTIONS				1
#define CONFIG_TERMINAL_UART						0
#define CONFIG_TERMINAL_CAN							0
#define CONFIG_TERMINAL_CLI							1





#define CONFIG_ADC0									0
#define CONFIG_ADC1									0
#define CONFIG_ADC_MODE								ADC_MODE_SYNC
#define CONFIG_ADC_CONVERSION_FREQ					20000

#define CONFIG_ADC_CHANNEL0_0						0
#define CONFIG_ADC_CHANNEL0_1						0
#define CONFIG_ADC_CHANNEL0_2						0
#define CONFIG_ADC_CHANNEL0_3						0
#define CONFIG_ADC_CHANNEL0_4						0
#define CONFIG_ADC_CHANNEL0_5						0
#define CONFIG_ADC_CHANNEL0_6						0
#define CONFIG_ADC_CHANNEL0_7						0
#define CONFIG_ADC_CHANNEL0_8						0
#define CONFIG_ADC_CHANNEL0_9						0
#define CONFIG_ADC_CHANNEL0_10						0
#define CONFIG_ADC_CHANNEL0_11						0
#define CONFIG_ADC_CHANNEL1_0						0
#define CONFIG_ADC_CHANNEL1_1						0
#define CONFIG_ADC_CHANNEL1_2						0
#define CONFIG_ADC_CHANNEL1_3						0
#define CONFIG_ADC_CHANNEL1_4						0
#define CONFIG_ADC_CHANNEL1_5						0
#define CONFIG_ADC_CHANNEL1_6						0
#define CONFIG_ADC_CHANNEL1_7						0
#define CONFIG_ADC_CHANNEL1_8						0
#define CONFIG_ADC_CHANNEL1_9						0
#define CONFIG_ADC_CHANNEL1_10						0
#define CONFIG_ADC_CHANNEL1_11						0









#define CONFIG_CAN									1
#define CONFIG_CAN0									1
#define CONFIG_CAN0_BAUDRATE						250000
#define CONFIG_CAN0_RX_BUFFER_SIZE					2048
#define CONFIG_CAN0_TX_BUFFER_SIZE					256
#define CONFIG_CAN1									0
#define CONFIG_CAN_LOG								0
#define CONFIG_CAN_ERROR_LOG						0



#define CONFIG_CANOPEN								1
#define CONFIG_CANOPEN_VENDOR_ID					CANOPEN_USEVOLT_VENDOR_ID
#define CONFIG_CANOPEN_PRODUCT_CODE					0
#define CONFIG_CANOPEN_REVISION_NUMBER				0
#define CONFIG_CANOPEN_LOG							0
#define CONFIG_CANOPEN_NMT_MASTER					1
#define CONFIG_CANOPEN_AUTO_PREOPERATIONAL			0
#define CONFIG_CANOPEN_RXPDO_COUNT					0
#define CONFIG_CANOPEN_RXPDO_TIMEOUT_MS				500
#define CONFIG_CANOPEN_TXPDO_COUNT					0
#define CONFIG_CANOPEN_SDO_SYNC						1
#define CONFIG_CANOPEN_SDO_SEGMENTED				1
#define CONFIG_CANOPEN_SDO_BLOCK_TRANSFER			1
#define CONFIG_CANOPEN_SDO_BLOCK_SIZE				256
#define CONFIG_CANOPEN_SDO_TIMEOUT_MS				1000
#define CONFIG_CANOPEN_SDO_SERVER					0
#define CONFIG_CANOPEN_OBJ_DICT_APP_PARAMS			obj_dict
#define CONFIG_CANOPEN_OBJ_DICT_APP_PARAMS_COUNT	obj_dict_len
#define CONFIG_CANOPEN_HEARTBEAT_PRODUCER			0
#define CONFIG_CANOPEN_PRODUCER_HEARTBEAT_TIME_MS	1000
#define CONFIG_CANOPEN_HEARTBEAT_CONSUMER			0
#define CONFIG_CANOPEN_CONSUMER_HEARTBEAT_COUNT		0
#define CONFIG_CANOPEN_CHANNEL						dev.can_channel
#define CONFIG_CANOPEN_DEFAULT_NODE_ID				1
#define CONFIG_CANOPEN_EMCY_RX_BUFFER_SIZE			3
#define CONFIG_CANOPEN_EMCY_INHIBIT_TIME_MS			200
#define CONFIG_CANOPEN_UPDATE_PDO_MAPPINGS_ON_NODEID_WRITE	0
#define CONFIG_CANOPEN_OBJ_DICT_IN_RISING_ORDER		0



#define CONFIG_OUTPUT								0

#define CONFIG_SOLENOID_OUTPUT						0
#define CONFIG_SOLENOID_P							10
#define CONFIG_SOLENOID_I							0

#define CONFIG_PID									0

/**** END OF USER CONFIGURATIONS ****/




#endif /* UV_HAL_CONFIG_H_ */
