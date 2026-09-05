# Live-burn queue — what simulate cannot answer

**Opened 2026-08-28 at Keith's instruction.** Blanks ordered (CD-R plus one
CD-RW), expected the week of 2026-08-31. Until they arrive every burn here is
`--simulate`, laser off.

## Why this file exists

`--simulate` runs the whole host and command path with the laser off. It has
found real defects that no device-free test could reach — a drive left mid-DAO,
a write speed that reached nobody, a FIFO that starved. But there is a class of
question it *cannot* answer, and the failure mode is quiet: a simulate run
returns 0 and the caller cannot tell whether the thing it was testing happened.

Every entry below states **what a live burn would settle that simulate cannot**,
and how many blanks it costs. Anything that does not need a blank is not here.

**Entries are ordered by what one blank buys.** Several can share a disc, and
that is called out — the CD-RW in particular is re-usable, so anything that only
needs *a burn to have occurred* should go there first.

> **BUT A CD-RW IS NOT UNLIMITED, and reading this line as though it were cost us
> one on 2026-09-03.** Budget it in **write operations**, not in discs. 15 burns +
> 14 erases in one hour ended in `3/02/00` MEDIUM ERROR (no seek complete) on
> spin-up and `cdrecord: OPC failed` — the disc unwritable, the drive unharmed.
> Full account: `private/research/incoming/2026-09-03-first-cd-rw-live-burns.md` §9.
>
> **The MECHANISM stated here until 2026-09-04 — PCA/OPC exhaustion — is
> REFUTED**, from primary sources: ECMA-395 §5.3 gives each PCA 100 test
> partitions and a disc has two, against ~37 calibrations that night. The rules
> below are unaffected: they were derived from what the loop did, not from why
> the disc died. The surviving candidate is lead-in/PMA destruction, and it is
> **inference, not a finding**. See `TODO.md`, "CD-RW MEDIA SAFEGUARDS".
>
> **Three rules for any burn loop, all learned by breaking them:**
> 1. **Re-verify `disc_status=0` between iterations and ABORT on the first
>    anomaly.** One harness that night had this; its replacement dropped it, and
>    8 erases went into an already-dead disc.
> 2. **Never discard the erase tool's output.** It is the only record that
>    distinguishes "the erase failed" from "the erase broke it".
> 3. **Pace it.** Back-to-back ~70 s cycles for an hour is a thermal and
>    calibration load, not a free loop.
>
> **And when a drive misbehaves, swap the disc before diagnosing the drive.** A
> pressed CD costs nothing and separates medium from mechanism in one step; a
> whole evening was spent escalating against a healthy drive for want of it.

---

## A. Ran on the one CD-RW, 2026-09-03 — A1 and A2 ANSWERED

> **THERE WILL BE NO MORE CD-RW MEDIA** (Keith, 2026-09-03: *"I won't be wasting
> any more money on CD-RWs"*). **CD-R is a different position as of 2026-09-05:
> 50 blanks, JVC-branded Ritek — see section D.** Every CD-R is still one-shot,
> so the write-side half of the discipline below is unchanged; what no longer
> applies is cramming every question onto a single disc.
>
> The CD-RW this section was written for was consumed in one session — see the banner above and
> `private/research/incoming/2026-09-03-first-cd-rw-live-burns.md`.
>
> **Everything still open in this file now costs a CD-R and is one-shot.** The
> erase-and-repeat method is gone. Anything needing replicates must be designed
> to extract its answer from a SINGLE burn, or be dropped.

### A1. Does `--simulate` pace the medium, or only the host?  `[ANSWERED 2026-09-03]`

> **ANSWERED, and the premise below is corrected rather than confirmed.** The
> "~5x below request" reading was an artefact of burn LENGTH: a burn costs
> `fixed + n x rate`, and at 2400 sectors the fixed term is ~82% of it, so that
> column was mostly measuring overhead. Measured at two lengths (2400 and 9600
> sectors) so the fixed term cancels in the slope:
>
> | | marginal | fixed cost |
> |---|---|---|
> | simulate | 0.595 ms/sector (22.4x), both pairs | 6.42, 6.43 s |
> | live | 0.617 and 0.810 ms/sector (16.5-21.6x) | 12.9, 12.0 s |
>
> **The solid result: simulate understates the FIXED cost of a burn by 1.87-2.01x.**
> The streaming rate is bounded, not measured — live short-burn times came out
> bimodal (a 12.8-13.0 s cluster and a scattered 13.9-17.7 s one, unexplained),
> so the ratio is quoted as a range on purpose. **It is now unimprovable**: it
> would take ~10 more burns to tighten, and there is no cheap medium left.
>
> **Simulate is NOT pacing at a fifth of anything.** Its timing figures remain
> usable as between-rung comparators; they are not absolute rates, and they are
> optimistic about fixed cost by about 2x.
>
> **Also settled: on Ultra Speed CD-RW the requested speed is inert.** The drive
> pinned to the ATIP 2T high (24x) for every request from 4x to 48x, simulate and
> live alike (simulate payload spread 0.7% across the whole range). So the
> comparison this entry specifies — 4x against 32x — had no separation to measure
> on that medium at all. On CD-R the request IS live, so a future CD-R burn can
> still exercise it.

**The original question, as written 2026-08-28:**

**The question.** Today's write-speed ladder (`2026-08-28-speed-leak-and-48x.md`
§3) measured, in simulate, 2400 sectors with the engine's lead-in settle
subtracted: 4x = 38958 ms, 8x = 19659, 16x = 10039, 24x = 10021, 32x = 7690,
40x = 7711, 48x = 7683. Two facts do not fit a real burn:

  - the delivered rates are ~5x BELOW the request throughout;
  - the lead-in settle ALSO scales with requested speed (32.3 s at 4x against
    8.1 s at 32x), which no physical calibration should do.

