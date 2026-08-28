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

# --- a value-taking option given no value ------------------------------------
# THE DEFECT (fixed 2026-08-28): every option was guarded `&& i + 1 < argc`, so
#   TRAILING  `read --chunk`        dumped usage without naming the argument;
#   MID-LINE  `read --chunk --map`  consumed "--map" AS THE VALUE. strtol gives
#                                   0, 0 is the sentinel for "use the default",
#                                   so it ran, EXITED 0, applied no chunk and
#                                   rendered no map. `--progress-fd --map` gave
#                                   fd 0 — machine tokens written to STDIN.
# NOTE THE DEVICE: /dev/null, which OPENS. main() opens the device before it
# dispatches to a command (the wart pinned above), so a nonexistent device
# short-circuits before the per-command parser is ever reached — a test using
# one would pass while checking nothing. Found by writing it that way first. Exit 1 is the usage/argument code and was already
# correct for the trailing form; the mid-line form is the one that used to
# exit 0.
for opt in --chunk --progress-fd --start --count --retries --verify --ladder --pcm; do
	run "value-taking $opt trailing" 1 -- --device /dev/null read "$opt"
	# `--` before the pattern: $opt starts with "--" and grep would otherwise
	# read it as an option ("grep: unrecognized option '--chunk requires a
	# value'"). The same class of bug as the one under test, in its own test.
	if grep -qF -- "$opt requires a value" "$TMP/err"; then
		ok "$opt trailing names the option"
	else
		bad "$opt trailing" "stderr: $(head -1 "$TMP/err")"
	fi

	run "value-taking $opt eats next flag" 1 -- --device /dev/null read "$opt" --map
	if grep -qF -- "the next argument is '--map'" "$TMP/err"; then
		ok "$opt does not swallow the next flag"
	else
		bad "$opt mid-line" "stderr: $(head -1 "$TMP/err")"
	fi
done

# A LONE "-" IS NOT FLAG-SHAPED and must still be accepted as a value: it is the
# conventional stdin/stdout filename. Falsifies the guard in the other
# direction — a check that rejected everything starting with "-" would pass the
# cases above and break this one. Exit 2 = it got past parsing to the device.
run "a lone - is a value, not a flag" 2 -- --device /nonexistent/zz read --pcm -

# Global options are parsed by main(), a different loop from the per-command
# ones. Covered so the fix is not half-applied.
run "global --device with no value" 1 -- --device
run "global --driver eats next flag" 1 -- --device /dev/null --driver --help

if [ "$fails" -gt 0 ]; then
	printf 'cli_surface: %d failure(s)\n' "$fails"
	exit 1
fi
printf 'cli_surface: all checks passed\n'
