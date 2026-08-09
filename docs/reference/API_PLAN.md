# AccuDisc — Library API completion plan

Status: **PLAN ONLY — nothing in phases 1–5 is started.** Raised 2026-07-25 by
an audit asking whether `libaccudisc` has parity with the `accudisc` binary.
Answer: **yes at the ABI level, no at the policy level.** This document is the
design for closing that gap and then building the bindings on top of it.

cdda2img was **pinned to a snapshot fork of the binary** (on `$PATH`, symlinked
where cdda2img expects it) for the duration of this work, so nothing here could
break it mid-flight. Every observable change accumulated in §8, the
communication ledger, and went to them in one message when the rewrite landed
(2026-07-26, §ay; verified inert item-by-item in their §80).

**The pin was retired 2026-07-26**, phases 0–3 complete: the symlinks
(`~/.local/bin/accudisc` and `cdda2img/tools/accudisc/accudisc`) point back at
this tree's `build/cli/accudisc`, as they did before the rewrite. Two standing
consequences, neither of which existed while the pin held:

- **§8 is now a live obligation, not an accumulating one.** Any CLI-surface
  change reaches cdda2img on our next build. The ledger stops being a document
  we hand over at the end and becomes something to write *before* the commit.
- **A rebuild swaps their engine mid-run.** They invoke per command, so
  rebuilding during one of their multi-hour bench runs changes the binary
  between invocations *within* a single run, and their `# engine:` sha256 line
  is captured at the header and would not notice. Same class as the shared-drive
  problem in §8.1, one layer up — and the same protocol covers it:
  `/var/tmp/sr0.owner` already says when a long run is in flight, so read it
  before `cmake --build`, not only before touching `/dev/sr0`.

Also note `setcap` now disarms *their* binary on every rebuild of ours, which is
the second reason to cap the installed copy rather than the build-tree one.

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

## 4. Phase 1 — the two silent-failure guards `[P1]` — **ONE LANDED, ONE REMOVED 2026-08-09**

> **Guard 4.1 (SpeedRead + subchannel) NO LONGER EXISTS.** Removed in 0.6.0 by
> Keith's ruling: the drive governs CD-DA to 40× whatever the uncap is set to,
> and mode page 2A reports the requested figure rather than the governed rate,
> so the dangerous state the guard refused was never reachable on an audio disc.
> Everything below about 4.1 — the refusal, `allow_unsafe`,
> `ACCUDISC_ERR_UNSAFE_COMBINATION`, the CLI interlock and the pre-read warning
> — describes code that has been deleted. It is kept because §9.1/§9.2's
> reasoning about *detection* (three sources, authoritative vs inferred) is
> still live: the state is still probed and reported, just never enforced.
>
> **Guard 4.2 (read-only fd + vendor opcodes) STANDS.** It reports a condition
> rather than predicting a failure, and is correct under both the setcap and the
> default regimes.
>
> The section's old status line read *"WRITTEN 2026-07-25, refusal
> unexercised"*. That never changed, and is now permanent: the end-to-end
> refusal never executed once in its lifetime. Worth remembering the next time a
> guard is justified by an inference from its parts.

Highest value, smallest change, no hardware needed to write (hardware needed to
confirm). Do these first and independently; they are useful even if the rest of
the plan is never executed.

| piece | where |
|---|---|
| `accudisc_uncap_state`, `accudisc_speed_uncap_probe` | `include/accudisc/accudisc.h` |
| stock-ceiling table + `adsc_uncap_classify` (pure) | `src/drive/uncap.c` |
| `uncap_set` on the handle, set only on a successful set | `src/internal.h`, `src/drive/driver.c` |
| refusal on authoritative `ON` | `src/read/engine.c` |
| `allow_unsafe` opt-out (placed in existing padding) | `include/accudisc/accudisc.h` |
| `ACCUDISC_ERR_UNSAFE_COMBINATION` + `strerror` | `include/accudisc/accudisc.h`, `src/device.c` |
| read-only-handle diagnosis at attach | `src/drive/driver.c` |
| CLI warning re-sourced from the probe | `cli/main.c` |
| 15 cases, no hardware | `tests/test_uncap.c` |

**Accurate status, because "LANDED" flatters it:** both guards are written and
the suite is green at 28/28, and the *decision* each keys on is tested —
`adsc_uncap_classify` over the whole table, `adsc_uncap_authoritative` over all
three handle states including that it never yields `LIKELY_ON`.

**The end-to-end refusal has never executed.** Nothing has yet made
`accudisc_read_cdda` return `ACCUDISC_ERR_UNSAFE_COMBINATION`. It cannot be
reached device-free — `main()` opens the device before dispatch, so the CLI's
own `--uncap` + `--sub` interlock is equally unreachable from
`tests/cli_surface.sh` — and reaching it needs a Plextor with the uncap actually
toggled. That is the one outstanding item, and until it is done "the guard
works" is an inference from its parts, not an observation.

