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

---

## A. Would share ONE CD-RW (re-usable — do these first)

### A1. Does `--simulate` pace the medium, or only the host?  `[HIGH VALUE]`

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

### A2. The simulate/real lead-in discrepancy — 13.2 s vs 8.2 s  `[MEDIUM]`

Already recorded in RECORDING_PLAN.md §9 and NOT yet tested. Simulate holds off
13.2 s at LBA -150; the one real burn we have held off 8.2 s. Hypothesis:
`burn.c` skips SEND OPC in simulate, so a real burn does its power calibration
*before* the first write and a simulate pays it *at* the first write.

**What live settles.** One real burn with the flow tally captured. If the
hypothesis holds, the live settle is ~8 s regardless of speed — which would ALSO
answer A1's second limb, since the speed-scaling settle would then be a simulate
artefact. **Run A1 and A2 on the same disc: A2 is A1's control.**

**Cost:** shares A1's disc.

### A3. Does the FIFO's ride-through match its label under real timing? `[MEDIUM]`

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

**Cost:** shares A1's disc — it is the same ladder with one extra variable.

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

**Cost:** shares A1's disc — poll NWA alongside the existing buffer poll during
whichever burn runs there, and compare the trace against the simulate.

---

## C. Explicitly NOT queued, with reasons

- **Anything the drive already answers three ways.** B2 above.
- **Re-running arm 0/1/2 of the FIFO starvation suite.** The policy question
  ("does it stop, or defer?") is answered and the answer does not depend on the
  laser. Only B1 — does the failover produce good audio — needs media.
- **The write-offset measurement.** Already done live on 2026-08-27,
  `-30` confirmed end to end.

---

## When the blanks arrive

1. **CD-RW first**, A1 + A2 together on one disc — they are each other's
   control, and it is re-usable if a rung needs repeating.
2. **Then B1 on a CD-R**, because it is the only entry whose value is the
   physical disc rather than a timing number.
3. Re-check this file against `docs/reference/TODO.md` before starting: several
   entries here were opened by defects found the same day, and a fix landing in
   the meantime may have moved the question.

**Every run: verify `kind=BLANK disc_status=0` before, and record what the disc
reads as after. State how many blanks remain in the commit message.**
