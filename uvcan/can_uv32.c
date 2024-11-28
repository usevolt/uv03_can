#include <uv_utilities.h>
#include <uv_canopen.h>
#include "main.h"
#include "can_uv32.h"


const uv_canopen_non_volatile_st uv32_canopen_init = {
    .producer_heartbeat_time_ms = 400,
    .rxpdo_coms = {
        {
            .cob_id = CANOPEN_RXPDO1_ID + UV26_NODEID + CANOPEN_PDO_DISABLED,
            .transmission_type = CANOPEN_PDO_TRANSMISSION_ASYNC
        },
        {
            .cob_id = CANOPEN_RXPDO1_ID + UV26_NODEID + CANOPEN_PDO_DISABLED,
            .transmission_type = CANOPEN_PDO_TRANSMISSION_ASYNC
        },
        {
            .cob_id = CANOPEN_RXPDO1_ID + UV26_NODEID + CANOPEN_PDO_DISABLED,
            .transmission_type = CANOPEN_PDO_TRANSMISSION_ASYNC
        },
        {
            .cob_id = CANOPEN_RXPDO1_ID + UV26_NODEID + CANOPEN_PDO_DISABLED,
            .transmission_type = CANOPEN_PDO_TRANSMISSION_ASYNC
        },
        {
            .cob_id = CANOPEN_RXPDO1_ID + UV26_NODEID + CANOPEN_PDO_DISABLED,
            .transmission_type = CANOPEN_PDO_TRANSMISSION_ASYNC
        },
        {
            .cob_id = CANOPEN_RXPDO1_ID + UV26_NODEID + CANOPEN_PDO_DISABLED,
            .transmission_type = CANOPEN_PDO_TRANSMISSION_ASYNC
        },
        {
            .cob_id = CANOPEN_RXPDO1_ID + UV26_NODEID + CANOPEN_PDO_DISABLED,
            .transmission_type = CANOPEN_PDO_TRANSMISSION_ASYNC
        }
    },
    .rxpdo_maps = {
        {
            .mappings = {
            }
        },
        {
            .mappings = {
            }
        },
        {
            .mappings = {
            }
        },
        {
            .mappings = {
            }
        },
        {
            .mappings = {
            }
        },
        {
            .mappings = {
            }
        },
        {
            .mappings = {
            }
        }
    },
    .txpdo_coms = {
        {
            .cob_id = CANOPEN_TXPDO1_ID + UV26_NODEID,
            .transmission_type = CANOPEN_PDO_TRANSMISSION_ASYNC,
            .inhibit_time = 20,
            .event_timer = 100
        },
        {
            .cob_id = CANOPEN_TXPDO2_ID + UV26_NODEID,
            .transmission_type = CANOPEN_PDO_TRANSMISSION_ASYNC,
            .inhibit_time = 20,
            .event_timer = 100
        },
        {
            .cob_id = CANOPEN_TXPDO3_ID + UV26_NODEID,
            .transmission_type = CANOPEN_PDO_TRANSMISSION_ASYNC,
            .inhibit_time = 20,
            .event_timer = 100
        }
    },
    .txpdo_maps = {
        {
            .mappings = {
                {
                    .main_index = 0x0,
                    .sub_index = 0,
                    .length = 0
                },
                {
                    .main_index = 0x0,
                    .sub_index = 0,
                    .length = 0
                },
                {
                    .main_index = 0x0,
                    .sub_index = 0,
                    .length = 0
                },
                {
                    .main_index = 0x0,
                    .sub_index = 0,
                    .length = 0
                },
                {
                    .main_index = 0x0,
                    .sub_index = 0,
                    .length = 0
                },
                {
                    .main_index = 0x0,
                    .sub_index = 0,
                    .length = 0
                },
                {
                    .main_index = 0x0,
                    .sub_index = 0,
                    .length = 0
                },
                {
                    .main_index = 0x0,
                    .sub_index = 0,
                    .length = 0
                },
            }
        },
        {
            .mappings = {
                {
                    .main_index = 0x0,
                    .sub_index = 0,
                    .length = 0
                },
                {
                    .main_index = 0x0,
                    .sub_index = 0,
                    .length = 0
                },
                {
                    .main_index = 0x0,
                    .sub_index = 0,
                    .length = 0
                },
                {
                    .main_index = 0x0,
                    .sub_index = 0,
                    .length = 0
                },
                {
                    .main_index = 0x0,
                    .sub_index = 0,
                    .length = 0
                },
                {
                    .main_index = 0x0,
                    .sub_index = 0,
                    .length = 0
                },
                {
                    .main_index = 0x0,
                    .sub_index = 0,
                    .length = 0
                },
                {
                    .main_index = 0x0,
                    .sub_index = 0,
                    .length = 0
                },
            }
        },
        {
            .mappings = {
                {
                    .main_index = 0x0,
                    .sub_index = 0,
                    .length = 0
                },
                {
                    .main_index = 0x0,
                    .sub_index = 0,
                    .length = 0
                },
                {
                    .main_index = 0x0,
                    .sub_index = 0,
                    .length = 0
                },
                {
                    .main_index = 0x0,
                    .sub_index = 0,
                    .length = 0
                },
                {
                    .main_index = 0x0,
                    .sub_index = 0,
                    .length = 0
                },
                {
                    .main_index = 0x0,
                    .sub_index = 0,
                    .length = 0
                },
                {
                    .main_index = 0x0,
                    .sub_index = 0,
                    .length = 0
                },
                {
                    .main_index = 0x0,
                    .sub_index = 0,
                    .length = 0
                },
            }
        }

    }
};