A hot-path note that shaped the code: the engine calls
`adsc_uncap_authoritative`, not `accudisc_speed_uncap_probe`. The full probe
issues a MODE SENSE(10), and it runs at the head of *every* subchannel read; the
inferred state it would compute cannot change the refusal, so the split keeps
the guard free of any extra drive command.

### 4.1 SpeedRead + subchannel — **REMOVED 2026-08-09; historical from here**

`accudisc_read_cdda` must refuse `sub != ACCUDISC_SUB_NONE` while the vendor
read-speed uncap is on. Today the CLI interlocks at `cli/main.c:1502`; the
library states nothing, so a caller doing `accudisc_speed_uncap_set(dev, 1)`
then reading with `ACCUDISC_SUB_RAW` gets a corrupted Q subchannel on inner/mid
tracks — **measured 0% Q-CRC** — with a success return and no warning. See
`drivers/plextor/FEATURES.md`.

New error: `ACCUDISC_ERR_UNSAFE_COMBINATION`, and an explicit opt-out field in
`accudisc_read_req` for a caller who genuinely wants it (diagnostics, our own
measurement runs).

**Scope grew when §9.1 was resolved.** The uncap is persistent drive state, so
"is it on?" is not always answerable with certainty, and a guard that refuses on
a guess is worse than one that reports. This guard therefore ships as *two*
pieces: a new `accudisc_speed_uncap_probe` returning a four-valued
`accudisc_uncap_state`, and a refusal in `read_cdda` that fires only on the
authoritative values. See §9.1–9.2 for the resolution order, the per-model stock
ceiling table that replaces the CLI's hardcoded `> 40`, and the residual hole
that is deliberately reported rather than closed.

### 4.2 Read-only fd + vendor opcodes — **premise corrected 2026-07-25**

The kernel's SG_IO filter blocks vendor opcodes (and WRITE(10)/SEND CUE SHEET)
on a read-only fd. Measured here, `/dev/sr1`, opcode `0xEA`, no capability:

```
O_RDONLY  ioctl fails EPERM        <- filter rejects it; the drive never sees it
O_RDWR    ioctl OK, sense key 0x05 <- reaches the device, which refuses it
```

Two corrections to what this section originally said.

**"The attach appears to succeed" was wrong.** The selftest issues a real vendor
opcode (`px_selftest`, `drivers/plextor/plextor.c`), so on a read-only fd it
fails and the attach is correctly refused. The defect is not a false success —
it is that the failure is reported as `driver plextor: 0xEA arm refused`, which
names the symptom and not the cause, and is indistinguishable from "this is not
a Plextor".

**"Must fail loudly on a device opened without `ACCUDISC_OPEN_RDWR`" is not
implementable as stated.** The filter short-circuits entirely under
`CAP_SYS_RAWIO`, which this project's *optional* `setcap` build target installs
(`CMakeLists.txt:39-44`). So vendor opcodes on a read-only handle work fine in a
configuration we ship and support. A hard refusal would break it, and the
library cannot detect which regime it is in without probing for a capability it
has no business asking about.

**What landed instead**, correct under both regimes and with no false positives:

- At attach, if the handle is read-only, log the *condition* — "vendor opcodes
  need `ACCUDISC_OPEN_RDWR` or `CAP_SYS_RAWIO`" — not a prediction of failure.
  Under setcap it is noise; under the default it is the hint that saves the
  debugging session.
- Keep the selftest as the gate: it answers empirically under either regime.
- When it fails, distinguish the two failures the measurement above separates.
  A filter rejection arrives as `ERR_IO` with `dev->last_io` set; a drive
  rejection as `ERR_SENSE`. Only the second is evidence about the drive, and the
  log now says so.

`dev->last_io` is cleared immediately before the selftest — it deliberately
survives a success (`src/device.c:141-145`), so a stale entry from an unrelated
earlier failure could otherwise be misattributed to the selftest.

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

## 7. Phases 3–4 — bindings, and the three questions that gate them — **RESOLVED 2026-07-26**

Previously deferred by explicit decision; reopened by this audit. **Do not start
either binding until these are settled**, because both bindings inherit the
answers and fixing a shared design flaw twice is the avoidable cost.

### 7.1 Transparent structs are an ABI hazard — **RESOLVED: size field, landed**

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

**Growth is measured, not assumed** — each historical header compiled and its
`sizeof` printed:

| struct | first | | | today | direction |
|---|---|---|---|---|---|
| `accudisc_read_req` | 32 | 40 | 56 | **56** | caller fills, library reads |
| `accudisc_read_stats` | 80 | 104 | 128 | **136** | library writes |
| `accudisc_chunk` | 32 | 32 | 32 | **32** | library allocates, sink reads |

