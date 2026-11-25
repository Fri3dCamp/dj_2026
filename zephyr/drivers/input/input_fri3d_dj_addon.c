/*
 * Copyright (c) 2026 Bert Outtier <outtierbert@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Controls of the Fri3d Camp 2026 DJ add-on: buttons as keys, potmeters and
 * faders as absolute axes, encoders as relative axes.
 */

#define DT_DRV_COMPAT fri3d_dj_addon_input

#include <stdlib.h>

#include <zephyr/drivers/mfd/fri3d_dj_addon.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(input_fri3d_dj_addon, CONFIG_INPUT_LOG_LEVEL);

#define ANALOG_MAX     4095
/* The encoder counters wrap at 128 */
#define ENCODER_RANGE  128
#define MAX_EVENTS     (FRI3D_DJ_ADDON_BUTTONS + FRI3D_DJ_ADDON_ANALOG + FRI3D_DJ_ADDON_ENCODERS)

struct input_fri3d_dj_addon_config {
	const struct device *mfd;
	uint16_t button_codes[FRI3D_DJ_ADDON_BUTTONS];
	uint16_t analog_codes[FRI3D_DJ_ADDON_ANALOG];
	uint16_t encoder_codes[FRI3D_DJ_ADDON_ENCODERS];
	uint16_t analog_threshold;
	uint16_t analog_dead_zone;
};

struct input_fri3d_dj_addon_data {
	const struct device *dev;
	/* Last reported state, valid after the first report */
	bool valid;
	uint8_t buttons;
	uint16_t analog[FRI3D_DJ_ADDON_ANALOG];
	uint16_t encoder[FRI3D_DJ_ADDON_ENCODERS];
};

struct input_event_entry {
	uint8_t type;
	uint16_t code;
	int32_t value;
};

/* Raw ADC value to 0-ANALOG_MAX, 0 at the physical minimum, without the dead zones */
static uint16_t input_fri3d_dj_addon_scale(const struct input_fri3d_dj_addon_config *config,
					   uint16_t raw)
{
	/* the add-on reads ANALOG_MAX at the physical minimum */
	int32_t value = ANALOG_MAX - MIN(raw, ANALOG_MAX);
	int32_t dz = config->analog_dead_zone;

	if (dz == 0) {
		return value;
	}

	value = (value - dz) * ANALOG_MAX / (ANALOG_MAX - 2 * dz);

	return CLAMP(value, 0, ANALOG_MAX);
}

static void input_fri3d_dj_addon_report(const struct device *dev,
					const struct input_event_entry *events, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		input_report(dev, events[i].type, events[i].code, events[i].value,
			     i == count - 1, K_FOREVER);
	}
}

static void input_fri3d_dj_addon_changed(const struct device *mfd,
					 const struct mfd_fri3d_dj_addon_inputs *inputs,
					 void *user_data)
{
	const struct device *dev = user_data;
	const struct input_fri3d_dj_addon_config *config = dev->config;
	struct input_fri3d_dj_addon_data *data = dev->data;
	struct input_event_entry events[MAX_EVENTS];
	size_t count = 0;

	ARG_UNUSED(mfd);

	if (inputs == NULL) {
		/* removed: release the buttons that were pressed */
		for (int i = 0; data->valid && i < FRI3D_DJ_ADDON_BUTTONS; i++) {
			if (data->buttons & BIT(i)) {
				events[count++] = (struct input_event_entry){
					INPUT_EV_KEY, config->button_codes[i], 0};
			}
		}
		data->valid = false;
		input_fri3d_dj_addon_report(dev, events, count);
		return;
	}

	for (int i = 0; i < FRI3D_DJ_ADDON_BUTTONS; i++) {
		bool pressed = inputs->buttons & BIT(i);

		if (!data->valid || pressed != (bool)(data->buttons & BIT(i))) {
			events[count++] = (struct input_event_entry){
				INPUT_EV_KEY, config->button_codes[i], pressed};
		}
	}
	data->buttons = inputs->buttons;

	for (int i = 0; i < FRI3D_DJ_ADDON_ANALOG; i++) {
		uint16_t value = input_fri3d_dj_addon_scale(config, inputs->analog[i]);

		if (!data->valid || abs(value - data->analog[i]) >= config->analog_threshold) {
			events[count++] = (struct input_event_entry){
				INPUT_EV_ABS, config->analog_codes[i], value};
			data->analog[i] = value;
		}
	}

	for (int i = 0; i < FRI3D_DJ_ADDON_ENCODERS; i++) {
		/* steps since the last read, -64 to 63 */
		int delta = ((inputs->encoder[i] - data->encoder[i] + ENCODER_RANGE / 2) &
			     (ENCODER_RANGE - 1)) - ENCODER_RANGE / 2;

		if (data->valid && delta != 0) {
			events[count++] = (struct input_event_entry){
				INPUT_EV_REL, config->encoder_codes[i], delta};
		}
		data->encoder[i] = inputs->encoder[i];
	}

	data->valid = true;
	input_fri3d_dj_addon_report(dev, events, count);
}

static int input_fri3d_dj_addon_init(const struct device *dev)
{
	const struct input_fri3d_dj_addon_config *config = dev->config;
	struct input_fri3d_dj_addon_data *data = dev->data;

	if (!device_is_ready(config->mfd)) {
		LOG_ERR("%s: DJ add-on not ready", dev->name);
		return -ENODEV;
	}

	data->dev = dev;
	mfd_fri3d_dj_addon_set_input_callback(config->mfd, input_fri3d_dj_addon_changed,
					      (void *)dev);

	return 0;
}

#define INPUT_FRI3D_DJ_ADDON_DEFINE(inst)                                                          \
	BUILD_ASSERT(DT_INST_PROP_LEN(inst, button_codes) == FRI3D_DJ_ADDON_BUTTONS,               \
		     "button-codes needs 8 codes");                                                \
	BUILD_ASSERT(DT_INST_PROP_LEN(inst, analog_codes) == FRI3D_DJ_ADDON_ANALOG,                \
		     "analog-codes needs 9 codes");                                                \
	BUILD_ASSERT(DT_INST_PROP_LEN(inst, encoder_codes) == FRI3D_DJ_ADDON_ENCODERS,             \
		     "encoder-codes needs 2 codes");                                               \
	BUILD_ASSERT(DT_INST_PROP(inst, analog_dead_zone) < ANALOG_MAX / 4,                         \
		     "analog-dead-zone too large");                                                \
                                                                                                   \
	static const struct input_fri3d_dj_addon_config input_fri3d_dj_addon_config##inst = {      \
		.mfd = DEVICE_DT_GET(DT_INST_PARENT(inst)),                                        \
		.button_codes = DT_INST_PROP(inst, button_codes),                                  \
		.analog_codes = DT_INST_PROP(inst, analog_codes),                                  \
		.encoder_codes = DT_INST_PROP(inst, encoder_codes),                                \
		.analog_threshold = DT_INST_PROP(inst, analog_threshold),                          \
		.analog_dead_zone = DT_INST_PROP(inst, analog_dead_zone),                          \
	};                                                                                         \
                                                                                                   \
	static struct input_fri3d_dj_addon_data input_fri3d_dj_addon_data##inst;                   \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(inst, input_fri3d_dj_addon_init, NULL,                               \
			      &input_fri3d_dj_addon_data##inst,                                    \
			      &input_fri3d_dj_addon_config##inst, POST_KERNEL,                     \
			      CONFIG_INPUT_FRI3D_DJ_ADDON_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(INPUT_FRI3D_DJ_ADDON_DEFINE)
