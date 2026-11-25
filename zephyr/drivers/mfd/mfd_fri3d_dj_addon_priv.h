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
	uint32_t boot_timeout_ms;
#ifdef CONFIG_MFD_FRI3D_DJ_ADDON_UPDATE
	/* RVSWD interface on the add-on's I2C pins, or NULL */
	const struct device *rvswd;
#endif
};

struct mfd_fri3d_dj_addon_data {
	const struct device *dev;
	struct k_work_delayable work;
	/* a firmware update is running: the I2C pins are SWD */
	bool updating;
	/* Report the inputs even if they did not change (after init or an update) */
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
#ifdef CONFIG_MFD_FRI3D_DJ_ADDON_UPDATE
	mfd_fri3d_dj_addon_pre_update_cb_t pre_update;
	mfd_fri3d_dj_addon_post_update_cb_t post_update;
	void *update_user_data;
#endif
};


/* Wait until the add-on answers on I2C, and read its version */
int mfd_fri3d_dj_addon_wait(const struct device *dev, k_timepoint_t timeout);

/*
 * At init, with CONFIG_MFD_FRI3D_DJ_ADDON_UPDATE_AT_BOOT: install the bundled
 * firmware when the add-on runs an older one or does not answer (answered:
 * the result of waiting for it). Returns 0 when it answers now.
 */
int mfd_fri3d_dj_addon_update_at_boot(const struct device *dev, int answered);

/* What the update code holds, for the shell (CONFIG_MFD_FRI3D_DJ_ADDON_UPDATE) */
struct mfd_fri3d_dj_addon_update_info {
	/* bundled image (CONFIG_MFD_FRI3D_DJ_ADDON_FIRMWARE): size, 0 without one */
	size_t bundled_len;
	uint8_t bundled_version[3];
	/* successful updates since boot */
	unsigned int updates;
	/* the update at boot installed the bundled image */
	bool updated_at_boot;
};

void mfd_fri3d_dj_addon_get_update_info(struct mfd_fri3d_dj_addon_update_info *info);

/* Install the bundled image: -ENOENT without one */
int mfd_fri3d_dj_addon_update_bundled(const struct device *dev,
				      mfd_fri3d_dj_addon_progress_cb_t cb, void *user_data);

#endif /* ZEPHYR_DRIVERS_MFD_MFD_FRI3D_DJ_ADDON_PRIV_H_ */
