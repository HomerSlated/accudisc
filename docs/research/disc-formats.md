# CD family formats: what they are, what they cost us

Scope survey written 2026-07-22, prompted by an Enhanced CD that wedged a rip
and exposed a multi-session geometry bug. The question behind it: *which of
these formats is worth supporting, and what would each actually cost?*

**Confidence marking.** Statements are marked where they rest on measurement
(**measured**), on the specification (**spec**), or on my own recall that has
not been checked against a normative source (**unverified**). The last category
is not decoration — act on it accordingly. **Most of the unverified marks were
retired on 2026-07-22** when the specifications arrived; see §10 for what
changed and §11 for the copy-protection pass. Two of them turned out to be
wrong rather than merely unconfirmed (§7), which is the argument for the
marking scheme.

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
| **CD+G** | **done 2026-07-22** | plausible | `read --cdg FILE` emits the 24-byte pack stream, R–W de-interleaved and Reed-Solomon corrected. See §7. Reading is complete bar end-to-end verification on an actual CD+G disc; writing would need the R–W encoder and interleaver, which is the same maths run backwards. |
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
- **Copy-protection schemes** — **this one matters to us**, and the defensive
  pass is **done, 2026-07-22**. See §11 for the taxonomy and what it found.
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

---

## 11. Deliberately malformed TOCs, and what defending against them found

