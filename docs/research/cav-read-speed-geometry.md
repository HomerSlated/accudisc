# CAV read speed is a function of radius, and `speeds` measures it

**Drive:** Plextor PX-716 (panel read `PX-716UF`; the PX-716A is the same
mechanism on a different bus).
**Source:** PlexTools Professional XL V3.16, *Drive Information → CD Read*.
**Validated against:** AccuDisc 0.9.0, `accudisc speeds`, one sweep on a
36-minute CD-DA.

The short version: **a rung reading below its own requested speed is normally
geometry, not a fault.** Subtract the geometry and this drive's published curve
predicts all nine of AccuDisc's CAV band measurements to within 1.22%.

---

## 1. The published curve

PlexTools lists CD read speeds as a grid of *mode* columns. Transcribed:

| | Mode 1 | Mode 2 | Audio CD | CD-RW |
|---|---|---|---|---|
| | (\*)20–48X CAV | — | — | — |
| | 17–40X CAV | 17–40X CAV | 17–40X CAV | 17–40X CAV |
| | 14–32X CAV | 14–32X CAV | 14–32X CAV | 14–32X CAV |
| | 10–24X CAV | 10–24X CAV | 10–24X CAV | 10–24X CAV |
| | 8X CLV | 8X CLV | 8X CLV | 8X CLV |
| | 4X CLV | 4X CLV | 4X CLV | 4X CLV |

Its three notes, verbatim:

> AudioCD on CD-RW cannot be read at 17-40X.
> (\*)20-48X CAV can be reached by setting the SpeedRead option.
> The indicated max speed for CAV is achieved at address 68:00:00

Four things follow, in descending order of how much they change what we do.

### 1.1 Nominal speed is a rate at one radius, and the address is given

`68:00:00` is MSF; as an LBA it is `(68*60)*75 - 150 = 305850`. That is near the
outer edge of a full-length disc. Under CAV the disc turns at fixed angular
velocity, so the linear rate — and therefore the data rate — is proportional to
radius. "40X" is the rate **there** and nowhere else. A disc that ends before
that address cannot reach 40X at any setting, in any band, however healthy it
is. §2 turns this into a prediction.

### 1.2 SpeedRead's band is offered for Mode 1 only

The `20–48X CAV` row has an entry in the **Mode 1 column alone**; Mode 2, Audio
CD and CD-RW are blank on that row. So the vendor's own tool does not offer the
SpeedRead band for audio, while offering every other rung to all four columns.

This is independent confirmation of the 2026-08-09 ruling recorded in
`docs/reference/TODO.md` — *the drive is physically incapable of reading CD-DA
above 40x and the governor ignores SpeedRead for CD-DA entirely* — which until
now rested on our own A/B measurements and on Keith's reading of the hardware.
It also sharpens it. The ×1.2 RPM scaling in that item (`17–40x → 20–48x`) is
**real as a mechanism** and appears here as a published row; what is false is its
applicability to CD-DA. Mechanism confirmed, scope denied. That is a better
description than "the ×1.2 claim was wrong", and it is why the guards removed at
0.6.0/0.8.0 were correctly removed rather than removed by luck.

### 1.3 There is no 16X rung

The Audio CD column lists exactly {40, 32, 24, 8, 4}. No 16X entry appears
anywhere in the grid. AccuDisc's `req=16 page2a=8 verdict=quantized:8` is
therefore the drive reporting a rung that does not exist and rounding down to
the next one that does — `ACCUDISC_RUNG_QUANTIZED` reporting a documented
hardware fact rather than a refusal.

> Not inferred from the table's blank row. The grid has two blank rows, and the
> one below `4X CLV` cannot mean a missing rung below 4X, so the widget is
> drawing a fixed row count. The finding rests on the *absence of a 16X entry*,
> which is a property of the data; the blank cell is a property of the renderer.

### 1.4 The rungs split CLV from CAV at 8X, and media interacts

8X and 4X are CLV — flat, no radius term. Everything above is CAV. Separately,
the first note records an interaction the column headings cannot express: the
CD-RW column advertises 17–40X, but **audio on CD-RW** does not get it. Any
rung table derived from this grid is wrong for CD-RW unless it carries that
exception.

---

## 2. The model, and what it predicts

Two external inputs. Nothing is fitted to AccuDisc's measurements.

