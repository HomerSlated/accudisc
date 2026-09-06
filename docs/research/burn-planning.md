# Planning a burn: choosing the speed and the ring from measurement

**Drive:** Plextor PX-716A, firmware 1.11, USB.
**Media:** Ritek CD-R, ATIP 97:15:17 / 79:59:70.
**Image:** 204 143 sectors (45:21), 480 144 336 bytes.
**Measured:** 2026-09-06, eleven real burns and one simulated one.

The question this answers: *given a machine and a drive nobody has characterised
in advance, what write speed and what FIFO size give the fastest safe burn — with
no BURN-Proof to fall back on?*

Everything below is arithmetic on quantities the tool can measure at run time.
None of it needs a database of drives.

---

## 1. Why the obvious answer is wrong

The intuitive rule is "pick a speed the source can sustain and skip the buffer".
Both halves fail.

**Sustained bandwidth is the wrong criterion.** What kills a burn with no
failover is a *stall*, not a shortfall. Without a ring, the only thing between a
stalled host and a coaster is the drive's own buffer:

| drive buffer | 1x | 2x | 4x | 8x | 16x | 48x |
|---|---:|---:|---:|---:|---:|---:|
| 512 KB (early-90s writer) | 2.97 | 1.49 | 0.74 | 0.37 | 0.19 | 0.06 |
| 2 MB (mid-90s) | 11.89 | 5.94 | 2.97 | 1.49 | 0.74 | 0.25 |
| 4 802 784 B (this PX-716A) | 27.23 | 13.61 | 6.81 | 3.40 | 1.70 | 0.57 |

*(seconds of tolerable host stall)*

No general-purpose OS offers a sub-second latency guarantee, and this machine has
twice been driven into a multi-second stall merely by filling a tmpfs.

**Choosing a low speed does not remove the need for a ring — it makes one cheap.**
Five seconds of extra ride-through costs 0.88 MB at 1x, 3.53 MB at 4x, 14.11 MB
at 16x, 42.34 MB at 48x. Speed and buffer are complements: a low rung is where
seconds are affordable, not where they are unnecessary.

**And the historical case is the memory limit, not the bandwidth limit.** A stock
A1200's internal IDE sustains ~1.7 MB/s = 9.64x, which is ample to feed a 4x or
8x burn. What it cannot do is spare 3.5 MB of a 2 MB machine for a ring. At 1x a
4.8 MB drive buffer already gives 27 seconds of slack, which is why 1x burning
worked with no host buffer at all. Speed was traded for latency tolerance.

---

## 2. The write-side CAV profile, and that a test write measures it

**Page 2A reports the speed that was requested, not the one delivered** — the
standing rule in this project, and it applies to writing as it does to reading
(`docs/research/cav-read-speed-geometry.md` is the read-side counterpart).

Measured over 90 telemetry windows across two real 48x burns (discs 1 and 2):

```
x(LBA) = 21.349 + 8.8139e-05 · LBA      21.35x at the hub, 39.34x at the rim
```

Integrated over this 45:21 image that is a **29.44x mean at a nominal 48x**. The
consequence matters: a source delivering 30x can feed a "48x" burn outright,
while the nominal figure says it needs 48x. Planning against the rung over-buys by
60%.

### The profile can be measured without spending a disc

A simulated burn (MMC test write: laser at read power, nothing committed) run in
disc 1's exact configuration:

| LBA | real | simulated | diff |
|---:|---:|---:|---:|
| 3 159 | 21.06x | 21.16x | +0.10 |
| 49 133 | 25.88x | 25.91x | +0.03 |
| 109 856 | 31.42x | 31.45x | +0.03 |
| 171 180 | 36.18x | 36.22x | +0.04 |
| 199 263 | 38.14x | 38.19x | +0.05 |

```
real       x = 21.349 + 8.8139e-05 · LBA
simulated  x = 21.379 + 8.8130e-05 · LBA
```

Slopes agree to **0.01%**, mean delivered rate to **0.14%**, and the disc read
back `kind=BLANK` afterwards and was reused.

**Three limits on that result, all from the same run:**

1. The simulate was faster at **all eight** sample points, not randomly — a real
   +0.14% bias, and in the unsafe direction (calibrating on it slightly
   *over*-estimates the drive, so slightly *under*-sizes the ring). Carry margin;
   do not correct it away.
2. **The settle does not transfer.** The drive held off its first write for
   13 320 ms / 333 retries in the simulate against 8 560 ms / 214 real. Only the
   payload rate is transferable.
3. One drive, one media type, one rung, one image length.

### Test write cannot be assumed available

`cdrecord` ships `cdr_simul` and `dvd_simul`, pseudo-drivers whose documented
purpose is *"timing tests for drives that do not support the `-dummy` option"* —
the reference implementation carries a software drive emulation precisely because
writers lacking test-write were common.

**Gate on the feature, per media, not on the drive.** MMC exposes the bit in
three places that can disagree:

- mode page 2A byte 3 bit 2 — the drive's general claim
- **GET CONFIGURATION feature 002Eh (CD Mastering / SAO)** — what DAO uses
- feature 002Dh (CD Track at Once) — a separate bit with a separate answer

