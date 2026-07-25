#!/bin/sh
# Golden test for the device-free surface of the `accudisc` binary.
#
# docs/reference/cli-machine-interface.md declares the CLI stable and
# additive-only. Nothing enforced that. This does, for every path that reaches
# a decision WITHOUT opening a device — which is a small set, because main()
# opens the device before dispatching (see the note on `unknown command`
# below), but it covers the parts a packager, a script, or a human meets first:
# the usage text, the version format, and the 0/1/2 exit codes.
#
# Deliberately NOT here: anything needing a disc. Those live in
# private/bench/refactor-baseline-*/ as one-time refactor proof and are diffed
# by hand — wiring them into ctest would fail the build gate for anyone without
# that exact pressing in the tray.
#
#   usage: cli_surface.sh <path-to-accudisc> <golden-dir>

set -u

BIN="$1"
GOLDEN="$2"
TMP="${TMPDIR:-/tmp}/accudisc_cli_surface.$$"
mkdir -p "$TMP" || exit 1
trap 'rm -rf "$TMP"' EXIT

fails=0
ok()   { printf '  ok   %s\n' "$1"; }
bad()  { printf '  FAIL %s: %s\n' "$1" "$2"; fails=$((fails + 1)); }

# run <name> <expected-exit> -- <args...>
run() {
	name="$1"; want_rc="$2"; shift 3
	"$BIN" "$@" >"$TMP/out" 2>"$TMP/err"
	rc=$?
	[ "$rc" = "$want_rc" ] || bad "$name" "exit $rc, wanted $want_rc"
}

# --- usage text, both destinations -----------------------------------------
# --help prints to stdout and exits 0; a bare invocation prints the SAME text
# to stderr and exits 1. That split is the contract: asking for help is not an
# error, failing to name a command is.
for flag in --help -h; do
	run "usage $flag" 0 -- "$flag"
	if diff -u "$GOLDEN/usage.txt" "$TMP/out" >"$TMP/diff" 2>&1; then
		[ -s "$TMP/err" ] && bad "usage $flag" "wrote to stderr" \
		                  || ok "usage $flag"
	else
		bad "usage $flag" "stdout differs from golden:
$(cat "$TMP/diff")"
	fi
done

run "usage bare" 1 --
if diff -u "$GOLDEN/usage.txt" "$TMP/err" >"$TMP/diff" 2>&1; then
	[ -s "$TMP/out" ] && bad "usage bare" "wrote to stdout" \
	                  || ok "usage bare (stderr, exit 1)"
else
	bad "usage bare" "stderr differs from golden:
$(cat "$TMP/diff")"
fi

# --- version ----------------------------------------------------------------
# Matched as a shape, not a literal: the string moves on every release and a
# literal golden would just be a chore that trains people to regenerate it
# without reading. What is contractual is `accudisc <semver>` on stdout, alone.
for form in --version -V version; do
	run "version $form" 0 -- "$form"
	if grep -qE '^accudisc [0-9]+\.[0-9]+\.[0-9]+' "$TMP/out"; then
		if [ "$(wc -l <"$TMP/out")" = 1 ] && [ ! -s "$TMP/err" ]; then
			ok "version $form"
		else
			bad "version $form" "expected exactly one stdout line, no stderr"
		fi
	else
		bad "version $form" "got: $(head -1 "$TMP/out")"
	fi
done

# --- exit 2: could not complete ---------------------------------------------
# An unopenable device is the cheapest deterministic route to the exit-2 path,
# and it also pins the message format, which is the only thing a caller can
# distinguish "no drive" from "no disc" by.
run "open failure" 2 -- --device /nonexistent/zz toc
if grep -qE '^accudisc: open /nonexistent/zz: ' "$TMP/err"; then
	ok "open failure (exit 2, stderr message)"
else
	bad "open failure" "stderr: $(head -1 "$TMP/err")"
fi

# --- a known wart, pinned so it cannot change silently -----------------------
# An unknown command exits 2 (device open failed), NOT 1 (usage), because
# main() opens the device before it dispatches. Arguably backwards — a
# misspelled command is a usage error and should not need hardware — but it is
# current behaviour and callers may key on it. Pinned here so that if it is
# ever fixed, it is fixed deliberately and lands in the cdda2img ledger rather
# than surprising a script. See docs/reference/API_PLAN.md §8.
run "unknown command (no device)" 2 -- --device /nonexistent/zz not-a-command
ok "unknown command exits 2 without a device (documented wart)"

if [ "$fails" -gt 0 ]; then
	printf 'cli_surface: %d failure(s)\n' "$fails"
	exit 1
fi
printf 'cli_surface: all checks passed\n'