canopen_object_st obj_dict[] = {
    {
        .main_index = UV32_INPUTS_INDEX,
        .string_len = UV32_INPUTS_STRING_LEN,
        .type = UV32_INPUTS_TYPE,
        .permissions = UV32_INPUTS_PERMISSIONS,
        .data_ptr = (void *) uv26_inputs_str
    },
    {
        .main_index = UV32_AIN1_NAME_INDEX,
        .string_len = UV32_AIN1_NAME_STRING_LEN,
        .type = UV32_AIN1_NAME_TYPE,
        .permissions = UV32_AIN1_NAME_PERMISSIONS,
        .data_ptr = (void *) &(((in_conf_st*) (&dev.op.ain1_conf))->name)
    },
    {
        .main_index = UV32_AIN1_VALUE_INDEX,
        .sub_index = UV32_AIN1_VALUE_SUBINDEX,
        .type = UV32_AIN1_VALUE_TYPE,
        .permissions = UV32_AIN1_VALUE_PERMISSIONS,
        .data_ptr = (void *) &(((in_st*) &dev.ain1)->value)
    },
    {
        .main_index = UV32_AIN1_ADC_INDEX,
        .sub_index = UV32_AIN1_ADC_SUBINDEX,
        .type = UV32_AIN1_ADC_TYPE,
        .permissions = UV32_AIN1_ADC_PERMISSIONS,
        .data_ptr = (void *) &(((halin_st*) &dev.ain1)->hal.out_adc)
    },
    {
        .main_index = UV32_AIN1_CONF_INDEX,
        .array_max_size = UV32_AIN1_CONF_ARRAY_MAX_SIZE,
        .type = UV32_AIN1_CONF_TYPE,
        .permissions = UV32_AIN1_CONF_PERMISSIONS,
        .data_ptr = (void *) &(((in_conf_st*) &dev.op.ain1_conf)->flags)
    },
    {
        .main_index = UV32_AIN1_STATE_INDEX,
        .sub_index = UV32_AIN1_STATE_SUBINDEX,
        .type = UV32_AIN1_STATE_TYPE,
        .permissions = UV32_AIN1_STATE_PERMISSIONS,
        .data_ptr = (void *) &(((halin_st*) &dev.ain1)->hal.state)
    },
    {
        .main_index = UV32_AIN1_REAL_VALUE_INDEX,
        .sub_index = UV32_AIN1_REAL_VALUE_SUBINDEX,
        .type = UV32_AIN1_REAL_VALUE_TYPE,
        .permissions = UV32_AIN1_REAL_VALUE_PERMISSIONS,
        .data_ptr = (void *) &(((in_st*) &dev.ain1)->real_value)
    },
    {
        .main_index = UV32_OUTPUTS_INDEX,
        .string_len = UV32_OUTPUTS_STRING_LEN,
        .type = UV32_OUTPUTS_TYPE,
        .permissions = UV32_OUTPUTS_PERMISSIONS,
        .data_ptr = (void *) uv26_outputs_str
    },
    {
        .main_index = UV32_N1OUT_NAME_INDEX,
        .string_len = UV32_N1OUT_NAME_STRING_LEN,
        .type = UV32_N1OUT_NAME_TYPE,
        .permissions = UV32_N1OUT_NAME_PERMISSIONS,
        .data_ptr = (void *) GET_UV26_N1OUT_NAME()
    },
    {
        .main_index = UV32_N1OUT_TARGET_INDEX,
        .sub_index = UV32_N1OUT_TARGET_SUBINDEX,
        .type = UV32_N1OUT_TARGET_TYPE,
        .permissions = UV32_N1OUT_TARGET_PERMISSIONS,
        .data_ptr = (void *) GET_UV26_N1OUT_TARGET()
    },
    {
        .main_index = UV32_N1OUT_TARGET_REQ_INDEX,
        .sub_index = UV32_N1OUT_TARGET_REQ_SUBINDEX,
        .type = UV32_N1OUT_TARGET_REQ_TYPE,
        .permissions = UV32_N1OUT_TARGET_REQ_PERMISSIONS,
        .data_ptr = (void *) GET_UV26_N1OUT_TARGET_REQ()
    },
    {
        .main_index = UV32_N1OUT_MODECONF_INDEX,
        .array_max_size = UV32_N1OUT_MODECONF_ARRAY_MAX_SIZE,
        .type = UV32_N1OUT_MODECONF_TYPE,
        .permissions = UV32_N1OUT_MODECONF_PERMISSIONS,
        .data_ptr = (void *) GET_UV26_N1OUT_MODECONF()
    },
    {
        .main_index = UV32_N1OUT_ENABLE_INDEX,
        .sub_index = UV32_N1OUT_ENABLE_SUBINDEX,
        .type = UV32_N1OUT_ENABLE_TYPE,
        .permissions = UV32_N1OUT_ENABLE_PERMISSIONS,
        .data_ptr = (void *) GET_UV26_N1OUT_ENABLE()
    },
    {
        .main_index = UV32_AUTOCONTROLS_INDEX,
        .string_len = UV32_AUTOCONTROLS_STRING_LEN,
        .type = UV32_AUTOCONTROLS_TYPE,
        .permissions = UV32_AUTOCONTROLS_PERMISSIONS,
        .data_ptr = (void *) uv26_auto_str
    },
    {
        .main_index = UV32_CAN_IF_VERSION_INDEX,
        .sub_index = UV32_CAN_IF_VERSION_SUBINDEX,
        .type = UV32_CAN_IF_VERSION_TYPE,
        .permissions = UV32_CAN_IF_VERSION_PERMISSIONS,
        .data_ptr = (void *) &can_if_version
    },
    {
        .main_index = UV32_DEV_TYPE_INDEX,
        .array_max_size = UV32_DEV_TYPE_ARRAY_MAX_SIZE,
        .type = UV32_DEV_TYPE_TYPE,
        .permissions = UV32_DEV_TYPE_PERMISSIONS,
        .data_ptr = (void *) 
    }
};

