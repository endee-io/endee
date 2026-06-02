#!/usr/bin/env bash
# Run every numbered test in this directory in order. Each test is a
# standalone Python program that exits 0 (pass) or non-zero (fail).
#
# Same scripts run on master and single_txn; behaviour differs by branch.
#
# Build the server binary first (e.g. `cmake --build build-acid --target
# ndd-avx2`) or set NDD_BINARY=/path/to/ndd-avx2.

set -uo pipefail

cd "$(dirname "$0")"

passes=0
fails=0
fail_names=()

for test in $(ls *.py | grep -E '^[0-9]+_' | sort); do
    echo
    echo "===== $test ====="
    if python3 "$test"; then
        passes=$((passes + 1))
    else
        fails=$((fails + 1))
        fail_names+=("$test")
    fi
done

echo
echo "================================================================"
echo "Summary: $passes passed, $fails failed"
if [ $fails -gt 0 ]; then
    echo "Failed tests:"
    for name in "${fail_names[@]}"; do
        echo "  - $name"
    done
    exit 1
fi
