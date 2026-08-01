#!/bin/sh
# Phase 3b — A/B parity of src/repair/ against cdda2img's pinned ctanalyse.
#
# Eight arms across three fixtures. SKIPS (77) when the fixtures are absent,
# which is every machine but this one: they are 1.6 GB of real disc images
# staged in /var/tmp and deliberately not in the repo. A skip is not a pass and
# ctest reports it as its own state.
#
# The reference JSON was produced by cdda2img commit 0e94be1, tagged
# `ctanalyse-ab-baseline` BEFORE any fixture was generated. Nothing here links
# or runs their binary; two programs' output files are compared.
#
# Coverage, and why each arm is here:
#   npar 8 and 16                    both values occur on one disc
#   raw (-639) and stored (-669)     the two domains; +30 read offset apart
#   bounds[0] = 0 and 33             the ABBA Gold defect was invisible at 0
#   zero-correction arms             an EMPTY list at the right offset is a
#                                    positive result: a shifted list cannot be
#                                    empty, so it pins geometry
#   error-only and errors+erasures   the erasure path is where the reporting
#                                    conventions differ
#   the misaligned erasure control   a deliberately wrong bitmap, which must
#                                    still reach the same corrections
usage() { echo "usage: $0 <ctdb_ab-binary>" >&2; exit 2; }
[ $# -eq 1 ] || usage
AB="$1"
HERE=$(dirname "$0")
CMP="$HERE/ctdb_ab_compare.py"

F=/var/tmp/ctdb-fixture-tracy-erasures
G=/var/tmp/ctdb-fixture
A=/var/tmp/ctdb-fixture-abba

for d in "$F" "$G" "$A"; do
    if [ ! -d "$d" ]; then
        echo "SKIP: fixture $d absent (see private/docs/ctdb-wire-findings-2026-08-01.md)"
        exit 77
    fi
done
command -v python3 >/dev/null 2>&1 || { echo "SKIP: no python3"; exit 77; }

TMP=$(mktemp -d) || exit 2
trap 'rm -rf "$TMP"' EXIT
fail=0
n=0

arm() { # label ref-json -- ctdb_ab args...
    label="$1"; ref="$2"; shift 3
    n=$((n + 1))
    if ! "$AB" "$@" > "$TMP/out.json" 2> "$TMP/err"; then
        echo "=== $label"
        echo "  harness FAILED: $(cat "$TMP/err")"
        fail=$((fail + 1))
        return
    fi
    python3 "$CMP" "$TMP/out.json" "$ref" "$label" || fail=$((fail + 1))
}

arm "npar=16 error-only, Tracy raw @ -639" \
    "$F/ctanalyse_npar16_entry67116_error_only.json" -- \
    "$F/tracy_erasure_image.pcm" "$F/parity_npar16_entry67116.bin" 16 -639 0 162892

arm "npar=8 error-only, Tracy raw @ +30" \
    "$F/ctanalyse_npar8_entry2451243_error_only.json" -- \
    "$F/tracy_erasure_image.pcm" "$F/parity_npar8_entry2451243.bin" 8 30 0 162892

arm "npar=16 ERASURES, Tracy raw @ -639" \
    "$F/ctanalyse_npar16_entry67116_erasures.json" -- \
    "$F/tracy_erasure_image.pcm" "$F/parity_npar16_entry67116.bin" 16 -639 0 162892 \
    "$F/tracy_erasures.bin"

arm "npar=8 ERASURES, Tracy raw @ +30" \
    "$F/ctanalyse_npar8_entry2451243_erasures.json" -- \
    "$F/tracy_erasure_image.pcm" "$F/parity_npar8_entry2451243.bin" 8 30 0 162892 \
    "$F/tracy_erasures.bin"

arm "npar=16 ERASURES MISALIGNED (negative control)" \
    "$F/ctanalyse_npar16_entry67116_erasures_misaligned.json" -- \
    "$F/tracy_erasure_image.pcm" "$F/parity_npar16_entry67116.bin" 16 -639 0 162892 \
    "$F/tracy_erasures_misaligned.bin"

arm "npar=16 identity, Tracy STORED @ -669" \
    "$G/ctanalyse_npar16_entry67116.json" -- \
    "$G/tracy_image.pcm" "$G/parity_npar16_entry67116.bin" 16 -669 0 162892

arm "ABBA npar=8 entry 829896 @ 0, bounds[0]=33" \
    "$A/ctanalyse_npar8_entry829896_error_only.json" -- \
    "$A/abba_image.pcm" "$A/parity_npar8_entry829896.bin" 8 0 33 347175

arm "ABBA npar=8 entry 10612122 @ -1734, bounds[0]=33" \
    "$A/ctanalyse_npar8_entry10612122_error_only.json" -- \
    "$A/abba_image.pcm" "$A/parity_npar8_entry10612122.bin" 8 -1734 33 347175

echo "ctdb_ab: $n arms, $fail differing"
[ "$fail" -eq 0 ] || exit 1
exit 0
