/*
 * Copyright (c) 2026 Bert Outtier <outtierbert@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_MFD_FRI3D_DJ_ADDON_H_
#define ZEPHYR_INCLUDE_DRIVERS_MFD_FRI3D_DJ_ADDON_H_

/**
 * @defgroup mdf_interface_fri3d_dj_addon Fri3d Camp 2026 DJ add-on MFD
 * @ingroup mfd_interfaces
 *
 * The DJ add-on of the Fri3d Camp 2026 badge has a CH32X035 that runs the
 * dj_2026 firmware and acts as an I2C slave with a register map: 8 buttons,
 * 6 potmeters, 3 faders, 2 encoders and 8 RGB LEDs. It sends MIDI on its UART
 * on every input change: the driver reads the inputs when data arrives there.
 * The add-on is optional: the driver looks for it for a few seconds after
 * boot (plugging it in or out at runtime is not supported). The child devices
 * (input, LED strip) use the functions below.
 * @{
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @name Registers (little endian)
 * @{
 */
#define FRI3D_DJ_ADDON_REG_VERSION 0x00 /**< 3 bytes: major, minor, patch (R) */
#define FRI3D_DJ_ADDON_REG_BUTTONS 0x03 /**< 1 byte: button bits, 1 = pressed (R) */
#define FRI3D_DJ_ADDON_REG_ANALOG  0x04 /**< 9 x 2 bytes: potmeters and faders, 12 bit (R) */
#define FRI3D_DJ_ADDON_REG_ENCODER 0x16 /**< 2 x 2 bytes: encoder counters 0-127 (R) */
#define FRI3D_DJ_ADDON_REG_LEDS    0x1a /**< 8 x 3 bytes: LED colors, G R B (R/W) */
#define FRI3D_DJ_ADDON_REG_CONFIG  0x32 /**< 1 byte: FRI3D_DJ_ADDON_CONFIG_* bits (R/W) */
/** @} */

/** @name Configuration register bits
 * @{
 */
#define FRI3D_DJ_ADDON_CONFIG_REMAP BIT(0) /**< write 1: I2C pins become SWD */
/** @} */

#define FRI3D_DJ_ADDON_BUTTONS  8 /**< Number of buttons */
#define FRI3D_DJ_ADDON_ANALOG   9 /**< Number of potmeters and faders */
#define FRI3D_DJ_ADDON_ENCODERS 2 /**< Number of encoders */
#define FRI3D_DJ_ADDON_LEDS     8 /**< Number of RGB LEDs */

/** Input registers of the add-on, as read from FRI3D_DJ_ADDON_REG_BUTTONS on */
struct mfd_fri3d_dj_addon_inputs {
	/** Button bits, 1 = pressed */
	uint8_t buttons;
	/** Raw 12 bit ADC values of the potmeters and faders */
	uint16_t analog[FRI3D_DJ_ADDON_ANALOG];
	/** Encoder counters, wrapping at 128 */
	uint16_t encoder[FRI3D_DJ_ADDON_ENCODERS];
};

/**
 * @brief Called with the new inputs, or with NULL when a firmware update starts.
 *
 * Called from the system work queue, when a read found a change. The inputs
 * are read when the add-on sends MIDI on its UART.
 */
typedef void (*mfd_fri3d_dj_addon_input_cb_t)(const struct device *dev,
					      const struct mfd_fri3d_dj_addon_inputs *inputs,
					      void *user_data);

/**
 * @brief Read registers.
 *
 * @param dev DJ add-on MFD device.
 * @param reg First register.
 * @param data Buffer for the register contents.
 * @param len Number of bytes to read.
 * @retval 0 on success, negative errno otherwise.
 */
int mfd_fri3d_dj_addon_reg_read(const struct device *dev, uint8_t reg, void *data, size_t len);

/**
 * @brief Write registers.
 *
 * @param dev DJ add-on MFD device.
 * @param reg First register.
 * @param data Register contents.
 * @param len Number of bytes to write.
 * @retval 0 on success, negative errno otherwise.
 */
int mfd_fri3d_dj_addon_reg_write(const struct device *dev, uint8_t reg, const void *data,
				 size_t len);

/** Counters of the driver, since boot */
struct mfd_fri3d_dj_addon_stats {
	/** Bytes received on the UART (the add-on's MIDI output) */
	uint32_t uart_bytes;
	/** UART receptions that scheduled a read of the inputs */
	uint32_t triggers;
	/** Reads of the inputs */
	uint32_t reads;
	/** Reads of the inputs that failed */
	uint32_t read_errors;
	/** Error of the last failed read */
	int last_error;
};

/**
 * @brief Get the counters of the driver.
 */
void mfd_fri3d_dj_addon_get_stats(const struct device *dev, struct mfd_fri3d_dj_addon_stats *stats);

/**
 * @brief Get the firmware version of the add-on, read at init (and after an update).
 *
 * @param dev DJ add-on MFD device.
 * @param version Buffer for major, minor and patch.
 */
void mfd_fri3d_dj_addon_get_version(const struct device *dev, uint8_t version[3]);

/**
 * @brief Set the callback for input changes (one per add-on).
 *
 * @param dev DJ add-on MFD device.
 * @param cb Callback, NULL to remove it.
 * @param user_data Passed to the callback.
 */
void mfd_fri3d_dj_addon_set_input_callback(const struct device *dev,
					   mfd_fri3d_dj_addon_input_cb_t cb, void *user_data);

/**
 * @brief Firmware update progress.
 *
 * @param done Bytes written.
 * @param total Bytes to write.
 * @param user_data User data passed to mfd_fri3d_dj_addon_update().
 */
typedef void (*mfd_fri3d_dj_addon_progress_cb_t)(size_t done, size_t total, void *user_data);