Both are consistent with simulate pacing the *whole operation* at some fraction
of the requested rate rather than writing at it.

**What live settles.** Run the identical ladder for real and compare the shape.
If the live curve tracks the request while simulate sits at ~1/5 of it, then
every simulate timing figure in this repo is a *relative* comparator between
rungs and never an absolute rate — which is how they are currently labelled, and
this would confirm the labelling rather than change it. If the two agree,
simulate is a faithful timing proxy and a lot of future questions get cheaper.

**Cost:** 1 CD-RW, erased between rungs — or one CD-R if only two rungs are run
(4x and 32x are the widest separated pair that both sit on the write ladder).

**Do not infer this from cdrecord's numbers.** Its dummy run reported
`Last actual write speed: 25x` for a requested 48x, which says the DRIVE is
honest about dummy pacing; it does not say ours is, because we set the speed by
a different command path.

### A2. The simulate/real lead-in discrepancy — 13.2 s vs 8.2 s  `[REFUTED 2026-09-03]`

> **REFUTED. It was spin-up state, and the two figures were never in the same
> state.** `accudisc stop`, 12 s, then measure; then measure again immediately:
>
>     COLD  12320  12280  12360 ms      (spread 0.65%)
>     WARM   7400   7400   7400 ms      (identical to the millisecond)
>
> 4.9 s of spin-up. A2's 13.2 s sits on the cold value; its 8.2 s near the warm.
>
> **And with the state controlled, the ordering INVERTS**: warm simulate settle
> 7360-7520 ms against warm live 9960-10240 ms — live is 1.35x LONGER, where the
> SEND OPC hypothesis predicted simulate longer. The hypothesis does not survive
> in either direction.
>
> **A2's second limb is moot**: the settle does not scale with the requested speed
> at all (7360-7560 ms across 4x-48x), because the medium pinned the rate.
>
> **Warm settle repeats to 0 ms.** Any settle measurement that does not state the
> drive's spin state is uninterpretable — that is the durable lesson here.

**The original question, as written 2026-08-28:**

Already recorded in RECORDING_PLAN.md §9 and NOT yet tested. Simulate holds off
13.2 s at LBA -150; the one real burn we have held off 8.2 s. Hypothesis:
`burn.c` skips SEND OPC in simulate, so a real burn does its power calibration
*before* the first write and a simulate pays it *at* the first write.

**What live settles.** One real burn with the flow tally captured. If the
hypothesis holds, the live settle is ~8 s regardless of speed — which would ALSO
answer A1's second limb, since the speed-scaling settle would then be a simulate
artefact. **Run A1 and A2 on the same disc: A2 is A1's control.**

**Cost:** shares A1's disc.

### A3. Does the FIFO's ride-through match its label under real timing? `[OPEN — now CD-R only]`

