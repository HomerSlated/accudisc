#!/usr/bin/env bash
#
# install.sh — build, install and uninstall AccuDisc in one command.
#
# This is a convenience wrapper around the CMake build, not a second build
# system. Every step below is a documented `cmake` invocation you can run by
# hand (see README.md, "Installing"); the script exists because the *defaults*
# CMake ships are tuned for this project's development machine, and three of
# them are actively wrong for anyone else. It picks the right ones, does the
# privilege split correctly, and tells you what to do next.
#
#   ./install.sh                      # build + install to /usr/local
#   ./install.sh --prefix ~/.local    # ...somewhere else
#   ./install.sh uninstall            # remove it again
#   ./install.sh --help
#
# WHAT IT INSTALLS: the CLI, the shared and static library, the public headers,
# the vendor drivers, the pkg-config file, the man pages, and — by default — the
# Python *wheel*. See "THE PYTHON QUESTION" below for why a wheel and not a
# site-packages copy.
#
# WHAT IT DOES NOT INSTALL: recovery profiles. AccuDisc has none and ships none.
# It provides the composable recovery *levers* (--verify, --c2-retries,
# --ladder); which levers to pull for a given damage class is policy, and policy
# lives in the calling application — as do the absolute gates (AccurateRip,
# CTDB) that a profile is scored against. cdda2img owns its profiles and ships
# them itself. Do not add a profiles directory here; it would be a second,
# silently divergent source of truth for something this project does not decide.
# (The `profile=0x0008` in `accudisc disc` output is the MMC *media type* code —
# an unrelated homonym. README.md:222 spells this out.)
#
# ---------------------------------------------------------------------------
# THE THREE DEFAULTS THIS SCRIPT OVERRIDES, AND WHY
# ---------------------------------------------------------------------------
#
# 1. ACCUDISC_SETCAP_AFTER_BUILD=OFF (CMake default: ON)
#
#    The CMake default re-applies cap_sys_rawio to the build-tree binary after
#    every link, because a file capability binds to the INODE and every relink
#    produces a fresh one. That is a genuine convenience *on a machine with a
#    passwordless privilege rule for setcap*, and a hard build failure on every
#    machine without one — which is every machine but the developer's. So the
#    distribution default has to be OFF. Nothing is lost: the capability that
#    matters is the one on the INSTALLED binary (ACCUDISC_SETCAP_ON_INSTALL,
#    left ON), whose inode is stable and therefore stays armed across rebuilds.
#
# 2. ACCUDISC_INSTALL_PYTHON=OFF + ACCUDISC_INSTALL_WHEEL=ON (defaults: ON/OFF)
#
#    See "THE PYTHON QUESTION".
#
# 3. ACCUDISC_BUILD_TESTS follows --run-tests rather than being always on.
#
# Override any of them yourself with --cmake-arg; yours are passed after ours,
# and CMake honours the last occurrence.
#
# ---------------------------------------------------------------------------
# THE PYTHON QUESTION — why a wheel, and not site-packages
# ---------------------------------------------------------------------------
#
# `make install` can copy the Python package into
# $PREFIX/lib/pythonX.Y/site-packages. That directory is derived correctly (it
# is exactly what Python's own sysconfig names for this prefix) and is, on many
# systems, on NO interpreter's sys.path. Measured on Void Linux: a /usr/local
# install produced a package that `import accudisc` could not find, because the
# system interpreter searches /usr/lib/python3.14/site-packages and nothing
# under /usr/local.
#
# The obvious "fix" is to install into /usr instead. Do not: that tree is owned
# by the distribution's package manager, and modern ones mark it
# EXTERNALLY-MANAGED (PEP 668) precisely to stop this. Writing there produces
# files no package manager knows about, and a package manager that does not know
# about a file cannot upgrade, remove, or reason about it.
#
# So this script installs the WHEEL, into
#
#     $PREFIX/share/accudisc/wheel/accudisc-<version>-cp310-abi3-<plat>.whl
#
# and leaves the choice of environment to whoever consumes it. A wheel is what
# every Python installer already understands, so it drops into a venv, a pipx
# venv, or a user install without any of them having to know AccuDisc exists.
#
# WHY UNDER THE PREFIX: the wheel's compiled extension carries a RUNPATH
# pointing at this prefix's libdir, so it is only valid for the libaccudisc.so.0
# installed beside it. Keeping the two together is the invariant; a fixed global
# location would break the moment a second prefix existed.
#
# THE DIRECTORY IS THE CONTRACT, NOT THE FILENAME. The name carries the version
# and the ABI/platform tags, which installers parse, so it changes on every
# version bump and must not be renamed to something stable. Consumers glob:
#
#     ls "$PREFIX"/share/accudisc/wheel/accudisc-*.whl
#
# The same path is also published as a pkg-config variable —
# `pkg-config --variable=wheeldir accudisc` — for consumers that already use
# pkg-config. Treat that as the bonus route, not the primary one: pkg-config
# does not search /usr/local's pkgconfig directory on every distribution, so it
# can fail on a perfectly good install.
#
# Pass --sitedir if you want the old behaviour as well; it is additive, not a
# replacement, and it is only a good idea when $PREFIX is /usr (distro
# packaging, where the package manager is the one writing the file) or when you
# have checked that the prefix's site-packages really is on sys.path.
#
# ---------------------------------------------------------------------------
# PRIVILEGE: the build is unprivileged, only the install is not
# ---------------------------------------------------------------------------
#
# Do NOT run this script with sudo. It escalates only around `cmake --install`,
# and does so by itself.
#
# Running the whole thing as root leaves a build tree full of root-owned object
# files, which then breaks the next ordinary `cmake --build` with permission
# errors that name a .o file and nothing about the cause. It also runs a
# compiler, and every code generator in the build, as root for no reason.
#
# If $PREFIX is already writable (say ~/.local) nothing is escalated at all.
#
set -euo pipefail

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
PREFIX=/usr/local
BUILD_DIR=build
JOBS=""
BUILD_TYPE=Release
WANT_WHEEL=1        # ACCUDISC_INSTALL_WHEEL
WANT_SITEDIR=0      # ACCUDISC_INSTALL_PYTHON
WANT_TESTS=0        # build and run ctest before installing
WANT_LDCONFIG=0     # run ldconfig after installing
EMPTY_RPATH=0       # -DACCUDISC_INSTALL_RPATH=""
DRY_RUN=0
ACTION=install
EXTRA_CMAKE_ARGS=()

