#!/bin/sh
# Safety properties of the `uninstall` target (cmake/uninstall.cmake).
#
# Asserts the two rules that stop an uninstall becoming a delete:
#
#   1. A directory the install did not populate is NEVER removed, even when it
#      is named in CANDIDATES. Authorisation comes from the manifest, not from
#      the list.
#   2. A directory that is not empty is NEVER removed. Only DERIVED artefacts
#      (__pycache__ — made by the interpreter from a file we installed) are
#      purged by exact name; anything else keeps the directory alive.
#
# Plus the property those two exist to serve: a normal uninstall must leave NO
# importable `accudisc`. Deleting the files but keeping the directory turns
# site-packages/accudisc/ into a PEP 420 namespace package, and `import
# accudisc` then succeeds with an empty module — worse than failing.
#
# Runs a real install into a temp prefix; touches nothing outside it.
#
# usage: uninstall_safety.sh <source-dir> <cmake> <python3-or-empty>
set -eu

SRC=$1
CMAKE=$2
PY=${3:-}

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM
PREFIX=$WORK/prefix
BUILD=$WORK/build

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok   $*"; }

"$CMAKE" -B "$BUILD" -S "$SRC" \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DACCUDISC_BUILD_TESTS=OFF \
    -DACCUDISC_SETCAP_AFTER_BUILD=OFF \
    -DACCUDISC_SETCAP_ON_INSTALL=OFF \
    >"$WORK/cfg.log" 2>&1 || { cat "$WORK/cfg.log" >&2; fail "configure"; }
"$CMAKE" --build "$BUILD" -j4 >"$WORK/build.log" 2>&1 \
    || { tail -30 "$WORK/build.log" >&2; fail "build"; }
"$CMAKE" --install "$BUILD" >"$WORK/inst.log" 2>&1 \
    || { cat "$WORK/inst.log" >&2; fail "install"; }

MANIFEST=$BUILD/install_manifest.txt
[ -s "$MANIFEST" ] || fail "no install manifest"

LIBDIR=$(sed -n 's|^\(.*\)/libaccudisc\.so\.[0-9].*$|\1|p' "$MANIFEST" | head -1)
[ -n "$LIBDIR" ] || fail "could not locate the installed libdir from the manifest"

# ---- property 1: a directory we never installed into is not ours ------------
# .../lib*/accudisc/drivers IS in CANDIDATES. Its sibling is not, but the point
# here is stronger: make a directory that matches nothing in the manifest and
# confirm the run does not touch it even though it sits inside our own tree.
NOTOURS=$LIBDIR/accudisc/notours
mkdir -p "$NOTOURS"
: >"$NOTOURS/keepme"

# ---- property 2: a non-empty directory of ours survives, with its contents --
# Put a foreign file in a directory the install DID populate and that would
# otherwise be removed. Both the file and the directory must survive.
FOREIGN=$LIBDIR/accudisc/drivers/foreign.conf
echo "not ours" >"$FOREIGN"

# Make the __pycache__ the manifest cannot see, if we have an interpreter.
PYPKG=$(sed -n 's|^\(.*/site-packages/accudisc\)/__init__\.py$|\1|p' "$MANIFEST" | head -1)
if [ -n "$PY" ] && [ -n "$PYPKG" ]; then
    SITE=$(dirname "$PYPKG")
    PYTHONPATH=$SITE "$PY" -c "import accudisc" >/dev/null 2>&1 || true
fi

"$CMAKE" --build "$BUILD" --target uninstall >"$WORK/uninst.log" 2>&1 \
    || { cat "$WORK/uninst.log" >&2; fail "uninstall exited nonzero"; }

# --- property 1 ---
[ -d "$NOTOURS" ] || fail "removed a directory the install never populated: $NOTOURS"
[ -f "$NOTOURS/keepme" ] || fail "removed a file inside a directory that is not ours"
pass "a directory with no installed file under it is left alone"

# --- property 2 ---
[ -f "$FOREIGN" ] || fail "removed a foreign file from a directory of ours: $FOREIGN"
[ -d "$LIBDIR/accudisc/drivers" ] || fail "removed a NON-EMPTY directory: $LIBDIR/accudisc/drivers"
pass "a non-empty directory of ours is kept, with its foreign contents"

# The driver we installed must still be gone — keeping the directory must not
# mean keeping our files. Without this, "keep everything" would pass above.
if ls "$LIBDIR"/accudisc/drivers/accudisc-drv-*.so >/dev/null 2>&1; then
    fail "kept an installed driver"
fi
pass "our own files are removed even when the directory has to stay"

# --- every manifest entry is gone ---
while IFS= read -r f; do
    [ -e "$f" ] && fail "manifest entry survived: $f"
    [ -L "$f" ] && fail "manifest symlink survived: $f"
done <"$MANIFEST"
pass "every manifest entry removed (including dangling symlinks)"

# --- no importable phantom ---
if [ -n "$PY" ] && [ -n "$PYPKG" ]; then
    SITE=$(dirname "$PYPKG")
    [ -d "$PYPKG" ] && fail "package directory survived: $PYPKG (namespace phantom)"
    if (cd / && PYTHONPATH=$SITE "$PY" -c "import accudisc" >/dev/null 2>&1); then
        fail "'import accudisc' still succeeds after uninstall (PEP 420 phantom)"
    fi
    pass "no importable accudisc remains"
else
    echo "skip python phantom check (no interpreter or no binding installed)"
fi

echo "uninstall safety: all checks passed"