Source: Kris Kaspersky, *CD Cracking Uncovered: Protection Against Unsanctioned
CD Copying*, ch. 6 ("Anti-Copying Mechanisms") and ch. 7 ("Protection
Mechanisms for Preventing Playback in PC CD-ROM"). Held in `private/research/`;
licensed, summaries only. Read 2026-07-22, and it retired the **unverified**
mark that stood on §8.

The schemes do not corrupt discs by accident. They malform the lead-in *on
purpose*, exploiting the fact that a low-end audio player ignores most of the
TOC while a PC drive believes it.

### 11.1 The taxonomy

| Attack (Kaspersky's naming) | What it does |
|---|---|
| Incorrect Starting Address for the Track | track numbers ascend, addresses do not |
| Fictitious Track in the Lead-Out Area | a track parked past the lead-out |
| Fictitious Track Coinciding with the Genuine Track | two tracks claiming one region |
| Invalidating Track Numbering | gaps, duplicates, a last-track pointer naming a track that is not there |
| Incorrect Starting Number for the First Track | first track is not 1 |
| Track with Non-Standard Number | a point outside 1–99 |
| Data Track Disguised as Audio | CTRL lies about the track type |
| Castrated Lead-Out (ch. 7) | lead-out pointer aimed back toward the disc start |
| Negative Starting Address of the First Audio Track (ch. 7) | an address below LBA 0 |

Two of his observations are worth carrying beyond the immediate fix:

- **A drive may remap non-standard point numbers into the legal range.** He
  cites an NEC unit reporting point `0xAB` as `0x6F`. So a track number *being*
  in 1–99 is not proof it was recorded that way — worth remembering before any
  future check treats an in-range number as trustworthy.
- **Non-standard points are invisible to READ TOC entirely**, including format
  2; reaching them needs subchannel reads of a later session's lead-in. That
  bounds what our parser can ever see, and is why detection here is about
  *self-consistency* rather than about spotting every trick.

### 11.2 What the audit found: one real hole, not a crash

The parser's memory safety held up — every array index was already bounds
checked, and the whole suite now runs clean under ASan + UBSan with
`-fno-sanitize-recover=all` on a hostile-input test file.

The real defect was **the third failure mode**, the one the design rule names:
not crashing, not refusing, but *silently normalising* a contradictory TOC into
a plausible-looking one.

`toc_fill_extents()` walked tracks in **track-number** order and treated that as
**address** order. On every honest disc the two coincide. "Incorrect Starting
Address" exists to break that coincidence, and when it did:

```
track 1 lba      0 sectors   1000 -> [0,1000)   audio
track 2 lba   1000 sectors      0 -> [1000,1000) DATA   <- extent collapsed
track 3 lba    500 sectors  29500 -> [500,30000) audio  <- swallowed track 2

accudisc_check_audio_range(500, 29500) -> ok
```

The data track's extent collapsed to zero because its "next" track had a
*lower* address, so it owned no sector and became **invisible** to the map.
Track 3's extent then stretched across the region it vacated. The guard walked
a span containing a data track and reported it rippable — an authoritative
wrong answer, which is worse than no answer.

**Measured, on a synthetic TOC, before and after.** This was a real hole, not a
theoretical one.

### 11.3 The fix: correct geometry, then refuse anyway

Two independent defences, deliberately:

1. **Extents are computed in address order** (`next_by_address()`). This is not
   a hardening measure but the *correct* definition — a track runs to whatever
   comes next on the disc, which is a fact about addresses. Same result on
   honest media; on the attack above it now yields track 2 = DATA owning
   `[1000,30000)`, visible and correctly typed.
2. **A self-contradicting TOC is refused outright.** `accudisc_toc.anomalies`
   records what was found; three of the flags (`lba_order`, `overlap`,
   `leadout_before`) mean the map cannot be believed, and
   `accudisc_check_audio_range()` returns `toc_untrusted` without consulting it.

Either defence alone would stop the demonstrated attack. Keeping both is
deliberate: the first is only as good as our imagination about *which*
orderings can be violated, while the second does not depend on having predicted
the specific trick.

The remaining flags (`past_leadout`, `empty_track`, `negative_lba`,
`bad_track_num`, `range_mismatch`, `bad_session`) are **reported only**. Their
discs are still described correctly by the map, and over-refusing would break
media that reads perfectly well.

### 11.4 What is deliberately NOT defended against

**"Data Track Disguised as Audio" cannot be caught here, and we should not
pretend otherwise.** CTRL is the only statement the TOC makes about track type;
if it lies, no amount of TOC self-consistency checking detects it. It surfaces
at read time as the drive's categorical refusal (sense key 5 / ASC 0x64), which
the read engine already recognises and stops on rather than retrying. Recorded
so nobody later "fixes" this by adding a heuristic that guesses track type from
content — that would be analysis, which CLAUDE.md puts out of scope, and it
would be guessing besides.

### 11.5 Checked against real schemes, 2026-07-22

A research pass surveyed the commercial schemes (Cactus Data Shield, key2audio,
MediaCloQ, MediaMax, XCP, SafeAudio, Alpha-Audio, LabelGate, DocLock) and
mapped each against §11.1's taxonomy. Full findings and the acquisition
shortlist are in `private/research/incoming/`. Three results matter here.

**Two suspected gaps were closed in code, without needing a disc.** Both are now
permanent tests in `tests/test_toc_hostile.c`:

- *Cactus Data Shield 200* is reported to duplicate session 1's track addresses
  onto a second session. That is a different shape from same-session overlap,
  and it was unclear whether our check crossed session boundaries. **It does** —
  the overlap loop compares every pair of tracks without filtering by session,
  and session-bounded extents mean a legitimate multi-session disc never
  overlaps, so the broader check costs nothing in false positives. A synthetic
  CDS-200 shape raises `overlap` and is refused as `toc_untrusted`.
- *key2audio* is reported to present **three** sessions, two small data ones
  bracketing the audio. Nothing there is malformed — the lead-in does not
  contradict itself, there is simply more of it — so no anomaly should fire, and
  none does. The question that mattered was whether anything assumes the last
  session is the interesting one. It does not:
  `accudisc_toc_default_audio_session()` selects the session *containing audio*,
  correctly returning the **middle** session on a synthetic three-session disc.

**MediaCloQ: the first account of it was wrong, and the correction matters.**
An initial pass, working from cdmediaworld, described MediaCloQ purely as a
multi-radius trick — a TOC arranged so a drive's outer-to-inner search for the
newest session hangs — and concluded it was outside anything we could address.
The comp.publish.cdrom FAQ gives the *observed* behaviour instead, and it is
much more concrete:

> a PC drive reports **two sessions and 16 data tracks**, where a standard CD
> player sees **15 audio tracks**.

So whatever the search-order mechanics, the effect visible to a computer is
**track-type inversion**: the audio is presented as DATA. That is squarely
inside our model, not outside it. Modelled synthetically
(`tests/test_toc_hostile.c`), our behaviour is:

- the TOC parses cleanly and **raises no anomaly — correctly**. It is entirely
  self-consistent. It is not malformed, it is *lying*, and those are different
  things. CTRL is the only statement a TOC makes about track type, so no
  consistency check can ever catch this;
- `accudisc_toc_default_audio_session()` returns `ACCUDISC_ERR_NOTFOUND`, and
  the range guard refuses with `data_track`.

Refusing is the right answer — we report what the disc claims rather than
overriding it on a hunch. But this is the **calibration case** flagged in TODO:
a disc that plays perfectly in a hi-fi and that we decline by default. So the
message now names the situation specifically (*"N tracks, all marked data"*),
says that CTRL may be misreporting and that some schemes do this deliberately,
and points at `--force`. Failing informatively is the whole design rule; a bare
"no session contains audio tracks" satisfied the letter of it and not the
spirit.

The residual multi-radius concern still stands, narrowly: if a drive resolves
its own session search differently than another drive, we see only the answer
it returns. That remains a drive-firmware and physical-mastering interaction we
cannot address in parsing — but it is a smaller and better-defined gap than the
first account suggested.

The track-type-inversion account now rests on **four independent sources**:
cdrfaq, the comp.publish.cdrom FAQ, BinaryObjectScanner ("a multisession CD, and
all the audio tracks are erroneously marked as data tracks"), and the DRM
Library. The original multi-radius account is withdrawn, not merely amended.

**A confirmed test article exists, and it covers two subsystems at once.**
Sound Choice *Karaoke Spotlight Series — Pop Hits Vol. 132* (SC8732) is a
confirmed MediaCloQ Version 1 disc — the protection is printed on the label —
**and** a confirmed CD+G disc. So one purchase would give the R-W decoder its
end-to-end verification (which `tests/test_rw.c` cannot, being a round trip
against our own encoder) *and* exercise the track-type-inversion path. Sound
Choice reportedly shipped ~35 CD+G discs with MediaCloQ across their 8700
series, so the family is wide rather than a single scarce item.

**On safety, since the scheme's reputation invites caution**: MediaCloQ installs
no driver and no kernel component — that was XCP (`Aries.sys`) and MediaMax
(`sbcphid.sys`), different schemes entirely. The privacy allegations against
MediaCloQ in *DeLise v. Fahrenheit Entertainment* concern **web-side** tracking:
cookies, web bugs and personal data collected after a user followed the disc's
download offer to SunnComm's site and typed their details in. There is no
executable payload an SG_IO tool would ever consult, and those servers have been
gone for two decades. Reading such a disc is ordinary.

**Method note.** The correction came from a second, better source on a claim
already written down, not from new analysis. The first account was plausible,
cited, and wrong in the part that mattered — which is the same failure mode as
§7's CD+G errors. Prefer the source that reports *observed drive behaviour*
over the one that explains a mechanism.

### 11.6 Why a structural view is worth having at all

BinaryObjectScanner detects copy protection by scanning **files** — every
interface it exposes (`IContentCheck`, `IExecutableCheck`, `IPathCheck`,
`IDiskImageCheck`) reads file contents or paths, and no detector reads a TOC,
session table or subchannel.

The consequence is visible in its own source: the schemes that are **purely
structural** are all unimplemented stubs — DocLoc, SafeAudio, Alpha-Audio, and
the original key2audio. That is not an oversight. A scheme that leaves no file
signature is invisible to a file scanner by construction, and its MediaCloQ
detector says so plainly about one disc: "currently undetected, due to there
seeming to be no reference to MediaCloQ in the disc's contents."

So `anomalies=` covers something a mature, well-maintained tool in this space
structurally cannot — not through any cleverness on our part, but because we
happen to look at the layer where these schemes actually operate. The two
compose: a file scanner answers *which scheme*, a structural reader answers
*what it did to the disc*. Worth stating because it is the clearest argument
that §11's work has value beyond our own rips.

It also cuts the other way, and the honest version includes this: for the
schemes that hide in an autorun payload, a file scanner will identify the disc
and we never will, because nothing about its structure is unusual.

**Several schemes do not touch the TOC at all**, which is worth stating so
acquisition effort is not wasted. *MediaMax* and *XCP* are Windows kernel-driver
attacks on an ordinary, well-formed Enhanced-CD-shaped disc — a Linux SG_IO tool
is unaffected, and their discs serve only as negative controls. "Second session
present" is not evidence of malformation.

*SafeAudio / MusicGuard* is the interesting one. It deliberately inserts short
bursts of unrecoverable noise into the audio, sized so a CD player's
interpolation conceals them while a bit-exact ripper cannot recover them
(confirmed from contemporaneous reporting, 2001). That is a read-integrity
problem for the C2 path and never a TOC anomaly — but it is **not** therefore
low value to us:

- Its errors are **mastered into the disc**, so they are *static* by
  construction: identical on every re-read. Real media damage mixes transient
  and static populations, which is precisely what makes them hard to separate.
  A SafeAudio disc is a **known-pure static population** — the control the
  C2/re-read work has never had.
- It is the case where **a CD player interpolates and AccuDisc must not**.
  RECOVERY.md's rule is report, never interpolate, so a SafeAudio rip should
  surface hard errors *by design*. Worth knowing before such a rip is mistaken
  for a damaged disc.
- Diagnostically: systematic unrecoverable C2 at **consistent locations across
  re-reads**, on a disc with no visible damage, is a SafeAudio signature rather
  than a scratch.

So it belongs on the acquisition list against the **read path**, not the parser.
`private/research/incoming/` names *Volumia! — Puur* (Netherlands, BMG, 2001)
as the one reasonably-sourced title; the labels deliberately shipped these discs
with no packaging notice, which is why identifying them is so hard.

Also untouched: schemes based on physical characteristics rather than the TOC —
deliberate defects, timing/angle measurement, weak sectors (his ch. 9). Those
are binding-to-media mechanisms, not TOC malformations, and nothing in this
pass addresses them.
