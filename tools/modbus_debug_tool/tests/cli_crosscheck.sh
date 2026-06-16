#!/usr/bin/env bash
# cli_crosscheck.sh — Differential test: hvbctrl vs mbpoll.
#
# Regression guard for the partial-frame read bug: ModbusLib's blocking serial
# read issued a single ::read() and treated a truncated frame as complete, so
# any response >~31 bytes (>=14 registers) failed with a spurious CRC error and
# decoded reads (e.g. `info`, which reads 15 input regs) silently returned all
# zeros. mbpoll/libmodbus loops until the full frame arrives, so it is the
# known-good reference. This test fails if hvbctrl diverges from mbpoll on a
# large block read.
#
# Usage: PORT=/dev/ttyUSB0 HVBCTRL=/path/to/hvbctrl ./cli_crosscheck.sh

set -u
PORT="${PORT:-/dev/ttyUSB0}"
HVBCTRL="${HVBCTRL:-../build/linux-release/cli/hvbctrl}"
MB="mbpoll -m rtu -a 1 -b 115200 -d 8 -s 1 -P none -0"

echo "--- CLI Cross-check (hvbctrl vs mbpoll) ---"
echo "PORT=$PORT  HVBCTRL=$HVBCTRL"
echo ""

if [ ! -x "$HVBCTRL" ]; then
    echo "FAIL: hvbctrl not found/executable at '$HVBCTRL' (set HVBCTRL=...)"
    exit 1
fi

PASS=0
FAIL=0

# mbpoll reference: space-separated decimal values for input regs [r..r+n-1]
mb_input() {  # args: start count
    $MB -r "$1" -c "$2" -t 3 -1 "$PORT" 2>/dev/null | grep '^\[' | awk '{printf "%s ", $NF}'
}
# hvbctrl raw fc04 -> space-separated decimal values
hvb_input() {  # args: start count
    "$HVBCTRL" -p "$PORT" -b 115200 -i 1 raw fc04 "$1" "$2" 2>/dev/null \
        | tr '\n' ' ' \
        | awk '{ for (i=1;i<=NF;i++) printf "%d ", strtonum("0x" $i) }'
}

cmp_block() {  # args: desc start count
    local desc="$1" start="$2" count="$3"
    echo -n "$desc (regs $start..$((start+count-1))) ... "
    local ref hvb
    ref=$(mb_input "$start" "$count")
    hvb=$(hvb_input "$start" "$count")
    if [ -n "$hvb" ] && [ "$ref" = "$hvb" ]; then
        echo "PASS"
        PASS=$((PASS+1))
    else
        echo "FAIL"
        echo "    mbpoll : [$ref]"
        echo "    hvbctrl: [$hvb]"
        FAIL=$((FAIL+1))
    fi
}

# 1. Small block (below the old ~31-byte cliff) — always worked.
cmp_block "1. Small input block (5 regs)" 0 5

# 2. The regression case: 15-register block crosses the old cliff.
cmp_block "2. Large input block (15 regs)" 0 15

# 3. Repeat the large read to catch the old flaky CRC failure.
echo -n "3. Large block stable over 5 reads ... "
ok=1
for _ in 1 2 3 4 5; do
    [ -n "$(hvb_input 0 15)" ] || { ok=0; break; }
done
if [ "$ok" = 1 ]; then echo "PASS"; PASS=$((PASS+1)); else echo "FAIL (read returned empty/error)"; FAIL=$((FAIL+1)); fi

# 4. Decoded `info` reflects real values, not the zero-struct fallback.
echo -n "4. hvbctrl info decodes non-zero protocol/variant ... "
info_out=$("$HVBCTRL" -p "$PORT" -b 115200 -i 1 info 2>/dev/null)
proto=$(echo "$info_out" | awk -F: '/Protocol:/ {gsub(/ /,"",$2); print $2}')
if [ "$proto" != "0.0" ] && [ -n "$proto" ]; then
    echo "PASS (Protocol=$proto)"; PASS=$((PASS+1))
else
    echo "FAIL (Protocol=$proto — zero-struct fallback?)"; FAIL=$((FAIL+1))
fi

echo ""
echo "--- CLI Cross-check: $PASS passed, $FAIL failed ---"
[ "$FAIL" -eq 0 ]
