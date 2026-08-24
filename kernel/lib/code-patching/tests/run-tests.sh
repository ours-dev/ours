#!/bin/bash
# Run code-patching assembly macro tests.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJ_DIR="$SCRIPT_DIR/.."
TMPDIR="${TMPDIR:-/tmp}"
PASS=0
FAIL=0

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

INCLUDES=(
  -I "$SCRIPT_DIR"
  -I "$PROJ_DIR/include"
  -I "$PROJ_DIR/../arch/include"
)

echo "=== Code-Patching Assembly Macro Tests ==="
echo ""

# ── Assembly test ────────────────────────────────────────────────────────

SMOKE_SRC="$SCRIPT_DIR/code-patching-smoke.S"
SMOKE_O="$TMPDIR/code-patch-test-smoke.o"

if gcc -x assembler-with-cpp "${INCLUDES[@]}" -c -o "$SMOKE_O" "$SMOKE_SRC" 2>"$TMPDIR/code-patch-test-smoke.err"; then
  echo -e "${GREEN}PASS${NC} smoke"
  PASS=$((PASS + 1))
else
  echo -e "${RED}FAIL${NC} smoke — unexpected assembly error:"
  cat "$TMPDIR/code-patch-test-smoke.err" | sed 's/^/  /'
  FAIL=$((FAIL + 1))
fi

# ── Deep verification ────────────────────────────────────────────────────

echo ""
echo "=== Deep Verification ==="

# Verify .code-patches section: 8 entries × 24 bytes = 192 = 0xc0
SECTION_SIZE=$(objdump -h "$SMOKE_O" 2>/dev/null | awk '/code-patches/ {print "0x"$3; exit}')
EXPECTED_SIZE="0x000000c0"
if [ "$SECTION_SIZE" = "$EXPECTED_SIZE" ]; then
  echo -e "${GREEN}PASS${NC} .code-patches section size = $SECTION_SIZE (8 entries × 24 bytes)"
  PASS=$((PASS + 1))
else
  echo -e "${RED}FAIL${NC} .code-patches section size: expected $EXPECTED_SIZE, got $SECTION_SIZE"
  FAIL=$((FAIL + 1))
fi

# Verify patch label sizes (readelf reports decimal)
check_size() {
  local label="$1" expected="$2"
  local actual
  actual=$(readelf -s "$SMOKE_O" 2>/dev/null | awk -v sym="$label" '$NF == sym {print $3; exit}')
  if [ "$actual" = "$expected" ]; then
    echo -e "${GREEN}PASS${NC} $label .size = $expected"
    PASS=$((PASS + 1))
  else
    echo -e "${RED}FAIL${NC} $label .size: expected $expected, got $actual"
    FAIL=$((FAIL + 1))
  fi
}

check_size "patch_alt_larger"      "5"     # 3 code + 2 pad
check_size "patch_alt_much_larger" "32"    # 7 code + 25 pad
check_size "patch_alt_equals"      "5"     # 5 code + 0 pad
check_size "patch_no_alt"          "7"     # 7 code + 0 pad
check_size "patch_code_larger"     "8"     # 8 code + 0 pad (code > alt, no padding)
check_size "patch_first"           "5"     # 3 code + 2 pad
check_size "patch_second"          "32"    # 8 code + 24 pad
check_size "test_hermetic_blob"    "16"    # from .code_patching.blob .skip

echo ""
echo "=== Metadata hex dump (.code-patches section) ==="
objdump -s -j .code-patches "$SMOKE_O" 2>/dev/null | tail -n +5

echo ""
echo -e "Total: ${GREEN}$PASS passed${NC}, ${RED}$FAIL failed${NC}"
if [ "$FAIL" -gt 0 ]; then
  exit 1
fi
