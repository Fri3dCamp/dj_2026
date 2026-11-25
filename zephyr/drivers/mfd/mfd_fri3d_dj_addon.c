/*
 * Copyright (c) 2026 Bert Outtier <outtierbert@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * DJ add-on of the Fri3d Camp 2026 badge (CH32X035 with the dj_2026
 * firmware) on the expansion connector.
 *
 * The add-on is optional: the driver looks for it for a few seconds after
 * boot (plugging it in or out at runtime is not supported). Its firmware (a
 * release build) sends a MIDI message on its UART on every input change: data
 * on the UART triggers a read of the inputs over I2C, so they are not polled.
 * It needs firmware v1.0.4 or later: older firmware can return a 16-bit value
 * with its two bytes from before and after an ADC update.
 *
 * With CONFIG_MFD_FRI3D_DJ_ADDON_UPDATE its firmware can be updated over
 * RVSWD on the same pins: the add-on gives its I2C pins to SWD on request.
 */

#define DT_DRV_COMPAT fri3d_dj_addon

#include <stdlib.h>
#include <string.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/mfd/fri3d_dj_addon.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#ifdef CONFIG_MFD_FRI3D_DJ_ADDON_UPDATE
#include <rvswd/ch32.h>
#include <rvswd/rvswd.h>
#include <zephyr/sys/reboot.h>
#endif

LOG_MODULE_REGISTER(mfd_fri3d_dj_addon, CONFIG_MFD_LOG_LEVEL);

/* Input registers: buttons, analog channels and encoders in one read */
#define INPUTS_SIZE   (FRI3D_DJ_ADDON_REG_LEDS - FRI3D_DJ_ADDON_REG_BUTTONS)
/* Writable registers: LEDs and configuration */
#define WRITABLE_SIZE (FRI3D_DJ_ADDON_REG_CONFIG + 1 - FRI3D_DJ_ADDON_REG_LEDS)

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

int mfd_fri3d_dj_addon_reg_read(const struct device *dev, uint8_t reg, void *data, size_t len)
{
	const struct mfd_fri3d_dj_addon_config *config = dev->config;

	return i2c_write_read_dt(&config->i2c, &reg, sizeof(reg), data, len);
}

int mfd_fri3d_dj_addon_reg_write(const struct device *dev, uint8_t reg, const void *data,
				 size_t len)
{
	const struct mfd_fri3d_dj_addon_config *config = dev->config;
	uint8_t buf[1 + WRITABLE_SIZE];

	if (len > WRITABLE_SIZE) {
		return -EINVAL;
	}

	buf[0] = reg;
	memcpy(&buf[1], data, len);

	return i2c_write_dt(&config->i2c, buf, 1 + len);
}

void mfd_fri3d_dj_addon_get_stats(const struct device *dev, struct mfd_fri3d_dj_addon_stats *stats)
{
	struct mfd_fri3d_dj_addon_data *data = dev->data;

	*stats = data->stats;
}

bool mfd_fri3d_dj_addon_is_connected(const struct device *dev)
{
	struct mfd_fri3d_dj_addon_data *data = dev->data;

	return data->connected;
}

void mfd_fri3d_dj_addon_get_version(const struct device *dev, uint8_t version[3])
{
	struct mfd_fri3d_dj_addon_data *data = dev->data;

	memcpy(version, data->version, sizeof(data->version));
}

void mfd_fri3d_dj_addon_set_input_callback(const struct device *dev,
					   mfd_fri3d_dj_addon_input_cb_t cb, void *user_data)
{
	struct mfd_fri3d_dj_addon_data *data = dev->data;

	data->user_data = user_data;
	data->cb = cb;
	data->report = true;
}

static int mfd_fri3d_dj_addon_read_inputs(const struct device *dev,
					  struct mfd_fri3d_dj_addon_inputs *inputs)
{
	uint8_t buf[INPUTS_SIZE];
	int ret;

	ret = mfd_fri3d_dj_addon_reg_read(dev, FRI3D_DJ_ADDON_REG_BUTTONS, buf, sizeof(buf));
	if (ret != 0) {
		return ret;
	}

	inputs->buttons = buf[0];
	for (int i = 0; i < FRI3D_DJ_ADDON_ANALOG; i++) {
		inputs->analog[i] = sys_get_le16(&buf[1 + 2 * i]);
	}
	for (int i = 0; i < FRI3D_DJ_ADDON_ENCODERS; i++) {
		inputs->encoder[i] = sys_get_le16(&buf[1 + 2 * FRI3D_DJ_ADDON_ANALOG + 2 * i]);
	}

	return 0;
}

#ifdef CONFIG_MFD_FRI3D_DJ_ADDON_UPDATE

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
static void mfd_fri3d_dj_addon_update_on_connect(const struct device *dev)
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

#endif /* CONFIG_MFD_FRI3D_DJ_ADDON_UPDATE */

static int mfd_fri3d_dj_addon_connect(const struct device *dev)
{
	struct mfd_fri3d_dj_addon_data *data = dev->data;
	int ret;

	/* The add-on firmware enables I2C one second after power-on */
	ret = mfd_fri3d_dj_addon_reg_read(dev, FRI3D_DJ_ADDON_REG_VERSION, data->version,
					  sizeof(data->version));
	if (ret != 0) {
		return ret;
	}

	data->connected = true;
	data->report = true;

	LOG_INF("%s: connected, firmware v%u.%u.%u", dev->name, data->version[0],
		data->version[1], data->version[2]);

#ifdef CONFIG_MFD_FRI3D_DJ_ADDON_UPDATE_ON_CONNECT
	mfd_fri3d_dj_addon_update_on_connect(dev);
#endif

	return 0;
}

