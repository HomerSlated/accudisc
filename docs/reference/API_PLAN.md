# AccuDisc — Library API completion plan

Status: **PLAN ONLY — nothing in phases 1–5 is started.** Raised 2026-07-25 by
an audit asking whether `libaccudisc` has parity with the `accudisc` binary.
Answer: **yes at the ABI level, no at the policy level.** This document is the
design for closing that gap and then building the bindings on top of it.

cdda2img is **pinned to a snapshot fork of the binary** (on `$PATH`, symlinked
where cdda2img expects it) for the duration of this work, so nothing here can
break it mid-flight. Every observable change accumulates in §8, the
communication ledger, and goes to them in one message when the rewrite lands.

## 1. What the audit measured

Not asserted — measured, and reproducible:

| check | result |
|---|---|
| exported `T` symbols in `libaccudisc.so` | 65 |
| functions declared in `include/accudisc/accudisc.h` | 65 |
| exported but undeclared (undocumented ABI) | 0 |
| declared but unexported (broken contract) | 0 |
| exports not prefixed `accudisc_` | 0 |
| symbols `cli/main.c` imports that are not public | 0 |
| public functions the CLI never calls | 7 |

`cli/main.c` includes `<accudisc/accudisc.h>` and nothing else — there is no
reach-through into `src/`. Reproduce with:

```sh
nm -D --defined-only build/src/libaccudisc.so | awk '$2=="T"{print $3}' | sort >/tmp/exported
grep -oh 'accudisc_[a-z0-9_]*(' include/accudisc/accudisc.h | tr -d '(' | sort -u >/tmp/declared
comm -3 /tmp/exported /tmp/declared      # must be empty
```

**So a library consumer can reach every primitive. What it cannot reach is the
judgement in `cli/main.c`** — 1838 lines against a 1199-line header, and the
difference is not all `printf`.

## 2. The three-layer model

The eight CLI-only behaviours are not one category, and treating them as one is
the main way this job goes wrong.

| layer | rule | examples |
|---|---|---|
| **Primitives** | already complete, do not touch | `accudisc_read_cdda`, `accudisc_read_toc` |
| **Guards** | go **inside** the primitive — never an opt-in helper | uncap+sub interlock, RDWR-vs-driver |
| **Policy** | new public helpers; the CLI is **rewritten onto them** | pregap scan, range resolution, census cadence, uncap scope |
| **Conventions** | stay in the CLI, get documented, **never** move to C | exit codes, `--progress-fd`, `render_map` |

The guard/policy split is decided by failure semantics, not convenience:

- A **policy helper** that isn't called degrades to *"the caller did it manually"*.
- A **guard** that isn't called degrades to *"the caller shipped corrupt data"*.

That is why the two items in §4 must not ship as helpers. The failure mode being
fixed is *forgetting*, so the check has to live where forgetting is impossible.

## 3. Non-goals — what must NOT move into the library

Stated up front because "mirror the binary" invites all of it:

- **Exit codes.** A library returns errors; it does not exit. The 0/1/2/3 map is
  a process convention.
- **`--progress-fd` line protocol.** It exists *because* the consumer is a
  separate process. An in-process caller already has the sink callback and the
  status map. Reproducing it inside one address space would be recreating an IPC
  workaround for no reason.
- **`render_map`.** Terminal presentation.
- **Human diagnostics.** The multi-line stderr prose (the MediaCloQ hint, the
  session listing on ambiguity) stays in the CLI — see §5.2 for how it survives.
- **A `.toc` writer.** Out of charter: the strings come from MusicBrainz and
  AccuDisc does no lookups. cdda2img authors `.toc`, we consume it.