**33 public structs are transparent, and that is the wrong denominator.** The
hazard needs all three of: caller-allocated, crosses the FFI boundary, *and*
can grow. Most of the rest are fixed by an external format and cannot grow at
all — `accudisc_track` (16 B, a Red Book TOC entry), `accudisc_q` (38 B,
on-disc), `accudisc_sense` (4 B, SCSI), `accudisc_fulltoc_entry` (9 B).
Widening the question to all 33 turns a decidable problem into an unbounded
one.

**Resolved: a leading `uint32_t size` on the two that grow.** Implemented in
this phase rather than deferred to phase 4, because the fact that makes the
break free is not "we are 0.x" — it is that **nothing outside this repo links
the library**. cdda2img drives the *binary* (§7.3). That stops being true the
moment the Python binding ships, and a binding written against the unsized
struct would then have to be rewritten.

The rules are asymmetric, because the direction of the write is:

- **IN** (`read_req`): short is zero-extended — an older caller gets older
  behaviour. Long is accepted only if every byte past our end is zero; a set
  one means the caller is asking for a feature this build does not have, and
  gets `ACCUDISC_ERR_ABI` instead of silence. (The kernel's
  `copy_struct_from_user` model.)
- **OUT** (`read_stats`): short is honoured by writing only that far. Long is
  refused — the library would leave counters the caller believes in unfilled,
  and a zero meaning *"never computed"* is indistinguishable from a zero
  meaning *"none observed"*.
- **Zero is always refused.** It is what a caller who forgot
  `ACCUDISC_READ_REQ_INIT` produces, so forgetting fails on the first call.

`ACCUDISC_ERR_ABI` (−12) is deliberately distinct from `ERR_INVAL`: it means
*"rebuild against this header"*, not *"fix your arguments"* — and it is what
makes the accept path testable device-free, since an `ERR_INVAL` from
`accudisc_read_cdda(NULL, &req, …)` is positive evidence that the ABI layer
accepted the struct and execution reached the checks behind it.

The `size` field cost `read_req` **nothing** — it landed in existing padding,
still 56 bytes. `read_stats` went 128 → 136.

**The first version of the guard had the bug it was written to prevent.** The
long-struct path scans the caller's bytes past our end looking for fields we
cannot honour — and `src_size` is the caller's *claim*, unbounded, straight out
of caller memory. An uninitialised `accudisc_read_req req;` leaves stack garbage
there, routinely large, so the loop that exists to stop the library running off
the end of a struct ran off the end of that struct by megabytes. Confirmed by
removing the fix and rebuilding under ASan: `stack-buffer-overflow, READ of size
1`. `ADSC_ABI_GROWTH_MAX` (256 bytes) now bounds it before the scan.

Stated honestly, because it narrows the hole rather than closing it: a garbage
value landing *inside* the accepted window still permits a short overread. An
in-process callee cannot verify a caller's allocation — the size field is a
claim either way. What the bound removes is the unbounded case, which is the one
that faults.

**The layout change bumped the version to 0.2.0.** Every `read_req` field moved
4 bytes and `read_stats` grew 8, so leaving the number at 0.1.0 would have made
the version useless as exactly the pin marker cdda2img said (§80.4) they intend
to use it as. soname stays `.so.0`.

**Structs without a size field are frozen**, and `tests/test_abi.c` pins them
with `_Static_assert` so growth is a deliberate act. `accudisc_chunk` is the
one that matters: the library allocates it and the caller's sink reads it, so
the hazard runs the other way and a size field would not help. It has been 32
bytes since it was introduced.

**Version drift, found while resolving this and fixed with it.** `accudisc.h`
said `0.1.0` (and the binary printed it) while CMake built
`libaccudisc.so.0.0.1`. A policy of "rebuild on a version bump" is
unenforceable while a binding can ask two sources and get two answers. CMake
now parses the version out of the installed header, and `tests/test_version.c`
asserts the two agree — previously it compared the header's macros against
themselves, which cannot fail.

### 7.2 The sink callback across the FFI boundary — **RESOLVED: not the cost; lifetime is**

`accudisc_sink_fn` fires per chunk with a pointer valid only during the call.

- **Python**: a GIL round-trip per chunk, and the buffer must be copied or
  wrapped in a `memoryview` invalidated on return. Measure at whole-disc scale
  (~350k sectors) before committing to a chunk size.
- **Rust**: an `extern "C"` trampoline that must not unwind across the boundary.

This is the hardest part of both bindings. Prove it once.

**The stated worry is arithmetic, and the arithmetic kills it.** Chunks are
bounded by `ADSC_MAX_XFER` (65535) and `ADSC_CHUNK_MAX` (32), so the sink fires
far less often than "per 350k sectors" suggests:

| request | `sector_len` | chunk | sink calls, 79-min disc | delivered |
|---|---|---|---|---|
| audio only | 2352 | 27 | 13,167 | 836 MB |
| audio + C2 | 2646 | 24 | 14,813 | 941 MB |
| audio + C2 + raw sub | 2742 | 23 | 15,457 | 975 MB |

