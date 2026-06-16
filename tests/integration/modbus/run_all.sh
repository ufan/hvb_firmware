#!/usr/bin/env bash
# run_all.sh — Orchestrator for all integration test scripts
# Usage: PORT=/dev/ttyUSB0 ./run_all.sh

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
export PORT="${PORT:-/dev/ttyUSB0}"

echo "============================================"
echo "  HVB Modbus Integration Tests"
echo "  PORT=$PORT"
echo "============================================"
echo ""

PASS=0
FAIL=0
FAIL_NAMES=""

run_test() {
    local test="$1"
    local name
    name=$(basename "$test")

    echo ""
    if "$test"; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        FAIL_NAMES="$FAIL_NAMES $name"
        echo "*** $name FAILED ***"
    fi
}

run_test "$SCRIPT_DIR/smoke.sh"
run_test "$SCRIPT_DIR/simulation.sh"
run_test "$SCRIPT_DIR/protection.sh"
run_test "$SCRIPT_DIR/validation.sh"

echo ""
echo "============================================"
echo "  Results: $PASS passed, $FAIL failed"
if [ "$FAIL" -gt 0 ]; then
    echo "  Failed:$FAIL_NAMES"
fi
echo "============================================"

[ "$FAIL" -eq 0 ]