# The script's own directory, so it works from anywhere. Not $PWD: `cd /tmp &&
# ~/src/accudisc/install.sh` must configure the checkout, not /tmp.
SRC_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)

# ---------------------------------------------------------------------------
# Output helpers. Everything diagnostic goes to stderr so that stdout stays
# usable for the few things worth capturing (--print-wheel).
# ---------------------------------------------------------------------------
if [ -t 2 ]; then
    C_B=$'\033[1m'; C_R=$'\033[31m'; C_Y=$'\033[33m'; C_G=$'\033[32m'; C_0=$'\033[0m'
else
    C_B=""; C_R=""; C_Y=""; C_G=""; C_0=""
fi
say()  { printf '%s==>%s %s\n' "$C_B" "$C_0" "$*" >&2; }
warn() { printf '%swarning:%s %s\n' "$C_Y" "$C_0" "$*" >&2; }
die()  { printf '%serror:%s %s\n'   "$C_R" "$C_0" "$*" >&2; exit 1; }
ok()   { printf '%s  ok%s %s\n'     "$C_G" "$C_0" "$*" >&2; }

# Echo a command, then run it — unless --dry-run, in which case only echo. Every
# privileged or filesystem-touching step goes through this, so --dry-run is a
# complete and honest preview rather than an approximation.
run() {
    printf '    %s\n' "$*" >&2
    [ "$DRY_RUN" -eq 1 ] && return 0
    "$@"
}

usage() {
    cat <<'EOF'
usage: ./install.sh [options]            build and install
       ./install.sh uninstall [options]  remove a previous install
       ./install.sh --print-wheel        print the built wheel's path and exit

Options:
  --prefix DIR       install prefix (default: /usr/local)
  --build-dir DIR    build tree (default: build) — for uninstall, this MUST be
                     the tree that performed the install
  --jobs N           parallel build jobs (default: cmake's own choice)
  --debug            CMAKE_BUILD_TYPE=Debug (implies no stripping)
  --run-tests        build and run the test suite before installing
  --no-wheel         do not build or install the Python wheel. On its own —
                     without --sitedir — this installs NO Python binding at all
  --sitedir          ALSO install the Python package into the prefix's
                     site-packages (see the header; usually not what you want)
  --empty-rpath      record no RUNPATH — correct for --prefix=/usr and distro
                     packaging, broken anywhere ld.so does not already search
                     the install libdir
  --ldconfig         run ldconfig after installing (Linux, root, glibc)
  --cmake-arg ARG    pass ARG to cmake configure (repeatable, wins over ours)
  -n, --dry-run      print every command without running any of them
  -h, --help         this text

Environment:
  ACCUDISC_SUDO      privilege command to use (default: sudo, then doas)
EOF
}

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
if [ $# -gt 0 ] && [ "${1#-}" = "$1" ]; then
    case "$1" in
        install|uninstall) ACTION=$1; shift ;;
        *) die "unknown command '$1' (expected: install, uninstall)" ;;
    esac