> **Not run, and no longer shareable with a cheap disc.** No starvation ever
> occurred across 15 live burns: `starv=0`, minimum fill 98%, every time. Forcing
> one needs the rig, which **exists** at `/home/kgr/tmp/system/bin/slowdisk` (with
> `slowdisk-run` and `docs/slowdisk.md`) — the System agent's workspace, not
> either repo. An earlier search of this tree alone wrongly concluded it was gone.
>
> **Re-home it onto B1's CD-R.** B1 already burns a deliberately starved image and
> reads it back; A3 is the same burn with the FIFO tally captured. One disc, both
> answers. Do not spend a separate blank.

**The original question:**

0.29.0 made the ring report its true duration against the drive's stated rate
(4.95 s at 4x, 5.00 s at 16x, where it previously claimed 5.0 s over 1.02 s of
reality). Those figures are computed from page 2A, which echoes the request.

**What live settles.** Whether the *seconds of protection* the caller is promised
are the seconds they get, measured against a real drive consuming real data. The
label is only as good as the rate it divides by.

**Cost:** shares A1's disc if the ladder runs long enough to see a stall.

---

## B. Needs a CD-R (one-shot, the burn is the artefact)

### B1. BURN-Proof actually works  `[HIGH VALUE, DESTRUCTIVE BY DESIGN]`

**The question.** `accudisc_write_opts.burnproof` is reported as *claimed by the
drive*, never as tested — RECORDING_PLAN.md §9 makes that explicit and treats it
as a deliberate exception to this project's usual gate order. Today's arm 2 ran
846 FIFO starvations with BURN-Proof linked through all of them and finished
clean, at 96.6% of the source-rate ceiling. In simulate.

**What live settles.** Whether the link/relink leaves an audible or measurable
seam. This is the one entry where the DISC is the instrument: burn a starved
image for real, then read it back and compare against the source, sample for
sample, with the read offset applied.

**Cost:** 1 CD-R, destroyed (the point is that it may be a coaster). The rig is
already built and validated — `slowdisk` at `--rcdda 1` reproduces the
starvation on demand.

### B1b. Does POWEREC do anything on a real burn?  `[MEDIUM]`

Added 2026-08-28 (`private/research/incoming/2026-08-28-powerec.md` §2).

**What is already known.** POWEREC on/off makes no measurable difference to a
simulated write: 0.02–0.09% across 4x/16x/48x, against a 0.03–0.16% spread
between two runs of the *same* state. The null is bounded — anything above ~0.2%
would have shown.

**Why that is not the answer.** Three reasons simulate may be the wrong
instrument: the governor governs recording power and rate on real media, and
with the laser off there may be nothing to govern; cdrecord's dummy run had it
ON, recommending 48x, and delivered 25x, so something capped that path with the
governor active and we cannot tell from here whether it was the governor; and
our simulate ladder saturates at 32x anyway, so a governor that only ever
*lowers* a rate cannot be seen against a ceiling already below its choices.

**What live settles.** Run the same three-speed ladder for real with POWEREC on
and off. If the delivered rates differ, the governor is live on the write path
and the burn engine has a decision to make (cdrecord turns it off when forcing a
speed). If they do not, the simulate null generalises and the API stays a probe.

**Cost — RE-COSTED 2026-09-03, this basis is gone.** "Shares A1's disc" assumed a
re-usable CD-RW; there is none, and on that medium the ladder had **no spread to
modulate** (the drive pinned to 24x for every request). A POWEREC on/off
comparison there would have been two runs at the same rate. **It needs a CD-R,
where the request is live** — and it is a poor use of a one-shot disc unless it
rides along with B1.

### B2. Whether a 48x write request does anything on real media  `[LOW]`

The drive accepts 48x, page 2A echoes 48x, and cdrecord's dummy run delivered
25x with POWEREC recommending the full 48. Our own simulate saturates at 32x.
Three independent measurements agree that 48x is not delivered.

**What live settles.** Very little — it is already answered three ways. Listed
only so it is not re-asked. **Do not spend a blank on this.**

### B3. CD-Text packing against cdrdao's genuine output  `[BLOCKED ON A DECISION]`

RECORDING_PLAN.md §11.9: no virtual writer can produce the cdrdao oracle,
because a text round-trip destroys packing by construction. Getting cdrdao's
real packing needs a real burn to real media.

**Status: open pending a decision from Keith, not queued.** Do not assume into
it. Recorded here so the blank-arrival checklist does not silently skip it.

