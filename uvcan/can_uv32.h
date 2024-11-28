


/// @file: UVCAN generated header file describing the object dictionary
/// of this device. DO NOT EDIT DIRECTLY

#ifndef can_uv32
#define can_uv32

#include <stdint.h>
#include <string.h>

#define UV32_NODEID           0x32



#define UV32_VENDOR_ID    0x49b


#define UV32_PRODUCT_CODE    0x26


#define UV32_REVISION_NUMBER    0x1


#define UV32_EMCY_START_INDEX    0x0

#define UV32_EMCYSTR_INFO_STR_COUNT    0

typedef enum {
    UV32_EMCY_COUNT =            0
} uv32_emcy_err_codes_e;





#define UV32_NODEID_MIN_LIMIT            0x02
#define UV32_NODEID_MAX_LIMIT            0xFE
#define UV32_NONVOL_DATA_LEN            0x2000
enum {
    UV32_IN_MAPPINGS_AIN1 = (1 << 0),
    UV32_IN_MAPPINGS_AIN2 = (1 << 1),
    UV32_IN_MAPPINGS_AIN3 = (1 << 2),
    UV32_IN_MAPPINGS_AIN4 = (1 << 3),
    UV32_IN_MAPPINGS_DIN1 = (1 << 4),
    UV32_IN_MAPPINGS_NONE = 0,
    UV32_IN_MAPPINGS_COUNT = 6
};
typedef uint16_t uv32_in_mappings_e;

#define UV32_AIN_COUNT            4

#define UV32_DIN_COUNT            1

#define UV32_NOUT_COUNT            3





#define UV32_RXPDO_COUNT            7
#define UV32_TXPDO_COUNT            3


#define UV32_INPUTS_INDEX           0x2100
#define UV32_INPUTS_STRING_LEN            17
#define UV32_INPUTS_TYPE            CANOPEN_STRING
#define UV32_INPUTS_PERMISSIONS            CANOPEN_RO
#define UV32_INPUTS_DEFAULT				""


#define UV32_AIN1_NAME_INDEX           0x2111
#define UV32_AIN1_NAME_STRING_LEN            8
#define UV32_AIN1_NAME_TYPE            CANOPEN_STRING
#define UV32_AIN1_NAME_PERMISSIONS            CANOPEN_RW
#define UV32_AIN1_NAME_DEFAULT				"AIN1"


#define UV32_AIN1_VALUE_INDEX           0x2112
#define UV32_AIN1_VALUE_SUBINDEX            0
#define UV32_AIN1_VALUE_TYPE            CANOPEN_SIGNED8
#define UV32_AIN1_VALUE_PERMISSIONS            CANOPEN_RO
#define UV32_AIN1_VALUE_VALUE            0
#define UV32_AIN1_VALUE_DEFAULT            
#define UV32_AIN1_VALUE_MIN            0
#define UV32_AIN1_VALUE_MAX            0


#define UV32_AIN1_ADC_INDEX           0x2113
#define UV32_AIN1_ADC_SUBINDEX            0
#define UV32_AIN1_ADC_TYPE            CANOPEN_UNSIGNED16
#define UV32_AIN1_ADC_PERMISSIONS            CANOPEN_RO
#define UV32_AIN1_ADC_VALUE            0
#define UV32_AIN1_ADC_DEFAULT            
#define UV32_AIN1_ADC_MIN            0
#define UV32_AIN1_ADC_MAX            0


#define UV32_AIN1_CONF_INDEX           0x2114
#define UV32_AIN1_CONF_ARRAY_MAX_SIZE            7
#define UV32_AIN1_CONF_TYPE            CANOPEN_ARRAY16
#define UV32_AIN1_CONF_PERMISSIONS            CANOPEN_RW
#define UV32_AIN1_CONF_FLAGS_SUBINDEX            1
#define UV32_AIN1_CONF_FLAGS_MIN            0
#define UV32_AIN1_CONF_FLAGS_DEFAULT            0
#define UV32_AIN1_CONF_SCALE_FACTOR_SUBINDEX            2
#define UV32_AIN1_CONF_SCALE_FACTOR_MIN            0
#define UV32_AIN1_CONF_SCALE_FACTOR_DEFAULT            127
#define UV32_AIN1_CONF_MIN_VALUE_SUBINDEX            3
#define UV32_AIN1_CONF_MIN_VALUE_MIN            0
#define UV32_AIN1_CONF_MIN_VALUE_DEFAULT            0
#define UV32_AIN1_CONF_MAX_VALUE_SUBINDEX            4
#define UV32_AIN1_CONF_MAX_VALUE_MIN            0
#define UV32_AIN1_CONF_MAX_VALUE_DEFAULT            0
#define UV32_AIN1_CONF_MIDDLE_VALUE_SUBINDEX            5
#define UV32_AIN1_CONF_MIDDLE_VALUE_MIN            0
#define UV32_AIN1_CONF_MIDDLE_VALUE_DEFAULT            0
#define UV32_AIN1_CONF_MIDDLE_TOLERANCE_SUBINDEX            6
#define UV32_AIN1_CONF_MIDDLE_TOLERANCE_MIN            0
#define UV32_AIN1_CONF_MIDDLE_TOLERANCE_DEFAULT            200
#define UV32_AIN1_CONF_PROGRESSION_SUBINDEX            7
#define UV32_AIN1_CONF_PROGRESSION_MIN            0
#define UV32_AIN1_CONF_PROGRESSION_DEFAULT            2
extern const uint16_t uv32_ain1_conf_defaults[];
uint32_t uv32_ain1_conf_defaults_size(void);


