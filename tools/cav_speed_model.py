#!/usr/bin/env python3
"""Predict per-band CD read speeds from a drive's published CAV curve.

WHY THIS EXISTS. `accudisc speeds` reports a rate per radial band, and those
rates are almost always BELOW the requested speed. That looks like a defect and
usually is not: under CAV the rate is proportional to radius, a drive's
advertised "Nx" is the rate at an outer radius the drive chooses, and a disc
that ends short of that radius cannot reach Nx anywhere on its surface. This
tool subtracts the geometry so that whatever is left can be argued about.

THE MODEL. Track pitch is constant within one disc, so an LBA counts AREA:

    r(n) = sqrt(r0^2 + c*n),   r0 = 25,0 mm (ECMA-130 §8.6, d7 = 50,0 mm)

Under CAV the rate is proportional to r, so a rung's rate at LBA n is
`nominal * r(n)/r(n_nominal)`.

`c` IS NOT A UNIVERSAL CONSTANT, AND THIS IS THE THING TO GET RIGHT. It is a
property of the individual disc:

    c = pitch * v / (75 * pi)      [mm^2 per sector]

ECMA-130 §11.3 allows a pitch of 1,6 +/- 0,1 um and §11.4 a scanning velocity of
1,20 to 1,40 m/s, so a legal disc's `c` ranges over 0,00764 .. 0,01010 — a factor
of 1,32. That tolerance is exactly HOW a 79-minute disc exists: tighten the pitch
and slow the velocity and more playing time fits inside the same 58 mm. It
follows that:

  * A disc's outer radius is NOT a function of its playing time alone. Two
    74-minute discs with different pitch end at different radii.
  * You CANNOT take the `c` that suits one disc and apply it to a disc of
    another length. Doing so is the single easiest way to misuse this tool, and
    it does not fail quietly — it computes a radius no disc has, which is what
    the domain check below reports.

The default `c` is derived from the drive's published curve (see DEFAULT_CURVE):
under CAV, floor/nominal = r0/r_nominal, so each rung is an independent estimate
of the radius at the nominal address, and `c` follows. That is a statement about
the REFERENCE disc the vendor characterised, not about the disc in your drive.
Use --c to supply a better one, and --fit-c to see what the measurements imply.

USAGE
    tools/cav_speed_model.py --leadout 162892
    tools/cav_speed_model.py --leadout 162892 --sensitivity --fit-c \\
        --measured 16.84,23.01,27.67 \\
        --measured 14.45,19.04,22.65 \\
        --measured 11.40,14.69,17.32

The default curve is the PX-716's, from PlexTools Professional XL V3.16; see
docs/research/cav-read-speed-geometry.md for the transcription and the reading
this tool was written to check.
"""
from __future__ import annotations

import argparse
import math
import sys

# --- ECMA-130 2nd ed., read from the spec (docs/research/) -------------------
# §8.6  d7 = 50,0 mm      user data zone STARTS here  -> r0 = 25,0 mm exactly
#       d8 = 116 mm max   user data zone ENDS here    -> r <= 58,0 mm
#       d5 = 118 mm max   information area outer bound-> r <= 59,0 mm
# §11.3 physical track pitch 1,6 um +/- 0,1
# §11.4 scanning velocity 1,20 m/s .. 1,40 m/s
R0_MM = 25.0
R_USER_MAX_MM = 58.0
R_INFO_MAX_MM = 59.0
PITCH_UM_RANGE = (1.5, 1.7)
VELOCITY_RANGE = (1.20, 1.40)

# --- the PX-716 CD Read curve, Audio CD column (PlexTools XL V3.16) ----------
# (nominal x, x at the innermost radius). CLV rungs are flat by definition and
# are listed separately. NOTE the floors are printed as whole multiples of 1x
# and may be truncated rather than rounded -- see --sensitivity.
DEFAULT_CURVE = [(40, 17), (32, 14), (24, 10)]
DEFAULT_CLV = [8, 4]
DEFAULT_NOMINAL_MSF = (68, 0, 0)  # "The indicated max speed for CAV is
#                                    achieved at address 68:00:00"