/**
 * @brief Called by mfd_fri3d_dj_addon_update() before the SWD traffic starts.
 *
 * The add-on has given its I2C pins to SWD: switch off or keep away what must
 * not see that traffic (it starts like an I2C start condition), e.g. the other
 * devices on the bus and the bus pull-ups.
 *
 * @param dev DJ add-on MFD device.
 * @param user_data As given to mfd_fri3d_dj_addon_set_update_callbacks().
 */
typedef void (*mfd_fri3d_dj_addon_pre_update_cb_t)(const struct device *dev, void *user_data);

/**
 * @brief Called by mfd_fri3d_dj_addon_update() after the SWD session.
 *
 * The pins are I2C again: undo what the pre-update callback did, e.g. switch
 * the other devices on the bus on again and restart them.
 *
 * @param dev DJ add-on MFD device.
 * @param result 0 when the add-on runs the new firmware, else a negative errno.
 * @param user_data As given to mfd_fri3d_dj_addon_set_update_callbacks().
 */
typedef void (*mfd_fri3d_dj_addon_post_update_cb_t)(const struct device *dev, int result,
						    void *user_data);

/**
 * @brief Set the callbacks around a firmware update (one pair per device).
 *
 * Needs CONFIG_MFD_FRI3D_DJ_ADDON_UPDATE. Both are optional (NULL).
 *
 * @param dev DJ add-on MFD device.
 * @param pre Called before the SWD traffic starts.
 * @param post Called after the SWD session.
 * @param user_data Passed to both.
 */
void mfd_fri3d_dj_addon_set_update_callbacks(const struct device *dev,
					     mfd_fri3d_dj_addon_pre_update_cb_t pre,
					     mfd_fri3d_dj_addon_post_update_cb_t post,
					     void *user_data);

/**
 * @brief Install new add-on firmware over RVSWD.
 *
 * Needs CONFIG_MFD_FRI3D_DJ_ADDON_UPDATE and the rvswd property. The firmware
 * gives its I2C pins (the expansion I2C bus) to SWD on request (remap); the
 * add-on is programmed (every page is read back) and restarts into the new
 * firmware; the pins go back to I2C and the driver reads its version again
 * (input callback with NULL first, then all inputs). The expansion I2C bus is
 * unavailable meanwhile, about 10 seconds for 11 kB. The callbacks of
 * mfd_fri3d_dj_addon_set_update_callbacks() run around the SWD session, e.g.
 * to switch the other devices on the bus off while it runs.
 *
 * @param dev DJ add-on MFD device.
 * @param image Firmware binary (dj_2026 release build).
 * @param len Image length.
 * @param cb Progress callback, or NULL.
 * @param user_data Passed to the callback.
 * @retval 0 on success.
 * @retval -ENOTSUP without the rvswd property.
 * @retval -ETIMEDOUT if the add-on did not answer on I2C after the update.
 * @retval -ENODEV if the RVSWD interface is not ready or the target is not a
 *         CH32X03x.
 * @retval other negative errno on a programming error.
 */
int mfd_fri3d_dj_addon_update(const struct device *dev, const void *image, size_t len,
			      mfd_fri3d_dj_addon_progress_cb_t cb, void *user_data);

/**
 * @brief Get the version of the bundled firmware image.
 *
 * The image comes from CONFIG_MFD_FRI3D_DJ_ADDON_FIRMWARE, its version from
 * CONFIG_MFD_FRI3D_DJ_ADDON_FIRMWARE_VERSION. Together with
 * mfd_fri3d_dj_addon_get_version() an application can keep the add-on on the
 * bundled version, e.g. at start:
 *
 * @code{.c}
 * uint8_t running[3], bundled[3];
 *
 * if (mfd_fri3d_dj_addon_get_bundled_version(bundled) == 0) {
 *         if (device_is_ready(dev)) {
 *                 mfd_fri3d_dj_addon_get_version(dev, running);
 *         }
 *         if (!device_is_ready(dev) || memcmp(running, bundled, sizeof(bundled)) < 0) {
 *                 mfd_fri3d_dj_addon_update_bundled(dev, NULL, NULL);
 *         }
 * }
 * @endcode
 *
 * An add-on without firmware does not answer and its device does not
 * initialize; the update still installs firmware on it (restart afterwards).
 * Debug builds of the add-on firmware report 255.255.255, so they are left
 * alone. Needs CONFIG_MFD_FRI3D_DJ_ADDON_UPDATE.
 *
 * @param version Buffer for major, minor and patch.
 *
 * @retval 0 Success.
 * @retval -ENOENT No bundled image.
 */
int mfd_fri3d_dj_addon_get_bundled_version(uint8_t version[3]);

/**
 * @brief Program the bundled firmware image into the add-on.
 *
 * As mfd_fri3d_dj_addon_update() with the image of
 * CONFIG_MFD_FRI3D_DJ_ADDON_FIRMWARE. Needs CONFIG_MFD_FRI3D_DJ_ADDON_UPDATE.
 *
 * @param dev DJ add-on MFD device.
 * @param cb Progress callback, or NULL.
 * @param user_data Passed to @p cb.
 *
 * @retval 0 Success: the add-on runs the bundled firmware.
 * @retval -ENOENT No bundled image.
 * @retval <0 As mfd_fri3d_dj_addon_update().
 */
int mfd_fri3d_dj_addon_update_bundled(const struct device *dev,
				      mfd_fri3d_dj_addon_progress_cb_t cb, void *user_data);

#ifdef __cplusplus
}
#endif

/** @} */

#endif /* ZEPHYR_INCLUDE_DRIVERS_MFD_FRI3D_DJ_ADDON_H_ */
