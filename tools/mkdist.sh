#!/usr/bin/env bash
#
# mkdist.sh — build a source distribution tarball in dist/.
#
#   ./tools/mkdist.sh                 # -> dist/accudisc-<version>.tar.gz
#   ./tools/mkdist.sh --output-dir X  # somewhere else
#   ./tools/mkdist.sh --force         # build anyway despite the warnings below
#
# SOURCE ONLY. No binaries, no wheel, no build tree. The wheel is deliberately
# absent: its compiled extension records a RUNPATH naming the prefix it was
# built for, so a wheel shipped in a tarball is valid on exactly one machine.
# Whoever unpacks this builds their own with `./install.sh`, which produces one
# matching *their* prefix.
#
# ---------------------------------------------------------------------------
# THIS IS A USER DISTRIBUTION, NOT A DEVELOPER ONE (Keith, 2026-08-01)
# ---------------------------------------------------------------------------
#
# The rule, and it is the whole specification:
#
#   > The tarball contains only the sources of what the installer INSTALLS.
#   > Extract it, run ./install.sh, and you have exactly what we have installed.
#
# So the contents are derived from the install manifest, not from the repo:
#
#   installed artefact                     source shipped
#   ------------------------------------   ----------------------------------
#   bin/accudisc                           cli/, src/, include/, CMakeLists.txt
#   lib*/libaccudisc.so*, libaccudisc.a    src/, include/, cmake/
#   lib*/accudisc/drivers/*.so             drivers/
#   lib*/pkgconfig/accudisc.pc             cmake/accudisc.pc.in
#   include/accudisc/*.h                   include/
#   share/man/man{1,8}/accudisc.*          docs/man/
#   share/accudisc/wheel/*.whl             bindings/python/
#
# EXCLUDED, and each for a reason rather than by omission:
#
#   tests/            not installed. It is also the largest single directory
#                     here, and a user who wants it wants the git repo anyway.
#                     CMakeLists.txt and install.sh both guard on its absence
#                     with a message naming this decision — without that, the
#                     failure is a raw CMake error about add_subdirectory.
#   tools/            developer scripts, including this one. The generated
#                     tables the build needs (src/drive/*.inc) are TRACKED, so
#                     nothing here is required to build — verified, not assumed.
#   docs/reference/   API_PLAN, RECORDING_PLAN, TODO: internal planning.
#     (most)          RECOVERY.md especially — it is hardlinked to cdda2img and
#                     is a design document, not user material. ATTRIBUTION.md
#                     and cli-machine-interface.md ARE shipped: the first is a
#                     credit obligation, the second is the stable contract for
#                     driving the CLI that gets installed.
#   docs/research/    shareable, but research rather than product.
#   docs/guardian_public.asc
#                     verifies detached .sig files. There are none tracked
#                     (checked), so it would verify nothing.
#   bindings/rust/    a placeholder README; nothing is installed from it.
#   CLAUDE.md         agent instructions for working ON this project.
#   .editorconfig .githooks/ .gitignore
#                     development tooling.
#
# HOW THE LIST IS BUILT, and why it is still `git ls-files` underneath: the
# curated prefixes below are a FILTER over the tracked-file list, never a
# replacement for it. That keeps the safety property intact — only tracked files
# can ship, so .gitignore's exclusions (private/ with the licensed T10 MMC-5
# spec and the Guardian signing key) are still inherited — while narrowing to
# the user distribution. A curated list that globbed the filesystem directly
# would drop that protection entirely, which is the one thing worth not
# getting wrong here.
#
# ---------------------------------------------------------------------------
# THE FILE LIST COMES FROM GIT, AND THAT IS A SAFETY PROPERTY
# ---------------------------------------------------------------------------
#
# `git ls-files` enumerates TRACKED files only, so the archive inherits every
# exclusion in .gitignore for free — and this repository's .gitignore is not a
# tidiness measure. `private/` holds a licensed copy of the T10 MMC-5
# specification, which must never be redistributed, and the Guardian agent's
# private signing key. `.claude/` and `.remember/` hold local tooling. Root-level
# *.pcm and friends are real captured audio.
#
# Building the archive with `tar` over the working tree would include all of it.
# That is why this script does not do that, and why the check at the end runs on
# the finished archive rather than on the intent.
#
# ---------------------------------------------------------------------------
# WORKING TREE, NOT HEAD — AND WHY THAT NEEDS A WARNING
# ---------------------------------------------------------------------------
#
# Content is taken from the WORKING TREE, with the file list from git. The
# obvious alternative, `git archive HEAD`, reads the object store instead, so it
# silently omits anything edited or added but not yet committed — which produces
# a tarball that does not match the tree it was made from, with no error.
#
# The cost is that a tarball built from a dirty tree is not reproducible from any
# commit. So a dirty tree is reported loudly, and untracked files are FATAL: an
# untracked file is invisible to `git ls-files`, so a new source file nobody
# staged would be missing from the archive and the build would fail for the
# recipient and nobody else. `git add` it (staging is enough — no commit needed),
# or pass --force if you know it does not belong.
#
set -euo pipefail

