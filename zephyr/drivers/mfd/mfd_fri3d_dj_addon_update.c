/*
 * Copyright (c) 2026 Bert Outtier <outtierbert@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Firmware update of the Fri3d Camp 2026 DJ add-on over RVSWD
 * (CONFIG_MFD_FRI3D_DJ_ADDON_UPDATE): the add-on gives its I2C pins to SWD
 * on request, the zephyr-rvswd module programs it, and the pins go back to
 * I2C. Also the bundled image, its installation on connect and the shell.
 */

#define DT_DRV_COMPAT fri3d_dj_addon

#include <stdlib.h>
#include <string.h>

#include <rvswd/ch32.h>
#include <rvswd/rvswd.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>

#include "mfd_fri3d_dj_addon_priv.h"

LOG_MODULE_DECLARE(mfd_fri3d_dj_addon, CONFIG_MFD_LOG_LEVEL);

/* successful updates since boot (for the shell) */
static unsigned int mfd_fri3d_dj_addon_updates;

/* Delay for the add-on to give its I2C pins to SWD after the remap command */
#define REMAP_DELAY_MS 200
/* Time for the other devices on the bus to power down and up */
#define POWER_DELAY_MS 50
/* Debug module status register: answers when a target is there */
#define DMSTATUS       0x11

/*
 * Find the add-on on SWD and halt it before it runs any code. A target that
 * just powered up needs the interface reset of rvswd_connect() before it
 * answers, so redo it for every probe.
 */
static int mfd_fri3d_dj_addon_find(const struct device *rvswd, k_timeout_t wait,
				   uint32_t vendor[4])
{
	k_timepoint_t end = sys_timepoint_calc(wait);

	do {
		uint32_t status;

		if (rvswd_connect(rvswd) == 0 && rvswd_read(rvswd, DMSTATUS, &status) == 0 &&
		    ch32_identify(rvswd, vendor) == 0) {
			return 0;
		}
		k_msleep(2);
	} while (!sys_timepoint_expired(end));

	return -ETIMEDOUT;
}

int mfd_fri3d_dj_addon_update(const struct device *dev, const void *image, size_t len,
			      mfd_fri3d_dj_addon_progress_cb_t cb, void *user_data,
			      k_timeout_t wait)
{
	const struct mfd_fri3d_dj_addon_config *config = dev->config;
	struct mfd_fri3d_dj_addon_data *data = dev->data;
	struct k_work_sync sync;
	uint32_t vendor[4];
	const char *chip;
	int ret;

	if (config->rvswd == NULL) {
		return -ENOTSUP;
	}
	if (!device_is_ready(config->rvswd) ||
	    (config->update_power.port != NULL && !gpio_is_ready_dt(&config->update_power))) {
		return -ENODEV;
	}

	data->updating = true;
	k_work_cancel_delayable_sync(&data->work, &sync);

	if (data->connected) {
		uint8_t cfg = FRI3D_DJ_ADDON_CONFIG_REMAP;

		/* firmware with the configuration register gives its pins to SWD */
		mfd_fri3d_dj_addon_reg_write(dev, FRI3D_DJ_ADDON_REG_CONFIG, &cfg, 1);
		data->connected = false;
		if (data->cb != NULL) {
			data->cb(dev, NULL, data->user_data);
		}
		k_msleep(REMAP_DELAY_MS);
	}

	/*
	 * The other devices on the bus must not see the SWD traffic (it starts
	 * like an I2C start condition): switch them off. The add-on has its own
	 * supply. After the remap command: the bus pull-ups are on that rail too.
	 */
	if (config->update_power.port != NULL) {
		gpio_pin_set_dt(&config->update_power, 0);
		k_msleep(POWER_DELAY_MS);
	}

	ret = mfd_fri3d_dj_addon_find(config->rvswd, wait, vendor);
	if (ret != 0) {
		LOG_ERR("%s: no add-on on SWD", dev->name);
	} else {
		chip = ch32_chip_name(vendor);
		if (chip == NULL || strncmp(chip, "CH32X03", 7) != 0) {
			LOG_ERR("%s: not a CH32X03x (vendor bytes %08x %08x)", dev->name,
				vendor[0], vendor[1]);
			ret = -ENODEV;
		} else {
			LOG_INF("%s: programming %s (%zu bytes)", dev->name, chip, len);
			/* ends with a reset into the new firmware */
			ret = ch32x03x_program(config->rvswd, image, len, cb, user_data);
			if (ret != 0) {
				LOG_ERR("%s: programming failed: %d", dev->name, ret);
			}
		}
		if (ret != 0) {
			ch32_reset_and_run(config->rvswd);
		}
	}

	if (ret == 0) {
		mfd_fri3d_dj_addon_updates++;
	}

	/* the pins go back to I2C, the add-on answers again one second after it starts */
	rvswd_disconnect(config->rvswd);
	if (config->update_power.port != NULL) {
		gpio_pin_set_dt(&config->update_power, 1);
		k_msleep(POWER_DELAY_MS);
	}
	data->updating = false;
	data->connect_end = sys_timepoint_calc(K_MSEC(config->connect_timeout_ms));
	k_work_reschedule(&data->work, K_NO_WAIT);

	return ret;
}