What *is* owed: a mapping table in `cli-machine-interface.md` so binding authors
reproduce equivalent **semantics** (e.g. "`rc > 0` from `accudisc_write` is the
exit-3 condition") without reproducing the mechanism.

## 4. Phase 1 — the two silent-failure guards `[P1]`

Highest value, smallest change, no hardware needed to write (hardware needed to
confirm). Do these first and independently; they are useful even if the rest of
the plan is never executed.

### 4.1 SpeedRead + subchannel

`accudisc_read_cdda` must refuse `sub != ACCUDISC_SUB_NONE` while the vendor
read-speed uncap is on. Today the CLI interlocks at `cli/main.c:1543`; the
library states nothing, so a caller doing `accudisc_speed_uncap_set(dev, 1)`
then reading with `ACCUDISC_SUB_RAW` gets a corrupted Q subchannel on inner/mid
tracks — **measured 0% Q-CRC** — with a success return and no warning. See
`drivers/plextor/FEATURES.md`.

New error: `ACCUDISC_ERR_UNSAFE_COMBINATION`, and an explicit opt-out field in
`accudisc_read_req` for a caller who genuinely wants it (diagnostics, our own
measurement runs).

### 4.2 Read-only fd + vendor opcodes

`accudisc_driver_attach` must fail loudly on a device opened without
`ACCUDISC_OPEN_RDWR`. The kernel's SG_IO filter blocks vendor opcodes (and
WRITE(10)/SEND CUE SHEET) on a read-only fd. The CLI infers the mode at
`cli/main.c:1762`; the library says nothing, so the attach appears to succeed
and every subsequent vendor command fails obscurely.

**Both are behaviour changes to shipped functions** and need release notes even
at 0.x. Guard 4.1 in particular can break a caller currently getting bad data
silently — which is the point.

## 5. Phase 2 — promote the acquisition strategies

Each is a real algorithm with domain constants, not formatting. Each would
otherwise be reimplemented — differently — by Python and by Rust.

**The load-bearing constraint: the CLI is rewritten onto each helper in the same
commit that introduces it.** Never a state where the helper exists and
`cmd_read` keeps a copy. Otherwise the policy exists three times (CLI, Python,
Rust) and parity decays from the day it ships — which is exactly the defect
being fixed.

### 5.1 Pregap / index scan

From `cmd_pregaps`: `PREGAP_WINDOW 400` / `PREGAP_TAIL 4`, **one read per
boundary** to defeat the drive cache, synthesising a single-track
`accudisc_toc` to feed `accudisc_index_map_decode`.

```c
typedef struct accudisc_pregap_scan_opts {
    uint32_t window;            /* sectors before each boundary; 0 = 400 */
    uint32_t tail;              /* sectors after; 0 = 4 */
    uint16_t speed_x;           /* 0 = leave as-is */
    const volatile int *cancel;
} accudisc_pregap_scan_opts;

ACCUDISC_API int accudisc_scan_pregaps(accudisc_device *dev,
                                       const accudisc_toc *toc,
                                       const accudisc_pregap_scan_opts *opts,
                                       accudisc_index_map *out, uint8_t max,
                                       uint8_t *n_out);
```

### 5.2 Read-range resolution — **make it a pure function**

From `cmd_read`: session → `accudisc_toc_session_audio_range` → fall through on
`ERR_INVAL` → whole-disc default, then the `accudisc_check_audio_range` guard.
Every primitive is public; the ordering and the defaults are not.

This one needs no device — TOC in, plan out — which is the single biggest
testability win in the plan. Today every one of these branches (HTOA pregap,
Mixed Mode split, multi-session ambiguity, degraded lead-in) is reachable only
with the right physical disc in the drive. As a pure function they all become
unit tests against synthetic TOCs.

```c
typedef struct accudisc_range_spec {
    int32_t session;      /* -1 = unspecified */
    int32_t first_track;  /* -1 = unspecified */
    int32_t last_track;
    int64_t start;        /* -1 = unspecified */
    int64_t count;        /* -1 = unspecified (through end) */
    uint8_t force;        /* skip the audio-range guard */
} accudisc_range_spec;

typedef struct accudisc_range_plan {
    uint32_t lba, count;
    uint8_t  session;     /* session actually chosen; 0 = flat/no structure */
    uint8_t  reason;      /* accudisc_range_reason_* — see below */
    accudisc_range_check check; /* populated when the guard refused */
} accudisc_range_plan;

ACCUDISC_API int accudisc_plan_read_range(const accudisc_toc *toc,
                                          const accudisc_range_spec *spec,
                                          accudisc_range_plan *out);
```

