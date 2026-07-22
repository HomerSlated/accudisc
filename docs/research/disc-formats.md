# CD family formats: what they are, what they cost us

Scope survey written 2026-07-22, prompted by an Enhanced CD that wedged a rip
and exposed a multi-session geometry bug. The question behind it: *which of
these formats is worth supporting, and what would each actually cost?*

**Confidence marking.** Statements are marked where they rest on measurement
(**measured**), on the specification (**spec**), or on my own recall that has
not been checked against a normative source (**unverified**). The last category
is not decoration — act on it accordingly. See [TODO §Formats and specs] for
the Orange Book acquisition that would retire most of the unverified marks.

---

## 1. Four of the ten items collapse

| Claimed format | Reality |
|---|---|
| **Enhanced CD** | One format, three names. The Philips/Sony spec is **CD Extra** |
| **CD-Extra** | (Blue Book, 1995). "Enhanced CD" is the generic industry term; |
| **CD-Plus** | "CD Plus" was the original marketing name, dropped after a trademark dispute. **unverified** (the dispute, not the equivalence). |
| **XRCD** | Not a format at all. A JVC *mastering and manufacturing* process (K2 encoding, specific glass-mastering and pressing discipline). The output is bit-standard Red Book with no on-disc marker. Nothing to support; already works. |
| **HDCD** | Ordinary CD-DA with a watermark in the PCM LSBs. A bit-exact rip preserves it automatically. |

So of the ten items, **three are one item**, **two need no work at all**, and
**one is impossible** (§2).

---

## 2. SACD — out of scope, and not for want of effort

