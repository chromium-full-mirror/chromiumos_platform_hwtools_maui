/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/*
 * @file
 * @brief TI TPS6699x Register addresses and i2c command structure
 */
#ifndef ZEPHYR_DRIVERS_USBC_TPS6699X_REG_H_
#define ZEPHYR_DRIVERS_USBC_TPS6699X_REG_H_

#include <stdint.h>

#include <zephyr/toolchain.h>

/**
 * @brief TPS6699x Registers Addresses
 */
enum tps6699x_reg {
	REG_VENDOR_ID = 0x00,
	REG_DEVICE_ID = 0x01,
	REG_PROTOCOL_VERSION = 0x02,
	REG_MODE = 0x03,
	REG_UID = 0x05,
	REG_CUSTOMER_USE = 0x06,
	REG_COMMAND_FOR_I2C1 = 0x08,
	REG_DATA_FOR_CMD1 = 0x09,
	REG_DEVICE_CAPABILITIES = 0x0d,
	REG_VERSION = 0x0f,
	REG_COMMAND_FOR_I2C2 = 0x10,
	REG_DATA_FOR_CMD2 = 0x11,
	REG_INTERRUPT_EVENT_FOR_I2C1 = 0x14,
	REG_INTERRUPT_EVENT_FOR_I2C2 = 0x15,
	REG_INTERRUPT_MASK_FOR_I2C1 = 0x16,
	REG_INTERRUPT_MASK_FOR_I2C2 = 0x17,
	REG_INTERRUPT_CLEAR_FOR_I2C1 = 0x18,
	REG_INTERRUPT_CLEAR_FOR_I2C2 = 0x19,
	REG_STATUS = 0x1a,
	REG_SX_CONFIG = 0x1f,
	REG_SET_SX_APP_CONFIG = 0x20,
	REG_DISCOVERED_SVIDS = 0x21,
	REG_CONNECTION_MANAGER_STATUS = 0x22,
	REG_USB_CONFIG = 0x23,
	REG_USB_STATUS = 0x24,
	REG_CONNECTION_MANAGER_CONTROL = 0x25,
	REG_POWER_PATH_STATUS = 0x26,
	REG_GLOBAL_SYSTEM_CONFIGURATION = 0x27,
	REG_PORT_CONFIGURATION = 0x28,
	REG_PORT_CONTROL = 0x29,
	REG_BOOT_FLAG = 0x2d,
	REG_BUILD_DESCRIPTION = 0x2e,
	REG_DEVICE_INFORMATION = 0x2f,
	REG_RECEIVED_SOURCE_CAPABILITIES = 0x30,
	REG_RECEIVED_SINK_CAPABILITIES = 0x31,
	REG_TRANSMIT_SOURCE_CAPABILITES = 0x32,
	REG_TRANSMIT_SINK_CAPABILITES = 0x33,
	REG_ACTIVE_PDO_CONTRACT = 0x34,
	REG_ACTIVE_RDO_CONTRACT = 0x35,
	REG_AUTONEGOTIATE_SINK = 0x37,
	REG_SPM_CLIENT_CONTROL = 0x3c,
	REG_SPM_CLIENT_STATUS = 0x3d,
	REG_PD_STATUS = 0x40,
	REG_PD3_STATUS = 0x41,
	REG_PD3_CONFIGURATION = 0x42,
	REG_DELAY_CONFIG = 0x43,
	REG_TX_IDENTITY = 0x47,
	REG_RECEIVED_SOP_IDENTITY_DATA_OBJECT = 0x48,
	REG_RECEIVED_SOP_PRIME_IDENTITY_DATA_OBJECT = 0x49,
	REG_USER_ALTERNATE_MODE_CONFIGURATION = 0x4a,
	REG_RECEIVED_ATTENTION_VDM = 0x4e,
	REG_DISPLAY_PORT_CONFIGURATION = 0x51,
	REG_THUNDERBOLT_CONFIGURATION = 0x52,
	REG_SPECIAL_CONFIGURATION = 0x55,
	REG_PROCHOT_CONFIGURATION = 0x56,
	REG_USER_VID_STATUS = 0x57,
	REG_DISPLAY_PORT_STATUS = 0x58,
	REG_INTEL_VID_STATUS = 0x59,
	REG_RETIMER_DEBUG = 0x5d,
	REG_DATA_STATUS = 0x5f,
	REG_RECEIVED_USER_SVID_ATTENTIONVDM = 0x60,
	REG_RECEIVED_USER_SVID_OTHER_VDM = 0x61,
	REG_APP_CONFIG_BINARY_DATA_INDICES = 0x62,
	REG_I2C_CONTROLLER_CONFIG = 0x64,
	REG_TYPEC_STATUS = 0x69,
	REG_ADC_RESULTS = 0x6a,
	REG_APP_CONFIG = 0x6c,
	REG_STATE_CONFIG = 0x6f,
	REG_SLEEP_CONTROL = 0x70,
	REG_GPIO_STATUS = 0x72,
	REG_TX_MANUFACTURER_INFO_SOP = 0x73,
	REG_RECEIVED_ALERT_DATA_OBJECT = 0x74,
	REG_TX_ALERT_DATA_OBJECT = 0x75,
	REG_TX_SOURCE_CAPABILITIES_EXTENDED_DATA_BLOCK = 0x77,
	REG_TRANSMITTED_STATUS_DATA_BLOCK = 0x79,
	REG_TRANSMITTED_PPS_STATUS_DATA_BLOCK = 0x7a,
	REG_TRANSMITTED_BATTERY_STATUS_DATA_OBJECT = 0x7b,
	REG_TX_BATTERY_CAPABILITIES = 0x7d,
	REG_TRANSMIT_SINK_CAPABILITIES_EXTENDED_DATA_BLOCK = 0x7e,
	REG_UUID_HANDLE = 0x80,
	REG_EXTERNAL_DCDC_STATUS = 0x94,
	REG_EXTERNAL_DCDC_PARAMETERS = 0x95,
	REG_EPR_CONFIG = 0x97,
	REG_GPIO_P0 = 0xa0,
	REG_GPIO_P1 = 0xa1,
	REG_GPIO_EVENT_CONFIG = 0xa3
};