### A4. Does NEXT WRITABLE ADDRESS mean the same thing in simulate?  `[MEDIUM]`

Added 2026-08-28 while designing the write progress surface
(`private/research/incoming/2026-08-28-write-progress-surface.md` §2.1).

**The question.** MMC-5 §6.27.3.14 defines NWA as *the next user data block the
drive expects to receive* when streaming — the drive's own idea of where the
burn is, as against our host-side count of what we have handed over. It is the
one genuinely new signal available to a write progress bar, because it exposes
the in-flight depth that a buffered pipeline otherwise hides.

**What live settles.** Whether NWA advances the same way with the laser off. A
simulate does not commit anything to the medium, so "the next block the drive
expects" may be tracking the buffer rather than the disc — or may be frozen. If
it behaves differently, an NWA-derived depth measured in simulate is not
evidence about a real burn, and the field must be documented as such.

**Cost — RE-COSTED 2026-09-03. Needs CODE before it needs a disc**, and the disc
is now a CD-R. Verified by grep, not inferred from the CLI: there is **no `0x52`
READ TRACK INFORMATION and no NWA anywhere** in `src/` or `include/`. The burn
engine polls the drive buffer (`0x5C`) and nothing else. Write the poll first,
then ride it along with whichever CD-R burn happens next — never spend a blank on
this alone.

---

## D. DISC #1 — the clean reference burn `[DRAFTED 2026-09-05, AWAITING SIGN-OFF]`

> **MEDIA POSITION CHANGED 2026-09-05.** 50 CD-R blanks are now available (JVC
> branded, **Ritek** manufactured; ATIP `97:15:17`, lead-out `79:59:70`, read on
> hardware). The scarcity premise this file was rewritten under — one disc, every
> question crammed onto it — no longer holds. **The DESIGN RULE below still
> does**, in its important half: each disc is still one-shot, so everything
> write-side must be instrumented before the laser starts. What relaxes is only
> the "cram everything onto one disc" corollary. Keith's instruction with the
> stock was "use them responsibly", and this plan spends **one**.

### Why disc #1 must be a CLEAN burn, and why B1 cannot share it

The existing plan (step 2 of "When the blanks arrive") puts B1 — the deliberately
**starved** BURN-Proof burn — on the first CD-R, carrying A3 with it. That was
correct under one-disc scarcity. It is now the wrong first move, for a reason
that has nothing to do with cost:

**You cannot commission a new instrument against a deliberately broken specimen.**
`accudisc_verify` (0.35.0) has **zero hardware validation** — every result it has
ever produced came from a stub drive. Its first real run needs a disc whose
expected answer is *known*: zero differing samples. On a starved disc with
BURN-Proof link seams, a `result=differ` is ambiguous between "the seam is
audible in the data" and "the verify has a bug", and there is no third
measurement to break the tie. Disc #1 is therefore the **control**, and B1's
starved burn becomes disc #2 — where a difference *is* the finding, measured with
an instrument already known to work.

The same argument applies to the census: a starved disc's C1/C2 profile confounds
link seams with media quality, and nothing on that disc separates them.

### The organising principle: write-side is one-shot, read-side is forever

The CD-RW taught this file to treat every measurement as scarce. **A CD-R is
not** — it is a permanent artefact, and reading it does not consume it. That
splits the work cleanly, and getting the split right is most of what this plan
contributes:

| | must be instrumented BEFORE the laser starts | can be done AFTER, any time, repeatably |
|---|---|---|
| | write-health timing envelope | `accudisc_verify`, all three tiers |
| | FIFO tally (stderr log sink) | `cxscan` counter census |
| | drive buffer poll | Jitter/Beta (`0xEA` CDB[2]=`0x10`) |
| | BURN-Proof / POWEREC state | `0x51` byte 2, both fields |
| | speed request and what was delivered | the `bler == e11+e21+e31` identity |
| | NWA poll (**needs code — A4**) | the `uncr` byte-18 question |

**Item 2 (Jitter/Beta) therefore does NOT block this burn.** It is a read-side
scan of a finished disc, on the same `0x15`/`0x16`/`0x17` lifecycle as the error
counters. It can be built after disc #1 exists and run against it whenever it is
ready. That realisation is what makes a single disc sufficient — the earlier
assumption that jitter/beta had to be built first would have delayed the burn
behind an ABI bump for no reason.

### PRE-BURN BLOCKER — one code change, and it is not optional

