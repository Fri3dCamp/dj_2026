/*
 * Copyright (c) 2026 Bert Outtier <outtierbert@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shell commands of the Fri3d Camp 2026 DJ add-on
 * (CONFIG_MFD_FRI3D_DJ_ADDON_SHELL): "djaddon version", and with a bundled
 * firmware image "djaddon update".
 */

#define DT_DRV_COMPAT fri3d_dj_addon

#include <zephyr/drivers/mfd/fri3d_dj_addon.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include "mfd_fri3d_dj_addon_priv.h"

static const struct device *const shell_dj_addon = DEVICE_DT_GET(DT_DRV_INST(0));

static int shell_dj_addon_ready(const struct shell *sh)
{
	if (!device_is_ready(shell_dj_addon)) {
		shell_error(sh, "%s not ready (no add-on at boot)", shell_dj_addon->name);
		return -ENODEV;
	}

	return 0;
}

static int cmd_djaddon_version(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t version[3];
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	ret = shell_dj_addon_ready(sh);
	if (ret != 0) {
		return ret;
	}

	mfd_fri3d_dj_addon_get_version(shell_dj_addon, version);
	shell_print(sh, "DJ add-on firmware v%u.%u.%u", version[0], version[1], version[2]);

#ifdef CONFIG_MFD_FRI3D_DJ_ADDON_UPDATE
	struct mfd_fri3d_dj_addon_update_info info;

	mfd_fri3d_dj_addon_get_update_info(&info);
	if (info.bundled_len > 0) {
		shell_print(sh, "bundled v%u.%u.%u (%zu bytes)%s", info.bundled_version[0],
			    info.bundled_version[1], info.bundled_version[2], info.bundled_len,
			    info.updated_at_boot ? ", installed at boot" : "");
	}
	shell_print(sh, "updates since boot: %u", info.updates);
#endif

	return 0;
}

#ifdef FRI3D_DJ_ADDON_HAS_FIRMWARE
static void shell_dj_addon_progress(size_t done, size_t total, void *user_data)
{
	const struct shell *sh = user_data;

	/* every 25 % */
	if (done == total || (done * 4 / total) != ((done + 256) * 4 / total)) {
		shell_print(sh, "  %u %%", (unsigned int)(done * 100 / total));
	}
}

static int cmd_djaddon_update(const struct shell *sh, size_t argc, char **argv)
{
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	ret = shell_dj_addon_ready(sh);
	if (ret != 0) {
		return ret;
	}

	shell_print(sh, "installing the bundled DJ add-on firmware");
	ret = mfd_fri3d_dj_addon_update_bundled(shell_dj_addon, shell_dj_addon_progress,
						(void *)sh);
	if (ret != 0) {
		shell_error(sh, "update failed: %d", ret);
		return ret;
	}

	return cmd_djaddon_version(sh, 0, NULL);
}
#endif /* FRI3D_DJ_ADDON_HAS_FIRMWARE */

SHELL_STATIC_SUBCMD_SET_CREATE(sub_djaddon,
	SHELL_CMD(version, NULL, "Add-on (and bundled) firmware versions", cmd_djaddon_version),
#ifdef FRI3D_DJ_ADDON_HAS_FIRMWARE
	SHELL_CMD(update, NULL, "Install the bundled add-on firmware", cmd_djaddon_update),
#endif
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(djaddon, &sub_djaddon, "Fri3d DJ add-on", NULL);