- **Red Book.** The program area starts at r₀ = 25.0 mm with constant track
  pitch, so an LBA counts *area*: `r(n) = √(r₀² + c·n)`.
- **The curve above.** Under CAV, `floor / nominal = r₀ / r_nominal`. Each CAV
  rung is therefore an independent estimate of the radius at `68:00:00`, and `c`
  follows from it.

The three rungs disagree about that radius by about 3 mm, because the published
floors (17, 14, 10) are printed as whole multiples of 1X. That disagreement is
reported below rather than averaged away — but it is a **lower bound** on the
model's error bar, not the whole of it. **Read §3.1 before carrying any of this
to another disc:** `c` is a property of the individual disc rather than of the
format, and the floors may be truncated rather than exact, which widens the range
of geometries consistent with this table.

Reproduce with `tools/cav_speed_model.py`:

```
tools/cav_speed_model.py --leadout 162892 --sensitivity \
  --measured 16.84,23.01,27.67 \
  --measured 14.45,19.04,22.65 \
  --measured 11.40,14.69,17.32
```

```
curve: PlexTools XL V3.16, PX-716, Audio CD column
nominal reached at MSF 68:00:00 = LBA 305850
  40x rung, floor 17x  ->  nominal radius 58.82 mm
  32x rung, floor 14x  ->  nominal radius 57.14 mm
  24x rung, floor 10x  ->  nominal radius 60.00 mm

geometry: c = 0.009205 mm^2/sector (from the curve above)
  implies pitch 1,6 um at v = 1.36 m/s (ECMA §11.4 allows 1.20-1.40)

disc: lead-out LBA 162892 = MSF 36:11, outer radius 46.09 mm
  this disc's own ceiling on each rung (rate at its lead-out):
    40x rung -> 31.43x maximum, anywhere on this disc
    32x rung -> 25.15x maximum, anywhere on this disc
    24x rung -> 18.86x maximum, anywhere on this disc
window layout: speeds.c @ 530bd43, ncand=6 points=3

 rung    band      LBA   r/mm  predicted  measured    delta
-----------------------------------------------------------
  40x   inner        0  25.00     17.05x    16.84x   -1.22%
  40x  middle    54294  33.54     22.87x    23.01x   +0.61%
  40x   outer   108588  40.31     27.49x    27.67x   +0.67%
  32x   inner     9049  26.61     14.52x    14.45x   -0.48%
  32x  middle    63343  34.76     18.96x    19.04x   +0.41%
  32x   outer   117637  41.33     22.55x    22.65x   +0.46%
  24x   inner    18098  28.14     11.51x    11.40x   -0.97%
  24x  middle    72392  35.94     14.70x    14.69x   -0.09%
  24x   outer   126686  42.32     17.32x    17.32x   +0.02%
-----------------------------------------------------------
worst deviation 1.22%, mean |deviation| 0.55%

sensitivity -- the published floors are rounded, so each one implies
a different nominal radius. Re-fitting against each in turn:

  nominal radius pinned by   r/mm   worst   mean
       the 40x floor (17x)  58.82   0.94%  0.51%
       the 32x floor (14x)  57.14   3.77%  1.37%
       the 24x floor (10x)  60.00   1.64%  1.06%
     the mean of the three  58.66   1.22%  0.55%
```

**Worst deviation 0.94–3.77% across every plausible choice of the pinning
constant.** The conclusion does not depend on which floor is trusted.

### 2.1 The measurements this was checked against

One sweep, not a stitch — all six rows come from a single invocation:

```
speed req=40 page2a=40 measured=23.01 inner=16.84 middle=23.01 outer=27.67 ...
speed req=32 page2a=32 measured=19.04 inner=14.45 middle=19.04 outer=22.65 ...
speed req=24 page2a=24 measured=14.69 inner=11.40 middle=14.69 outer=17.32 ...
speed req=16 page2a=8  measured=8.01  inner=8.00  middle=8.01  outer=8.01  ...
speed req=8  page2a=8  measured=8.00  inner=8.00  middle=8.00  outer=7.99  ...
speed req=4  page2a=4  measured=4.01  inner=4.01  middle=4.01  outer=4.01  ...
```

Provenance matters here more than usual. Across separate runs the 32X middle
band ranges 18.17–19.55 and the 24X middle 14.30–15.22 — a spread the same size
as the deviation being reported as agreement. Picking rows individually would
have let the selection produce the result. The rows above were required to
co-occur in one output before any of them was used.

