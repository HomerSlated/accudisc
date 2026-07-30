#!/bin/sh
# The installed RUNPATH must track the prefix the tree is CURRENTLY configured
# for — including after a reconfigure, which is where it broke.
#
# ACCUDISC_INSTALL_RPATH is a cache knob defaulting to the installed libdir, and
# a cache default is evaluated ONLY when the entry is absent. So it was right on
# the first configure and frozen after: re-pointing a /usr/local tree at /usr
# left both the CLI and the Python extension carrying RUNPATH=/usr/local/lib64,
# into a directory the uninstall had just emptied. Nothing said so, because
# every log line afterwards was consistent with itself, and the install still
# ran because /usr/lib64 is a default loader path — ld.so never consulted the
# RUNPATH at all. Aim the second prefix somewhere unsearched and the same
# install is dead on arrival.
#
# Both prefixes here are under a temp dir precisely so the loader has no
# fallback: if the RUNPATH is wrong, it is wrong in a way that can be observed.
#
# The second half is the other failure this guard has to prevent: a knob that
# follows the prefix must NOT overwrite a value the user chose. Both directions
# are asserted, because a fix for one is a natural way to break the other.
#
# usage: install_relocate.sh <source-dir> <cmake>
set -eu

SRC=$1
CMAKE=$2

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok   $*"; }

if command -v readelf >/dev/null 2>&1; then
    runpath() { readelf -d "$1" | sed -n 's/.*R\(UN\)\?PATH.*\[\(.*\)\]/\2/p' | head -1; }
elif command -v objdump >/dev/null 2>&1; then
    runpath() { objdump -p "$1" | awk '/RUNPATH|RPATH/ {print $2; exit}'; }
else
    echo "skip: neither readelf nor objdump available"
    exit 77
fi

# Configure + build + install into $2, reusing build tree $1.
install_to() {
    _b=$1; _p=$2
    shift 2
    "$CMAKE" -B "$_b" -S "$SRC" \
        -DCMAKE_INSTALL_PREFIX="$_p" \
        -DACCUDISC_BUILD_TESTS=OFF \
        -DACCUDISC_SETCAP_AFTER_BUILD=OFF \
        -DACCUDISC_SETCAP_ON_INSTALL=OFF \
        "$@" \
        >"$WORK/cfg.log" 2>&1 || { cat "$WORK/cfg.log" >&2; fail "configure $_p"; }
    "$CMAKE" --build "$_b" -j4 >"$WORK/build.log" 2>&1 \
        || { tail -30 "$WORK/build.log" >&2; fail "build $_p"; }
    "$CMAKE" --install "$_b" >"$WORK/inst.log" 2>&1 \
        || { cat "$WORK/inst.log" >&2; fail "install $_p"; }
}

# The libdir name is the toolchain's choice (lib vs lib64 vs lib/<triple>), so
# read it back from the manifest rather than assuming one.
libdir_of() {
    sed -n 's|^\(.*\)/libaccudisc\.so\.[0-9].*$|\1|p' "$1/install_manifest.txt" | head -1
}

# ---- the knob follows the prefix across a reconfigure ------------------------
BUILD=$WORK/build
A=$WORK/a
B=$WORK/b

install_to "$BUILD" "$A"
LIB_A=$(libdir_of "$BUILD")
[ -n "$LIB_A" ] || fail "no libaccudisc in the first manifest"
got=$(runpath "$A/bin/accudisc")
[ "$got" = "$LIB_A" ] || fail "first install: RUNPATH is '$got', expected '$LIB_A'"
pass "a fresh configure records the installed libdir"

# The same build tree, re-pointed. This is the case that was broken.
install_to "$BUILD" "$B"
LIB_B=$(libdir_of "$BUILD")
[ -n "$LIB_B" ] || fail "no libaccudisc in the second manifest"
[ "$LIB_B" != "$LIB_A" ] || fail "the two prefixes produced the same libdir — the test proves nothing"

got=$(runpath "$B/bin/accudisc")
[ "$got" = "$LIB_B" ] || fail "after reconfigure: CLI RUNPATH is '$got', expected '$LIB_B' (stale cache)"
pass "a reconfigured prefix moves the CLI RUNPATH with it"

# The extension shares the knob deliberately — it loads the same library, so a
# stale value there is the same defect wearing different clothes.
EXT=$(sed -n 's|^\(.*_accudisc\.abi3\.so\)$|\1|p' "$BUILD/install_manifest.txt" | head -1)
if [ -n "$EXT" ] && [ -f "$EXT" ]; then
    got=$(runpath "$EXT")
    [ "$got" = "$LIB_B" ] || fail "after reconfigure: extension RUNPATH is '$got', expected '$LIB_B'"
    pass "the Python extension moves with it"
else
    echo "skip extension check (binding not installed)"
fi

# The package must also LAND under the new prefix, not the old one.
grep -q "^$B/" "$BUILD/install_manifest.txt" \
    || fail "nothing was installed under the new prefix"
grep -q "^$A/" "$BUILD/install_manifest.txt" \
    && fail "the second install still wrote under the old prefix"
pass "everything lands under the new prefix"

# ---- an explicit value is NOT overwritten -----------------------------------
# The fix for the above must not become "always follow the prefix", which would
# silently discard a deliberate override — including the EMPTY one that distro
# packaging uses.
BUILD2=$WORK/build2
C=$WORK/c
D=$WORK/d
CHOSEN=/opt/accudisc-chosen-by-the-user

install_to "$BUILD2" "$C" -DACCUDISC_INSTALL_RPATH="$CHOSEN"
got=$(runpath "$C/bin/accudisc")
[ "$got" = "$CHOSEN" ] || fail "an explicit RPATH was not honoured: got '$got'"

install_to "$BUILD2" "$D"
got=$(runpath "$D/bin/accudisc")
[ "$got" = "$CHOSEN" ] || fail "a user's explicit RPATH was overwritten on reconfigure: got '$got'"
pass "an explicitly-set RPATH survives a prefix change"

echo "install relocation: all checks passed"