**10⁴ callbacks, not 10⁵–10⁶.** Against a rip measured in minutes, per-call FFI
overhead is noise at any plausible per-call cost. No benchmark is needed to
choose a chunk size, and none was built. The real cost is the ~1 GB that has to
cross the boundary, which is a copy question, not a call-overhead question.

**So spend the effort on the thing a benchmark cannot find: lifetime.**
`chunk->data` is valid only during the call. A Python `memoryview` handed to
user code and retained outlives the buffer and then reads freed memory as
plausible PCM — well-formed, wrong referent, no exception, no C2 flag. That is
this project's dominant failure class arriving through the FFI. Decided once,
for both bindings:

- **Python**: copy by default. Zero-copy behind an explicit opt-in, and the
  view is *released* on return, so a stored reference raises `ValueError`
  instead of reading freed memory. A binding that is merely documented as
  "don't keep the buffer" has no way to enforce it.
- **Rust**: `catch_unwind` in the `extern "C"` trampoline — unwinding across
  the boundary is UB — returning nonzero to cancel, which the engine already
  maps to `ACCUDISC_ERR_CANCELLED`. The borrow checker gives the lifetime rule
  for free; Python has to be given it.

### 7.3 What a binding buys over the subprocess path — **RESOLVED: streaming API, not a CLI mirror**

Worth writing down, because it decides whether the binding is a thin CLI mirror
or a richer streaming API. cdda2img drives the binary today
(`src/cdda2img/accudisc_reader.py`, `subprocess` throughout) and it works;
`cli-machine-interface.md` is a stable contract, and `--map-file` already
exposes the per-sector status map to another process via `mmap` — the
frame-accurate status surface is *already* available across a process boundary.

The genuine wins are: zero-copy PCM instead of a pipe, the status map without a
backing file, and no line-protocol parsing. Those are real. They are not
"the subprocess path is a hack".

**Resolved, and it falls out of §7.2 rather than needing its own
investigation.** The one thing the subprocess path structurally cannot do is
hand the caller the PCM without copying it through a pipe — ~1 GB per disc.
That is the binding's reason to exist, so the binding is a **streaming API**
built around the sink, not a `subprocess.run` replacement that returns a blob.

Concretely, what a binding must NOT do: shell out, re-parse
`--progress-fd` lines, or reimplement the exit-code mapping. Those are §3
conventions and reproducing them in-process would be recreating an IPC
workaround inside one address space. What it MUST do: expose the sink, expose
the status map as a buffer over `req.status_map`, and surface `accudisc_err`
as typed exceptions — with `ACCUDISC_ERR_NOTFOUND` mapped as *absence*, never
as failure (`cli-machine-interface.md`, exit-code table), and
`accudisc_write`'s **positive** return kept positive.

**Order: Python first, then Rust.** cdda2img is the only consumer that can
validate parity empirically — same disc, subprocess vs binding, compare bytes.

> **CORRECTION 2026-07-27 — the premise above is false, and the conclusion it
> supports is wrong in its emphasis.** "The one thing the subprocess path
> structurally cannot do is hand the caller the PCM without copying it through
> a pipe" describes a transport nobody uses: `accudisc read --pcm FILE` has the
> CLI write the file, and the consumer never touches the bytes. cdda2img raised
> this (§101.2) with their call sites, and they are right.
>
> What actually justifies the binding is the **small, repeated** read, not the
> big one: a bounded span today requires writing a temp file and reading it
> straight back, and their AR-recovery ladder does that `passes x rungs` times
> **per failed track**. So `read_span() -> bytes` is the binding's reason to
> exist, and whole-disc reads stay file-based.
>
> The measurement that settles the whole-disc half, taken 2026-07-27 at
> cdda2img's own chunk figures (14,813 x 24 x 2646 B = 0.941 GB): the per-chunk
> Python copy costs 0.02 s cache-warm, and copy + write + fsync of the full
> ~1 GB costs 0.51 s. Sub-second against a multi-minute rip. The file path wins
> on simplicity and on what the downstream wants, **not** on copy cost — and
> saying so keeps the next person from "optimising" it back.
>
> §7.2's lifetime conclusions are unaffected and survived contact. Two hazards
> it did not cover are recorded in TODO task 6: cffi's default callback returns
> 0 (== continue) when a sink raises, and "released on return" does not reach a
> memoryview *slice*.

## 8. Communication ledger — everything cdda2img must be told

They are pinned to a snapshot fork, so this accumulates and goes in **one**
message when the rewrite lands. Do not dribble it out.