#ifdef FRI3D_DJ_ADDON_HAS_FIRMWARE
static const uint8_t mfd_fri3d_dj_addon_firmware[] = {
#include "fri3d_dj_addon_firmware.inc"
};

static void mfd_fri3d_dj_addon_bundled_version(uint8_t version[3])
{
	const char *p = CONFIG_MFD_FRI3D_DJ_ADDON_FIRMWARE_VERSION;

	for (int i = 0; i < 3; i++) {
		char *end;

		version[i] = strtoul(p, &end, 10);
		p = *end == '.' ? end + 1 : end;
	}
}
#endif

/*
 * After an update that switched off the other devices on the bus (display,
 * touch, IMU, ...): their drivers do not initialize them again, so restart.
 */
static void __maybe_unused mfd_fri3d_dj_addon_restart_after_update(const struct device *dev)
{
	const struct mfd_fri3d_dj_addon_config *config = dev->config;

	if (config->update_power.port == NULL) {
		return;
	}

#ifdef CONFIG_REBOOT
	LOG_INF("%s: restarting, the devices on the bus were switched off", dev->name);
	k_msleep(100);
	sys_reboot(SYS_REBOOT_COLD);
#else
	LOG_WRN("%s: restart: the devices on the bus were switched off", dev->name);
#endif
}

#ifdef CONFIG_MFD_FRI3D_DJ_ADDON_UPDATE_ON_CONNECT
static void mfd_fri3d_dj_addon_update_work(struct k_work *work)
{
	const struct device *dev = DEVICE_DT_INST_GET(0);

	ARG_UNUSED(work);

#ifdef FRI3D_DJ_ADDON_HAS_FIRMWARE
	if (mfd_fri3d_dj_addon_update(dev, mfd_fri3d_dj_addon_firmware,
				      sizeof(mfd_fri3d_dj_addon_firmware), NULL, NULL,
				      K_NO_WAIT) == 0) {
		mfd_fri3d_dj_addon_restart_after_update(dev);
	}
#endif
}

static K_WORK_DEFINE(mfd_fri3d_dj_addon_update_work_item, mfd_fri3d_dj_addon_update_work);

/*
 * An add-on with older firmware connected: install the bundled one, once per
 * boot. Only firmware with the remap command (v1.0.4 and later) gives its pins
 * to SWD now: older firmware only does so during the first second after it
 * powers up, and a failed attempt would leave the devices on the bus, which
 * were switched off, without their initialization.
 */
