# Copyright (c) 2026 Bert Outtier <outtierbert@gmail.com>
# SPDX-License-Identifier: Apache-2.0

# sources go into Zephyr's drivers__<class> libraries (named relative to zephyr/)
zephyr_library_amend(${ZEPHYR_CURRENT_MODULE_DIR}/zephyr)
zephyr_library_sources_ifdef(CONFIG_LED_STRIP_FRI3D_DJ_ADDON led_strip_fri3d_dj_addon.c)