**Reason codes are mandatory, not optional.** A bare `ACCUDISC_ERR_UNSUPPORTED`
cannot tell the CLI whether to print the MediaCloQ explanation or the session
listing, so moving the decision into the library would silently downgrade the
CLI's error quality — the one thing a refactor must be incapable of. Precedent
already exists: `accudisc_check_audio_range` fills `chk.reason` alongside
`accudisc_range_reason_str()`. Follow it. Needed at minimum:

`MULTIPLE_AUDIO_SESSIONS`, `NO_AUDIO_SESSION` (carries the CTRL-may-be-lying
hint), `TRACKS_CROSS_SESSION`, `TRACKS_NOT_FOUND`, `SESSION_SPLIT_BY_DATA`,
`START_PAST_LEADOUT`, `EMPTY_RANGE`, `GUARD_REFUSED`.

### 5.3 Counter census cadence

From `cmd_cxscan`: 75-sector (one audio second) sampling while the vendor
counters are armed. A callback per sample, so the CLI's `printf` and a Python
generator are both thin over it.

### 5.4 Uncap scope — push/pop

The leave-the-drive-as-found discipline (`cli/main.c` `uncap_prior`, restored in
the `out:` block). Names follow the project's existing push/pop state SOP: pop
only what you pushed, restore to prior — never to factory.

```c
ACCUDISC_API int accudisc_speed_uncap_push(accudisc_device *dev, int on,
                                           int *prior_out);
ACCUDISC_API int accudisc_speed_uncap_pop(accudisc_device *dev, int prior);
```

Rust `Drop` and a Python context manager map onto this exactly — the bindings
will do it *better* than C can, which is fine. Parity means no capability or
safety is lost, not that every layer has the same shape.

## 6. Phase 0 — the seatbelt, built before phase 1 — **LANDED 2026-07-25**

What exists now:

| artefact | what it pins |
|---|---|
| `cli/format.c` + `format.h` | `cmd_toc`'s output split into a pure function of `(toc, info)` — no device handle. This is what made the rest testable. |
| `tests/test_cli_toc_format.c` | 4 cases through the real chain `vec_fulltoc → accudisc_fulltoc_parse → adsc_toc_from_fulltoc → adsc_cli_fmt_toc`. Covers the `pregap` token, a format-0 degrade, and an anomalous lead-in with `toc_trusted=0` — three branches previously reachable only with the right physical disc. |
| `tests/cli_surface.sh` + `tests/golden/usage.txt` | The 111-line usage text on both stdout and stderr, the `accudisc <semver>` version shape, and exit codes 0/1/2 — asserted against the real binary. |
| `tests/policy_constants.sh` | The drift detector. See below. |

Verified against real media before the drive became unavailable: `info disc
media features toc fulltoc text scan` all byte-identical pre- and
post-refactor, exit codes unchanged. Baseline captures live in
`private/bench/refactor-baseline-2026-07-25/` (git-ignored) for phases 1–2 to
diff against.

**The drift detector enforces "in exactly one of `cli/` and `src/`", not "not
in both."** A not-in-both rule passes when a constant is renamed away or when
the source path is wrong — green forever, checking nothing. Requiring a
positive hit on exactly one side makes a bad path a failure. Verified against
four synthetic trees: correct tree passes, duplicated constant fails, missing
constant fails, bad root fails. `CXSCAN_CADENCE` was named in the same commit
(it was a bare `75`) so it could be tracked at all.

Original reasoning follows.



Not a branch. A branch isolates a regression; it does not detect one. What
detects one is a golden-output test.

`cli-machine-interface.md` already declares the CLI stable and additive-only.
Make that executable: record the output of every non-hardware command path and
diff on each build. Where hardware is unavoidable, record one captured TOC/blob
fixture and drive the parse path from it (the existing `tests/vectors.h` model).

Second, cheaper guard against the *specific* drift this plan exists to fix: a
test asserting the policy constants (`PREGAP_WINDOW`, `PREGAP_TAIL`, the census
cadence) appear in `src/` and **not** in `cli/`. That would have caught this
divergence as it happened rather than a year later.

**Phase 0 makes the whole job faster,** by converting "re-read everything
carefully" into "run `ctest`".

