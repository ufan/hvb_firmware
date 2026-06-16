#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build/test"

echo "=== Building tests ==="
cmake -B "${BUILD_DIR}" -G Ninja -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -3
cmake --build "${BUILD_DIR}" --target hvb_tests 2>&1 | tail -5

echo ""
echo "=== Running tests ==="
"${BUILD_DIR}/tests/hvb_tests" "$@"
echo ""

if [ -n "${PORT:-}" ]; then
    echo "=== Running CLI integration tests (PORT=$PORT) ==="
    "$SCRIPT_DIR/tests/cli_crosscheck.sh"
    "$SCRIPT_DIR/tests/cli_run_all.sh"
    echo ""
fi

echo "=== Tests complete ==="
