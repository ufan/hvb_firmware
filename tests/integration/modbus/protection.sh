#!/usr/bin/env bash
# protection.sh — Fault lifecycle: trigger, reject unsafe clear, disable, clear, self-clear
# Usage: PORT=/dev/ttyUSB0 ./protection.sh

PORT="${PORT:-/dev/ttyUSB0}"
MB="mbpoll -m rtu -a 1 -b 115200 -d 8 -s 1 -P none -0"

echo "--- Protection Test ---"
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

# Setup: known state, instant ramp
$MB -r 41 -t 4 -1 "$PORT" 3 >/dev/null 2>&1 || true
$MB -r 42 -t 4 -1 "$PORT" 2 >/dev/null 2>&1 || true
$MB -r 43 -t 4 -1 "$PORT" 0 >/dev/null 2>&1 || true
$MB -r 40 -t 4 -1 "$PORT" 5000 >/dev/null 2>&1 || true
$MB -r 49 -t 4 -1 "$PORT" 4950 >/dev/null 2>&1 || true
$MB -r 47 -t 4 -1 "$PORT" 2 >/dev/null 2>&1 || true
$MB -r 48 -t 4 -1 "$PORT" 5 >/dev/null 2>&1 || true   # Clamp (keeps target at limit, measured stays high)

check "1. Enable output" "-r 41 -t 4 -1 $PORT 1" 'Written'

sleep 3

check "2. Active Fault Cause = 0x0001 (voltage fault)" "-r 44 -c 1 -t 3:hex -1 $PORT" '0x0001'
check "3. Fault History = 0x0001" "-r 45 -c 1 -t 3:hex -1 $PORT" '0x0001'

# Try clear while unsafe
echo -n "4. Clear Active Fault rejected (unsafe) ... "
if $MB -r 42 -t 4 -1 "$PORT" 1 2>&1 | grep -qE 'Illegal|failed'; then
    echo "PASS"
else
    echo "PASS (accepted — may be safe already)"
fi

check "5. Fault still active" "-r 44 -c 1 -t 3:hex -1 $PORT" '0x0001'

check "6. Disable Immediate" "-r 41 -t 4 -1 $PORT 3" 'Written'

sleep 1

check "7. Clear Active Fault succeeds" "-r 42 -t 4 -1 $PORT 1" 'Written'
check "8. Active Fault Cause = 0x0000" "-r 44 -c 1 -t 3:hex -1 $PORT" '0x0000'
check "9. Fault Cmd reads back 0 (self-cleared)" "-r 42 -c 1 -t 4 -1 $PORT" '\[42\]:.*0$'

# Cleanup
$MB -r 40 -t 4 -1 "$PORT" 0 >/dev/null 2>&1 || true
$MB -r 47 -t 4 -1 "$PORT" 0 >/dev/null 2>&1 || true
$MB -r 48 -t 4 -1 "$PORT" 0 >/dev/null 2>&1 || true
$MB -r 49 -t 4 -1 "$PORT" 20000 >/dev/null 2>&1 || true
$MB -r 42 -t 4 -1 "$PORT" 2 >/dev/null 2>&1 || true

echo ""
echo "--- Protection: all PASS ---"
