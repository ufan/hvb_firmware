#!/usr/bin/env bash
# validation.sh — Edge cases: reserved writes, invalid enums, unsupported channels
# Usage: PORT=/dev/ttyUSB0 ./validation.sh

PORT="${PORT:-/dev/ttyUSB0}"
MB="mbpoll -m rtu -a 1 -b 115200 -d 8 -s 1 -P none -0"

echo "--- Validation Test ---"
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

check_err() {
    local desc="$1" cmd="$2"
    echo -n "$desc ... "
    if $MB $cmd 2>&1 | grep -qE 'Illegal|failed'; then
        echo "PASS"
    else
        echo "FAIL"
        exit 1
    fi
}

# Reserved register writes rejected
check_err "1. Write reserved sys register (offset 9) rejected" "-r 9 -t 4 -1 $PORT 42"
check_err "2. Write reserved ch register (offset 61) rejected" "-r 61 -t 4 -1 $PORT 42"

# Invalid enum values
check_err "3. Operating Mode = 2 rejected" "-r 0 -t 4 -1 $PORT 2"
check_err "4. Recovery Policy = 4 rejected" "-r 3 -t 4 -1 $PORT 4"
check_err "5. Voltage Protection Mode = 3 rejected" "-r 47 -t 4 -1 $PORT 3"
check_err "6. Current Protection Mode = 3 rejected" "-r 50 -t 4 -1 $PORT 3"

# Range validation
check_err "7. Voltage Safe Band = 51 (>50) rejected" "-r 7 -t 4 -1 $PORT 51"
check_err "8. Slave Address = 248 (>247) rejected" "-r 1 -t 4 -1 $PORT 248"
check_err "9. Save Target Policy = 2 rejected" "-r 54 -t 4 -1 $PORT 2"

# Host output action context
check_err "10. Output Action=4 (ForceZero) rejected from host" "-r 41 -t 4 -1 $PORT 4"
check_err "11. Output Action=5 (Clamp) rejected from host" "-r 41 -t 4 -1 $PORT 5"

# Current protection output action context
check_err "12. Current Prot Action=5 (Clamp) rejected" "-r 51 -t 4 -1 $PORT 5"
check     "13. Current Prot Action=2 (Disable Graceful) accepted" "-r 51 -t 4 -1 $PORT 2" 'Written'
$MB -r 51 -t 4 -1 "$PORT" 0 >/dev/null 2>&1 || true

# Unsupported channel
check_err "14. Channel 2 holding read rejected" "-r 120 -c 1 -t 4 -1 $PORT"
check_err "15. Channel 2 input read rejected" "-r 120 -c 1 -t 3 -1 $PORT"

# Extension block
check "16. Extension block read = 0" "-r 200 -c 1 -t 4 -1 $PORT" '\[200\]:.*0$'
check_err "17. Extension block write rejected" "-r 200 -t 4 -1 $PORT 42"

# Channel 1 write/readback
echo -n "18. Channel 1 write 8000, readback ... "
if $MB -r 80 -t 4 -1 "$PORT" 8000 2>&1 | grep -q 'Written'; then
    if $MB -r 80 -c 1 -t 4 -1 "$PORT" 2>/dev/null | grep -q '\[80\]:.*8000$'; then
        $MB -r 80 -t 4 -1 "$PORT" 0 >/dev/null 2>&1 || true
        echo "PASS"
    else
        echo "FAIL (readback mismatch)"
        exit 1
    fi
else
    echo "FAIL (write failed)"
    exit 1
fi

echo ""
echo "--- Validation: 18/18 PASS ---"