static void mfd_fri3d_dj_addon_work(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct mfd_fri3d_dj_addon_data *data =
		CONTAINER_OF(dwork, struct mfd_fri3d_dj_addon_data, work);
	const struct device *dev = data->dev;
	const struct mfd_fri3d_dj_addon_config *config = dev->config;
	struct mfd_fri3d_dj_addon_inputs inputs;
	mfd_fri3d_dj_addon_input_cb_t cb;
	int ret;

	if (data->updating) {
		/* the update connects again when it is done */
		return;
	}

	if (!data->connected && mfd_fri3d_dj_addon_connect(dev) != 0) {
		if (sys_timepoint_expired(data->connect_end)) {
			LOG_INF("%s: no add-on", dev->name);
		} else {
			k_work_reschedule(dwork, K_MSEC(config->probe_period_ms));
		}
		return;
	}

	/* UART data from here on means a change after this read */
	atomic_clear(&data->uart_data);
	data->last_read = k_uptime_get_32();

	ret = mfd_fri3d_dj_addon_read_inputs(dev, &inputs);
	data->stats.reads++;
	if (ret != 0) {
		data->stats.read_errors++;
		data->stats.last_error = ret;
		/* the next UART data reads again */
		LOG_WRN("%s: read failed: %d", dev->name, ret);
		return;
	}

	cb = data->cb;
	if (data->report || memcmp(&inputs, &data->inputs, sizeof(inputs)) != 0) {
		data->report = false;
		data->inputs = inputs;
		if (cb != NULL) {
			cb(dev, &inputs, data->user_data);
		}
	}

	/* a change while reading: read again */
	if (atomic_get(&data->uart_data)) {
		k_work_reschedule(dwork, K_MSEC(config->poll_period_ms));
	}
}

/*
 * The add-on sent MIDI on its UART: an input changed. Read the inputs, at
 * most once per poll period.
 */
static void mfd_fri3d_dj_addon_uart_isr(const struct device *uart, void *user_data)
{
	const struct device *dev = user_data;
	const struct mfd_fri3d_dj_addon_config *config = dev->config;
	struct mfd_fri3d_dj_addon_data *data = dev->data;
	uint8_t buf[16];
	bool received = false;
	uint32_t elapsed;

	uart_irq_update(uart);
	while (uart_irq_rx_ready(uart) > 0) {
		int len = uart_fifo_read(uart, buf, sizeof(buf));

		if (len <= 0) {
			break;
		}
		data->stats.uart_bytes += len;
		received = true;
	}

	if (!received || !data->connected || data->updating) {
		return;
	}

	data->stats.triggers++;
	atomic_set(&data->uart_data, 1);
	elapsed = k_uptime_get_32() - data->last_read;
	k_work_reschedule(&data->work, elapsed >= config->poll_period_ms
					       ? K_NO_WAIT
					       : K_MSEC(config->poll_period_ms - elapsed));
}

static int mfd_fri3d_dj_addon_init(const struct device *dev)
{
	const struct mfd_fri3d_dj_addon_config *config = dev->config;
	struct mfd_fri3d_dj_addon_data *data = dev->data;

	data->dev = dev;
	k_work_init_delayable(&data->work, mfd_fri3d_dj_addon_work);

	if (!i2c_is_ready_dt(&config->i2c)) {
		LOG_ERR("%s: I2C bus not ready", dev->name);
		return -ENODEV;
	}

	if (!device_is_ready(config->uart)) {
		LOG_ERR("%s: UART not ready", dev->name);
		return -ENODEV;
	}

	uart_irq_callback_user_data_set(config->uart, mfd_fri3d_dj_addon_uart_isr, (void *)dev);
	uart_irq_rx_enable(config->uart);

	/* look for the add-on from the work queue, it may not be connected */
	data->connect_end = sys_timepoint_calc(K_MSEC(config->connect_timeout_ms));
	k_work_schedule(&data->work, K_NO_WAIT);

	return 0;
}

#define MFD_FRI3D_DJ_ADDON_DEFINE(inst)                                                            \
	static const struct mfd_fri3d_dj_addon_config mfd_fri3d_dj_addon_config##inst = {          \
		.i2c = I2C_DT_SPEC_INST_GET(inst),                                                 \
		.uart = DEVICE_DT_GET(DT_INST_PHANDLE(inst, uart)),                                \
		.poll_period_ms = DT_INST_PROP(inst, poll_period_ms),                              \
		.probe_period_ms = DT_INST_PROP(inst, probe_period_ms),                            \
		.connect_timeout_ms = DT_INST_PROP(inst, connect_timeout_ms),                      \
		IF_ENABLED(CONFIG_MFD_FRI3D_DJ_ADDON_UPDATE,                                       \
			   (.rvswd = COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, rvswd),               \
						 (DEVICE_DT_GET(DT_INST_PHANDLE(inst, rvswd))),     \
						 (NULL)),                                           \
			    .update_power = COND_CODE_1(                                           \
				    DT_INST_NODE_HAS_PROP(inst, update_power_domain),              \
				    (GPIO_DT_SPEC_GET(DT_INST_PHANDLE(inst, update_power_domain),  \
						      enable_gpios)),                              \
				    ({0})),))                                                      \
	};                                                                                         \
                                                                                                   \
	static struct mfd_fri3d_dj_addon_data mfd_fri3d_dj_addon_data##inst;                       \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(inst, mfd_fri3d_dj_addon_init, NULL,                                 \
			      &mfd_fri3d_dj_addon_data##inst, &mfd_fri3d_dj_addon_config##inst,    \
			      POST_KERNEL, CONFIG_MFD_FRI3D_DJ_ADDON_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(MFD_FRI3D_DJ_ADDON_DEFINE)

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