void mfd_fri3d_dj_addon_update_on_connect(const struct device *dev)
{
#ifdef FRI3D_DJ_ADDON_HAS_FIRMWARE
	static const uint8_t has_remap[3] = {1, 0, 4};
	struct mfd_fri3d_dj_addon_data *data = dev->data;
	static bool tried;
	uint8_t bundled[3];

	mfd_fri3d_dj_addon_bundled_version(bundled);
	if (tried || memcmp(data->version, bundled, sizeof(bundled)) >= 0) {
		return;
	}

	if (memcmp(data->version, has_remap, sizeof(has_remap)) < 0) {
		LOG_WRN("%s: firmware v%u.%u.%u is older than v%u.%u.%u; it cannot be updated "
			"while it runs: use \"djaddon update <seconds>\" and plug it in again",
			dev->name, data->version[0], data->version[1], data->version[2],
			bundled[0], bundled[1], bundled[2]);
		tried = true;
		return;
	}

	tried = true;
	LOG_INF("%s: firmware v%u.%u.%u is older than v%u.%u.%u, updating", dev->name,
		data->version[0], data->version[1], data->version[2], bundled[0], bundled[1],
		bundled[2]);
	k_work_submit(&mfd_fri3d_dj_addon_update_work_item);
#else
	ARG_UNUSED(dev);
#endif
}
#endif /* CONFIG_MFD_FRI3D_DJ_ADDON_UPDATE_ON_CONNECT */

#if defined(CONFIG_SHELL) && defined(FRI3D_DJ_ADDON_HAS_FIRMWARE)
#include <zephyr/shell/shell.h>

static const struct device *const shell_dj_addon = DEVICE_DT_INST_GET(0);

static void mfd_fri3d_dj_addon_shell_progress(size_t done, size_t total, void *user_data)
{
	const struct shell *sh = user_data;

	/* every 25 % */
	if (done == total || (done * 4 / total) != ((done + 256) * 4 / total)) {
		shell_print(sh, "  %u %%", (unsigned int)(done * 100 / total));
	}
}

static int cmd_djaddon_version(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t version[3], bundled[3];

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	mfd_fri3d_dj_addon_bundled_version(bundled);
	if (mfd_fri3d_dj_addon_is_connected(shell_dj_addon)) {
		mfd_fri3d_dj_addon_get_version(shell_dj_addon, version);
		shell_print(sh, "DJ add-on firmware v%u.%u.%u, bundled v%u.%u.%u (%u bytes), "
			    "updates since boot: %u",
			    version[0], version[1], version[2], bundled[0], bundled[1], bundled[2],
			    (unsigned int)sizeof(mfd_fri3d_dj_addon_firmware),
			    mfd_fri3d_dj_addon_updates);
	} else {
		shell_print(sh, "DJ add-on not connected, bundled v%u.%u.%u (%u bytes)", bundled[0],
			    bundled[1], bundled[2],
			    (unsigned int)sizeof(mfd_fri3d_dj_addon_firmware));
	}

	return 0;
}

static int cmd_djaddon_update(const struct shell *sh, size_t argc, char **argv)
{
	unsigned long wait_s = argc > 1 ? strtoul(argv[1], NULL, 10) : 0;
	int ret;

	if (wait_s > 0) {
		shell_print(sh, "plug in the add-on within %lu s (firmware without the remap command)",
			    wait_s);
	}
	shell_print(sh, "installing the bundled DJ add-on firmware");
	ret = mfd_fri3d_dj_addon_update(shell_dj_addon, mfd_fri3d_dj_addon_firmware,
					sizeof(mfd_fri3d_dj_addon_firmware),
					mfd_fri3d_dj_addon_shell_progress, (void *)sh,
					K_SECONDS(wait_s));
	if (ret != 0) {
		shell_error(sh, "update failed: %d", ret);
		return ret;
	}

	/* it answers on I2C one second after it starts */
	for (int i = 0; i < 40 && !mfd_fri3d_dj_addon_is_connected(shell_dj_addon); i++) {
		k_msleep(100);
	}

	cmd_djaddon_version(sh, 0, NULL);
	mfd_fri3d_dj_addon_restart_after_update(shell_dj_addon);

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_djaddon,
	SHELL_CMD(version, NULL, "Add-on and bundled firmware versions", cmd_djaddon_version),
	SHELL_CMD_ARG(update, NULL,
		      "Install the bundled add-on firmware [seconds to wait for a plugged in add-on]",
		      cmd_djaddon_update, 1, 1),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(djaddon, &sub_djaddon, "Fri3d DJ add-on", NULL);
#endif