FORCE=0
OUT_DIR=""

SRC_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)

if [ -t 2 ]; then
    C_B=$'\033[1m'; C_R=$'\033[31m'; C_Y=$'\033[33m'; C_G=$'\033[32m'; C_0=$'\033[0m'
else
    C_B=""; C_R=""; C_Y=""; C_G=""; C_0=""
fi
say()  { printf '%s==>%s %s\n' "$C_B" "$C_0" "$*" >&2; }
warn() { printf '%swarning:%s %s\n' "$C_Y" "$C_0" "$*" >&2; }
die()  { printf '%serror:%s %s\n'   "$C_R" "$C_0" "$*" >&2; exit 1; }
ok()   { printf '%s  ok%s %s\n'     "$C_G" "$C_0" "$*" >&2; }

# Written out rather than extracted from the comment block above with a
# line-number range: the range silently drifts the first time anyone edits the
# header, and prints the wrong thing with no error.
usage() {
    cat <<'EOF'
usage: ./tools/mkdist.sh [options]

Builds a SOURCE-ONLY distribution tarball: dist/accudisc-<version>.tar.gz

Options:
  --output-dir DIR   write the tarball here instead of dist/
  --force            build even when untracked files would be omitted
  -h, --help         this text

The file list comes from `git ls-files`, so .gitignore's exclusions are
inherited — which is what keeps private/ (the licensed T10 MMC-5 spec, the
Guardian signing key) out of the archive. Content is taken from the working
tree, so staged-but-uncommitted work is included; untracked files are fatal,
because they would be missing with no error. See the comments in this file.
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --output-dir) [ $# -ge 2 ] || die "--output-dir needs an argument"; OUT_DIR=$2; shift 2 ;;
        --force)      FORCE=1; shift ;;
        -h|--help)    usage; exit 0 ;;
        *)            usage >&2; die "unknown option '$1'" ;;
    esac
done