| # | change | status | breaks a parser? |
|---|---|---|---|
| 1 | `toc`: `pregaps=` → `subq_indices=` | **DONE 2026-07-25** | No — nothing consumed it; they derive pregaps from their own subchannel decode (`subchannel.py:673`, `subq_toc.py:106-139`) |
| 2a | `pregaps`: rows that decoded before a failed boundary are now **printed** rather than discarded, so **exit 2 can carry usable stdout** | **DONE** `2f9ce4e` | **Yes, additive** — see below |
| 2b | `cxscan`: the TOC read now precedes arming, so sample 1 no longer includes its own lead-in traffic | **DONE** `c78fd33` | No — same columns; sample 1's counters are lower and more honest |
| 2c | Everything else through the refactor | **no change**, verified byte-identical for `info disc media features toc fulltoc text scan` | No — that is the phase-0 test's job |
| 3 | Guard 4.1/4.2 | CLI already interlocks both; no CLI-visible change | No |
| 3b | Five subcommands (`pregaps`, `c2lag`, `media`, `write`, `disc`) could always exit **3**; the machine-interface doc did not say so | **DONE 2026-07-26** `2618d23` — documented, plus `tests/exit_codes.sh` | No behaviour change — but the **contract** changed |
| 4 | Bindings availability + ABI policy | §7.1 **resolved and landed 2026-07-26**: `size` field on `read_req`/`read_stats`, `ACCUDISC_ERR_ABI`, version single-sourced | No — library ABI only; they use the binary |
| 5 | An unknown command exits **2**, not 1 | pinned by `cli_surface.sh`, unchanged | No — but see below |
| 6 | `speeds --sweep`: appends `min=`/`max=` per rung; `accudisc_speed_rung` grows 6 → 10 bytes and `accudisc_probe_speed_ladder` takes a new `points` argument | **DONE 2026-07-28** | No — keys are appended, `measured=` unchanged (see below) |
| 6b | `speeds --sweep` also appends `verdict=` per rung and a `ladder admitted=` line; `accudisc_speed_rung` grows 10 → 14 bytes (`equiv_x`, `verdict`) | **DONE 2026-07-28** | No — additive, and both are absent without `--sweep`. **But it makes their `drive_speed.admitted_ladder` redundant** |
| 7 | `accudisc_write_opts` gains a leading `uint32_t size` + `ACCUDISC_WRITE_OPTS_INIT`; `accudisc_write` returns `ACCUDISC_ERR_ABI` when it is 0 or unhonourable | **DONE 2026-07-29** | No — library ABI only; the CLI is rebuilt with it. **But it breaks any already-compiled caller, deliberately — see below** |
| 8 | **Version 0.2.0 → 0.3.0**, and the Python binding's skew check tightened from `major.minor` to the full triple | **DONE 2026-07-29** | No — but they **pin on `accudisc_version_string()`**, so the number they hold moves |
| 9 | `accudisc_read_req` gains `uint8_t *subq_map` (56 → **64** bytes) and the header gains `ACCUDISC_SUBQ_*`; new CLI flag `--subq-map-file`; Python `read(subq_map=True)`, `ReadResult.subq_map`, `SubQState`, `subq_state()`. **Version 0.4.0 → 0.5.0** | **DONE 2026-08-08** | No — additive on every surface. The field is **last**, so a 0.4 caller's shorter struct zero-extends to NULL and behaves exactly as before (the IN rule, §7.1); the CLI flag is new; the binding keyword defaults off. Requires `sub=RAW`, else `ERR_INVAL` |

| 10 | **The SpeedRead subchannel guard is GONE.** Removed: `accudisc_read_req.allow_unsafe`, `ACCUDISC_ERR_UNSAFE_COMBINATION` (**-11, retired, never to be reused**), the library refusal, the CLI's `--uncap`+`--sub` interlock and its pre-read warning, and the Python `UnsafeCombination` class and `allow_unsafe=` keyword. **Version 0.5.0 → 0.6.0** | **DONE 2026-08-09** | **Yes, for a compiled caller — this is our first SUBTRACTIVE change.** `sizeof(accudisc_read_req)` is unchanged at 64 (the byte was padding), so a stale caller setting `allow_unsafe = 1` writes into padding and is ignored — benign, but by luck of layout, not by the `size` rule, which only protects a caller that is SHORT. A Python consumer catching `accudisc.UnsafeCombination` gets `AttributeError` at import. Neither applies to cdda2img: their tree references neither name (grepped 2026-08-09) |

| 11 | `accudisc_read_stats` gains `speed_requested_x` / `speed_honoured_x` (136 → **144** bytes): the pass speed the drive ACTUALLY adopted, so a quantized `--speed` (16× → 8× on a PX-716A) is machine-detectable. Python: `ReadStats.speed_quantized` + `features` name `speed_honoured`. **Version 0.6.0 → 0.7.0** | **DONE 2026-08-09** | No — additive at the END of an **OUT** struct, which is the safe direction: `adsc_abi_export` REFUSES a caller declaring more than we have and never truncates, so a 0.6 caller simply never sees the fields. Their CLI parsing is untouched |

