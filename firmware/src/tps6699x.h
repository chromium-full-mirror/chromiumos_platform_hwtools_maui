/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef ZEPHYR_DRIVERS_USBC_TPS6699X_H_
#define ZEPHYR_DRIVERS_USBC_TPS6699X_H_

#include "tps6699x_reg.h"

#include <zephyr/device.h>

/**
 * @brief Read Mode
 *
 * @param dev Pointer to the TPS6699x device instance
 * @param buf pointer where data is stored
 *
 * @return 0 on success, else -EIO
 */
int tps_rd_mode(const struct device *dev, union reg_mode *buf);

/**
 * @brief Read or Write Command for I2C1
 *
 * @param dev Pointer to the TPS6699x device instance
 * @param buf pointer where data is stored
 * @param int flag set to I2C_MSG_READ for read and I2C_MSG_WRITE for write
 *
 * @return 0 on success, else -EIO
 */
int tps_rw_command_for_i2c1(const struct device *dev, union reg_command *buf,
			    int flag);

/**
 * @brief Read or Write Data for command 1
 *
 * @param dev Pointer to the TPS6699x device instance
 * @param buf pointer where data is stored
 * @param len number of bytes to read or write
 * @param int flag set to I2C_MSG_READ for read and I2C_MSG_WRITE for write
 *
 * @return 0 on success, else -EIO
 */
int tps_rw_data_for_cmd1(const struct device *dev, union reg_data *buf,
			 size_t len, int flag);

/**
 * @brief Perform bulk transfers to the PDC
 *
 * @param dev Pointer to the TPS6699x device instance
 * @param broadcast_address I2C address to stream data to
 * @param buf Data payload to transmit
 * @param buf_len Number of bytes from `buf` to transmit
 *
 * @return 0 on success, or negative error code
 */
int tps_stream_data(const struct device *dev, const uint8_t broadcast_address,
		    const uint8_t *buf, size_t buf_len);

/**
 * @brief Read Version
 *
 * @param dev Pointer to the TPS6699x device instance
 * @param buf pointer where data is stored
 *
 * @return 0 on success, else -EIO
 */
int tps_rd_version(const struct device *dev, union reg_version *buf);

/**
 * @brief Read Customer Use
 *
 * @param dev Pointer to the TPS6699x device instance
 * @param buf pointer where data is stored
 *
 * @return 0 on success, else -EIO
 */
int tps_rd_customer_use(const struct device *dev, union reg_customer_use *buf);

/**
 * @brief Run a 4CC command synchronously
 *
 * @param dev Pointer to the TPS6699x device instance
 * @param task 4CC task to run
 * @param cmd_data pointer to data to write (optional)
 * @param write_len number of bytes to write
 * @param read_len number of bytes to read (optional)
 * @param user_buf pointer where read data is stored (optional)
 *
 * @return 0 on success, else -EIO or -ETIMEDOUT
 */
int run_task_sync(const struct device *dev, enum command_task task,
		  union reg_data *cmd_data, size_t write_len, size_t read_len,
		  uint8_t *user_buf);

/**
 * @brief Run SBUd command to control SBU MUX EN
 *
 * @param i2c device pointer to i2c device
 * @param conn_status 0 to disable, 1 to enable
 *
 * @return 0 on success, else negative error code
 */
int tps_cmd_sbud(const struct device *dev, uint8_t en);

/**
 * @brief Run SBDF command to control SBU MUX polarity
 *
 * @param dev Pointer to the TPS6699x device instance
 * @param flip 0 to not flip, 1 to flip
 *
 * @return 0 on success, else negative error code
 */
int tps_cmd_sbdf(const struct device *dev, uint8_t flip);

/**
 * @brief PD Data Roles
 */
enum tps_pd_data_role {
	TPS_PD_DATA_ROLE_UFP = 0,
	TPS_PD_DATA_ROLE_DFP = 1,
};

/**
 * @brief Run SWDF or SWUF command to perform a data role swap
 *
 * @param dev Pointer to the TPS6699x device instance
 * @param role Data role to swap to (TPS_PD_DATA_ROLE_UFP or TPS_PD_DATA_ROLE_DFP)
 *
 * @return 0 on success, else negative error code
 */
int tps_cmd_data_role_swap(const struct device *dev, enum tps_pd_data_role role);

/**
 * @brief Run DISC command to simulate port disconnect
 *
 * @param dev Pointer to the TPS6699x device instance
 * @param delay Delay in seconds before automatic reconnect. 0 for no automatic
 * reconnect.
 *
 * @return 0 on success, else negative error code
 */
int tps_cmd_disc(const struct device *dev, uint8_t delay);

/**
 * @brief Run GAID command to perform cold reset
 *
 * @param dev Pointer to the TPS6699x device instance
 * @param switch_banks If true, switch to the other firmware bank
 *
 * @return 0 on success, else negative error code
 */
int tps_cmd_gaid(const struct device *dev, bool switch_banks);

/**
 * @brief Structure to hold decoded identity information
 */
struct tps_pd_identity {
	bool usb_host;
	bool usb_device;
	uint16_t vid;
	uint16_t pid;
};

/**
 * @brief Issue Discovery Identity command and get raw response
 *
 * @param dev Pointer to the TPS6699x device instance
 * @param rx_id Pointer to store the raw identity register content
 *
 * @return 0 on success, else negative error code
 */
int tps_discover_identity(const struct device *dev,
			  union reg_rx_identity_sop *rx_id);

/**
 * @brief Read Status
 *
 * @param dev Pointer to the TPS6699x device instance
 * @param buf pointer where data is stored
 *
 * @return 0 on success, else -EIO
 */
int tps_rd_status(const struct device *dev, union reg_status *buf);

/**
 * @brief Read Port Configuration
 *
 * @param dev Pointer to the TPS6699x device instance
 * @param buf pointer where data is stored
 *
 * @return 0 on success, else -EIO
 */
int tps_rd_port_config(const struct device *dev, union reg_port_config *buf);

/**
 * @brief Write Port Configuration
 *
 * @param dev Pointer to the TPS6699x device instance
 * @param buf pointer where data is stored
 *
 * @return 0 on success, else -EIO
 */
int tps_wr_port_config(const struct device *dev, union reg_port_config *buf);

/**
 * @brief Decode raw identity register into structured data
 *
 * @param rx_id Pointer to the raw identity register content
 * @param id Pointer to the structure to fill with decoded data
 */
void tps_decode_identity(const union reg_rx_identity_sop *rx_id,
			 struct tps_pd_identity *id);

#endif /* ZEPHYR_DRIVERS_USBC_TPS6699X_H_ */