On this drive with a CD-R loaded, 002Eh reports `Test write=1, BUF=1,
SAO=1`, `current=1`. The descriptor is media-dependent, and that is not a
technicality: **DVD+R carries no test-write bit at all**, while DVD-R/-RW (002Fh)
does. "Does this drive simulate?" is not a well-formed question.

**When the bit is 0**, fall back to the *nominal* rung rate with no CAV discount.
That over-sizes the ring and never under-sizes it. Do not fall back to a real
burn: buying a coaster to discover how fast you can burn is the outcome the
feature exists to prevent.

---

## 3. The sizing formula

```
B(v) = max over t [ W_cum(t; v) − R·t ]        peak cumulative deficit
     + max(0, σ · W_peak(v) − D)               worst-case stall, less the drive's buffer

choose v minimising   B(v)/R + T_write(v)      subject to B(v) ≤ M
```

| term | meaning | measured how |
|---|---|---|
| `S` | image bytes | the TOC |
| `W(l; v)` | delivered rate vs LBA = `min(CAV(l), v)` | one simulated burn, **cached per (drive, media class)** |
| `R` | sustained source read rate | short cold calibration read |
| `σ` | worst-case source stall | latency tail of that same read |
| `D` | drive buffer | `READ BUFFER CAPACITY` (0x5C) — **4 802 784 B here, not page 2A's 8 MB claim** |
| `v` | candidate rungs | page 2A write-speed descriptors, verified by SET CD SPEED + read-back |
| `M` | lockable budget | `MemAvailable` ∩ `RLIMIT_MEMLOCK` × policy |

The deficit term is a **running maximum**, not an end-point difference. With CAV
the ring can drain across the outer radius and refill across the inner one, so the
minimum occupancy is not necessarily at the end.

**The simulation is amortised, not per-burn.** `CAV(l)` is calibrated once per
drive and media class and then integrated for any image length and any rung. That
is what makes this a production feature rather than a multi-minute tax on every
burn.

### The objective is total time, not the fastest rung

| source | best rung | ring | delay | write | total | next rung up |
|---|---|---:|---:|---:|---:|---|
| 3.90x | 4x | 12.0 MB | 17 s | 698 s | **715 s** | 8x → 246 MB, 1056 s (slower) |
| 8x | 8x | 0 | 0 | 340 s | **340 s** | 16x → 241 MB, 511 s (slower) |
| 16x | 16x | 0.8 MB | 0.3 s | 170 s | **170 s** | 32x → 216 MB, 247 s (slower) |
| 24x | **32x** | 80.5 MB | 19 s | 113 s | **132 s** | 16x → 170 s (slower) |
| 30x | **48x** | 9.1 MB | 1.7 s | 91 s | **92 s** | — |

The 24x row is why the rule cannot be "never outrun the source": there, exceeding
it wins. The planner has to evaluate rather than apply a heuristic.

**The cliff is the design's whole shape.** Between a 24x and a 30x source the 48x
ring falls from 97.7 MB to 9.1 MB, because you cross `R = W̄(v)` and the deficit
term collapses to zero, leaving only the stall term. Which means the valuable
output of the planner is not a buffer size — it is **the speed**. The ring is the
cheap consequence of choosing `v` well, and a ruinously expensive compensation for
choosing it badly.

---

## 4. What this cannot do

- **`σ` is unmeasurable.** A p99.9 sampled over a calibration read is a sample;
  the tail that ruins a burn is the one that was not in it. This belongs in the
  interface as a confidence setting with an honest label, never as a constant
  presented as a guarantee.
- **A cold-calibrated curve overstates a hot drive.** Measured the same day: two
  identical starved 48x burns differed by 22.7 s over 693 s depending only on
  whether the drive had rested 46 minutes or 58 seconds, with the hot one showing
  six multi-second stalls the cold one did not have. A planner calibrated on a
  rested drive and applied to the fourth disc of a session will under-size.
- **It says nothing about burn quality**, only about not underrunning. Whether a
  given speed writes *better* marks is a separate question and needs real media.

## 5. Implementation sketch

A planning call that reports and does not act:

```
accudisc_plan_burn(dev, toc, source_fd, opts) -> {
    speed_x, fifo_bytes,
    expected_prefill_s, expected_write_s,
    measured_source_bps, measured_stall_p999_s,
    cav_source: MEASURED | CACHED | NOMINAL_FALLBACK,
    confidence
}
```

Gate order, matching the house rule of never inferring a setting from page 2A:

1. `READ BUFFER CAPACITY` for `D`; page 2A + SET CD SPEED read-back for the rungs.
2. `GET CONFIGURATION` 002Eh for the test-write bit **on the loaded media**.
3. `CAV(l)` from cache, else one simulated burn, else the nominal fallback —
   and report *which*, because a plan built on a nominal fallback is a different
   object from one built on a measurement.
4. Calibrate `R` and `σ` on a cold read of the source.
5. Evaluate every rung; return the one minimising total time within `M`.

The caller decides. As everywhere else here, the library reports and the
application chooses.