fi

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix)      [ $# -ge 2 ] || die "--prefix needs an argument";    PREFIX=$2; shift 2 ;;
        --build-dir)   [ $# -ge 2 ] || die "--build-dir needs an argument"; BUILD_DIR=$2; shift 2 ;;
        --jobs|-j)     [ $# -ge 2 ] || die "--jobs needs an argument";      JOBS=$2; shift 2 ;;
        --cmake-arg)   [ $# -ge 2 ] || die "--cmake-arg needs an argument"; EXTRA_CMAKE_ARGS+=("$2"); shift 2 ;;
        --debug)       BUILD_TYPE=Debug; shift ;;
        --run-tests)   WANT_TESTS=1; shift ;;
        --no-wheel)    WANT_WHEEL=0; shift ;;
        --sitedir)     WANT_SITEDIR=1; shift ;;
        --empty-rpath) EMPTY_RPATH=1; shift ;;
        --ldconfig)    WANT_LDCONFIG=1; shift ;;
        --print-wheel) ACTION=print-wheel; shift ;;
        -n|--dry-run)  DRY_RUN=1; shift ;;
        -h|--help)     usage; exit 0 ;;
        *)             usage >&2; die "unknown option '$1'" ;;
    esac
done

# Relative --build-dir is resolved against the SOURCE tree, not $PWD, for the
# same reason SRC_DIR is: `./install.sh` from elsewhere must not scatter build
# trees wherever the user happened to be standing. An absolute path is honoured
# as given.
case "$BUILD_DIR" in
    /*) ;;
    *)  BUILD_DIR="$SRC_DIR/$BUILD_DIR" ;;
esac

# ---------------------------------------------------------------------------
# Privilege
# ---------------------------------------------------------------------------
# Resolved lazily and only for the steps that need it. `need_privilege` answers
# "is $1 a directory tree we cannot write to?" by walking up to the nearest
# existing ancestor — because the prefix itself usually does not exist yet, and
# testing an absent directory for writability always says no.
need_privilege() {
    local d=$1
    while [ ! -e "$d" ] && [ "$d" != "/" ]; do d=$(dirname "$d"); done
    [ ! -w "$d" ]
}

# Is this command the tool it claims to be? Not "does it exist" — that is the
# question that produced the bug this guards against.
#
# On Keith's Void box /bin/sudo is a 61-byte SHELL SCRIPT that prints "This is a
# dummy file. This system actually uses doas." and **exits 0**. `command -v sudo`
# finds it, `set -e` is happy, and `sudo cmake --build ... --target uninstall`
# printed the dummy's message and removed nothing — while install.sh reported
# "Done." Measured after the fact: 0 of 12 manifest files still gone.
#
# What does NOT detect it: `$SUDO true`, which succeeds because the placeholder
# exits 0 whatever you ask it. Nor `$SUDO id -u` requiring "0" — that one does
# reject the placeholder, but it needs an actual escalation, so it prompts, and
# in any non-interactive context it also rejects a PERFECTLY GOOD tool that
# simply cannot ask for a password right now. Measured: it rejected the real
# doas. A probe that cannot tell "broken" from "cannot ask yet" is the wrong
# probe.
#
# Keith's discriminator (2026-08-01) is better and is what this uses: ask the
# tool to identify itself. It costs no privilege, no prompt and no side effect.
#
#   sudo --version  -> "Sudo version 1.9.15p5"    genuine
#                   -> "This is a dummy file..."  placeholder
#   doas --version  -> "doas: invalid option" + "usage: doas [-Lns] ..."
#                      genuine — doas has NO version flag, and says so in a way
#                      only doas says it (measured; do not "fix" this to expect
#                      a version string).
#
# Unknown commands (a user's ACCUDISC_SUDO) are ACCEPTED: we cannot identify
# what we do not recognise, and refusing on that basis would break a legitimate
# wrapper. They are covered by the outcome verification after each privileged
# step, which is the backstop for every cause this cannot see — including a
# genuine sudo the user is not permitted to use.
# The output is CAPTURED FIRST, then matched — never `cmd | grep` directly.
# This script runs under `set -o pipefail`, and `doas --version` EXITS 1 because
# --version is not a doas option (that error text is exactly the evidence we
# want). Piped straight into grep, pipefail propagates the 1 and discards grep's
# success, so the real doas was rejected. Measured: the same expression returns
# 0 without pipefail and 1 with it — which is why the first version of this
# function tested clean in isolation and failed in the script.
looks_genuine() {
    local out
    out=$("$1" --version 2>&1 || true)
    case "${1##*/}" in
        sudo) printf '%s\n' "$out" | grep -qiE '^sudo version [0-9]' ;;
        doas) printf '%s\n' "$out" | grep -qiE 'usage: doas|^doas [0-9]' ;;
        *)    return 0 ;;
    esac
}