### 2.2 The predicted LBAs depend on a layout that can move

`speeds.c` places rung *i*'s window in band *b* at `lba + b·band + i·slot`, with
`slot = count/(points·ncand)` (`src/drive/speeds.c:56-58`, `:248`). Every LBA in
the table above is computed from that rule at **530bd43**. Changing the layout
does not change the shape of this output — it silently changes what the rows
mean. Re-run the tool rather than trusting the table if that code moves.

---

## 3. What this establishes about `speeds`

- **The band figures measure radius, and only radius.** Nine independent
  predictions from a vendor curve land within 1.22%. Whatever else `band_cx[]`
  is doing, it is not being dominated by overhead, cache or scheduling.
- **The CLV/CAV split falls exactly where the curve puts it.** Measured band
  spreads: 0.01X at 8X and 0.00X at 4X, against 10.83X, 8.20X and 5.92X for the
  three CAV rungs. This confirms rather than discovers — CLV behaviour at 8X and
  below was already recorded — but it is a clean check that the field means what
  the header says.
- **The 36-minute test disc caps the 40X rung at 31.43X** at its own lead-out,
  and the outer band sits at two-thirds of the span rather than at the edge, so
  27.67X is what a correct drive produces. The residual after geometry is ~1%,
  not the ~31% the raw comparison suggests.
- **The phantom-rung artefact, read backwards.** The single-band output that
  prompted the 0.9.0 work showed `req=40 measured=17.46` against
  `req=32 measured=18.23`. 17.46 is essentially the 40X rung's own published
  floor of 17X: the descending ladder had put that rung's only window at the
  innermost radius, so it reported the bottom of its range while 32X reported
  further out. The old output was not noisy — it was faithfully reporting a
  number from the wrong place on the disc. This is the artefact the default
  sweep exists to remove.

## 3.1 The limit of this model — read before applying it to anything

This accounts for the **absolute** shortfall on a given disc: why a rung reads
below its nameplate at all. It has **zero explanatory power for a change between
two runs of the same disc**, because for a fixed disc every quantity here is a
constant, and a constant cannot explain a difference. If an admitted ladder moves
between runs, nothing on this page is a candidate cause.

**Note also that MSF `68:00:00` is LBA 305850, while a disc holding 68:00 of
audio has its lead-out at LBA 306000** — a 68-minute disc *reaches* the address
rather than falling short of it. That off-by-a-lead-out is easy to get backwards,
and it inverts the conclusion.

### `c` belongs to the disc, not to the format — and this bounds everything above

An earlier revision of this section printed a table of "40x ceiling by disc
length", derived by holding `c` fixed and varying the lead-out. **That table was
unsound and has been removed.** `c` is not a property of CD-DA; it is a property
of the individual disc:

```
c = pitch * v / (75 * pi)      mm^2 per sector
```

ECMA-130 §11.3 allows a pitch of 1,6 µm ± 0,1 and §11.4 a scanning velocity of
1,20–1,40 m/s, so a legal disc's `c` spans **0,00764 – 0,01010, a factor of
1,32.** That tolerance is precisely *how* a 79-minute disc exists: tighten the
pitch, slow the velocity, and more playing time fits inside the same 58 mm. Two
consequences, and the first is what broke the table:

- **A disc's outer radius is not a function of its playing time.** Longer discs
  do not extend further out; they pack tighter. Holding `c` fixed and increasing
  the lead-out models a disc growing past the edge of the medium, which is why
  the removed table claimed a 74:00 disc reaches r = 60,75 mm — beyond ECMA's
  d5/2 = 59 mm outer bound for the whole information area. No disc has that
  radius. `tools/cav_speed_model.py` now reports that as OUT OF DOMAIN rather
  than printing it.
- **The `c` fitted from one disc may not be carried to another.** Ours is not a
  nominal-geometry disc: the cleanest independent estimate from our own bands
  (below) is ~0,0093, against ~0,0082 for a 74:00 disc spanning r 25→58 mm.

### `v` is not recoverable from `c`, so no cross-disc rate is a number

The withdrawn table was one instance of a wider limit, and the general form only
became visible when the table was gone. `c` fixes the **product** pitch·v and
nothing more. That would be harmless if "x" were a linear velocity, but it is an
**absolute data rate** — 75 sectors/s — so the rate delivered at radius `r` under
CAV is

```
x = omega * r / v
```