/**
 * @brief Standard Task Response
 *
 * Returned in Output DATAX, bits 3:0, when a 4CC Task is sent
 */
enum std_task_response {
	TASK_COMPLETED_SUCCESSFULLY = 0,
	TASK_TIMED_OUT_OR_ABORTED = 1,
	TASK_REJECTED = 3,
	TASK_REJECTED_RX_BUFFER_LOCKED = 4,
};

/**
 * @brief Px_EXT VBUS Switch Status
 */
enum px_ext_vbus_sw {
	EXT_VBUS_SWITCH_DISABLED = 0,
	EXT_VBUS_SWITCH_DISABLED_FAULT = 1,
	EXT_VBUS_SWITCH_ENABLED_OUTPUT = 2,
	EXT_VBUS_SWITCH_ENABLED_INPUT = 3,
};

/**
 * @brief Chip operating modes
 */
enum tps_mode {
	/** Chip is booting */
	REG_MODE_BOOT = 0x544f4f42,
	/** Firmware update / both banks corrupted */
	REG_MODE_F211 = 0x31313246,
	/** Flash code running pre-appconfig */
	REG_MODE_APP0 = 0x30505041,
	/** Flash code running post-appconfig */
	REG_MODE_APP1 = 0x31505041,
	/** Flash code is waiting for power */
	REG_MODE_WTPR = 0x52505457,
};

/**
 * @brief Command "Trig" gpio input events
 */
enum trig_gpio_events {
	FALLING_EDGE = 0x0,
	RISING_EDGE = 0x1,
	EVENT_MRESET = 0x45,
	EVENT_I2C3_CNTLR_IRQ = 0x38,
	EVENT_RETIMER_SOC_OVR_FORCE_PWR = 0x2A,
	EVENT_FAULT_INPUT_EVENT_PORT2 = 0x22,
	EVENT_FAULT_INPUT_EVENT_PORT1 = 0x21,
};