SUDO=""
resolve_sudo() {
    if [ "$(id -u)" -eq 0 ]; then SUDO=""; return 0; fi

    # An explicit ACCUDISC_SUDO wins, and is still identity-checked: naming
    # `sudo` on this machine would otherwise select the placeholder just as
    # surely as autodetection did. Only a RECOGNISED-and-fake command is
    # refused; anything unrecognised is accepted (see looks_genuine).
    if [ -n "${ACCUDISC_SUDO:-}" ]; then
        set -- $ACCUDISC_SUDO
        if looks_genuine "$1"; then SUDO=$ACCUDISC_SUDO; return 0; fi
        die "ACCUDISC_SUDO='$ACCUDISC_SUDO' does not look like a real '$1' — asking it
     to identify itself produced something else. On this system /bin/sudo is a
     placeholder that prints a message and exits 0. Nothing has been changed."
    fi

    # Candidates in preference order, but chosen by whether they are GENUINE.
    # Existence decides nothing; that was the bug. This costs no prompt, so a
    # placeholder is skipped silently rather than after an authentication round.
    for c in sudo doas; do
        command -v "$c" >/dev/null 2>&1 || continue
        if looks_genuine "$c"; then
            SUDO=$c
            return 0
        fi
        warn "'$c' exists but does not identify itself as $c — skipping it"
        warn "(on this system /bin/sudo is a placeholder that says to use doas)"
    done

    die "need to write to '$PREFIX', but no genuine privilege command was found.
     Tried: sudo, doas — one may exist as a placeholder; see the warnings above.
     Re-run as root, set ACCUDISC_SUDO=..., or choose a writable prefix:
         ./install.sh --prefix \"\$HOME/.local\""
}

# WILL THE INSTALL STEP BE PRIVILEGED? Decided HERE, before configure, because
# a CMake option depends on the answer and CMake options are fixed at configure
# time.
#
# ACCUDISC_SETCAP_ON_INSTALL defaults ON and is FATAL when setcap fails. That is
# right for the case it was written for — `sudo cmake --install` into a system
# prefix — and wrong for a prefix the user already owns: no escalation happens,
# so setcap runs unprivileged, fails with "unable to set CAP_SETFCAP effective
# capability", and takes the whole install down AFTER every file has landed.
# Found exactly that way, installing to a scratch prefix.
#
# So the option follows the privilege the install will actually have. Nothing is
# silently downgraded: when it goes OFF we say so, and the summary at the end
# prints the setcap command to run by hand.
#
# Only for `install`: uninstall derives its privilege from the manifest's own
# prefix instead, which is the one that reflects where the files really are.
INSTALL_PRIVILEGED=0
if [ "$ACTION" = install ]; then
    if [ "$(id -u)" -eq 0 ]; then
        INSTALL_PRIVILEGED=1
    elif need_privilege "$PREFIX"; then
        # Resolved now rather than after the build: discovering there is no
        # sudo should not cost a full compile first.
        resolve_sudo
        INSTALL_PRIVILEGED=1
    fi
fi

# The check the header warns about. Not fatal — a container or an image build is
# legitimately root and has no unprivileged user to drop to — but loud, because
# the failure it causes (root-owned objects in build/) surfaces much later and
# names only a .o file.
if [ "$(id -u)" -eq 0 ] && [ "$ACTION" = install ]; then
    warn "running as root: the COMPILER will run as root too, and '$BUILD_DIR'"
    warn "will fill with root-owned objects that a later unprivileged build"
    warn "cannot overwrite. Prefer running this as yourself — it escalates"
    warn "only for the install step, by itself."
fi

# ---------------------------------------------------------------------------
# Sanity checks
# ---------------------------------------------------------------------------
[ -f "$SRC_DIR/CMakeLists.txt" ] && [ -f "$SRC_DIR/include/accudisc/accudisc.h" ] \
    || die "'$SRC_DIR' does not look like an AccuDisc source tree"
