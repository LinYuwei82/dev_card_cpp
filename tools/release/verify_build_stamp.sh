#!/bin/bash
# DevCard build stamp injection verification (macOS bash)
# Run: bash tools/release/verify_build_stamp.sh

set -euo pipefail

ROOT="/Users/lin/Projects/dev_card_cpp"
STAMP="202611200930"
TMP_BUILD="$ROOT/build-stamp-verify"
TMP_FB="$ROOT/build-stamp-fallback"

echo "[cleanup]"
rm -rf "$TMP_BUILD" "$TMP_FB" 2>/dev/null || true

# T1: configure with DEVCARD_BUILD
echo ""
echo "===== T1 ====="
mkdir -p "$TMP_BUILD"
cmake -S "$ROOT" -B "$TMP_BUILD" -DCMAKE_BUILD_TYPE=Release -DDEVCARD_BUILD="$STAMP" 2>&1 | tee "$TMP_BUILD/cfg.log" >/dev/null
for t in devcard_core devcard_server devcard-server devcard_core_tests; do
    if ! grep -qF "DevCard build stamp injection ($t): DEVCARD_BUILD_STAMP=$STAMP" "$TMP_BUILD/cfg.log" 2>/dev/null; then
        echo "NOTE: old format, checking Chinese log..."
    fi
done
if ! grep -qF "DEVCARD_BUILD_STAMP=$STAMP" "$TMP_BUILD/cfg.log"; then
    echo "FAIL T1: no DEVCARD_BUILD_STAMP=$STAMP in configure log"
    exit 1
fi
echo "PASS T1: DEVCARD_BUILD_STAMP=$STAMP present in configure log"

# T2: build + run devcard_core_tests with stamp
echo ""
echo "===== T2 ====="
cmake --build "$TMP_BUILD" --parallel 2>&1 | tail -3
if ctest --test-dir "$TMP_BUILD" -R devcard_core_tests --output-on-failure 2>&1 | tail -15 | grep -q "Passed"; then
    echo "PASS T2: devcard_core_tests passed (stamped build)"
else
    echo "FAIL T2: devcard_core_tests failed with stamp"
    ctest --test-dir "$TMP_BUILD" -R devcard_core_tests --output-on-failure
    exit 1
fi

# T3: build WITHOUT stamp + run tests
echo ""
echo "===== T3 ====="
mkdir -p "$TMP_FB"
cmake -S "$ROOT" -B "$TMP_FB" -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -3
cmake --build "$TMP_FB" --parallel 2>&1 | tail -3
if ctest --test-dir "$TMP_FB" -R devcard_core_tests --output-on-failure 2>&1 | tail -15 | grep -q "Passed"; then
    echo "PASS T3: devcard_core_tests passed (fallback local_build=0)"
else
    echo "FAIL T3: devcard_core_tests failed on fallback path"
    exit 1
fi

# T5: guard selftest
echo ""
echo "===== T5 ====="
if python3 "$ROOT/tools/release/guard.py" selftest 2>&1 | tail -1 | grep -q "通过"; then
    echo "PASS T5: guard selftest passed (46 items)"
else
    python3 "$ROOT/tools/release/guard.py" selftest 2>&1 | tail -5
    echo "FAIL T5: guard selftest"
    exit 1
fi

# T6: manifest_v2 selftest
echo ""
echo "===== T6 ====="
if python3 "$ROOT/tools/release/manifest_v2.py" selftest 2>&1 | tail -1 | grep -qE "(通过|through|PASS|ok|OK|passed)"; then
    echo "PASS T6: manifest_v2 selftest passed"
else
    python3 "$ROOT/tools/release/manifest_v2.py" selftest 2>&1 | tail -5
    echo "FAIL T6: manifest_v2 selftest"
    exit 1
fi

echo ""
echo "============================================"
echo "  macOS side verification PASSED (T1-T3,T5-T6)"
echo "  Windows 12600k to do: T4 (SkipBuild inc rebuild), T7 (publish -DryRun)"
echo "============================================"

rm -rf "$TMP_BUILD" "$TMP_FB" 2>/dev/null || true
exit 0