#define UV32_AIN1_STATE_INDEX           0x2115
#define UV32_AIN1_STATE_SUBINDEX            0
#define UV32_AIN1_STATE_TYPE            CANOPEN_UNSIGNED8
#define UV32_AIN1_STATE_PERMISSIONS            CANOPEN_RW
#define UV32_AIN1_STATE_VALUE            
#define UV32_AIN1_STATE_DEFAULT            0
#define UV32_AIN1_STATE_MIN            0
#define UV32_AIN1_STATE_MAX            1


#define UV32_AIN1_REAL_VALUE_INDEX           0x2116
#define UV32_AIN1_REAL_VALUE_SUBINDEX            0
#define UV32_AIN1_REAL_VALUE_TYPE            CANOPEN_SIGNED32
#define UV32_AIN1_REAL_VALUE_PERMISSIONS            CANOPEN_RW
#define UV32_AIN1_REAL_VALUE_VALUE            
#define UV32_AIN1_REAL_VALUE_DEFAULT            0
#define UV32_AIN1_REAL_VALUE_MIN            INT32_MIN
#define UV32_AIN1_REAL_VALUE_MAX            INT32_MAX


#define UV32_OUTPUTS_INDEX           0x2200
#define UV32_OUTPUTS_STRING_LEN            17
#define UV32_OUTPUTS_TYPE            CANOPEN_STRING
#define UV32_OUTPUTS_PERMISSIONS            CANOPEN_RO
#define UV32_OUTPUTS_DEFAULT				""


#define UV32_N1OUT_NAME_INDEX           0x2211
#define UV32_N1OUT_NAME_STRING_LEN            8
#define UV32_N1OUT_NAME_TYPE            CANOPEN_STRING
#define UV32_N1OUT_NAME_PERMISSIONS            CANOPEN_RW
#define UV32_N1OUT_NAME_DEFAULT				"N1OUT"


#define UV32_N1OUT_TARGET_INDEX           0x2212
#define UV32_N1OUT_TARGET_SUBINDEX            0
#define UV32_N1OUT_TARGET_TYPE            CANOPEN_SIGNED8
#define UV32_N1OUT_TARGET_PERMISSIONS            CANOPEN_RO
#define UV32_N1OUT_TARGET_VALUE            0
#define UV32_N1OUT_TARGET_DEFAULT            
#define UV32_N1OUT_TARGET_MIN            0
#define UV32_N1OUT_TARGET_MAX            0


#define UV32_N1OUT_TARGET_REQ_INDEX           0x2213
#define UV32_N1OUT_TARGET_REQ_SUBINDEX            0
#define UV32_N1OUT_TARGET_REQ_TYPE            CANOPEN_SIGNED8
#define UV32_N1OUT_TARGET_REQ_PERMISSIONS            CANOPEN_RW
#define UV32_N1OUT_TARGET_REQ_VALUE            
#define UV32_N1OUT_TARGET_REQ_DEFAULT            0
#define UV32_N1OUT_TARGET_REQ_MIN            -127
#define UV32_N1OUT_TARGET_REQ_MAX            -127