command -v cmake >/dev/null 2>&1 || die "cmake not found (need 3.16 or newer)"

# --run-tests needs a tests/ directory, and the source DISTRIBUTION does not
# ship one: tools/mkdist.sh packages only the sources of what gets installed.
# Checked here rather than left to CMake so the message names the cause. The
# same guard exists in CMakeLists.txt for anyone configuring by hand.
if [ "$WANT_TESTS" -eq 1 ] && [ ! -f "$SRC_DIR/tests/CMakeLists.txt" ]; then
    die "--run-tests, but '$SRC_DIR/tests' does not exist.
     This is the source distribution, which ships only what the installer
     needs. The test suite lives in the git repository — clone it if you want
     to run the suite, or drop --run-tests to install."
fi

# The version, read from the one place that defines it. The header is the single
# source of truth — CMake derives the project version and the soname from these
# same three lines — so a hardcoded version here could disagree with the artefact
# it describes.
version_from_header() {
    awk '/#define ACCUDISC_VERSION_MAJOR/ {a=$3}
         /#define ACCUDISC_VERSION_MINOR/ {b=$3}
         /#define ACCUDISC_VERSION_PATCH/ {c=$3}
         END {if (a=="" || b=="" || c=="") exit 1; print a "." b "." c}' \
        "$SRC_DIR/include/accudisc/accudisc.h"
}
VERSION=$(version_from_header) || die "cannot read the version from include/accudisc/accudisc.h"

# ---------------------------------------------------------------------------
# uninstall
# ---------------------------------------------------------------------------
# Driven entirely by $BUILD_DIR/install_manifest.txt, which every `cmake
# --install` writes. That is why the build tree must be the one that installed:
# the manifest is the record of what was actually put where, and it is true by
# construction rather than by anyone maintaining a list.
#
# NOTHING here reconfigures or cleans the build tree. A reconfigure could change
# the prefix, and a clean would destroy the manifest — either way the target
# would then remove the wrong files or none at all.
if [ "$ACTION" = uninstall ]; then
    [ -d "$BUILD_DIR" ] || die "no build tree at '$BUILD_DIR' — uninstall needs
     the tree that performed the install (it holds install_manifest.txt).
     Point at it with --build-dir, or see README.md for the file list to
     remove by hand."
    [ -f "$BUILD_DIR/install_manifest.txt" ] || die \
        "'$BUILD_DIR' has no install_manifest.txt, so nothing was installed from
     it. Refusing to guess: removing paths we merely EXPECT to exist is a worse
     failure than not uninstalling."

    say "Uninstalling AccuDisc $VERSION (manifest: $BUILD_DIR/install_manifest.txt)"
    # Read the prefix out of the manifest rather than from --prefix, so the
    # privilege decision matches where the files really are.
    manifest_dir=$(head -n 1 "$BUILD_DIR/install_manifest.txt" 2>/dev/null || echo /)
    if need_privilege "$manifest_dir"; then resolve_sudo; fi

    # Counted BEFORE, so the report afterwards can distinguish "removed" from
    # "was already absent" — two very different things that look identical if
    # you only look at the end state.
    present_before=0
    while IFS= read -r f; do
        [ -n "$f" ] || continue
        [ -e "$f" ] && present_before=$((present_before + 1))
    done < "$BUILD_DIR/install_manifest.txt"

    run ${SUDO:+$SUDO} cmake --build "$BUILD_DIR" --target uninstall

    # VERIFY THE OUTCOME, not the exit status. An uninstall that removes nothing
    # and returns 0 is indistinguishable from a successful one at the exit
    # status — which is precisely what happened with a placeholder `sudo` that
    # printed a message and exited 0, leaving all 12 files in place under a
    # "Done." Checking the files themselves catches that, and every other cause
    # too: a refused rule, a wrong password, a read-only mount.
    if [ "$DRY_RUN" -eq 0 ]; then
        left=0; total=0
        while IFS= read -r f; do
            [ -n "$f" ] || continue
            total=$((total + 1))
            [ -e "$f" ] && left=$((left + 1))
        done < "$BUILD_DIR/install_manifest.txt"
        # `present_before` is captured ahead of the uninstall (above), so this
        # can say what was REMOVED rather than what is merely absent now. The
        # earlier version reported "12 files removed" for a run where CMake said
        # "0 file(s), 13 already absent" — true about the end state, and
        # misleading about what had just happened.
        removed=$((present_before - left))
        if [ "$left" -gt 0 ]; then
            die "the uninstall reported success but $left of $total files are still
     there — nothing was actually removed, or only part of it was. The first is:
         $(while IFS= read -r f; do [ -e "$f" ] && { printf '%s' "$f"; break; }; done < "$BUILD_DIR/install_manifest.txt")
     Most likely the privilege step did not really escalate. Re-run as root:
         doas ./install.sh uninstall --build-dir '$BUILD_DIR'
     or set ACCUDISC_SUDO to a command that works on this system."
        fi
        if [ "$present_before" -eq 0 ]; then
            ok "nothing to remove — all $total manifest entries were already absent"
        else
            ok "$removed of $total files removed, verified against the manifest"
        fi
    fi

    say "Done."
    printf '\n' >&2
    warn "if you ran ldconfig after installing, run it again now."
    exit 0