**Row 11's trap is the zero, and any consumer reading these two fields must be
told about it.** `speed_honoured_x == 0` means **no answer** — nothing was
requested, or the set failed, or page 2A did not read back. It does *not* mean
"ran at 0×", and it is *not* evidence the request was honoured. So the test is
`honoured && honoured < requested`, never a bare `<`: a bare comparison makes a
missing answer report as quantized (`0 < 16`), and the opposite phrasing makes it
report as fine. Both are wrong, in opposite directions, from the same zero. The
header states the four states explicitly and the Python property implements
exactly that test; anything hand-rolled will eventually disagree with the CLI.

Scope worth stating too: this is the **pass** speed only. `speed_ladder` moves
the speed mid-read for recovery rungs and is not covered — that question is
`accudisc_probe_speed_ladder`'s `ACCUDISC_RUNG_QUANTIZED`, asked per rung,
before the read.

**Row 10 is the one row here that the `size` field cannot make safe, and it is
worth saying why in the ledger rather than only in the header.** Every previous
change was additive: a field appended at the end, where an older caller's
shorter struct zero-extends and behaves as it always did. The IN rule covers
that completely. It does not cover *removal* — a caller that sets a field this
build no longer has is not short, it is wrong, and there is no length to check.
Here the consequence is harmless because the removed byte was padding, so
nothing else moved into its offset. Had it been a real field, a stale caller's
`1` would have landed in whatever succeeded it. **The lesson to carry: check
whether a removed field was padding before assuming a removal is free, and never
generalise from this row.** Keith ruled the guard out on 2026-08-09 — the drive
governs CD-DA to 40× regardless of the uncap, and page 2A reports the request
rather than the governed rate, so the state the guard defended against is one
the hardware cannot enter.

**Row 8 is the correction of a rule this document stated and we then did not
follow.** §8's own text says a layout change means "bumping
`ACCUDISC_VERSION_MINOR` so the .so version moves with the layout". Between rows
6, 6b and 7, `accudisc_speed_rung` went 6 → 10 → 14 bytes and
`accudisc_write_opts` gained a field — three layout changes, all inside 0.2.0,
none of them bumped. A binding compiled against any of those and loaded against
any other compares `0.2` to `0.2` and finds them equal.

cdda2img found it (§113.2) as a live hazard rather than a theoretical one: a
stale `_accudisc.abi3.so` from 2026-07-27 was the only extension loadable on
their Python 3.10 venv, and they were about to `pip install` it against a
library rebuilt three times that morning. Two fixes, and they are not
alternatives:

- **The bump**, which makes the *existing* check fire for anyone holding 0.2.
- **The tightening** to a full-triple comparison, so a future patch-level layout
  change is caught too.

Doing only the second would be the same defect one digit further right: a check
of any granularity is worth exactly what the discipline of bumping is worth.
Which is also the argument for why neither replaces the per-struct `size`
guards — those hold whatever the version says, and cdda2img made the same point
from their side: *"a version check at major.minor granularity is not a
substitute for per-struct size guards, it is a coarser thing that looks like
one."*

**Row 6 is an ABI break taken knowingly, and the last free one on that
struct.** `accudisc_speed_rung` gained `min_cx`/`max_cx` without a `size`
field: a per-element size on an OUT *array* would mean trusting N separate
caller claims, which is not what §7.1's OUT rule says, and §7.1 warns
against widening the guarded set. It was free because nothing outside this
repo linked the library and the probe was still unbound in the Python
binding — held unbound for exactly this. **Binding the probe closes it.**

The parser answer is No, and the reason matters more than the answer:
`measured=` still reports a single band — the *middle* one under `--sweep`.
It would have been easy to make it the mean of three, which every existing
parser would have kept reading without complaint while the quantity changed
underneath. `min`/`max` are also **per line, optional**: a rung whose bands
did not all measure omits them rather than printing `0.00`.

**Row 7 breaks compiled callers on purpose, and the mechanism is worth stating
because it looks like the opposite.** `size` landed in existing padding, so
`sizeof(accudisc_write_opts)` did **not** change — it is 24 bytes before and
after. That is not source compatibility bought for free. A caller built against
the previous header passes 24 bytes whose first four are `simulate`, so the
library reads a size of 0 or 1, and both are below `sizeof(uint32_t)` and
refused. Every stale caller gets `ACCUDISC_ERR_ABI` on its first call rather
than a burn from misaligned fields, which is the intended outcome; `tests/
test_abi.c` drives both values explicitly rather than leaving it to be inferred
from the layout.

Raised by cdda2img (§109.2, restated as a blocker in §111.2a) while auditing
which structs carry the §7.1 guard. Their argument is the one built to: without
it, a future field addition does not raise `AbiMismatch` and degrade them to the
subprocess — it produces a well-formed call about the wrong bytes on the one
operation in the system that is not idempotent.

Anything that lands in this table as "breaks a parser: Yes" needs a decision,
not just a note.

**Row 2a is that case, and the decision is to keep it.** `cmd_pregaps` used to
discard the whole scan when a boundary *read* failed; it now prints the rows
that decoded first, flushes stdout, and still exits **2** via `fail_dev`. So a
consumer that treats exit 2 as "assume no output" will now discard valid rows,
and one that treats a row count as a total will under-report. The rows were
always correct — they were being thrown away — so suppressing them again to
protect a parser would mean withholding good data to preserve an accident.

