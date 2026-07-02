#!/usr/bin/env bash
set -euo pipefail

: "${MOCK_STATE_DIR:?MOCK_STATE_DIR is required}"
mkdir -p "$MOCK_STATE_DIR"

args=("$@")
raw_index=-1
for i in "${!args[@]}"; do
    if [[ "${args[$i]}" == "raw" ]]; then
        raw_index=$i
        break
    fi
done
(( raw_index >= 0 )) || { echo "mock: raw command required" >&2; exit 2; }

cmd=("${args[@]:raw_index}")
printf '%s\n' "${cmd[*]}" >> "$MOCK_STATE_DIR/commands.log"

hex16() {
    printf '%04X' "$(( $1 & 0xFFFF ))"
}

hex32() {
    local value=$1
    printf '%04X %04X\n' "$(( (value >> 16) & 0xFFFF ))" "$(( value & 0xFFFF ))"
}

dac_for_addr() {
    local addr=$1
    local ch=$(( (addr - 40) / 40 ))
    local state="$MOCK_STATE_DIR/dac_$ch"
    [[ -f "$state" ]] && cat "$state" || printf '0\n'
}

case "${cmd[1]}" in
fc06)
    addr=${cmd[2]}
    value=${cmd[3]}
    if (( addr >= 40 && (addr - 40) % 40 == 31 )); then
        if [[ -n "${MOCK_FAIL_DAC:-}" && "$value" == "$MOCK_FAIL_DAC" ]]; then
            echo "mock: injected DAC write failure" >&2
            exit 1
        fi
        ch=$(( (addr - 40) / 40 ))
        printf '%s\n' "$value" > "$MOCK_STATE_DIR/dac_$ch"
    fi
    echo "OK"
    ;;
fc04)
    addr=${cmd[2]}
    count=${cmd[3]}
    if (( addr == 0 && count == 15 )); then
        echo "0003 0000 0001 0004 0003 0007 012C 01F4"
        echo "0000 0001 0000 0001 0002 0000 0000"
    elif (( count == 1 && addr == 49 )); then
        echo "000F"
    elif (( count == 1 && addr == 89 )); then
        echo "0003"
    elif (( count == 1 && addr == 129 )); then
        echo "0004"
    elif (( count == 1 && (addr - 40) % 40 == 10 )); then
        dac=$(dac_for_addr "$addr")
        hex16 "$(( dac / 10 ))"
        echo
    elif (( count == 1 && (addr - 40) % 40 == 11 )); then
        dac=$(dac_for_addr "$addr")
        hex16 "$(( -dac / 10 ))"
        echo
    elif (( count == 2 && (addr - 40) % 40 == 12 )); then
        if [[ -n "${MOCK_CONSTANT_RAW_V:-}" ]]; then
            hex32 1234
        else
            dac=$(dac_for_addr "$addr")
            hex32 "$(( dac * 10 ))"
        fi
    elif (( count == 2 && (addr - 40) % 40 == 14 )); then
        dac=$(dac_for_addr "$addr")
        hex32 "$(( -dac * 10 ))"
    else
        echo "mock: unsupported FC04 addr=$addr count=$count" >&2
        exit 2
    fi
    ;;
*)
    echo "mock: unsupported command: ${cmd[*]}" >&2
    exit 2
    ;;
esac