[ -z "$OUT_DIR" ] && OUT_DIR="$SRC_DIR/dist"
case "$OUT_DIR" in /*) ;; *) OUT_DIR="$SRC_DIR/$OUT_DIR" ;; esac

cd "$SRC_DIR"
command -v git >/dev/null 2>&1 || die "git not found — the file list comes from it"
git rev-parse --git-dir >/dev/null 2>&1 || die "'$SRC_DIR' is not a git repository"

# The version, from the single place that defines it. CMake derives the project
# version and the soname from these same three lines, so reading them here is
# what keeps the tarball's name honest about its contents.
VERSION=$(awk '/#define ACCUDISC_VERSION_MAJOR/ {a=$3}
               /#define ACCUDISC_VERSION_MINOR/ {b=$3}
               /#define ACCUDISC_VERSION_PATCH/ {c=$3}
               END {if (a=="" || b=="" || c=="") exit 1; print a "." b "." c}' \
    include/accudisc/accudisc.h) || die "cannot read the version from include/accudisc/accudisc.h"

NAME="accudisc-$VERSION"
TARBALL="$OUT_DIR/$NAME.tar.gz"

say "Packaging $NAME"

# ---- state checks ---------------------------------------------------------
# Untracked and not ignored: fatal, because it would be silently omitted.
untracked=$(git ls-files --others --exclude-standard)
if [ -n "$untracked" ]; then
    printf '%s\n' "$untracked" | sed 's/^/    /' >&2
    if [ "$FORCE" -eq 1 ]; then
        warn "the files above are untracked and will NOT be in the archive (--force)"
    else
        die "the files above are untracked, so they would be MISSING from the
     archive without any error. Stage them (a commit is not required):
         git add <file>...
     or re-run with --force if they genuinely do not belong."
    fi
fi

# Dirty relative to HEAD: allowed, but it means the tarball cannot be rebuilt
# from a commit, which matters the moment anyone asks "which source is this?"
if ! git diff --quiet HEAD -- 2>/dev/null; then
    warn "the working tree differs from HEAD, so this tarball is NOT reproducible"
    warn "from any commit. Fine for a test build; commit before a real release."
fi

# ---- build ----------------------------------------------------------------
mkdir -p "$OUT_DIR"
rm -f "$TARBALL"

# The user-distribution file set: tracked files (so .gitignore still governs)
# whose path matches one of these. Directory entries carry a trailing slash and
# are matched as prefixes; the rest are exact paths. See the header for why each
# is in or out.
DIST_PATHS='
CMakeLists.txt
LICENSE
README.md
install.sh
cmake/
include/
src/
cli/
drivers/
bindings/python/
docs/man/
docs/reference/ATTRIBUTION.md
docs/reference/cli-machine-interface.md
'

# Kept as `case` rather than one long `grep -E` so that adding a path above
# cannot silently become a regex metacharacter. Prefix match for anything ending
# in '/', exact match otherwise.
#
# Deliberately a plain loop with direct returns. Written first as a
# `printf | while ... exit 0; done && return 1` pipeline, which is WRONG in a
# way that reads fine: the while runs in a subshell, and a loop that completes
# without matching also exits 0, so both the match and the no-match case took
# the same branch and every file was excluded. Caught by the entry count.
#
# `set -f` because `for p in $DIST_PATHS` word-splits AND glob-expands; none of
# the literals contain glob characters today, and this makes that not matter.
in_dist() {
    case "$1" in
        bindings/python/tests/*) return 1 ;;   # dev-only, inside a shipped dir
    esac
    set -f
    for p in $DIST_PATHS; do
        case "$p" in
            */) case "$1" in "$p"*) set +f; return 0 ;; esac ;;
            *)  if [ "$1" = "$p" ]; then set +f; return 0; fi ;;
        esac
    done
    set +f
    return 1
}

# -z / --null throughout, so a path containing whitespace or a newline cannot
# split into two entries. --transform rewrites each stored path to sit under
# <name>/, so the archive unpacks into one directory rather than over $PWD.
#
# The transform's separator is ',' because '/' appears in the replacement.
filelist=$(mktemp)
trap 'rm -f "$filelist"' EXIT
excluded=0
# `-z` and `read -d ''` so a path containing whitespace or a newline stays one
# entry — the property the original `git ls-files -z | tar --null` pipeline had,
# and which a line-based read would have quietly dropped.
while IFS= read -r -d '' f; do
    if in_dist "$f"; then
        printf '%s\0' "$f" >> "$filelist"
    else
        excluded=$((excluded + 1))
    fi
done < <(git ls-files -z)

kept=$(tr -cd '\0' < "$filelist" | wc -c)
say "$kept of $((kept + excluded)) tracked files are in the user distribution"
[ "$kept" -gt 0 ] || die "the include filter matched nothing — DIST_PATHS is wrong"

tar --null --files-from="$filelist" \
    --transform="s,^,$NAME/," \
    --owner=0 --group=0 --numeric-owner \
    --mode='go-w' \
    -czf "$TARBALL"

# ---- verify ---------------------------------------------------------------
# On the FINISHED ARCHIVE, not on the file list that went in. The property that
# matters is what a recipient can extract, and only the archive can answer that.
say "Verifying $TARBALL"

contents=$(tar tzf "$TARBALL")
count=$(printf '%s\n' "$contents" | grep -c . || true)

