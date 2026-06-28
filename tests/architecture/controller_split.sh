#!/bin/sh
# SPDX-License-Identifier: Apache-2.0

set -eu

repo_root="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"

prod_main="$repo_root/applications/hvb_controller/src/main.c"
sim_main="$repo_root/demos/modbus_sim/src/main.c"
production_roots="$repo_root/include $repo_root/lib $repo_root/applications $repo_root/demos"

test -f "$sim_main"
test -f "$repo_root/demos/modbus_sim/CMakeLists.txt"
test -f "$repo_root/demos/modbus_sim/prj.conf"

if rg -n "sys_rand32_get|gen_noise|CONFIG_TEST_RANDOM_GENERATOR|CONFIG_TIMER_RANDOM_GENERATOR" \
	"$prod_main" "$repo_root/applications/hvb_controller/prj.conf"; then
	echo "production hvb_controller must not contain simulation random/noise support" >&2
	exit 1
fi

rg -n "sys_rand32_get|gen_noise|domain_tick" "$sim_main" >/dev/null

if rg -n "vc_dispatch|vc_query|struct vc_cmd|vc_cmd_" $production_roots \
	--glob '*.[ch]'; then
	echo "frontends must use the Register Catalog, not the removed VC facade" >&2
	exit 1
fi

rg -n "depends on MODBUS && VC_RUNTIME && VC_CHANNEL_CONTROLLER" \
	"$repo_root/lib/modbus_adapter/Kconfig" >/dev/null
