#!/bin/sh
# Drift detector for acquisition-policy constants.
#
# The defect this project found on 2026-07-25 was that `cli/main.c` holds real
# acquisition POLICY — scan windows, sampling cadence — that a library consumer
# cannot reach, so Python and Rust bindings would each reimplement it slightly
# differently. See docs/reference/API_PLAN.md §2 and §5.
#
# The refactor moves each constant from cli/ into src/. The dangerous middle
# state is the one where it exists in BOTH: the helper lands, the CLI keeps its
# copy, and the two drift apart silently because nothing compares them. That is
# exactly the failure this test exists to make impossible.
#
# The rule enforced is therefore "in EXACTLY ONE of cli/ and src/", not "not in
# both" — a not-in-both test passes when a constant has been deleted, renamed,
# or when the path is wrong, i.e. it is green forever while checking nothing.
# Requiring a positive hit on exactly one side makes a bad path a failure.
#
# Constants are expected in cli/ today and in src/ after API_PLAN.md phase 2.
# Either side satisfies the test; both, or neither, is a failure.
#
#   usage: policy_constants.sh <project-source-root>

set -u

ROOT="$1"
fails=0

[ -d "$ROOT/cli" ] && [ -d "$ROOT/src" ] || {
	printf 'policy_constants: FAIL: bad source root %s (no cli/ or src/)\n' "$ROOT"
	exit 1
}

# Each name is an acquisition-policy constant that must live on exactly one
# side of the cli/src line. Add to this list when a new one is introduced.
#
# All three have now migrated, each acquiring the ACCUDISC_ prefix on the way
# across: CXSCAN_CADENCE -> ACCUDISC_CENSUS_CADENCE in phase 2.2, and
# PREGAP_WINDOW/PREGAP_TAIL -> ACCUDISC_PREGAP_WINDOW/_TAIL in phase 2.4. This
# test caught BOTH renames by failing "found nowhere", which is what the
# positive-hit rule above is for: a not-in-both test would have gone green on a
# constant that had ceased to exist.
#
# With the migration complete the test changes job rather than retiring: it now
# guards the reverse direction, a copy reappearing in cli/ beside the public
# one. That is the same drift, and it is likelier now than before — the names
# are public, so re-#define-ing one in the CLI compiles and looks harmless.
for name in ACCUDISC_PREGAP_WINDOW ACCUDISC_PREGAP_TAIL ACCUDISC_CENSUS_CADENCE; do
	# Match DEFINITIONS, not mentions. A migrated constant is public, so the
	# CLI may legitimately reference it by name — that is the refactor working,
	# not drift. What must never exist twice is the #define itself, which is
	# precisely the "helper lands, CLI keeps its copy" state this test is for.
	# (Matching mentions flagged a comment in cli/main.c naming the constant it
	# had just given up. Right answer, wrong question — the check has to be
	# about the definition.)
	def="^[[:space:]]*#[[:space:]]*define[[:space:]]+$name\b"
	n_cli=$(grep -rlE "$def" "$ROOT/cli" 2>/dev/null | wc -l)
	n_src=$(grep -rlE "$def" "$ROOT/src" "$ROOT/include" 2>/dev/null | wc -l)

	if [ "$n_cli" -gt 0 ] && [ "$n_src" -gt 0 ]; then
		printf '  FAIL %s: defined in BOTH cli/ (%d files) and src|include/ (%d files)\n' \
		       "$name" "$n_cli" "$n_src"
		printf '       A policy constant on both sides of the boundary is the drift\n'
		printf '       this test exists to catch. If phase 2 moved it, delete the copy\n'
		printf '       in cli/ and rewire the CLI onto the library helper in the SAME\n'
		printf '       commit (API_PLAN.md §5).\n'
		fails=$((fails + 1))
	elif [ "$n_cli" -eq 0 ] && [ "$n_src" -eq 0 ]; then
		printf '  FAIL %s: found nowhere under %s\n' "$name" "$ROOT"
		printf '       Either it was renamed/removed without updating this list, or\n'
		printf '       the source root is wrong. Both make this test vacuous, which is\n'
		printf '       worse than the drift it was written to detect.\n'
		fails=$((fails + 1))
	elif [ "$n_cli" -gt 0 ]; then
		printf '  ok   %s: cli/ only (pre-phase-2)\n' "$name"
	else
		printf '  ok   %s: src|include/ only (migrated)\n' "$name"
	fi
done

if [ "$fails" -gt 0 ]; then
	printf 'policy_constants: %d failure(s)\n' "$fails"
	exit 1
fi
printf 'policy_constants: all constants on exactly one side\n'