## 7. Phases 3–4 — bindings, and the three questions that gate them

Previously deferred by explicit decision; reopened by this audit. **Do not start
either binding until these are settled**, because both bindings inherit the
answers and fixing a shared design flaw twice is the avoidable cost.

### 7.1 Transparent structs are an ABI hazard

`accudisc_read_req`, `accudisc_chunk` and `accudisc_read_stats` are
caller-allocated, transparent, and carry no size or version field.
`accudisc_read_req` is visibly still growing — the accuracy-strategy block
(`c2_retries`, `verify_passes`, `overlap_sectors`, `speed_ladder`,
`ladder_len`) was added in layers — and every added field changes `sizeof`. The
CLI is rebuilt with the library so it never notices; a binding compiled against
one header and loaded against a different `.so` will, by scribbling past the
end of a struct.

Options: leading `size` field, opaque + accessors, or an explicit "rebuild your
binding on every 0.x bump" policy. **soname is `.so.0`, so breaking this is
still free. It will not be later.** Decide in this phase or inherit it forever.

### 7.2 The sink callback across the FFI boundary

`accudisc_sink_fn` fires per chunk with a pointer valid only during the call.

- **Python**: a GIL round-trip per chunk, and the buffer must be copied or
  wrapped in a `memoryview` invalidated on return. Measure at whole-disc scale
  (~350k sectors) before committing to a chunk size.
- **Rust**: an `extern "C"` trampoline that must not unwind across the boundary.

This is the hardest part of both bindings. Prove it once.

### 7.3 What a binding buys over the subprocess path

Worth writing down, because it decides whether the binding is a thin CLI mirror
or a richer streaming API. cdda2img drives the binary today
(`src/cdda2img/accudisc_reader.py`, `subprocess` throughout) and it works;
`cli-machine-interface.md` is a stable contract, and `--map-file` already
exposes the per-sector status map to another process via `mmap` — the
frame-accurate status surface is *already* available across a process boundary.

The genuine wins are: zero-copy PCM instead of a pipe, the status map without a
backing file, and no line-protocol parsing. Those are real. They are not
"the subprocess path is a hack".

**Order: Python first, then Rust.** cdda2img is the only consumer that can
validate parity empirically — same disc, subprocess vs binding, compare bytes.

## 8. Communication ledger — everything cdda2img must be told

They are pinned to a snapshot fork, so this accumulates and goes in **one**
message when the rewrite lands. Do not dribble it out.

| # | change | status | breaks a parser? |
|---|---|---|---|
| 1 | `toc`: `pregaps=` → `subq_indices=` | **DONE 2026-07-25** | No — nothing consumed it; they derive pregaps from their own subchannel decode (`subchannel.py:673`, `subq_toc.py:106-139`) |
| 2 | CLI behaviour through the refactor | target: **no change at all** | Must be No — that is the phase-0 test's job |
| 3 | Guard 4.1/4.2 | CLI already interlocks both; no CLI-visible change | No |
| 4 | Bindings availability + ABI policy | phase 4 | N/A |
| 5 | An unknown command exits **2**, not 1 | pinned by `cli_surface.sh`, unchanged | No — but see below |

Anything that lands in this table as "breaks a parser: Yes" needs a decision,
not just a note.

Row 5 is a wart found while writing the phase-0 tests, not a change: `main()`
opens the device *before* it dispatches, so `accudisc not-a-command` fails with
`exit 2` (could not complete) rather than `exit 1` (usage). A misspelled
command should not require hardware. It is pinned by the golden test as-is so
that fixing it has to be deliberate — and if it is ever fixed, it changes an
exit code, which is exactly the class of change that belongs in this ledger
before it ships.

### 8.1 The drive is a shared, uninterlocked resource

Not an interface change, but it belongs here because it corrupts measurements
on both sides. There is **one optical drive and two agents**, with nothing
arbitrating access. On 2026-07-25 both projects drove `/dev/sr0` concurrently:
cdda2img's `recovery_bench.py` ran 14:00:44 onward while our post-refactor
verification ran 14:10:16–14:15:56, including a seek-heavy `pregaps` scan.