and `v` is the *disc's own* scanning velocity. Within one disc `v` is a constant
and cancels: the model's shape, rate ∝ √(r₀²+cn), is untouched, and so is every
figure in §2. Between two discs it does not cancel, and it cannot be measured
from our side.

Three control laws are consistent with everything observable, and they disagree:

| law | nominal is delivered at | on a 77:09 disc, 40× rung |
|---|---|---|
| **address** | LBA 305850, whatever radius that is | 42,09× |
| **radius** | the reference disc's radius, 58,65 mm | 39,55× |
| **fixed RPM** | the reference disc's angular velocity | 40,72–46,15× |

The vendor's note gives an **address**, but an address is what you write when you
have characterised **one** disc — it is a measurement report, not a control law,
and promoting it to one is the same move the withdrawn table made. The RPM law is
a range rather than a point precisely because `v` is only bounded.

**The dominant term is ignorance of the *reference* disc, not of the disc in
hand.** For a 77:09 disc `c` = 0,007889 admits v ∈ 1,200–1,239 m/s, a 3,3%
window; the reference disc's `c` = 0,009205 admits 1,276–1,400 m/s, **9,7%**. The
long disc pins itself from both sides — ECMA's floor at one end, the medium's
58 mm at the other — while the reference disc is pinned at neither. The transfer
band is the *ratio* of the two windows, so the widths compose multiplicatively
(1,033 × 1,097 = 1,133, exactly the 13,3% above) and the shares are log-widths:
ln(1,097)/ln(1,133) = **74%** of it is the reference disc. So the
residual here is a **missing vendor constant, not a missing measurement**: no
bench time narrows it, and the drive's own documentation would.

`tools/cav_speed_model.py` prints all three whenever `--c` is supplied and
declines to pick. The criterion is the presence of `--c`, not `c ≠ c_curve`:
`c` cannot establish disc identity, since two different discs may share a `c`
while their velocities remain independently uncertain.

### What the measurements pin on their own

Within one rung, the ratio between two bands depends only on `c` — not on the
nominal radius, the published curve, or any efficiency term. Solving for `c` from
those ratios is the one estimate the measurements make by themselves
(`--fit-c`):

```
    inner->outer: c = 0.009653   (1.05x the c in use)
   middle->outer: c = 0.009299   (1.01x the c in use)
   inner->middle: c = 0.009797   (1.06x the c in use)
```

`middle->outer` is the least contaminated — it is the only pair that excludes the
inner band, whose known depression (§4) inflates any slope measured from it, and
it agrees with the `c` used above to **1%**. So the geometry constant is
independently confirmed *for this disc*, and the two pairs that touch the inner
band disagree in exactly the direction §4 predicts.

### The floors do not pin `c` as tightly as §2 assumes

The model takes the published floors (17, 14, 10) as exact. They are printed as
whole multiples of 1x, and on a **nominal-geometry** disc the physics gives
17,84 / 14,28 / 10,71 — which truncate to exactly 17 / 14 / 10, three for three
(`--sensitivity` runs this test). So the panel may simply be truncating, in which
case the floors are consistent with a nominal disc too and do not select between
the two readings.

This does not undo §2. Nine band figures are predicted to 1,22% with **no
parameter fitted to them**, which tests the model's *shape* — rate ∝ √(r₀²+cn) —
and that result stands however the floors are read. What it does mean is that the
agreement confirms the shape more strongly than it confirms this particular `c`,
and that the sensitivity table in §2 understates the range, because it varies the
nominal radius while holding the floors exact.

### Why this section exists

The model was applied, within a day of being written, to an observation it could
not speak to; then its own author extended it across disc lengths it could not
cover. Both errors produced well-formed numbers. A result that explains something
real gets reached for again, and its boundary is never visible from inside the
result — so the boundary has to be written down beside it.

## 4. Known residual, not investigated

All three inner bands read low (−1.22%, −0.48%, −0.97%) while the six
middle/outer bands are near zero or slightly high. `speeds.c:253` streams a
warm-up chunk before the clock starts, but band 0 is reached from a park or
lead-in position, with a longer seek and possibly incomplete spin-up. That is a
plausible mechanism and it is **inference, not a finding** — no test has been
run and none is proposed.

---

## Attribution

The speed grid and its three notes are factual hardware capability data read
from PlexTools Professional XL V3.16, the vendor's own tool. See
`docs/reference/ATTRIBUTION.md`.
