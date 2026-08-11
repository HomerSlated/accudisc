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
floors (17, 14, 10) are rounded to whole multiples of 1X. That disagreement is
the model's real error bar and is reported rather than averaged away.

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
  mean nominal radius 58.66 mm; c = 0.009205 mm^2/sector
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