What the ledger owes cdda2img is the *pairing*, and it is the part that is easy
to get wrong: **this is exit 2, not exit 3.** Exit 3 remains the separate
`pregap_state == UNKNOWN` condition, where the scan completed and a boundary was
undecodable. Exit 2 now means "these rows, and the scan stopped". A row count
from a nonzero exit is a floor. Now stated in `cli-machine-interface.md`, which
is the document a binding actually reads.

**Row 3b is not a behaviour change and still belongs here.** Nothing about the
binary moved; what moved is what we *promise*. A wrapper that treats any
undocumented exit 3 as a hard error has been mishandling four commands since
they were written.

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

**Adopted by both projects 2026-07-25.** Wrap every `/dev/sr0` command in
`flock /var/tmp/sr0.lock -c '…'`, taking the lock for a whole long job rather
than per-command, and write who/what/ETA to `/var/tmp/sr0.owner` for anything
over a minute. Advisory, so it only works because both sides honour it — which
they did on its first real contention: both hit an empty tray, read each
other's owner note, no collision.

**We then over-reacted, and were corrected by data — worth recording because
the correction is the useful part.** Reasoning that contention fakes exactly
the "irreducible Q" signature in RECOVERY.md's two-lever model, we proposed
caveating those figures as being of unknown provenance. cdda2img audited all 41
baseline Q measurements instead of agreeing: **every repeated (disc, speed)
cell reproduces to within 0.04 pp** except the one known-contended run, with
cross-session replicates four hours apart holding ±0.04 pp. A contention
artefact does not reproduce to four significant figures. So the historical
figures stand, and the audit *supports* the static-Q population rather than
undermining it.

The lesson generalises: **caveating data you have positive evidence for is not
a free safety measure — it devalues good measurements.** Ask for the corpus
before flagging. What landed in `RECOVERY.md` §12 is therefore a methodology
note recording the hazard and its one measured magnitude, not a caveat on the
numbers — carrying an explicit scope limit that the audit bounds *large*
contention only and cannot exclude sub-percent effects.

## 9. Open questions — **RESOLVED 2026-07-25**

### 9.1 — How does guard 4.1 detect the uncap without a driver?

**Both, keyed — but they answer with different authority, and the design must
not flatten that.** Three sources, consulted in order:

| # | source | authority | yields |
|---|---|---|---|
| 1 | this handle called `accudisc_speed_uncap_set(dev, 1)` | certain | `ON` |
| 2 | driver attached → `speed_uncap_get` | certain | `ON` / `OFF` |
| 3 | no driver → INQUIRY + mode page 2A `max_x` vs a per-model stock ceiling | inference | `LIKELY_ON` / `OFF` / `UNKNOWN` |

```c
typedef enum {
    ACCUDISC_UNCAP_OFF = 0,   /* authoritative */
    ACCUDISC_UNCAP_ON,        /* authoritative */
    ACCUDISC_UNCAP_LIKELY_ON, /* max_x above this model's verified stock ceiling */
    ACCUDISC_UNCAP_UNKNOWN,   /* no driver, model not in table, or query failed */
} accudisc_uncap_state;

ACCUDISC_API int accudisc_speed_uncap_probe(accudisc_device *dev,
                                            accudisc_uncap_state *state,
                                            unsigned *max_x);
```

Source 1 needs a tri-state field on `struct accudisc_device`, exactly like the
existing `dev->streaming` (`src/internal.h:19-21`). It closes the commonest real
path — a caller that sets the uncap itself — with **certainty and no heuristic
at all**, which is the case source 3 was being asked to cover and should not be.

**Source 3 must key the threshold on the model, not hardcode 40×.** The CLI's
current `mk / 176 > 40` (`cli/main.c:1518`) is one drive's stock ceiling
promoted to a universal constant: `FEATURES.md:16` verifies the 40× → 48×
transition on the **PX-716A specifically**, and we own one drive. On any Plextor
whose stock CD read ceiling exceeds 40×, a bare `> 40` fires with SpeedRead off.
Comparing against `stock_read_x(product)` instead collapses that false-positive
population to "models not in the table", which resolve `UNKNOWN` and are
therefore *reported as unknown* rather than silently misjudged.

The table lives in `src/drive/` — factual hardware data, the same class as read
offsets, which CLAUDE.md explicitly permits in core. **Entry rule: a row exists
only where the uncap transition has been verified in both directions on that
model.** That rule is what makes "`max_x` at stock ⇒ `OFF`" sound rather than
hopeful. One row today (PX-716A: stock 40×, uncapped 48×); everything else
`UNKNOWN`. A one-row table that admits its ignorance beats a bare `> 40`
pretending to be general.

