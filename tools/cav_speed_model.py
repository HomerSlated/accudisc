#!/usr/bin/env python3
"""Predict per-band CD read speeds from a drive's published CAV curve.

WHY THIS EXISTS. `accudisc speeds` reports a rate per radial band, and those
rates are almost always BELOW the requested speed. That looks like a defect and
usually is not: under CAV the rate is proportional to radius, a drive's
advertised "Nx" is the rate at an outer radius the drive chooses, and a disc
that ends short of that radius cannot reach Nx anywhere on its surface. This
tool subtracts the geometry so that whatever is left can be argued about.

THE MODEL has two external inputs and no parameter fitted to AccuDisc's output:

  * Red Book: the program area starts at r0 = 25.0 mm and the track pitch is
    constant, so an LBA counts AREA, not radius:  r(n) = sqrt(r0^2 + c*n).
  * The drive's own published curve: for each CAV rung, the nominal speed and
    the speed at the innermost radius, plus the address at which nominal is
    reached. Under CAV, floor/nominal = r0/r_nominal, so each rung is an
    independent estimate of the outer radius, and c follows.

The rungs' estimates will disagree, because published floors are rounded to
whole multiples of 1x. That disagreement is the model's real error bar, and
--sensitivity reports it rather than hiding it behind the mean.

USAGE
    tools/cav_speed_model.py --leadout 162892
    tools/cav_speed_model.py --leadout 162892 --sensitivity
    tools/cav_speed_model.py --leadout 162892 --measured 16.84,23.01,27.67 \
                             --measured 14.45,19.04,22.65 \
                             --measured 11.40,14.69,17.32

The default curve is the PX-716's, from PlexTools Professional XL V3.16;
see docs/research/cav-read-speed-geometry.md for the transcription and for
the reading this tool was written to check.
"""
from __future__ import annotations

import argparse
import math
import sys

R0_MM = 25.0  # Red Book: inner radius of the program area

# --- the PX-716 CD Read curve, Audio CD column (PlexTools XL V3.16) ----------
# (nominal x, x at the innermost radius). CLV rungs are not part of the curve:
# they are flat by definition and are listed separately.
PX716_CAV = [(40, 17), (32, 14), (24, 10)]
PX716_CLV = [8, 4]
PX716_NOMINAL_MSF = (68, 0, 0)  # "The indicated max speed for CAV is
#                                 achieved at address 68:00:00"

# --- how src/drive/speeds.c lays its timed windows out -----------------------
# speeds.c:56-58 and :248 -- rung i's window in band b starts at
#   lba + b*band + i*slot,  slot = count/(points*ncand),  band = slot*ncand.
# Stamped because a change to that layout silently invalidates every predicted
# LBA below without changing the shape of this output.
LAYOUT_REV = "530bd43"
BAND_NAMES = ("inner", "middle", "outer")


def msf_to_lba(m: int, s: int, f: int) -> int:
    return (m * 60 + s) * 75 + f - 150


class Geometry:
    """Maps LBA to radius, and radius to a fraction of the nominal rate."""

    def __init__(self, r_nominal_mm: float, lba_nominal: int) -> None:
        self.r_nominal = r_nominal_mm
        self.c = (r_nominal_mm**2 - R0_MM**2) / lba_nominal

    def radius(self, lba: int) -> float:
        return math.sqrt(R0_MM**2 + self.c * lba)

    def rate(self, nominal_x: float, lba: int) -> float:
        return nominal_x * self.radius(lba) / self.r_nominal


def outer_radius_estimates(curve, lba_nominal):
    """One estimate of the nominal radius per rung. They disagree by rounding."""
    return [(nom, floor, nom * R0_MM / floor) for nom, floor in curve]


def window_lba(rung_index: int, band_index: int, leadout: int,
               ncand: int, points: int) -> int:
    slot = leadout // (points * ncand)
    return band_index * slot * ncand + rung_index * slot