uint32_t obj_dict_len(void) {
    return sizeof(obj_dict) / sizeof(canopen_object_st);
}

const uint16_t uv32_ain1_conf_defaults[] = {
    UV32_AIN1_CONF_FLAGS_DEFAULT,
    UV32_AIN1_CONF_SCALE_FACTOR_DEFAULT,
    UV32_AIN1_CONF_MIN_VALUE_DEFAULT,
    UV32_AIN1_CONF_MAX_VALUE_DEFAULT,
    UV32_AIN1_CONF_MIDDLE_VALUE_DEFAULT,
    UV32_AIN1_CONF_MIDDLE_TOLERANCE_DEFAULT,
    UV32_AIN1_CONF_PROGRESSION_DEFAULT
};
uint32_t uv32_ain1_conf_defaults_size(void) {
    return sizeof(uv32_ain1_conf_defaults) / sizeof(uv32_ain1_conf_defaults[0]);
}

const uint16_t uv32_n1out_modeconf_defaults[] = {
    UV32_N1OUT_MODECONF_FLAGS_DEFAULT,
    UV32_N1OUT_MODECONF_MAPPING_FLAGS_DEFAULT,
    UV32_N1OUT_MODECONF_IN_MAPPING_DEFAULT,
    UV32_N1OUT_MODECONF_OUT_MAPPING_DEFAULT,
    UV32_N1OUT_MODECONF_MAPPED_DEV_DEFAULT,
    UV32_N1OUT_MODECONF_MAPPED_INDEX_DEFAULT,
    UV32_N1OUT_MODECONF_ENABLE_MAPPING_FLAGS_DEFAULT,
    UV32_N1OUT_MODECONF_ENABLE_IN_MAPPING_DEFAULT,
    UV32_N1OUT_MODECONF_ENABLE_OUT_MAPPING_DEFAULT,
    UV32_N1OUT_MODECONF_SCALE_FACTOR_DEFAULT
};
uint32_t uv32_n1out_modeconf_defaults_size(void) {
    return sizeof(uv32_n1out_modeconf_defaults) / sizeof(uv32_n1out_modeconf_defaults[0]);
}

const void uv32_dev_type_defaults[] = {
};
uint32_t uv32_dev_type_defaults_size(void) {
    return sizeof(uv32_dev_type_defaults) / sizeof(uv32_dev_type_defaults[0]);
}

