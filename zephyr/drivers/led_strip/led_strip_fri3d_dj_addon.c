/*
 * Copyright (c) 2026 Bert Outtier <outtierbert@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The 8 RGB button LEDs of the Fri3d Camp 2026 DJ add-on, as an LED strip.
 */

#define DT_DRV_COMPAT fri3d_dj_addon_led_strip

#include <zephyr/drivers/led_strip.h>
#include <zephyr/drivers/mfd/fri3d_dj_addon.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(led_strip_fri3d_dj_addon, CONFIG_LED_STRIP_LOG_LEVEL);

struct led_strip_fri3d_dj_addon_config {
	const struct device *mfd;
};

static int led_strip_fri3d_dj_addon_update_rgb(const struct device *dev, struct led_rgb *pixels,
					       size_t num_pixels)
{
	const struct led_strip_fri3d_dj_addon_config *config = dev->config;
	uint8_t buf[FRI3D_DJ_ADDON_LEDS * 3];

	if (num_pixels > FRI3D_DJ_ADDON_LEDS) {
		return -EINVAL;
	}

	/* the add-on stores the colors in the WS2812 order */
	for (size_t i = 0; i < num_pixels; i++) {
		buf[3 * i] = pixels[i].g;
		buf[3 * i + 1] = pixels[i].r;
		buf[3 * i + 2] = pixels[i].b;
	}

	return mfd_fri3d_dj_addon_reg_write(config->mfd, FRI3D_DJ_ADDON_REG_LEDS, buf,
					    3 * num_pixels);
}

static size_t led_strip_fri3d_dj_addon_length(const struct device *dev)
{
	ARG_UNUSED(dev);

	return FRI3D_DJ_ADDON_LEDS;
}

static DEVICE_API(led_strip, led_strip_fri3d_dj_addon_api) = {
	.update_rgb = led_strip_fri3d_dj_addon_update_rgb,
	.length = led_strip_fri3d_dj_addon_length,
};

static int led_strip_fri3d_dj_addon_init(const struct device *dev)
{
	const struct led_strip_fri3d_dj_addon_config *config = dev->config;

	if (!device_is_ready(config->mfd)) {
		LOG_ERR("%s: DJ add-on not ready", dev->name);
		return -ENODEV;
	}

	return 0;
}

#define LED_STRIP_FRI3D_DJ_ADDON_DEFINE(inst)                                                      \
	static const struct led_strip_fri3d_dj_addon_config led_strip_fri3d_dj_addon_config##inst = { \
		.mfd = DEVICE_DT_GET(DT_INST_PARENT(inst)),                                        \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(inst, led_strip_fri3d_dj_addon_init, NULL, NULL,                     \
			      &led_strip_fri3d_dj_addon_config##inst, POST_KERNEL,                 \
			      CONFIG_LED_STRIP_FRI3D_DJ_ADDON_INIT_PRIORITY,                       \
			      &led_strip_fri3d_dj_addon_api);

DT_INST_FOREACH_STATUS_OKAY(LED_STRIP_FRI3D_DJ_ADDON_DEFINE)
