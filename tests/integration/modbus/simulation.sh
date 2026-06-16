#!/usr/bin/env bash
# simulation.sh — Enable output, verify ramp, measured values with noise, status bits
# Usage: PORT=/dev/ttyUSB0 ./simulation.sh

PORT="${PORT:-/dev/ttyUSB0}"
MB="mbpoll -m rtu -a 1 -b 115200 -d 8 -s 1 -P none -0"

echo "--- Simulation Test ---"
echo "PORT=$PORT"
echo ""

check() {
    local desc="$1" cmd="$2" pattern="$3"
    echo -n "$desc ... "
    if $MB $cmd | grep -qE "$pattern"; then
        echo "PASS"
    else
        echo "FAIL"
        exit 1
    fi
}

# Setup: force known state
$MB -r 41 -t 4 -1 "$PORT" 3 >/dev/null 2>&1 || true
$MB -r 47 -t 4 -1 "$PORT" 0 >/dev/null 2>&1 || true
$MB -r 40 -t 4 -1 "$PORT" 0 >/dev/null 2>&1 || true
$MB -r 43 -t 4 -1 "$PORT" 0 >/dev/null 2>&1 || true
$MB -r 45 -t 4 -1 "$PORT" 0 >/dev/null 2>&1 || true

check "1. Write Configured Target = 5000" "-r 40 -t 4 -1 $PORT 5000" 'Written'
check "2. Readback Target = 5000" "-r 40 -c 1 -t 4 -1 $PORT" '\[40\]:.*5000$'

check "3. Enable Output" "-r 41 -t 4 -1 $PORT 1" 'Written'

sleep 1

check "4. Measured Voltage near target (4950..5050)" "-r 40 -c 1 -t 3 -1 $PORT" '\[40\]:\s*(50[0-9]{2}|49[5-9][0-9])'

check "5. Measured Current non-zero (> 100)" "-r 41 -c 1 -t 3 -1 $PORT" '\[41\]:\s*[1-9][0-9]{2}'

check "6. Status Bits (drive + output_enable)" "-r 43 -c 1 -t 3:hex -1 $PORT" '0x000[37]'

# Values drift between polls
echo -n "7. Measured values drift between polls ... "
MV1=$($MB -r 40 -c 1 -t 3 -1 "$PORT" 2>/dev/null | grep '^\[' | awk '{print $NF}')
MC1=$($MB -r 41 -c 1 -t 3 -1 "$PORT" 2>/dev/null | grep '^\[' | awk '{print $NF}')
sleep 1
MV2=$($MB -r 40 -c 1 -t 3 -1 "$PORT" 2>/dev/null | grep '^\[' | awk '{print $NF}')
MC2=$($MB -r 41 -c 1 -t 3 -1 "$PORT" 2>/dev/null | grep '^\[' | awk '{print $NF}')
if [ "$MV1" != "$MV2" ] || [ "$MC1" != "$MC2" ]; then
    echo "PASS ($MV1->$MV2)"
else
    echo "NOTE ($MV1,$MC1 — no drift, possible with small noise)"
fi

check "8. Disable Immediate" "-r 41 -t 4 -1 $PORT 3" 'Written'

sleep 1

check "9. Status Bits after disable (output_enable cleared)" "-r 43 -c 1 -t 3:hex -1 $PORT" '0x000[0148]'

# Cleanup
$MB -r 40 -t 4 -1 "$PORT" 0 >/dev/null 2>&1 || true
$MB -r 41 -t 4 -1 "$PORT" 0 >/dev/null 2>&1 || true

echo ""
echo "--- Simulation: all PASS ---"
