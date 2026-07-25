#!/bin/sh
# Drift detector for the exit-3 contract.
#
# docs/reference/cli-machine-interface.md calls itself a STABLE INTERFACE, and
# on 2026-07-26 it was not: `pregaps`, `c2lag` and `media` could all exit 3 and
# none of the three was listed. Nothing caught it because a document cannot be
# out of date in a way the compiler notices, and the CLI tests check what the
# binary prints rather than what the contract promises.
#
# The rule: every place in cli/main.c that produces exit 3 belongs to a
# subcommand the document names in its exit-3 list.
#
# The map below is deliberately hand-maintained, and an UNMAPPED function is a
# FAILURE rather than a skip. That is the positive-hit rule from
# policy_constants.sh: a test that quietly ignores what it does not recognise
# goes green forever while checking nothing, which is worse than the drift it
# was written to detect. Adding a new exit-3 path means adding a row here and a
# bullet there — that is the point, not an obstacle.
#
#   usage: exit_codes.sh <project-source-root>

set -u

ROOT="$1"
SRC="$ROOT/cli/main.c"
DOC="$ROOT/docs/reference/cli-machine-interface.md"
fails=0

[ -f "$SRC" ] && [ -f "$DOC" ] || {
	printf 'exit_codes: FAIL: bad source root %s\n' "$ROOT"
	exit 1
}

# enclosing function -> the subcommand token the doc must name.
# dump_blob is `read`'s inline lead-in capture, so it maps to read.
map='cmd_read:read
cmd_text:cdtext
cmd_fulltoc_parsed:fulltoc
cmd_scan:scan
cmd_pregaps:pregaps
cmd_c2lag:c2lag
cmd_media:media
cmd_write:write
cmd_disc:disc
dump_blob:read'

# The exit-3 bullet list: from its heading to the next one.
doc_list=$(awk '/^Exit 3 per subcommand:/{f=1} /^## /{f=0} f' "$DOC")
[ -n "$doc_list" ] || {
	printf 'exit_codes: FAIL: no "Exit 3 per subcommand:" list in %s\n' "$DOC"
	exit 1
}

# Every exit-3 site, tagged with the function it sits in. The four forms are
# the ones the CLI actually uses; a fifth would show up as an unmapped or
# missing site rather than being silently skipped.
# The identifier class MUST admit digits. Without them `cmd_c2lag(` does not
# match, fn stays stale from the previous function, and the test attributes
# c2lag's exit 3 to whichever function was declared before it — reporting a
# real condition against the wrong subcommand. It did exactly that on first
# run: `cmd_speeds` flagged, `cmd_c2lag` invisible. Well-formed output, wrong
# referent, and the failure looked like a genuine finding.
sites=$(awk '
	/^static [A-Za-z_][A-Za-z0-9_]* \*?[A-Za-z_][A-Za-z0-9_]*\(/ {
		for (i = 1; i <= NF; i++)
			if ($i ~ /\(/) { fn = $i; break }
		sub(/\(.*/, "", fn); sub(/^\*/, "", fn)
	}
	/return 3;|ret = 3;|\? 3 :|: 3;|\? 3$/ { if (fn != "") print fn }
' "$SRC" | sort -u)

[ -n "$sites" ] || {
	printf 'exit_codes: FAIL: found no exit-3 sites in %s\n' "$SRC"
	printf '       The patterns went stale, which makes this test vacuous.\n'
	exit 1
}

for fn in $sites; do
	token=$(printf '%s\n' "$map" | sed -n "s/^$fn://p")
	if [ -z "$token" ]; then
		printf '  FAIL %s: reaches exit 3 but is not in this test'"'"'s map\n' "$fn"
		printf '       Add it, and add a bullet to the exit-3 list in\n'
		printf '       cli-machine-interface.md saying when it fires.\n'
		fails=$((fails + 1))
		continue
	fi
	if printf '%s\n' "$doc_list" | grep -q -- "\`$token\`"; then
		printf '  ok   %-20s -> `%s` documented\n' "$fn" "$token"
	else
		printf '  FAIL %s: exit 3 is undocumented — `%s` is not named in the\n' \
		       "$fn" "$token"
		printf '       exit-3 list of cli-machine-interface.md.\n'
		fails=$((fails + 1))
	fi
done

if [ "$fails" -gt 0 ]; then
	printf 'exit_codes: %d failure(s)\n' "$fails"
	exit 1
fi
printf 'exit_codes: every exit-3 path is documented\n'
