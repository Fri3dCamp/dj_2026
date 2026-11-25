# Copyright (c) 2026 Bert Outtier <outtierbert@gmail.com>
# SPDX-License-Identifier: Apache-2.0

# sources go into Zephyr's drivers__<class> libraries (named relative to zephyr/)
zephyr_library_amend(${ZEPHYR_CURRENT_MODULE_DIR}/zephyr)
zephyr_library_sources_ifdef(CONFIG_MFD_FRI3D_DJ_ADDON mfd_fri3d_dj_addon.c)

# DJ add-on firmware image to link in (CONFIG_MFD_FRI3D_DJ_ADDON_FIRMWARE)
if(CONFIG_MFD_FRI3D_DJ_ADDON_UPDATE AND NOT CONFIG_MFD_FRI3D_DJ_ADDON_FIRMWARE STREQUAL "")
  get_filename_component(fri3d_dj_addon_fw ${CONFIG_MFD_FRI3D_DJ_ADDON_FIRMWARE} ABSOLUTE
                         BASE_DIR ${APPLICATION_SOURCE_DIR})
  if(NOT EXISTS ${fri3d_dj_addon_fw})
    message(FATAL_ERROR "CONFIG_MFD_FRI3D_DJ_ADDON_FIRMWARE: ${fri3d_dj_addon_fw} not found")
  endif()
  generate_inc_file_for_target(${ZEPHYR_CURRENT_LIBRARY} ${fri3d_dj_addon_fw}
                               ${ZEPHYR_BINARY_DIR}/include/generated/fri3d_dj_addon_firmware.inc)
  zephyr_library_compile_definitions(FRI3D_DJ_ADDON_HAS_FIRMWARE)
endif()