The damage was measurable in both directions. Our own `pregaps` Q-CRC counters
moved between an idle-drive run and a contended one (`[141 ok, 9 bad]` →
`[142 ok, 8 bad]`) with the decoded values byte-identical — so contention
perturbs exactly the subchannel-quality signal their bench rungs measure, while
leaving derived geometry alone. That is a trap: the numbers that move are the
ones being measured, and the ones that stay put are the ones that would have
caught it.

Proposed to them as §57.3: wrap every `/dev/sr0` command in
`flock /var/tmp/sr0.lock -c '…'`, taking the lock for a whole long job rather
than per-command. Advisory, so it only works if both sides honour it. **Any
timed or quality-sensitive measurement taken without the lock should be
treated as suspect.**

## 9. Open questions — resolve before phase 1

1. **How does guard 4.1 detect the uncap without a driver?**
   `accudisc_speed_uncap_get` is driver-gated and returns `ERR_UNSUPPORTED`
   with no driver attached — but the uncap is *persistent drive state* a prior
   session may have left on. The CLI has a driver-free heuristic (PLEXTOR +
   mode-page-2A max read > 40×) at `cli/main.c:1560`. Does the guard use the
   driver query, the heuristic, or both? A guard that only fires when a driver
   happens to be attached is a guard with a hole.
2. **Refuse or warn?** Refusing is safer and is the stated intent, but it is a
   behaviour change. Proposal: refuse by default, explicit opt-out field in
   `accudisc_read_req`. Confirm before building.
3. **Does `accudisc_plan_read_range` own the `--force` semantics**, or does the
   caller skip the guard itself? Proposal: it owns it, via `spec.force`, so
   every consumer gets the same override with the same meaning.
4. **Is `subq_indices` the right final name**, given a future scanned value?
   `subq_indices=none|scanned` reads well; alternatives were `index_src=`.
   Locked for now — cheap to revisit while the fork is pinned.

## 10. Effort estimate

Grounded in this project's measured velocity: 116 commits over 16 calendar days
(13 active), ~17,900 lines across `src/ include/ cli/ tests/ drivers/` —
roughly 1,100 lines/active day including tests and docs. Closest comparable is
the DAO write engine: **+746 lines in one day**, 6 commits, covering write
params, disc-state check, cue-sheet builder, `.toc` parser and full engine.

| piece | estimate |
|---|---|
| Phase 0 seatbelt | ~150–200 lines |
| Phase 1 guards + tests | ~80–120 lines |
| Phase 2 helpers in `src/`+`include/` | ~450–550 lines |
| `cli/main.c` rewired | shrinks ~250–350 |
| new tests (codebase runs ~1:3 test:source) | ~150–250 lines |
| **total touched** | **~900–1,200 lines** |

At the observed cadence that is ~1 day of typing. **Budget 2–3 active days**: a
refactor carries a verification tax greenfield does not — you must prove nothing
changed — and two paths (5.1, 5.2) only prove out against real media. Phases
3–4 (bindings) are a separate, larger estimate not attempted here until §7 is
resolved.

## 11. Sequencing summary

```
phase 0   golden-output test + constants-location test      DONE 2026-07-25
phase 1   guards 4.1, 4.2                                   (no CLI change)
phase 2   5.4 uncap push/pop   -> CLI rewired, same commit   (mechanical)
          5.3 census cadence   -> CLI rewired, same commit   (mechanical)
          5.2 range resolution -> CLI rewired, same commit   (needs media)
          5.1 pregap scan      -> CLI rewired, same commit   (needs media)
phase 2b  doc: exit-code/semantic mapping table
          fix CLAUDE.md:56 "thin layer" (see §12)
phase 3   resolve §7.1-7.3; ONE message to cdda2img (§8)
phase 4   Python binding, validated A/B against the subprocess path
phase 5   Rust binding
```

`main` stays shippable at every commit; `scripts/sync.py` already gates on build
+ tests.

## 12. Documentation correction owed either way

`CLAUDE.md:56` calls `cli/` a "thin layer over the library". Structurally true
(no internal reach-through), behaviourally misleading given §4.
`docs/reference/cli-machine-interface.md` makes no parity claim and is correct
as it stands.
