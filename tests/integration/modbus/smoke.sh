#!/usr/bin/env bash
# smoke.sh — Protocol version, channels, config defaults, uptime
# Usage: PORT=/dev/ttyUSB0 ./smoke.sh

set -e
PORT="${PORT:-/dev/ttyUSB0}"
MB="mbpoll -m rtu -a 1 -b 115200 -d 8 -s 1 -P none -0"

echo "--- Smoke Test ---"
echo "PORT=$PORT"
echo ""

# Reset to known state
$MB -r 41 -t 4 -1 "$PORT" 3 >/dev/null 2>&1 || true
$MB -r 47 -t 4 -1 "$PORT" 0 >/dev/null 2>&1 || true
$MB -r 48 -t 4 -1 "$PORT" 0 >/dev/null 2>&1 || true
$MB -r 40 -t 4 -1 "$PORT" 0 >/dev/null 2>&1 || true
$MB -r 43 -t 4 -1 "$PORT" 0 >/dev/null 2>&1 || true
$MB -r 49 -t 4 -1 "$PORT" 20000 >/dev/null 2>&1 || true
$MB -r 52 -t 4 -1 "$PORT" 32767 >/dev/null 2>&1 || true
$MB -r 42 -t 4 -1 "$PORT" 2 >/dev/null 2>&1 || true

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

# --- Protocol version ---
check "1. Protocol Major = 2" "-r 0 -c 1 -t 3 -1 $PORT" '\[0\]:.*2$'
check "2. Protocol Minor = 0" "-r 1 -c 1 -t 3 -1 $PORT" '\[1\]:.*0$'
check "3. Variant ID = 1" "-r 2 -c 1 -t 3 -1 $PORT" '\[2\]:.*1$'

# --- Channels ---
check "4. Supported Channels = 2" "-r 4 -c 1 -t 3 -1 $PORT" '\[4\]:.*2$'
check "5. Active Channel Mask = 0x0003" "-r 5 -c 1 -t 3:hex -1 $PORT" '0x0003'

# --- Uptime tick ---
echo -n "6. Uptime increments ... "
U1=$($MB -r 8 -c 2 -t 3 -1 "$PORT" 2>/dev/null | grep '^\[' | awk '{print $NF}' | tail -1)
sleep 2
U2=$($MB -r 8 -c 2 -t 3 -1 "$PORT" 2>/dev/null | grep '^\[' | awk '{print $NF}' | tail -1)
if [ "$U1" != "$U2" ] && [ -n "$U1" ]; then
    echo "PASS"
else
    echo "FAIL ($U1 -> $U2)"
    exit 1
fi

# --- System holding defaults ---
check "7. Operating Mode = Normal (0)" "-r 0 -c 1 -t 4 -1 $PORT" '\[0\]:.*0$'
check "8. Slave Address = 1" "-r 1 -c 1 -t 4 -1 $PORT" '\[1\]:.*1$'
check "9. Recovery Policy = Manual Latch (0)" "-r 3 -c 1 -t 4 -1 $PORT" '\[3\]:.*0$'
check "10. Voltage Safe Band = 10" "-r 7 -c 1 -t 4 -1 $PORT" '\[7\]:.*10$'
check "11. Current Safe Band = 10" "-r 8 -c 1 -t 4 -1 $PORT" '\[8\]:.*10$'

# --- Channel 0 holding defaults ---
check "12. Target Voltage = 0" "-r 40 -c 1 -t 4 -1 $PORT" '\[40\]:.*0$'
check "13. Voltage Protection = Disabled (0)" "-r 47 -c 1 -t 4 -1 $PORT" '\[47\]:.*0$'
check "14. Voltage Limit Threshold = 20000" "-r 49 -c 1 -t 4 -1 $PORT" '20000'
check "15. Current Limit Threshold = 32767" "-r 52 -c 1 -t 4 -1 $PORT" '32767'
check "16. Output Cal K = 10000" "-r 55 -c 1 -t 4 -1 $PORT" '\[55\]:.*10000$'

# --- Self-clearing commands ---
check "17. Output Action self-clears (reads 0)" "-r 41 -c 1 -t 4 -1 $PORT" '\[41\]:.*0$'
check "18. Fault Cmd self-clears (reads 0)" "-r 42 -c 1 -t 4 -1 $PORT" '\[42\]:.*0$'
check "19. Channel Param Action self-clears (reads 0)" "-r 79 -c 1 -t 4 -1 $PORT" '\[79\]:.*0$'

# --- Channel 0 input defaults ---
check "20. No active fault (0x0000)" "-r 44 -c 1 -t 3:hex -1 $PORT" '0x0000'

echo ""
echo "--- Smoke: 20/20 PASS ---"