/**
 * @brief 4.4 Mode Register (Offset = 0x03)
 *
 * Indicates the operational state of a port.
 */
union reg_mode {
	struct {
		uint8_t data[4];
	} __packed;
	uint8_t raw_value[4];
};

/* Values to be written to the CMD registers, indicating the task to be started
 * by the PDC. The command field is nominally a 4-byte ASCII string, not
 * null-terminated. The values of this enum are the corresponding little-endian,
 * uint32_t values for each string of 4 bytes. These values are listed in
 * TPS6699x TRM chapter 10, 4CC Task Detailed Descriptions.
 */

/* Helper function to convert TI task names to UINT32*/
#define TASK_TO_UINT32(a, b, c, d) \
	((uint32_t)(a | (b << 8) | (c << 16) | (d << 24)))

enum command_task {
	/* Command complete: Not a real command. The TPS6699x clears the command
	 * register when a command completes.
	 */
	COMMAND_TASK_COMPLETE = 0,
	/* Invalid command */
	COMMAND_TASK_NO_COMMAND = TASK_TO_UINT32('!', 'C', 'M', 'D'),
	/* Cold reset request */
	COMMAND_TASK_GAID = TASK_TO_UINT32('G', 'A', 'I', 'D'),
	/* Simulate port disconnect */
	COMMAND_TASK_DISC = TASK_TO_UINT32('D', 'I', 'S', 'C'),
	/* Firmware update tasks */
	COMMAND_TASK_TFUS = TASK_TO_UINT32('T', 'F', 'U', 's'),
	COMMAND_TASK_TFUC = TASK_TO_UINT32('T', 'F', 'U', 'c'),
	COMMAND_TASK_TFUD = TASK_TO_UINT32('T', 'F', 'U', 'd'),
	COMMAND_TASK_TFUE = TASK_TO_UINT32('T', 'F', 'U', 'e'),
	COMMAND_TASK_TFUI = TASK_TO_UINT32('T', 'F', 'U', 'i'),
	COMMAND_TASK_TFUQ = TASK_TO_UINT32('T', 'F', 'U', 'q'),
	/* Abort current task */
	COMMAND_TASK_ABRT = TASK_TO_UINT32('A', 'B', 'R', 'T'),
	/* Start discovery process */
	COMMAND_TASK_AMDS = TASK_TO_UINT32('A', 'M', 'D', 's'),
	/* SBUd command */
	COMMAND_TASK_SBUD = TASK_TO_UINT32('S', 'B', 'U', 'd'),
	/* SBDF command */
	COMMAND_TASK_SBDF = TASK_TO_UINT32('S', 'B', 'D', 'F'),
	/* Data role swap to DFP */
	COMMAND_TASK_SWDF = TASK_TO_UINT32('S', 'W', 'D', 'F'),
	/* Data role swap to UFP */
	COMMAND_TASK_SWUF = TASK_TO_UINT32('S', 'W', 'U', 'F'),
	/* Send VDM */
	COMMAND_TASK_VDMS = TASK_TO_UINT32('V', 'D', 'M', 's'),
};
BUILD_ASSERT(sizeof(enum command_task) == sizeof(uint32_t));

/**
 * @brief 4.8 Command Register for I2C1 (Offset = 0x08)
 * @brief 4.12 Command Register for I2C2 (Offset = 0x10)
 *
 * Command register for the primary command interface. If an unrecognized
 * command is written to this register, it is replaced by a 4CC value of "!CMD".
 */
union reg_command {
	struct {
		uint32_t command : 32;
	} __packed;
	uint8_t raw_value[4];
};

/**
 * @brief 4.9 Data Register for CMD1 (Offset = 0x09)
 * @brief 4.13 Data Register for CMD2 (Offset = 0x11)
 *
 * Data register for the primary command interface.
 */
union reg_data {
	struct {
		uint8_t data[64];
	} __packed;
	uint8_t raw_value[64];
};