#define UV32_N1OUT_MODECONF_INDEX           0x2215
#define UV32_N1OUT_MODECONF_ARRAY_MAX_SIZE            10
#define UV32_N1OUT_MODECONF_TYPE            CANOPEN_ARRAY16
#define UV32_N1OUT_MODECONF_PERMISSIONS            CANOPEN_RW
#define UV32_N1OUT_MODECONF_FLAGS_SUBINDEX            1
#define UV32_N1OUT_MODECONF_FLAGS_MIN            0
#define UV32_N1OUT_MODECONF_FLAGS_DEFAULT            0
#define UV32_N1OUT_MODECONF_MAPPING_FLAGS_SUBINDEX            2
#define UV32_N1OUT_MODECONF_MAPPING_FLAGS_MIN            0
#define UV32_N1OUT_MODECONF_MAPPING_FLAGS_DEFAULT            0
#define UV32_N1OUT_MODECONF_IN_MAPPING_SUBINDEX            3
#define UV32_N1OUT_MODECONF_IN_MAPPING_MIN            0
#define UV32_N1OUT_MODECONF_IN_MAPPING_DEFAULT            0
#define UV32_N1OUT_MODECONF_OUT_MAPPING_SUBINDEX            4
#define UV32_N1OUT_MODECONF_OUT_MAPPING_MIN            0
#define UV32_N1OUT_MODECONF_OUT_MAPPING_DEFAULT            0
#define UV32_N1OUT_MODECONF_MAPPED_DEV_SUBINDEX            5
#define UV32_N1OUT_MODECONF_MAPPED_DEV_MIN            0
#define UV32_N1OUT_MODECONF_MAPPED_DEV_DEFAULT            0
#define UV32_N1OUT_MODECONF_MAPPED_INDEX_SUBINDEX            6
#define UV32_N1OUT_MODECONF_MAPPED_INDEX_MIN            8192
#define UV32_N1OUT_MODECONF_MAPPED_INDEX_DEFAULT            8192
#define UV32_N1OUT_MODECONF_ENABLE_MAPPING_FLAGS_SUBINDEX            7
#define UV32_N1OUT_MODECONF_ENABLE_MAPPING_FLAGS_MIN            0
#define UV32_N1OUT_MODECONF_ENABLE_MAPPING_FLAGS_DEFAULT            0
#define UV32_N1OUT_MODECONF_ENABLE_IN_MAPPING_SUBINDEX            8
#define UV32_N1OUT_MODECONF_ENABLE_IN_MAPPING_MIN            0
#define UV32_N1OUT_MODECONF_ENABLE_IN_MAPPING_DEFAULT            0
#define UV32_N1OUT_MODECONF_ENABLE_OUT_MAPPING_SUBINDEX            9
#define UV32_N1OUT_MODECONF_ENABLE_OUT_MAPPING_MIN            0
#define UV32_N1OUT_MODECONF_ENABLE_OUT_MAPPING_DEFAULT            0
#define UV32_N1OUT_MODECONF_SCALE_FACTOR_SUBINDEX            10
#define UV32_N1OUT_MODECONF_SCALE_FACTOR_MIN            0
#define UV32_N1OUT_MODECONF_SCALE_FACTOR_DEFAULT            127
extern const uint16_t uv32_n1out_modeconf_defaults[];
uint32_t uv32_n1out_modeconf_defaults_size(void);


#define UV32_N1OUT_ENABLE_INDEX           0x2216
#define UV32_N1OUT_ENABLE_SUBINDEX            0
#define UV32_N1OUT_ENABLE_TYPE            CANOPEN_UNSIGNED8
#define UV32_N1OUT_ENABLE_PERMISSIONS            CANOPEN_RO
#define UV32_N1OUT_ENABLE_VALUE            0
#define UV32_N1OUT_ENABLE_DEFAULT            
#define UV32_N1OUT_ENABLE_MIN            0
#define UV32_N1OUT_ENABLE_MAX            0


#define UV32_AUTOCONTROLS_INDEX           0x2300
#define UV32_AUTOCONTROLS_STRING_LEN            17
#define UV32_AUTOCONTROLS_TYPE            CANOPEN_STRING
#define UV32_AUTOCONTROLS_PERMISSIONS            CANOPEN_RO
#define UV32_AUTOCONTROLS_DEFAULT				""


#define UV32_CAN_IF_VERSION_INDEX           0x2fff
#define UV32_CAN_IF_VERSION_SUBINDEX            0
#define UV32_CAN_IF_VERSION_TYPE            CANOPEN_UNSIGNED16
#define UV32_CAN_IF_VERSION_PERMISSIONS            CANOPEN_RO
#define UV32_CAN_IF_VERSION_VALUE            1
#define UV32_CAN_IF_VERSION_DEFAULT            
#define UV32_CAN_IF_VERSION_MIN            1
#define UV32_CAN_IF_VERSION_MAX            1


#define UV32_DEV_TYPE_INDEX           0xd310
#define UV32_DEV_TYPE_ARRAY_MAX_SIZE            
#define UV32_DEV_TYPE_TYPE            
#define UV32_DEV_TYPE_PERMISSIONS            UNKNOWN
extern const void uv32_dev_type_defaults[];
uint32_t uv32_dev_type_defaults_size(void);



/// @brief: returns the length of object dictionary in objects.
uint32_t obj_dict_len(void);

#endif