def main() -> int:
    ap = argparse.ArgumentParser(
        description="predict per-band CD read speeds from a published CAV curve")
    ap.add_argument("--leadout", type=int, required=True,
                    help="disc lead-out LBA (accudisc toc)")
    ap.add_argument("--ncand", type=int, default=6,
                    help="candidates in the ladder (default 6)")
    ap.add_argument("--points", type=int, default=3,
                    help="bands per rung (default 3)")
    ap.add_argument("--measured", action="append", default=[],
                    help="inner,middle,outer for one CAV rung, in curve order; "
                         "repeatable. Omit to print predictions only.")
    ap.add_argument("--sensitivity", action="store_true",
                    help="re-fit against each rung's floor in turn, to show how "
                         "much the rounded floors move the answer")
    args = ap.parse_args()

    lba_nom = msf_to_lba(*PX716_NOMINAL_MSF)
    ests = outer_radius_estimates(PX716_CAV, lba_nom)
    r_nom = sum(e[2] for e in ests) / len(ests)
    geo = Geometry(r_nom, lba_nom)

    measured = []
    for spec in args.measured:
        parts = [float(x) for x in spec.split(",")]
        if len(parts) != args.points:
            print(f"--measured needs {args.points} values, got {len(parts)}",
                  file=sys.stderr)
            return 2
        measured.append(parts)
    if measured and len(measured) != len(PX716_CAV):
        print(f"--measured given {len(measured)} times, curve has "
              f"{len(PX716_CAV)} CAV rungs", file=sys.stderr)
        return 2

    print(f"curve: PlexTools XL V3.16, PX-716, Audio CD column")
    print(f"nominal reached at MSF "
          f"{PX716_NOMINAL_MSF[0]:02d}:{PX716_NOMINAL_MSF[1]:02d}:"
          f"{PX716_NOMINAL_MSF[2]:02d} = LBA {lba_nom}")
    for nom, floor, r in ests:
        print(f"  {nom:>2}x rung, floor {floor:>2}x  ->  nominal radius "
              f"{r:.2f} mm")
    print(f"  mean nominal radius {r_nom:.2f} mm; "
          f"c = {geo.c:.6f} mm^2/sector")
    lo = args.leadout
    print(f"disc: lead-out LBA {lo} = MSF {lo//(75*60):02d}:{(lo//75)%60:02d}, "
          f"outer radius {geo.radius(lo):.2f} mm")
    print(f"  this disc's own ceiling on each rung (rate at its lead-out):")
    for nom, _f in PX716_CAV:
        print(f"    {nom:>2}x rung -> {geo.rate(nom, lo):.2f}x maximum, "
              f"anywhere on this disc")
    print(f"window layout: speeds.c @ {LAYOUT_REV}, "
          f"ncand={args.ncand} points={args.points}\n")

    header = f"{'rung':>5} {'band':>7} {'LBA':>8} {'r/mm':>6} {'predicted':>10}"
    if measured:
        header += f" {'measured':>9} {'delta':>8}"
    print(header)
    print("-" * len(header))

    devs = []
    for i, (nom, _floor) in enumerate(PX716_CAV):
        for b in range(args.points):
            lba = window_lba(i, b, lo, args.ncand, args.points)
            p = geo.rate(nom, lba)
            row = (f"{nom:>4}x {BAND_NAMES[b]:>7} {lba:>8} "
                   f"{geo.radius(lba):>6.2f} {p:>9.2f}x")
            if measured:
                m = measured[i][b]
                d = (m - p) / p * 100.0
                devs.append(d)
                row += f" {m:>8.2f}x {d:>+7.2f}%"
            print(row)
    print("-" * len(header))
    if devs:
        print(f"worst deviation {max(map(abs, devs)):.2f}%, "
              f"mean |deviation| {sum(map(abs, devs))/len(devs):.2f}%")

    if args.sensitivity and measured:
        print("\nsensitivity -- the published floors are rounded, so each one "
              "implies\na different nominal radius. Re-fitting against each in "
              "turn:\n")
        print(f"{'nominal radius pinned by':>26} {'r/mm':>6} {'worst':>7} "
              f"{'mean':>6}")
        cands = [(f"the {nom}x floor ({floor}x)", r) for nom, floor, r in ests]
        cands.append(("the mean of the three", r_nom))
        for label, r in cands:
            g = Geometry(r, lba_nom)
            d = [(measured[i][b] - g.rate(nom, window_lba(i, b, lo, args.ncand,
                                                          args.points)))
                 / g.rate(nom, window_lba(i, b, lo, args.ncand, args.points))
                 * 100.0
                 for i, (nom, _f) in enumerate(PX716_CAV)
                 for b in range(args.points)]
            print(f"{label:>26} {r:>6.2f} {max(map(abs, d)):>6.2f}% "
                  f"{sum(map(abs, d))/len(d):>5.2f}%")

    print(f"\nCLV rungs in this curve ({', '.join(f'{c}x' for c in PX716_CLV)}) "
          f"are flat by definition:\nno radius term, so band spread should "
          f"collapse to measurement noise.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
