# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources_ifdef(CONFIG_LVB_VC_CHANNEL lvb_vc_channel.c)