# --- how src/drive/speeds.c lays its timed windows out -----------------------
# speeds.c:56-58 and :248 -- rung i's window in band b starts at
#   lba + b*band + i*slot,  slot = count/(points*ncand),  band = slot*ncand.
# Stamped because a change to that layout silently invalidates every predicted
# LBA below without changing the shape of this output.
LAYOUT_REV = "530bd43"
BAND_NAMES = ("inner", "middle", "outer")


def msf_to_lba(m: int, s: int, f: int) -> int:
    return (m * 60 + s) * 75 + f - 150


def c_from_pitch(pitch_um: float, velocity_ms: float) -> float:
    """Area swept per sector / pi.  dA = pitch * (v/75) per sector."""
    return (pitch_um * 1e-3) * (velocity_ms * 1000.0 / 75.0) / math.pi


def pitch_velocity_for(c: float) -> tuple[float, float]:
    """One (pitch, velocity) pair consistent with c -- at nominal pitch."""
    v = c * math.pi * 75.0 / (1.6e-3) / 1000.0
    return 1.6, v


class Geometry:
    def __init__(self, c: float, lba_nominal: int) -> None:
        self.c = c
        self.lba_nominal = lba_nominal
        self.r_nominal = self.radius(lba_nominal)

    def radius(self, lba: int) -> float:
        return math.sqrt(R0_MM**2 + self.c * lba)

    def rate(self, nominal_x: float, lba: int) -> float:
        return nominal_x * self.radius(lba) / self.r_nominal


def c_from_curve(curve, lba_nominal: int) -> tuple[float, list]:
    """Pin c from the published floors, treating them as exact."""
    ests = [(nom, floor, nom * R0_MM / floor) for nom, floor in curve]
    r_nom = sum(e[2] for e in ests) / len(ests)
    return (r_nom**2 - R0_MM**2) / lba_nominal, ests


def window_lba(rung_index: int, band_index: int, leadout: int,
               ncand: int, points: int) -> int:
    slot = leadout // (points * ncand)
    return band_index * slot * ncand + rung_index * slot


def solve_c(lba_a: int, lba_b: int, ratio: float) -> float:
    """c implied by the rate ratio between two bands of ONE rung.

    Depends only on the two LBAs and their measured ratio -- no nominal radius,
    no efficiency term, no reference to the published curve. This is the only
    quantity here that the measurements determine on their own.
    """
    rsq = ratio**2
    den = lba_b - rsq * lba_a
    return R0_MM**2 * (rsq - 1.0) / den if den else float("nan")


