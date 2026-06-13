#!/usr/bin/env bash
set -euo pipefail

build_dir="${1:-build}"
hex_file="${build_dir}/zephyr/zephyr.hex"
device="${JLINK_DEVICE:-STM32F429BI}"
iface="${JLINK_IFACE:-swd}"
speed="${JLINK_SPEED:-4000}"
commander="${JLINK_COMMANDER:-JLinkExe}"

if [[ ! -f "${hex_file}" ]]; then
  echo "Missing ${hex_file}. Run one of:" >&2
  echo "  west build -b jw_hvb -d build/hvb_controller applications/hvb_controller" >&2
  echo "  west build -b jw_hvb -d build/modbus_smoke demos/modbus_smoke" >&2
  exit 1
fi

script_file="$(mktemp)"
trap 'rm -f "${script_file}"' EXIT

cat > "${script_file}" <<JLINK_CMDS
ExitOnError 1
LE
loadfile "${hex_file}"
g
writeDP 1 0
readDP 1
q
JLINK_CMDS

"${commander}" \
  -nogui 1 \
  -if "${iface}" \
  -speed "${speed}" \
  -device "${device}" \
  -CommanderScript "${script_file}" \
  -nogui 1