/**
 * @brief 4.11 Version Register (Offset = 0x0f)
 *
 * Boot Firmware Version
 */
union reg_version {
	struct {
		uint32_t version : 32;
	} __packed;
	uint8_t raw_value[4];
};

/**
 * @brief 4.5 Customer Use (Offset = 0x06)
 *
 * Customer Version Register
 */
union reg_customer_use {
	struct {
		uint32_t app_config_version;
		uint32_t fw_version; /* Changed from 32 bytes to 32 bits */
	} __packed;
	uint8_t raw_value[8]; /* Updated size to 4 + 4 = 8 bytes */
};

/**
 * @brief 4.14 Interrupt Event for I2C1 (Offset = 0x14)
 *        4.15 Interrupt Event for I2C2 (Offset = 0x15)
 *        4.16 Interrupt Mask for I2C1 (Offset = 0x16)
 *        4.17 Interrupt Mask for I2C2 (Offset = 0x17)
 *        4.18 Interrupt Clear for I2C1 (Offset = 0x18)
 *        4.19 Interrupt Clear for I2C2 (Offset = 0x19)
 *
 * Interrupt Event:
 *   Interrupt event bit field for I1C_EC_IRQ. If any bit is 1, then the
 * I2C_EC_IRQ pin is pulled low.
 *
 * Interrupt Mask:
 *   Interrupt mask bit field for INT_EVENT. A bit cannot be set if it is
 * cleared in this register.
 *
 * Interrupt Clear:
 *   Interrpt clear bit field for INT_EVENT. Bits set in this register are
 * cleared from INT_EVENT.
 *
 */
union reg_interrupt {
	struct {
		/* Bits 0 - 7 */
		uint8_t reserved0 : 1;
		uint8_t pd_hardreset : 1;
		uint8_t reserved1 : 1;
		uint8_t plug_insert_or_removal : 1;
		uint8_t power_swap_complete : 1;
		uint8_t data_swap_complete : 1;
		uint8_t fr_swap_complete : 1;
		uint8_t source_cap_updated : 1;

		/* Bits 8 - 15 */
		uint8_t sink_ready : 1;
		uint8_t overcurent : 1;
		uint8_t attention_received : 1;
		uint8_t vdm_received : 1;
		uint8_t new_contract_as_consumer : 1;
		uint8_t new_contract_as_producer : 1;
		uint8_t source_caps_msg_received : 1;
		uint8_t sink_caps_msg_received : 1;

		/* Bits 16 - 23 */
		uint8_t reserved3 : 1;
		uint8_t power_swap_rquested : 1;
		uint8_t data_swap_requested : 1;
		uint8_t reserved4 : 1;
		uint8_t usb_host_present : 1;
		uint8_t usb_host_no_longer_present : 1;
		uint8_t reserved5 : 1;
		uint8_t power_path_switch_changed : 1;

		/* Bits 24 - 31 */
		uint8_t power_status_update : 1;
		uint8_t data_status_update : 1;
		uint8_t status_updated : 1;
		uint8_t pd_status_updated : 1;
		uint8_t reserved6 : 2;
		uint8_t cmd1_complete : 1;
		uint8_t cmd2_complete : 1;

		/* Bits 32 - 39 */
		uint8_t device_incompatible_error : 1;
		uint8_t cannot_provide_voltage_or_current_error : 1;
		uint8_t can_provide_voltage_or_current_later_error : 1;
		uint8_t power_event_occurred_error : 1;
		uint8_t missing_get_caps_msg_error : 1;
		uint8_t reserved7 : 1;
		uint8_t protocol_error : 1;
		uint8_t reserved8 : 1;

		/* Bits 40 - 47 */
		uint8_t reserved9 : 2;
		uint8_t sink_transition_completeed : 1;
		uint8_t plug_early_notification : 1;
		uint8_t prochot_notification : 1;
		uint8_t ucsi_connector_status_change_notification : 1;
		uint8_t unable_to_source_error : 1;
		uint8_t reserved11 : 1;