fi

# ---------------------------------------------------------------------------
# --print-wheel
# ---------------------------------------------------------------------------
# For scripting: prints the path of the wheel in the build tree, or fails. The
# INSTALLED wheel is found by globbing $PREFIX/share/accudisc/wheel/ instead.
if [ "$ACTION" = print-wheel ]; then
    shopt -s nullglob
    wheels=("$BUILD_DIR"/bindings/python/wheel/*.whl)
    shopt -u nullglob
    [ ${#wheels[@]} -gt 0 ] || die "no wheel in '$BUILD_DIR/bindings/python/wheel' —
     build one with: cmake --build '$BUILD_DIR' --target wheel"
    [ ${#wheels[@]} -eq 1 ] || die "${#wheels[@]} wheels in '$BUILD_DIR/bindings/python/wheel'"
    printf '%s\n' "${wheels[0]}"
    exit 0
fi

# ---------------------------------------------------------------------------
# install: configure
# ---------------------------------------------------------------------------
say "AccuDisc $VERSION"
printf '    source:  %s\n    build:   %s\n    prefix:  %s\n' \
    "$SRC_DIR" "$BUILD_DIR" "$PREFIX" >&2
printf '    python:  %s\n\n' \
    "$( [ "$WANT_WHEEL" -eq 1 ] && printf 'wheel' || printf 'none' )$( [ "$WANT_SITEDIR" -eq 1 ] && printf ' + site-packages' )" >&2

# ---------------------------------------------------------------------------
# ORPHAN GUARD: reconfiguring a tree that has already installed
# ---------------------------------------------------------------------------
#
# `cmake --install` REWRITES install_manifest.txt. So if this build tree
# previously installed something the new configuration will not install, those
# files vanish from the manifest while remaining on disk — and the manifest is
# the only thing `uninstall` consults. They become unremovable by any supported
# route, silently, with a successful-looking install in between.
#
# The concrete case is the one this script's own defaults create: a tree
# configured ACCUDISC_INSTALL_PYTHON=ON has installed into the prefix's
# site-packages, and we are about to configure it OFF. Left behind, that
# directory is a PEP 420 namespace package — `import accudisc` SUCCEEDS and
# yields an empty module with no version and no Device (README.md, "Uninstalling").
# A broken import that raises would be better than one that returns nothing.
#
# The fix is ordering, not force: uninstall with the CURRENT cache first, which
# removes exactly what that configuration installed, then install afresh.
# TEST THE FILESYSTEM, NOT THE MANIFEST. `cmake --build --target uninstall`
# removes the files it lists but does NOT delete install_manifest.txt, so the
# manifest still names them afterwards. An earlier version of this guard grepped
# the manifest and therefore fired forever after a successful uninstall —
# uninstall, re-run, refused, uninstall again, refused again. Keith hit the loop.
#
# The manifest is evidence ABOUT the filesystem; only the filesystem says what is
# there now. A file that no longer exists cannot be orphaned.
if [ -f "$BUILD_DIR/install_manifest.txt" ] && [ "$WANT_SITEDIR" -eq 0 ]; then
    orphans=""
    while IFS= read -r f; do
        case "$f" in
            *site-packages*) [ -e "$f" ] && orphans="${orphans}${f}
" ;;
        esac
    done < "$BUILD_DIR/install_manifest.txt"
    orphans=${orphans%
}
    if [ -n "$orphans" ]; then
        printf '%s\n' "$orphans" | sed 's/^/    /' >&2
        die "the files above were installed from '$BUILD_DIR' into the prefix's
     site-packages, and this run installs the wheel instead. Reconfiguring now
     would drop them from install_manifest.txt while leaving them on disk — after
     which nothing can remove them, and a surviving site-packages/accudisc/ is a
     PEP 420 namespace package that makes 'import accudisc' succeed and return an
     empty module.

     Remove them first, with the cache that installed them:
         ./install.sh uninstall --build-dir '$BUILD_DIR'
     then re-run this command.

     Or keep installing them, by adding --sitedir."
    fi
fi

cmake_args=(
    -B "$BUILD_DIR" -S "$SRC_DIR"
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
    -DCMAKE_INSTALL_PREFIX="$PREFIX"
    -DACCUDISC_SETCAP_AFTER_BUILD=OFF
    -DACCUDISC_BUILD_TESTS=$( [ "$WANT_TESTS" -eq 1 ] && echo ON || echo OFF )
    -DACCUDISC_INSTALL_PYTHON=$( [ "$WANT_SITEDIR" -eq 1 ] && echo ON || echo OFF )
    -DACCUDISC_INSTALL_WHEEL=$( [ "$WANT_WHEEL" -eq 1 ] && echo ON || echo OFF )
    -DACCUDISC_SETCAP_ON_INSTALL=$( [ "$INSTALL_PRIVILEGED" -eq 1 ] && echo ON || echo OFF )
)

if [ "$INSTALL_PRIVILEGED" -eq 0 ]; then
    warn "'$PREFIX' is writable by you, so the install will NOT be privileged and"
    warn "cap_sys_rawio cannot be applied (setcap needs root). Vendor drive"
    warn "features and burning will need root until you grant it by hand — the"
    warn "command is printed at the end."
fi

# An EMPTY RUNPATH is an instruction ("record none"), not an absence, and CMake
# treats it as such — see accudisc_derived_cache in CMakeLists.txt. It is the
# right setting for /usr, where ld.so already searches the libdir and a RUNPATH
# into a standard directory is flagged by distro packaging lint. It is fatal
# anywhere else: the CLI links, installs, and dies at startup unable to find
# libaccudisc.so.0.
if [ "$EMPTY_RPATH" -eq 1 ]; then
    cmake_args+=(-DACCUDISC_INSTALL_RPATH=)
    case "$PREFIX" in
        /usr) ;;
        *) warn "--empty-rpath with prefix '$PREFIX': the installed binaries will"
           warn "record NO library search path. Unless '$PREFIX/lib*' is already in"
           warn "/etc/ld.so.conf.d/, accudisc will fail to start." ;;
    esac
fi

# The user's arguments last: CMake takes the final occurrence of a -D, so this
# is what makes --cmake-arg able to override any default above.
cmake_args+=("${EXTRA_CMAKE_ARGS[@]+"${EXTRA_CMAKE_ARGS[@]}"}")

say "Configuring"
run cmake "${cmake_args[@]}"

# ---------------------------------------------------------------------------
# install: build
# ---------------------------------------------------------------------------
build_args=(--build "$BUILD_DIR")
[ -n "$JOBS" ] && build_args+=(-j "$JOBS")

say "Building"
run cmake "${build_args[@]}"

if [ "$WANT_TESTS" -eq 1 ]; then
    say "Testing"
    # Deliberately fatal. A test suite that is run and then ignored is worse
    # than one that is not run, because it produces the reassurance without the
    # assurance. --run-tests is opt-in; opting in means you want to know.
    run ctest --test-dir "$BUILD_DIR" --output-on-failure
fi

# The wheel is a separate target because it is not part of ALL: it needs pip,
# and an ordinary build should not require it. Built here, before the install
# step, so a pip failure happens unprivileged and before anything touches
# $PREFIX.
if [ "$WANT_WHEEL" -eq 1 ]; then
    say "Building the Python wheel"
    run cmake --build "$BUILD_DIR" --target wheel
fi

# ---------------------------------------------------------------------------
# install: install
# ---------------------------------------------------------------------------
# The only step that may need privilege — and the decision was already made
# above, before configure, because ACCUDISC_SETCAP_ON_INSTALL depends on it.
if [ -n "$SUDO" ]; then
    say "Installing to $PREFIX (escalating with $SUDO)"
else
    say "Installing to $PREFIX"
fi
# A marker to date the install against. `cmake --install` rewrites
# install_manifest.txt every time, so a manifest OLDER than this marker proves
# the install step did not run — the same silent no-op that a placeholder
# `sudo` produced on the uninstall path, and it would be just as invisible here
# because a previous install leaves every file exactly where the readback below
# expects to find it.
_adsc_marker="$BUILD_DIR/.accudisc-install-stamp"
[ "$DRY_RUN" -eq 1 ] || : > "$_adsc_marker"

run ${SUDO:+$SUDO} cmake --install "$BUILD_DIR"

if [ "$DRY_RUN" -eq 0 ]; then
    if [ ! -f "$BUILD_DIR/install_manifest.txt" ]; then
        die "no install_manifest.txt after 'cmake --install' — the install did not run."
    elif [ ! "$BUILD_DIR/install_manifest.txt" -nt "$_adsc_marker" ]; then
        die "install_manifest.txt was not rewritten, so 'cmake --install' did nothing.
     Any files under '$PREFIX' are left over from an EARLIER install, which is
     why nothing below would have looked wrong. Most likely the privilege step
     did not really escalate. Re-run as root:
         doas ./install.sh --prefix '$PREFIX'
     or set ACCUDISC_SUDO to a command that works on this system."
    fi
    rm -f "$_adsc_marker"
fi

# ldconfig. Not run by default, and that is deliberate rather than an oversight:
# it edits a system-wide cache, and a prefix that needs it is usually a prefix
# whose owner wants to make that decision. The RUNPATH recorded above already
# makes AccuDisc's own binaries work without it — ldconfig matters for OTHER
# programs linking against libaccudisc later.
if [ "$WANT_LDCONFIG" -eq 1 ]; then
    if command -v ldconfig >/dev/null 2>&1; then
        say "Running ldconfig"
        run ${SUDO:+$SUDO} ldconfig
    else
        warn "--ldconfig given but no ldconfig found; skipping"
    fi
fi

# ---------------------------------------------------------------------------
# What just happened, and what to do next
# ---------------------------------------------------------------------------
printf '\n' >&2
say "Installed AccuDisc $VERSION to $PREFIX"

if [ "$DRY_RUN" -eq 0 ]; then
    # Verified by running it, not by assuming the install rule fired. The
    # installed binary is the one to ask: a build-tree binary would answer
    # identically and prove nothing.
    if [ -x "$PREFIX/bin/accudisc" ]; then
        ok "$("$PREFIX/bin/accudisc" --version 2>&1 | head -n 1) -> $PREFIX/bin/accudisc"
    else
        warn "no executable at $PREFIX/bin/accudisc — the install did not land where expected"
    fi

    # CAP_SYS_RAWIO is what the vendor opcodes and the whole burn path need.
    # Reported rather than asserted: it legitimately fails on filesystems with
    # no capability support, and on a DESTDIR staging install it is skipped by
    # design.
    if command -v getcap >/dev/null 2>&1 && [ -x "$PREFIX/bin/accudisc" ]; then
        caps=$(getcap "$PREFIX/bin/accudisc" 2>/dev/null || true)
        if [ -n "$caps" ]; then
            ok "capabilities: $caps"
        else
            warn "no cap_sys_rawio on $PREFIX/bin/accudisc — vendor drive features"
            warn "and burning will need root. Grant it with:"
            printf '        %s setcap cap_sys_rawio=ep %s\n' \
                "${SUDO:-sudo}" "$PREFIX/bin/accudisc" >&2
        fi
    fi
fi

if [ "$WANT_WHEEL" -eq 1 ]; then
    wheel_dir="$PREFIX/share/accudisc/wheel"
    printf '\n' >&2
    say "The Python binding is a wheel, at:"
    printf '    %s/accudisc-%s-*.whl\n' "$wheel_dir" "$VERSION" >&2
    cat >&2 <<EOF

    Install it into whatever environment needs it. For cdda2img, which is
    itself a pipx application, inject it into that application's venv:

        pipx install /path/to/cdda2img      # a checkout: not on PyPI yet
        pipx inject cdda2img "\$(ls $wheel_dir/accudisc-*.whl)"
        cdda2img doctor

    'inject' rather than a separate install because a pipx application runs in
    its own isolated venv and cannot see packages installed anywhere else.
    The wheel declares its own dependency on cffi, so that arrives with it.

    Into an ordinary virtualenv instead:

        python3 -m venv .venv && . .venv/bin/activate
        pip install $wheel_dir/accudisc-*.whl

    Glob the name — do not hardcode it. It carries the version and ABI tags,
    so it changes whenever either does.

    To remove it again:  pipx uninject cdda2img accudisc
    To replace it after a rebuild:  pipx inject --force cdda2img <wheel>
EOF
fi

printf '\n' >&2
say "To uninstall:  ./install.sh uninstall --build-dir '$BUILD_DIR'"