Verified while resolving this: `accudisc_get_speed` issues a fresh MODE SENSE(10)
on every call (`src/device.c:244`) — nothing is cached at open — so the probe
does see an uncap that a prior session left on after our handle was created,
which is the entire reason source 3 exists.

### 9.2 — Refuse or warn? — **superseded: neither, as of 0.6.0**

> The answer this section reached was "refuse on certainty, report on
> inference". The refusal is gone (see §4's banner) and so is the CLI's warning
> on `LIKELY_ON`. What survives is the **detection**, which callers still query
> through `accudisc_speed_uncap_probe` — so the three-source split and the
> authoritative-vs-inferred distinction below remain accurate. Only the policy
> built on top of them was removed.

**Resolved by the split above: refuse on certainty, report on inference.**

`accudisc_read_cdda` returns `ACCUDISC_ERR_UNSAFE_COMBINATION` when
`sub != ACCUDISC_SUB_NONE` and the state is **authoritatively** `ON` (sources 1–2),
unless the caller sets the opt-out field in `accudisc_read_req`. On `LIKELY_ON`
and `UNKNOWN` it proceeds.

Refusing on `LIKELY_ON` would convert a silent-bad-data risk into a hard
inability to read subchannel at all on an unrecognised drive, with no diagnosis
available to the caller — a worse failure, and one we cannot justify from a
one-row table. Proceeding while making the state queryable hands the caller the
fact and lets it set policy, which is where §3 already puts policy and human
diagnostics.

**State the residual hole rather than papering over it: on `LIKELY_ON` the
library does not refuse. The hole is reported, not closed.** The application
closes it — the CLI keeps its hard refusal on *intent* (`cli/main.c:1502`, which
needs no probe: the request itself is contradictory) and its warning on *state*
(`cli/main.c:1513-1526`), with the latter re-sourced from the probe instead of
its own inline `drive_identify` + `get_speed`.

**The advisory channel is the probe, not `accudisc_read_stats`.** The CLI needs
this *before* the read — it warns, then reads anyway — and `read_stats` only
exists afterwards. One pre-flight function, matching §5.2's pure-function
preference.

### 9.3 — Does `accudisc_plan_read_range` own `--force`?

**Yes**, via `spec.force`, as proposed. Every consumer then gets the same
override with the same meaning, and the override is visible in the plan rather
than applied behind it.

### 9.4 — Is `subq_indices` the right final name?

**Yes, locked.** `subq_indices=none|scanned` still reads correctly against a
future scanned value, and the fork is pinned so it stays cheap to revisit.

### 9.5 — What this costs the phase-1 framing

The resolution adds public surface §4 did not anticipate: one enum, one probe
function, one `accudisc_read_req` field. That is defensible — the probe is
precisely what makes the §9.1 hole reportable instead of hidden — but §4.1 has
been updated to match, so the phase description and the design do not diverge.
Still no hardware needed to write; hardware confirmation waits on the drive.

## 10. Effort estimate

Grounded in this project's measured velocity: 116 commits over 16 calendar days
(13 active), ~17,900 lines across `src/ include/ cli/ tests/ drivers/` —
roughly 1,100 lines/active day including tests and docs. Closest comparable is
the DAO write engine: **+746 lines in one day**, 6 commits, covering write
params, disc-state check, cue-sheet builder, `.toc` parser and full engine.

| piece | estimate |
|---|---|
| Phase 0 seatbelt | ~150–200 lines |
| Phase 1 guards + tests | ~180–240 lines (was ~80–120 before §9.1 added the probe, the `accudisc_uncap_state` enum and the stock-ceiling table) |
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
phase 1   guards 4.1, 4.2 + uncap probe (§9.1)   DONE 2026-07-25, unconfirmed
          on hardware (CLI rewired: its inline identify+get_speed heuristic
          now re-sources from the probe)
phase 2   COMPLETE 2026-07-25. All four rewired the CLI in the same commit.
          5.4 uncap push/pop   fcbfaef
          5.3 census cadence   c78fd33  (+1 behaviour change: TOC read now
                               precedes arming, so sample 1 excludes its
                               lead-in traffic)
          5.2 range resolution 3c68971  (pure; needed no media after all —
                               found a silent wrong-extent bug where --tracks
                               and --session both wrote start)
          5.1 pregap scan      2f9ce4e  (+1 behaviour change: rows that
                               decoded before a failed boundary are now
                               printed rather than discarded)
phase 2b  DONE 2026-07-26  2618d23
          exit-code/semantic mapping table + tests/exit_codes.sh
          fix CLAUDE.md:56 "thin layer" (see §12)
phase 3   DONE 2026-07-26.  §7.1-7.3 resolved; §8 sent.
          §7.1 carried CODE, not just a decision — the `size` field is
          implemented now because the thing that makes the break free
          (nothing outside this repo links the library) expires when the
          Python binding ships.  §7.2/§7.3 are decisions only.
          Also fixed here: the header/CMake version drift that made any
          "rebuild on a bump" policy unenforceable.
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
