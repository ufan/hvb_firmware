#!/bin/sh
# SPDX-License-Identifier: Apache-2.0

set -eu

repo_root="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"

prod_main="$repo_root/applications/hvb_controller/src/main.c"
sim_main="$repo_root/demos/modbus_sim/src/main.c"

test -f "$sim_main"
test -f "$repo_root/demos/modbus_sim/CMakeLists.txt"
test -f "$repo_root/demos/modbus_sim/prj.conf"

if rg -n "sys_rand32_get|gen_noise|CONFIG_TEST_RANDOM_GENERATOR|CONFIG_TIMER_RANDOM_GENERATOR" \
	"$prod_main" "$repo_root/applications/hvb_controller/prj.conf"; then
	echo "production hvb_controller must not contain simulation random/noise support" >&2
	exit 1
fi

rg -n "sys_rand32_get|gen_noise|vc_domain_tick" "$sim_main" >/dev/null