The DSD layer of an SACD is **DVD-density, read at 650 nm, and encrypted**. A
CD/DVD drive cannot address it. This is a physical and cryptographic wall, not
a difficulty. **spec** (the physical parameters); **unverified** (the specific
encryption scheme's details, which do not matter to the conclusion).

Known SACD extractions came from specific Blu-ray players — early PS3 firmware,
certain Pioneer/Oppo units — exploited over the network, never from a PC
optical drive. **unverified** as to the exact model list.

A **hybrid** SACD's CD layer is a genuine Red Book CD and reads as ordinary
CD-DA today. Reporting CD-DA for a hybrid SACD is correct, not a bug: it is
the layer the drive can see. That is the whole of our SACD story.

CLAUDE.md amended 2026-07-22 to say *out of scope* rather than *eventually*.

---

## 3. The central distinction: one session, or two?

This is the part that matters for our code, and the part we got wrong.

### Mixed Mode CD — data and audio in ONE session

```
[lead-in][track 1: DATA][track 2: audio] ... [track n: audio][lead-out]
          \___________________ one TOC lists all n tracks ___________________/
```

- One TOC, one lead-in, one lead-out. Track type is distinguished **only** by
  CTRL bit 2 in each track's TOC entry. **spec**
- **Geometry is contiguous.** Track extents genuinely are "to the next track's
  start". Our pre-2026-07-22 code was correct for Mixed Mode; it broke only
  across sessions.
- Data goes **first**, where a filesystem is expected.
- **The flaw that killed the format:** a legacy audio player reads the TOC,
  sees track 1, and plays it. Yellow Book data rendered as CD-DA is full-scale
  noise. Well-behaved players check CTRL; many 1980s–90s units did not.

### Enhanced CD / CD Extra — data and audio in SEPARATE sessions

```
[lead-in₁][tracks 1-13: audio][lead-out₁]  [lead-in₂][track 14: DATA][lead-out₂]
 \___ TOC₁ lists ONLY audio ___/            \___ TOC₂ lists only data ___/
```

- **Each session has its own TOC.** A legacy audio player reads session 1's
  TOC, learns of 13 audio tracks, and *cannot see that session 2 exists*. That
  is the entire design purpose: it fixes the noise problem by hiding the data
  rather than by trusting players to check CTRL. **spec**
- Data goes **last**, necessarily.
- **The cost is the inter-session gap**, and it is large.

### The gap, measured

PX-716A, Enhanced CD, 2026-07-22 — **measured**, and reproducing the Orange
Book constants exactly:

```
session 1 lead-out starts     195656
  +  6750   session 1 lead-out      (90 s)
  +  4500   session 2 lead-in       (60 s)
  +   150   track 14 pregap          (2 s)
  =  207056                          <- track 14's measured start
```

**11,400 sectors** carrying no track payload and readable as CD-DA by nothing.
The arithmetic closing to the sector is good evidence the constants are right
and that this gap is *structural* — it will be present on every Enhanced CD,
not a quirk of this disc.

**All four constants are now confirmed against normative sources** (2026-07-22),
so this section is **spec** as well as **measured**:

| Constant | Sectors | Source |
|---|---|---|
| first session lead-out | 6750 (1 min 30 s) | ECMA-394 §5.7.1 — **public**, citable |
| later session lead-out | 2250 (30 s) | ECMA-394 §5.7.1 — **public**, citable |
| second+ session lead-in | 4500 (1 min 00 s) | Multisession CD spec §"length of the Lead-In Area of the second and higher Session(s) on a disc is 01:00:00" — licensed, summary only |
| pregap | 150 (2 s) | Multisession CD spec, "Pre-Gap for Data Tracks … length of 00:02:00" — licensed, summary only |

Note ECMA-394 is a **public** standard, so the two lead-out figures can be
quoted and cited in this repo. The Philips/Sony Multisession spec is licensed:
its numbers may be stated as fact, but the document itself never leaves
`private/`.

### Implications table

| | Mixed Mode | Enhanced CD |
|---|---|---|
| sessions | 1 | 2 |
| data track position | first | last |
| legacy audio player | **unsafe** (noise) | safe |
| inter-track gap | none | ~11,400 sectors |
| needs multi-session drive | no | yes |
| our track extents | always were right | **wrong until 35ba94c** |
| our `read` default | works (fixed 2026-07-22) | works |

---

## 4. Session and track limits

**Tracks: 99 per disc**, hard. TNO is 1–99 in the Q channel. This is per
*disc*, not per session. **spec**

**Sessions:** the commonly quoted ceiling is 99. On 2026-07-22 I searched the
Multisession Compact Disc specification — the normative document that would
define such a limit — and **found no stated maximum number of sessions**. That
is not proof none exists, but the absence of it in the one spec that ought to
carry it is a strong hint the "99" is folklore, borrowed from the (real,
spec-stated) 99-track limit. Treat as **unverified, and now actively doubted**.

Capacity binds long before any such ceiling in any case. Each session after the
first costs, in the program area:

```
4500 (lead-in) + 150 (pregap) + 300 (minimum 4-second track) + 2250 (lead-out)
= 7200 sectors ~ 96 seconds
```

Giving roughly **45 sessions** on a 74-minute disc (333,000 sectors) and **50**
on an 80-minute one. **Derived** from the constants above.

The **real** limit is drive firmware, which is soft, undocumented, and worth
measuring on our own hardware rather than assuming. Reports of unreliability
past ~20 sessions are **unverified**.

Note the first session's lead-in sits in the disc's lead-in area, outside the
program area, and so is not charged against capacity; every *subsequent*
lead-in is in the program area and is.

---

## 5. What can and cannot be mixed

**Possible:**

- Audio and data tracks in the same session, in any order (Mixed Mode).
- Any session may hold any mix; sessions are structurally independent.
- Audio in session 2+ — legal, but invisible to legacy audio players, which is
  why nobody does it deliberately.
- Different data modes across tracks on one disc (Mode 1 and Mode 2 XA).

**Not possible:**

- More than 99 tracks total.
- Changing sector type mid-track. CTRL is a per-track property.
- A legacy audio player seeing beyond session 1. Structural, not a bug.
- Zero-track sessions.
- Tracks shorter than 4 seconds (300 sectors) — Red Book minimum. **spec**

---

## 6. Per-format verdicts

| Format | Read | Write | Verdict |
|---|---|---|---|
| **SACD** | impossible | impossible | §2. Dropped from scope. |
| **HDCD** | already works | already works | Bit-exact rip preserves it. Detection and decode are analysis of delivered audio — out of scope per CLAUDE.md, and cdda2img's entirely. |
| **XRCD** | already works | n/a | A mastering process. Nothing on the disc to support. |
| **CD+G** | transport done, decode larger than thought | plausible | Still the best value on the list. `--sub raw` already captures all 96 subchannel bytes; graphics live in R–W. Needs R–W deinterleave, 8× de-interleave, **Reed-Solomon decode**, pack alignment, pack emission. See §7 — the spec is now in hand and revised the estimate upward. |
| **Enhanced CD / CD-Extra / CD-Plus** | done (35ba94c) | substantial | Reading works. Writing needs multi-session DAO: a second lead-in/lead-out and the drive's session reservation — considerably more than the single-session DAO we have. |
| **Mixed Mode** | done (2026-07-22) | mostly easy | See §6.1. Writing is easy *if* the caller supplies the data track's bits; we would write it as an opaque track with no filesystem knowledge. |
| **CD-i** | mostly N/A | no | Green Book, Mode 2 throughout, no audio tracks in a pure CD-i disc — nothing for a CD-DA tool. **Exception: CD-i Ready**, which hides data in an enormous *track 1 pregap* (index 0, sometimes minutes long) so audio players skip it. Interesting to us only because reading track 1's pregap is a capability question we face anyway. Mechanism **unverified**. |

### 6.1 A gap the session guard opened — CLOSED 2026-07-22

On a Mixed Mode CD, session 1 contains *both* the data track and the audio
tracks. So `accudisc_toc_default_audio_session()` correctly returns session 1,
`accudisc_toc_session_range()` returns the whole session, and
`accudisc_check_audio_range()` then correctly refuses it as `data_track`.

Every step is individually right and the outcome is that **`accudisc read` with
no arguments fails on a Mixed Mode CD** as of 35ba94c. The guard is not wrong —
that range genuinely contains unreadable sectors — but *session*-level
selection is too coarse for a format that mixes types *within* a session.

**Fixed and hardware-verified 2026-07-22** on a Taiyo Yuden CD-R carrying data
track 1 (138230 sectors) plus audio tracks 2-11, one session, lead-out 342197.

The fix is `accudisc_toc_session_audio_range()` — the default now resolves to
the audio *tracks* of the chosen session rather than the whole session span —
plus `--track N` / `--tracks A-B` for explicit selection.

Before: `refusing lba 0 count 342197: data_track at lba 0 (track 1)`
After: `session 1, lba 138230 count 203967`, and `138230 + 203967 = 342197`
lands exactly on the lead-out. `--tracks 2-11` resolves to the identical range,
which is independent confirmation the default picks the right thing. A full rip
of that range produced 479,730,384 bytes = 203967 × 2352 exactly, 0 C2-flagged
sectors.

The guard itself was **not** relaxed. `--track 1` and `--tracks 1-11` are still
refused as `data_track`, and a span across a session seam (`--tracks 13-14` on
the Enhanced CD) is refused rather than silently widened to include the 11,400
dead sectors.

One case is refused rather than solved: audio tracks sitting *either side* of a
data track within one session cannot be expressed as a single range, so
`accudisc_toc_session_audio_range()` returns `ACCUDISC_ERR_UNSUPPORTED` and the
caller must select tracks explicitly. Not known to occur in any shipped format,
but legal on the wire, and a silently truncated rip would be worse than a
refusal. Unit-tested; **never observed on real media**.

---

## 7. CD+G, in a little more detail

Graphics ride in the **R–W** subchannel. The normative source (Philips/Sony,
*Subcode/Control and Display System — Channels R–W*, November 1991, now in
`private/research/`) is in hand as of 2026-07-22, and it **corrected two errors
in the previous draft of this section**. Both are recorded rather than quietly
overwritten, because both would have produced wrong code.

**The structure, correctly** (§5.1, **spec**):

```
6 bits (R,S,T,U,V,W of one frame)  = 1 SYMBOL
24 SYMBOLS                         = 1 PACK
4 PACKS                            = 1 PACKET
```

A sector is 98 frames; the first two (S0, S1) are the subcode sync, leaving
**96 symbols = 4 packs = exactly one PACKET per sector**. At 75 sectors/s that
is **75 packets/s and 300 packs/s**.

> **Error 1 — inverted nesting.** The previous draft said "4 packets of 24
> bytes". It is 4 **packs** *per* packet, and a pack is 24 **symbols** of 6 bits,
> not 24 bytes. The old text also gave "300 packets/second"; 300 is the
> **pack** rate. This matters because the `.cdg` file format is a stream of
> 24-byte **packs** at 300/s (7200 bytes/s) — one byte per symbol, 6 bits
> significant. Emitting "packets" would have produced a file at ¼ the rate.

**Error 2 — R–W *is* error-protected.** The previous draft said the subchannel
"has no C1/C2 protection — a per-frame CRC-16 is its only check". That is true
of the **Q** channel and false of R–W. §5.1 specifies:

- a **(24,20) Reed-Solomon code over GF(2⁶)**, polynomial `P(X) = X⁶ + X + 1`,
  correcting across all 24 symbols of a pack (parity P);
- **8× interleaving** over that code, for burst-error tolerance;
- an additional **(4,2) Reed-Solomon** code protecting the first two symbols
  (parity Q) — the ones carrying MODE and ITEM, i.e. the fields that tell you
  how to interpret the rest.

So CD+G capture is **materially more robust** than the earlier text claimed, and
the design consequence is a real one: we should implement the RS decode rather
than merely reporting loss. The `.cdg` files in circulation are almost all
*post*-correction; a capture that skipped RS would be needlessly worse than the
existing corpus.

The Q-failure populations characterised in RECOVERY.md still apply to the **Q**
channel. They do not transfer to R–W unexamined — that was the error. Where RS
correction genuinely fails, loss must still be **reported, never interpolated**.

What we already have: `--sub raw` captures all 96 subchannel bytes per sector,
so the transport half is done.

What is missing: deinterleaving R–W out of P–W, undoing the 8× interleave,
**RS decode (24,20) and (4,2)**, pack-boundary alignment (the pack after S0/S1
is pack 0 of a packet), and emitting the 24-byte pack stream.

The RS decode is new scope relative to the earlier estimate — the "~80% there"
figure in §6 predates it and is now optimistic. The transport half genuinely is
done; the decode half is larger than one line of that table implied.

**Scope line agreed with cdda2img:** AccuDisc deinterleaves, RS-corrects and
emits packs; cdda2img renders them to images/video. Rendering is presentation
and belongs on their side of the "AccuDisc only moves bits" rule. RS correction
stays on our side: it is recovering the bits that were recorded, not
interpreting them.

---

## 8. Formats absent from the original list

- **DTS-CD** — a DTS bitstream in place of PCM. Rips as ordinary CD-DA;
  nothing to do. (Another legacy-player noise hazard when played undecoded.)
- **Copy-protection schemes** (Cactus Data Shield, key2audio, SafeDisc) —
  **this one matters to us.** They work by *deliberately malforming* the TOC
  and session structure: bogus second sessions, lead-outs pointing into the
  program area, illegal track types. Our new session model will meet these in
  the wild. The design goal is to **fail informatively** rather than either
  crash or "helpfully" normalise. A defensive pass over
  `adsc_toc_from_fulltoc()` is warranted. Specific mechanisms **unverified**.
- **CD-TEXT** — supported already.
- **Video CD / Super Video CD** (White Book), **Photo CD** — Mode 2 Form 2. No
  CD-DA content to rip. Photo CD is genuinely multi-session and would exercise
  our new code as a test article.
- **CD-MIDI** — allowed by Red Book, essentially never manufactured.

---

## 9. Recommended order

1. ~~**Close the Mixed Mode gap** (`--tracks`).~~ **Done 2026-07-22** (§6.1).
2. **CD+G packet extraction.** Highest value per line on the list, already in
   CLAUDE.md's scope, and the capture half is done. The normative source is now
   in hand — see §7.
3. **Defensive pass for malformed/protected TOCs.** Cheap, and the session
   model is new enough that its failure modes are untested against hostile
   input. A reference on the specific schemes is now in `private/research/`.

Deliberately *not* recommended: SACD (impossible), HDCD (nothing to do), XRCD
(nothing to do), CD-i (no audio), multi-session *writing* (large, and no
demand yet).

---

## 10. What would retire the unverified marks

**Largely done, 2026-07-22.** A substantial set of specifications arrived and
retired most of the marks:

**Public** — quotable and citable here. These are **not committed**: ECMA
publishes them for free download at <https://ecma-international.org/publications-and-standards/standards/>,
so a citation serves the reader as well as several MB of git history would.
`docs/research/.gitignore` excludes `*.pdf` for that reason.

| Standard | Edition | Covers |
|---|---|---|
| **ECMA-130** | 2nd, June 1996 | CD-ROM data interchange — the public counterpart to much of Red and Yellow Book |
| **ECMA-394** | 1st, Dec 2010 | CD-R Multi-Speed. **Source for the lead-out constants in §3** (§5.7.1) |
| **ECMA-395** | 1st, Dec 2010 | CD-RW Ultra-Speed |

**Licensed**, in `private/research/` — facts may be stated, documents never
leave, summaries only: Orange Book Part II (CD-R) Vols 1–2 and Part III
(CD-RW) Vol 1; the **Multisession CD** specification (§3, §4); **Subcode
Channels R–W** (§7 — corrected two errors); CD Text Mode; CD-ROM XA; the
Enhanced Music CD specification (§3); the original 1990 Recordable CD Systems
description; MMC-3; SCSI-2; the SACD specifications (Parts 1–3); *The CD
Family*; and *CD Cracking Uncovered* (Kaspersky) for §8's protection schemes.

What this changed:

| Was | Now |
|---|---|
| §3 overhead constants: measured + recall | **confirmed**, two from a public spec (ECMA-394) |
| §7 CD+G packing | **corrected** — nesting was inverted, and R–W *is* RS-protected |
| §4 "99 sessions" | **actively doubted** — absent from the spec that would define it |

Still open: the CD-i Ready mechanism (§6), the specific protection mechanisms
(§8 — the Kaspersky text is now available and unread), and any firmware-imposed
session ceiling, which is measurement work on our own hardware, not a spec
question.

Note the SACD specifications are now held too. They do not change §2 in the
slightest — the conclusion there is about *physics and optics*, not about a
missing document — but they make the reasoning checkable rather than asserted.
