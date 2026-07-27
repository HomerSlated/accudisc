#!/bin/sh
# Build and run the Python binding's device-free test suite.
#
# SKIPS (exit 77) rather than fails when Python or cffi is absent: the C
# library must stay buildable by someone who has no interest in the binding.
# It does NOT skip when the extension fails to compile or a test fails — those
# are real breakage, and a gate that treats them as "not applicable" is a gate
# that never fires.
#
# Args: <repo source dir> <library dir (where libaccudisc.so was built)>
set -eu

SRC="$1"
LIBDIR="$2"
PYDIR="$SRC/bindings/python"

command -v python3 >/dev/null 2>&1 || {
    echo "SKIP: no python3"; exit 77; }
python3 -c "import cffi" >/dev/null 2>&1 || {
    echo "SKIP: python3 has no cffi module"; exit 77; }
[ -d "$PYDIR" ] || { echo "SKIP: $PYDIR missing"; exit 77; }

# Point the builder at THIS build tree, not at whatever may be installed —
# otherwise the suite could validate a stale system library and pass.
ACCUDISC_INCLUDE_DIR="$SRC/include"
ACCUDISC_LIB_DIR="$LIBDIR"
export ACCUDISC_INCLUDE_DIR ACCUDISC_LIB_DIR

cd "$PYDIR"
echo "building extension against $ACCUDISC_LIB_DIR"
python3 build_accudisc.py >/dev/null

PYTHONPATH="$PYDIR" python3 tests/test_binding.py