**`accudisc write` does not report the write-health figures.** `cli/main.c` never
calls `accudisc_write_health_get`, and the envelope is **device-handle scoped**,
so the moment the process exits the settle and payload timings are gone. Burning
without this wired means the one write-side measurement 0.34.0 was built for is
silently lost, and the disc cannot be re-burnt to recover it.

Wire it, with a device-free test, **before** the burn. This is exactly the
"anything not instrumented before the laser starts is not measurable afterwards"
rule, caught by grep rather than after the fact.

**Verified as NOT blockers** (checked, not assumed):
- **A3's FIFO tally needs no code.** `burn.c:771` logs starvations, low-water
  slots and producer waits through `adsc_dev_log`, and `cli/main.c:2780`
  installs the stderr sink unconditionally. **Capture stderr and A3 is
  answered.** The earlier costing of A3 as needing the rig is about forcing a
  starvation, which disc #2 does; on a clean burn the tally is still the
  measurement, and `starv=0` on real media is a real result.
- **The census and verify need no code.** Both shipped in 0.35.0.

### The burn

**One disc. Full length, real audio, conservative speed, no starvation.**

| parameter | value | why |
|---|---|---|
| content | **real music or noise — NEVER digital silence** | `accudisc_verify` refuses to align on silence *by design* (it matches at every displacement). A silent test tone would produce `result=unaligned` and prove nothing. This falls straight out of the guard built today. |
| length | **full disc** (~74–80 min) | The outer radius is where write quality degrades first, and it is free to include. A short burn measures the easy part of the disc. |
| speed | **16x** | Deliberately mid-range, not minimum. A 48x-rated disc burned at 4x can be written *worse* than at 16x — the strategy is tuned for the middle of the range, and "slower is safer" is a CD-RW intuition that does not transfer. Stated as a recommendation; Keith's call. |
| BURN-Proof | default (auto) | Disc #1 is the control; leave the engine's normal choice in place. |
| starvation | **none** | That is disc #2. |
| `--simulate` | **off** | The whole point. |

**Keep the source `.bin` and `.toc`.** `accudisc verify --bin` needs the exact
source afterwards, and a regenerated file is not the same file.

### Order of operations

1. `flock /var/tmp/sr0.lock` for everything below — one drive, two agents.
2. **Before:** `accudisc disc` — require `kind=BLANK disc_status=0`. Record
   `accudisc media` (the ATIP) for *this* disc; 49 of the 50 are presumed the
   same spindle but unmeasured.
3. **Burn**, capturing **stdout and stderr both**, unbuffered, to a file. The
   FIFO tally and every `adsc_dev_log` line arrive on stderr and are not
   recoverable afterwards.
4. **Immediately after, same process where possible:** the write-health figures.
5. **After:** `accudisc disc`, plus a raw `0x51` read of **both** 2-bit fields of
   byte 2 — Disc Status and Last Session Status. They are separate fields and can
   disagree. CD-RW answered `0x1E` on 2026-09-03; **CD-R lead-out behaviour is
   unverified and this is the disc that settles it** (`OPCODES.md`, `0x5B` row).
6. **Read-side, in any order, repeatable:**
   - `accudisc verify --bin <source> --tier counters --driver auto` — the first
     hardware run of the 0.35.0 verify. **Expected: `aligned=1`,
     `differing=0`, `result=pass`.** A `shift_samples` near the drive's `-30`
     write offset plus `+30` read offset is the corroborating detail.
   - `accudisc cxscan --driver auto` — the census, and with it
     `bler_mismatch/bler_checked`. **This is the first time the
     `bler == e11+e21+e31` identity is evaluated on real hardware**;
     `bler_checked` has been 0 in every run so far.
   - Byte 18 (`uncr`) against byte 20 (`e32`) on a span with real error
     activity — `drivers/plextor/re-tools/cxdump.c` dumps all 26 bytes.
   - Jitter/Beta once item 2 is built.

### What each measurement settles

| question | where it was open | settled by |
|---|---|---|
| does `accudisc_verify` work on real media? | 0.35.0, stub-only | step 6, verify |
| `bler == e11+e21+e31` on real hardware | `OPCODES.md`, `bler_checked` = 0 | step 6, cxscan |
| what byte 18 (`uncr`) holds | `OPCODES.md`, open since QPxTool | step 6, cxdump |
| `0x51` byte 2 on a **CD-R** DAO burn | `OPCODES.md` `0x5B` row | step 5 |
| the timing envelope on real media | 0.34.0, never run live | step 4 |
| A3 — FIFO ride-through under real timing | this file, `[OPEN]` | step 3 stderr |
| jitter/beta on a known-good burn | item 2, unbuilt | step 6, later |