def main() -> int:
    ap = argparse.ArgumentParser(
        description="predict per-band CD read speeds from a published CAV curve")
    ap.add_argument("--leadout", type=int, required=True,
                    help="disc lead-out LBA (accudisc toc)")
    ap.add_argument("--ncand", type=int, default=6,
                    help="candidates in the ladder (default 6)")
    ap.add_argument("--points", type=int, default=3,
                    help="bands per rung (default 3)")
    ap.add_argument("--c", type=float, default=None,
                    help="geometry constant mm^2/sector for THIS disc; "
                         "default is derived from the published curve")
    ap.add_argument("--measured", action="append", default=[],
                    help="inner,middle,outer for one CAV rung, in curve order; "
                         "repeatable. Omit to print predictions only.")
    ap.add_argument("--fit-c", action="store_true",
                    help="report the c implied by the measurements themselves. "
                         "DIAGNOSTIC ONLY -- never used for the predictions.")
    ap.add_argument("--sensitivity", action="store_true",
                    help="re-fit against each rung's floor in turn, and test "
                         "whether the floors are truncated rather than exact")
    args = ap.parse_args()

    lba_nom = msf_to_lba(*DEFAULT_NOMINAL_MSF)
    c_curve, ests = c_from_curve(DEFAULT_CURVE, lba_nom)
    c = args.c if args.c is not None else c_curve
    geo = Geometry(c, lba_nom)

    measured = []
    for spec in args.measured:
        parts = [float(x) for x in spec.split(",")]
        if len(parts) != args.points:
            print(f"--measured needs {args.points} values, got {len(parts)}",
                  file=sys.stderr)
            return 2
        measured.append(parts)
    if measured and len(measured) != len(DEFAULT_CURVE):
        print(f"--measured given {len(measured)} times, curve has "
              f"{len(DEFAULT_CURVE)} CAV rungs", file=sys.stderr)
        return 2

    print("curve: PlexTools XL V3.16, PX-716, Audio CD column")
    print(f"nominal reached at MSF "
          f"{DEFAULT_NOMINAL_MSF[0]:02d}:{DEFAULT_NOMINAL_MSF[1]:02d}:"
          f"{DEFAULT_NOMINAL_MSF[2]:02d} = LBA {lba_nom}")
    for nom, floor, r in ests:
        print(f"  {nom:>2}x rung, floor {floor:>2}x  ->  nominal radius "
              f"{r:.2f} mm")
    src = "supplied with --c" if args.c is not None else "from the curve above"
    _, v_implied = pitch_velocity_for(c)
    print(f"\ngeometry: c = {c:.6f} mm^2/sector ({src})")
    print(f"  implies pitch 1,6 um at v = {v_implied:.2f} m/s "
          f"(ECMA §11.4 allows {VELOCITY_RANGE[0]:.2f}-{VELOCITY_RANGE[1]:.2f})")
    if not (VELOCITY_RANGE[0] <= v_implied <= VELOCITY_RANGE[1]):
        c_lo = c_from_pitch(PITCH_UM_RANGE[0], VELOCITY_RANGE[0])
        c_hi = c_from_pitch(PITCH_UM_RANGE[1], VELOCITY_RANGE[1])
        inside = c_lo <= c <= c_hi
        print(f"  ** that velocity is out of spec at nominal pitch. Across the "
              f"full pitch\n"
              f"     tolerance c may legally be {c_lo:.6f}..{c_hi:.6f}, so this "
              f"c is "
              f"{'INSIDE that range (a wider-pitch disc)' if inside else 'OUTSIDE it'}"
              f". **")

    lo = args.leadout
    r_out = geo.radius(lo)
    print(f"\ndisc: lead-out LBA {lo} = MSF {lo//(75*60):02d}:{(lo//75)%60:02d}"
          f", outer radius {r_out:.2f} mm")
    if r_out > R_INFO_MAX_MM:
        print(f"\n  *** OUT OF DOMAIN: {r_out:.2f} mm exceeds the information "
              f"area (ECMA-130 §8.6,\n"
              f"      d5 = 118 mm max -> r <= {R_INFO_MAX_MM} mm). No disc has "
              f"that radius, so this\n"
              f"      c does not belong to a disc of this length. A longer disc "
              f"is made by\n"
              f"      TIGHTENING the pitch, i.e. by having a SMALLER c -- not by "
              f"growing past\n"
              f"      the edge. Supply the right --c or do not use this length. "
              f"***\n")
    elif r_out > R_USER_MAX_MM:
        print(f"  note: past d8/2 = {R_USER_MAX_MM} mm, the user data zone's "
              f"outer bound. Plausible\n"
              f"        only for a full-length disc; otherwise the c is too "
              f"large for this length.")
    print("  this disc's own ceiling on each rung (rate at its lead-out):")
    for nom, _f in DEFAULT_CURVE:
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
    for i, (nom, _floor) in enumerate(DEFAULT_CURVE):
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

    if args.fit_c and measured:
        print("\nfit-c -- what the measurements alone imply, from within-rung")
        print("band ratios. Independent of the curve, the nominal radius and any")
        print("efficiency term. NOT used in the predictions above.\n")
        print(f"{'rung':>5} {'pair':>16} {'ratio':>8} {'implied c':>11}")
        groups: dict[str, list] = {}
        for i, (nom, _f) in enumerate(DEFAULT_CURVE):
            lbas = [window_lba(i, b, lo, args.ncand, args.points)
                    for b in range(args.points)]
            m = measured[i]
            for a, b, lab in ((0, 2, "inner->outer"), (1, 2, "middle->outer"),
                              (0, 1, "inner->middle")):
                if b >= args.points:
                    continue
                cc = solve_c(lbas[a], lbas[b], m[b] / m[a])
                groups.setdefault(lab, []).append(cc)
                print(f"{nom:>4}x {lab:>16} {m[b]/m[a]:>8.4f} {cc:>11.6f}")
        print()
        for lab, cs in groups.items():
            mean = sum(cs) / len(cs)
            print(f"  {lab:>14}: c = {mean:.6f}  "
                  f"({mean/c:.2f}x the c in use)")
        allc = [x for cs in groups.values() for x in cs]
        print(f"\n  overall c from data = {sum(allc)/len(allc):.6f} vs "
              f"{c:.6f} in use.")
        print("  Pairs involving the inner band imply a LARGER c than "
              "middle->outer alone;\n"
              "  a depressed inner band (seek, spin-up) does exactly that, so "
              "treat the\n"
              "  middle->outer figure as the least contaminated of the three.")

    if args.sensitivity and measured:
        print("\nsensitivity -- the published floors are whole multiples of 1x, "
              "so each\nimplies a different nominal radius. Re-fitting against "
              "each in turn:\n")
        print(f"{'nominal radius pinned by':>26} {'r/mm':>6} {'worst':>7} "
              f"{'mean':>6}")
        r_nom_mean = sum(e[2] for e in ests) / len(ests)
        cands = [(f"the {nom}x floor ({floor}x)", r) for nom, floor, r in ests]
        cands.append(("the mean of the three", r_nom_mean))
        for label, r in cands:
            g = Geometry((r**2 - R0_MM**2) / lba_nom, lba_nom)
            d = [(measured[i][b] - g.rate(nom, window_lba(i, b, lo, args.ncand,
                                                          args.points)))
                 / g.rate(nom, window_lba(i, b, lo, args.ncand, args.points))
                 * 100.0
                 for i, (nom, _f) in enumerate(DEFAULT_CURVE)
                 for b in range(args.points)]
            print(f"{label:>26} {r:>6.2f} {max(map(abs, d)):>6.2f}% "
                  f"{sum(map(abs, d))/len(d):>5.2f}%")

        # Are the floors TRUNCATED rather than exact? Test against a
        # nominal-geometry disc, which is the reference a vendor would use.
        c_nom_disc = (R_USER_MAX_MM**2 - R0_MM**2) / 333000  # 74:00 fills 25->58
        g = Geometry(c_nom_disc, lba_nom)
        print(f"\ntruncation test -- on a NOMINAL-geometry disc (74:00 spanning "
              f"r 25->58 mm,\nc = {c_nom_disc:.6f}) the nominal address sits at "
              f"{g.r_nominal:.2f} mm, giving:\n")
        allt = True
        for nom, floor in DEFAULT_CURVE:
            exact = nom * R0_MM / g.r_nominal
            trunc = math.floor(exact)
            allt &= (trunc == floor)
            print(f"  {nom:>2}x rung -> {exact:.2f}x at r0; printed {floor}x"
                  f"   {'= floor(), consistent' if trunc == floor else 'MISMATCH'}")
        if allt:
            print("\n  All three are consistent with the panel TRUNCATING. If it "
                  "does, the floors\n  do not pin c as tightly as the table above "
                  "assumes, and the good agreement\n  says the model's SHAPE is "
                  "right without confirming this particular c.")

    print(f"\nCLV rungs in this curve "
          f"({', '.join(f'{c_}x' for c_ in DEFAULT_CLV)}) are flat by "
          f"definition:\nno radius term, so band spread should collapse to "
          f"measurement noise.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