# Anything here is a leak, and the first two are the serious ones: a licensed
# specification that must not be redistributed, and a private signing key.
#
# THE CAPTURE EXTENSIONS ARE ROOT-ANCHORED, mirroring .gitignore's own design:
# a stray .sub at the repo root is a real disc capture, while tests/data/*.sub
# is a tracked fixture the suite needs. Written unanchored first, and it
# immediately condemned tests/data/abba_t16_unknown_boundary.sub — a check that
# fires on correct input is worse than none, because the next person silences
# it rather than reading it.
leaks=$(printf '%s\n' "$contents" \
    | grep -E "^$NAME/(private/|\.claude/|\.remember/|backups/|build/)|^$NAME/[^/]+\.(pcm|sub|c2|wav|bin|toc|cdtext|fulltoc)$" \
    || true)
if [ -n "$leaks" ]; then
    printf '%s\n' "$leaks" | sed 's/^/    /' >&2
    rm -f "$TARBALL"
    die "the archive contained the paths above and has been DELETED.
     private/ holds a licensed copy of the T10 MMC-5 specification and the
     Guardian signing key; neither may be redistributed."
fi
ok "no private/, key, capture or build-tree paths ($count entries)"

# Presence checks. The leak test above can only fail on what IS there; these
# catch the opposite failure, an archive missing something essential, which
# otherwise shows up as a build error on someone else's machine.
for required in CMakeLists.txt install.sh LICENSE README.md \
                include/accudisc/accudisc.h; do
    printf '%s\n' "$contents" | grep -qxF "$NAME/$required" \
        || die "'$required' is missing from the archive — is it tracked in git?"
done
ok "CMakeLists.txt, install.sh, LICENSE, README.md and the public header are present"

# Every source the installed artefacts are built from. A missing one is a build
# failure on someone else's machine, which is the failure this whole script
# exists to prevent — so each installed thing gets an assertion that its source
# travelled with it.
for required in cmake/accudisc.pc.in cmake/uninstall.cmake \
                include/accudisc/driver.h \
                src/CMakeLists.txt cli/CMakeLists.txt drivers/CMakeLists.txt \
                src/drive/media_atip_db.inc src/drive/offsets_db.inc \
                bindings/python/CMakeLists.txt bindings/python/build_accudisc.py \
                bindings/python/accudisc/__init__.py \
                docs/man/accudisc.1 docs/man/accudisc.8; do
    printf '%s\n' "$contents" | grep -qxF "$NAME/$required" \
        || die "'$required' is missing from the archive, and something installed
     is built from it. Either DIST_PATHS dropped it or it is untracked."
done
ok "every source of an installed artefact is present (libs, CLI, drivers, pkg-config, headers, man, wheel)"

# The other half of the user-distribution rule: developer material must be
# ABSENT. Asserted rather than trusted, because the include filter and this list
# are edited at different times and drift is exactly what would not be noticed.
devcruft=$(printf '%s\n' "$contents" \
    | grep -E "^$NAME/(tests/|tools/|bindings/rust/|docs/research/|docs/flow/|\.githooks/|CLAUDE\.md|\.editorconfig|docs/reference/(TODO|RECOVERY|API_PLAN|RECORDING_PLAN)\.md)" \
    || true)
if [ -n "$devcruft" ]; then
    printf '%s\n' "$devcruft" | sed 's/^/    /' >&2
    rm -f "$TARBALL"
    die "the archive carried the developer-only paths above and has been DELETED.
     This is the USER distribution: it ships the sources of what the installer
     installs, and nothing else. Fix DIST_PATHS in this script."
fi
ok "no tests/, tools/, rust/, research/ or planning docs ($excluded tracked files excluded)"

# The executable bit survives tar but not every extraction path; assert it here
# so `./install.sh` works straight out of the tarball.
mode=$(tar tzvf "$TARBALL" | awk -v f="$NAME/install.sh" '$NF == f {print $1}')
case "$mode" in
    *x*) ok "install.sh is executable ($mode)" ;;
    *)   die "install.sh is not executable in the archive ($mode) — chmod +x it" ;;
esac

printf '\n' >&2
say "$TARBALL"
printf '    %s\n' "$(du -h "$TARBALL" | cut -f1), $count entries" >&2
printf '\n    Test it the way a recipient would:\n' >&2
printf '        tar xzf %s -C /tmp && cd /tmp/%s && ./install.sh --prefix ~/.local\n\n' \
    "$TARBALL" "$NAME" >&2
