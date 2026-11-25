/*
 * Copyright (c) 2026 Bert Outtier <outtierbert@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared by the DJ add-on's MFD driver (mfd_fri3d_dj_addon.c) and its
 * firmware update over RVSWD (mfd_fri3d_dj_addon_update.c).
 */

#ifndef ZEPHYR_DRIVERS_MFD_MFD_FRI3D_DJ_ADDON_PRIV_H_
#define ZEPHYR_DRIVERS_MFD_MFD_FRI3D_DJ_ADDON_PRIV_H_

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/mfd/fri3d_dj_addon.h>
#include <zephyr/kernel.h>

struct mfd_fri3d_dj_addon_config {
	struct i2c_dt_spec i2c;
	/* UART that receives the add-on's MIDI output */
	const struct device *uart;
	uint32_t poll_period_ms;
	uint32_t probe_period_ms;
	uint32_t connect_timeout_ms;
#ifdef CONFIG_MFD_FRI3D_DJ_ADDON_UPDATE
	/* RVSWD interface on the add-on's I2C pins, or NULL */
	const struct device *rvswd;
	/* enable of the power domain of the other devices on the bus, or none */
	struct gpio_dt_spec update_power;
#endif
};

struct mfd_fri3d_dj_addon_data {
	const struct device *dev;
	struct k_work_delayable work;
	bool connected;
	/* stop looking for the add-on */
	k_timepoint_t connect_end;
	/* a firmware update is running: the I2C pins are SWD */
	bool updating;
	/* Report the inputs even if they did not change (after connecting) */
	bool report;
	/* data arrived on the UART since the last read started */
	atomic_t uart_data;
	/* k_uptime_get_32() at the start of the last read */
	uint32_t last_read;
	struct mfd_fri3d_dj_addon_stats stats;
	uint8_t version[3];
	struct mfd_fri3d_dj_addon_inputs inputs;
	mfd_fri3d_dj_addon_input_cb_t cb;
	void *user_data;
};

/*
 * An add-on connected, with CONFIG_MFD_FRI3D_DJ_ADDON_UPDATE_ON_CONNECT:
 * install the bundled firmware when it runs an older one (once per boot).
 */
void mfd_fri3d_dj_addon_update_on_connect(const struct device *dev);

#endif /* ZEPHYR_DRIVERS_MFD_MFD_FRI3D_DJ_ADDON_PRIV_H_ */
