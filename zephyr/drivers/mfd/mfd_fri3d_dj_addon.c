/*
 * Copyright (c) 2026 Bert Outtier <outtierbert@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * DJ add-on of the Fri3d Camp 2026 badge (CH32X035 with the dj_2026
 * firmware) on the expansion connector.
 *
 * The devicetree says the add-on is there: the driver connects to it once its
 * firmware is up after boot, and logs an error when it does not answer (data
 * on its UART makes it try again). Its firmware (a release build) sends a MIDI
 * message on its UART on every input change: data on the UART triggers a read
 * of the inputs over I2C, so they are not polled.
 * It needs firmware v1.0.4 or later: older firmware can return a 16-bit value
 * with its two bytes from before and after an ADC update.
 *
 * With CONFIG_MFD_FRI3D_DJ_ADDON_UPDATE its firmware can be updated over
 * RVSWD on the same pins (mfd_fri3d_dj_addon_update.c).
 */

#define DT_DRV_COMPAT fri3d_dj_addon

#include <string.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/mfd/fri3d_dj_addon.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include "mfd_fri3d_dj_addon_priv.h"

LOG_MODULE_REGISTER(mfd_fri3d_dj_addon, CONFIG_MFD_LOG_LEVEL);

/* Input registers: buttons, analog channels and encoders in one read */
#define INPUTS_SIZE   (FRI3D_DJ_ADDON_REG_LEDS - FRI3D_DJ_ADDON_REG_BUTTONS)
/* Writable registers: LEDs and configuration */
#define WRITABLE_SIZE (FRI3D_DJ_ADDON_REG_CONFIG + 1 - FRI3D_DJ_ADDON_REG_LEDS)


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

	/* the devicetree says there is an add-on: no answer is an error */
	if (!data->connected && mfd_fri3d_dj_addon_connect(dev) != 0) {
		LOG_ERR("%s: no answer from the add-on", dev->name);
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

	/* data from an add-on that did not answer yet: try to connect again */
	if (!received || data->updating) {
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

	/* connect from the work queue, once the add-on's firmware is up */
	k_work_schedule(&data->work, K_TIMEOUT_ABS_MS(MFD_FRI3D_DJ_ADDON_START_MS));

	return 0;
}

#define MFD_FRI3D_DJ_ADDON_DEFINE(inst)                                                            \
	static const struct mfd_fri3d_dj_addon_config mfd_fri3d_dj_addon_config##inst = {          \
		.i2c = I2C_DT_SPEC_INST_GET(inst),                                                 \
		.uart = DEVICE_DT_GET(DT_INST_PHANDLE(inst, uart)),                                \
		.poll_period_ms = DT_INST_PROP(inst, poll_period_ms),                              \
		IF_ENABLED(CONFIG_MFD_FRI3D_DJ_ADDON_UPDATE,                                       \
			   (.rvswd = COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, rvswd),               \
						 (DEVICE_DT_GET(DT_INST_PHANDLE(inst, rvswd))),     \
						 (NULL)),))                                        \
	};                                                                                         \
                                                                                                   \
	static struct mfd_fri3d_dj_addon_data mfd_fri3d_dj_addon_data##inst;                       \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(inst, mfd_fri3d_dj_addon_init, NULL,                                 \
			      &mfd_fri3d_dj_addon_data##inst, &mfd_fri3d_dj_addon_config##inst,    \
			      POST_KERNEL, CONFIG_MFD_FRI3D_DJ_ADDON_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(MFD_FRI3D_DJ_ADDON_DEFINE)