### What this disc does NOT answer, stated so it is not assumed

- **B1 (BURN-Proof under real starvation)** — needs a deliberately starved burn.
  **Disc #2.** Do not try to fold it in: see the commissioning argument above.
- **A3's forced-starvation half.** This disc measures the tally on a *clean*
  burn, which is a real result (`starv=0` on real media) but is not the
  ride-through test. That rides with disc #2.
- **B1b (POWEREC on/off)** — an on/off comparison is **two burns by
  construction**; it cannot be answered by any single disc. Discs #2 and #3, or
  dropped.
- **A4 (NWA)** — still needs `0x52` READ TRACK INFORMATION written first, and
  there is none in `src/` or `include/`. It cannot ride this disc.
- **The timing envelope is POPULATED, not VALIDATED.** The envelope compares each
  burn against the *first burn on the handle*, so one burn produces a baseline
  and nothing to test it against. Its anomaly detection cannot fire on disc #1
  by construction. What disc #1 buys is the first real-media settle and payload
  figures at a known speed and length. **Validating it needs a second burn**, and
  that is a legitimate use of disc #2 since the starved burn will have a timing
  signature of its own.

### Abort conditions

- `kind` is not `BLANK` or `disc_status` is not 0 → **stop**, do not force.
- Any sense other than the known-benign set during the burn → stop, keep the
  disc, read it as-is. A partial burn is evidence.
- **Never re-run the burn against the same disc.** CD-R is not erasable and there
  is no `blank=fast` path to reach for. This is the rule the CD-RW loop broke.
- If the burn fails, the next disc is a **repeat of disc #1**, not a move to
  disc #2 — the control has to exist before the experiment.

### Sign-off

**Not to be run until Keith signs this off.** Blanks: **50 before, 49 after.**
Record the ATIP of the disc actually used, and state the remaining count in the
commit message.

---

## C. Explicitly NOT queued, with reasons

- **Anything the drive already answers three ways.** B2 above.
- **Re-running arm 0/1/2 of the FIFO starvation suite.** The policy question
  ("does it stop, or defer?") is answered and the answer does not depend on the
  laser. Only B1 — does the failover produce good audio — needs media.
- **The write-offset measurement.** Already done live on 2026-08-27,
  `-30` confirmed end to end.

---

## When the blanks arrive — REWRITTEN 2026-09-03

Steps 1 and 2 below are spent: the CD-RW ran A1+A2 and did not survive. **What
remains is CD-R only, one-shot, and must be planned as such.**

1. ~~CD-RW first, A1 + A2 on one disc.~~ **DONE 2026-09-03.** A1 answered, A2
   refuted. The disc was consumed; there will be no replacement.
2. ~~B1 on a CD-R — now carrying A3 as well.~~ **SUPERSEDED 2026-09-05 by
   section D.** Two things changed: 50 blanks arrived, and 0.35.0 shipped an
   `accudisc_verify` with no hardware validation at all. Putting the
   *deliberately starved* burn first would commission a new instrument against a
   broken specimen — a `differ` result would be ambiguous between a link seam and
   a verify bug, with nothing to break the tie. **Disc #1 is now a clean control
   burn (section D); B1 + A3's forced starvation move to disc #2**, where a
   difference is the finding rather than a confound.
3. **Decide B1b and A4 before that burn, not after.** Both were costed as "shares
   A1's disc" and now have no home. A4 additionally needs code written first.
   Anything not instrumented before the laser starts is not measurable afterwards.
4. Re-check this file against `docs/reference/TODO.md` before starting: several
   entries here were opened by defects found the same day, and a fix landing in
   the meantime may have moved the question.

**DESIGN RULE, learned by losing the cheap medium:** with no re-usable disc, every
remaining question must be answerable from a **single burn**. Measure at two
lengths within one session where the quantity allows it (that is what separated
fixed cost from rate in A1); instrument everything you might want before starting;
and never plan an experiment whose precision depends on replicates you cannot
afford.

**Every run: verify `kind=BLANK disc_status=0` before, and record what the disc
reads as after. State how many blanks remain in the commit message.**