		/* Bits 48 - 55 */
		uint8_t am_entry_fail : 1;
		uint8_t am_entered : 1;
		uint8_t reserved12 : 1;
		uint8_t discover_mode_completed : 1;
		uint8_t exit_mode_completed : 1;
		uint8_t data_reset_start : 1;
		uint8_t usb_status_update : 1;
		uint8_t connection_manager_update : 1;

		/* Bits 56 - 63 */
		uint8_t usvid_mode_entered : 1;
		uint8_t usvid_mode_exited : 1;
		uint8_t usvid_attention_vdm_received : 1;
		uint8_t usvid_other_vdm_received : 1;
		uint8_t reserved13 : 1;
		uint8_t externl_dcdc_event_received : 1;
		uint8_t dp_sid_status_updated : 1;
		uint8_t intel_vid_status_updated : 1;

		/* Bits 64 - 71 */
		uint8_t pd3_status_updated : 1;
		uint8_t tx_memory_buffer_empty : 1;
		uint8_t mbrd_bufer_ready : 1;
		uint8_t reserved14 : 3;
		uint8_t event_soc_ack_timeout : 1;
		uint8_t not_supported_received : 1;

		/* Bits 72 - 79 */
		uint8_t reserved15 : 2;
		uint8_t i2c_comm_error_with_external_PP : 1;
		uint8_t externl_dcdc_status_change : 1;
		uint8_t frs_signal_received : 1;
		uint8_t chunk_response_received : 1;
		uint8_t chunk_request_received : 1;
		uint8_t alert_message_received : 1;

		/* Bits 80 - 87 */
		uint8_t patch_loaded : 1;
		uint8_t ready_for_f211_image : 1;
		uint8_t reserved16 : 2;
		uint8_t boot_error : 1;
		uint8_t ready_for_next_data_block : 1;
		uint8_t reserved17 : 2;
	} __packed;
	uint8_t raw_value[11];
};

/**
 * @brief 4.20 Status Register (Offset = 0x1a)
 */
union reg_status {
	struct {
		/* Byte 0 */
		uint8_t plug_present : 1;
		uint8_t connection_state : 3;
		uint8_t plug_orientation : 1;
		uint8_t port_role : 1;
		uint8_t data_role : 1;
		uint8_t epr_mode_is_active : 1;

		/* Byte 1 */
		uint8_t reserved0;

		/* Byte 2 */
		uint8_t reserved1 : 4;
		uint8_t vbus_status : 2;
		uint8_t usb_host_present : 2;

		/* Byte 3 */
		uint8_t acting_as_legacy : 2;
		uint8_t reserved2 : 1;
		uint8_t bist : 1;
		uint8_t reserved3 : 2;
		uint8_t soc_ack_timeout : 1;
		uint8_t reserved4 : 1;

		/* Byte 4 */
		uint8_t am_status : 2;
		uint8_t reserved5 : 6;
	} __packed;
	uint8_t raw_value[5];
};

/**
 * @brief 4.26 Port Configuration Register (Offset = 0x28)
 */
union reg_port_config {
	struct {
		/* Bit 0 - 7 */
		uint8_t reserved0 : 6;
		uint8_t debug_accessory_ad_enable : 1;
		uint8_t reserved1 : 1;
		uint8_t reserved2[7];
	} __packed;
	uint8_t raw_value[8];
};

/**
 * @brief 4.49 Received SOP Identity Data Object Register (Offset = 48h)
 */
union reg_rx_identity_sop {
	struct {
		uint8_t num_valid_vdos : 3;
		uint8_t reserved : 3;
		uint8_t response_type : 2;
		uint32_t vdo[6];
	} __packed;
	uint8_t raw_value[25];
};

#define GAID_MAGIC_VALUE 0xAC
union gaid_params {
	struct {
		uint8_t switch_banks;
		uint8_t copy_banks;
	} __packed;
	uint8_t raw[2];
};

#endif /* ZEPHYR_DRIVERS_USBC_TPS6699X_REG_H_ */
