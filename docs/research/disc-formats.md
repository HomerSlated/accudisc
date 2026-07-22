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
not a quirk of this disc. (The constants themselves are **spec**, but sourced
from recall until the Orange Book is in hand; the measurement is what currently
backs them.)

### Implications table

| | Mixed Mode | Enhanced CD |
|---|---|---|
| sessions | 1 | 2 |
| data track position | first | last |
| legacy audio player | **unsafe** (noise) | safe |
| inter-track gap | none | ~11,400 sectors |
| needs multi-session drive | no | yes |
| our track extents | always were right | **wrong until 35ba94c** |
| our `read` default | ⚠️ refuses — see §6 | works |

---

## 4. Session and track limits

**Tracks: 99 per disc**, hard. TNO is 1–99 in the Q channel. This is per
*disc*, not per session. **spec**

**Sessions:** commonly quoted ceiling is 99 — **unverified**, and exactly the
kind of number that circulates as folklore. Capacity binds long before it in
any case. Each session after the first costs, in the program area:

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
| **CD+G** | ~80% there | plausible | **Best value on the list.** `--sub raw` already captures all 96 subchannel bytes; graphics live in R–W. Needs R–W deinterleave, pack alignment, packet emission. See §7. |
| **Enhanced CD / CD-Extra / CD-Plus** | done (35ba94c) | substantial | Reading works. Writing needs multi-session DAO: a second lead-in/lead-out and the drive's session reservation — considerably more than the single-session DAO we have. |
| **Mixed Mode** | ⚠️ needs track selection | mostly easy | See §6.1. Writing is easy *if* the caller supplies the data track's bits; we would write it as an opaque track with no filesystem knowledge. |
| **CD-i** | mostly N/A | no | Green Book, Mode 2 throughout, no audio tracks in a pure CD-i disc — nothing for a CD-DA tool. **Exception: CD-i Ready**, which hides data in an enormous *track 1 pregap* (index 0, sometimes minutes long) so audio players skip it. Interesting to us only because reading track 1's pregap is a capability question we face anyway. Mechanism **unverified**. |

### 6.1 A gap the session guard opened

On a Mixed Mode CD, session 1 contains *both* the data track and the audio
tracks. So `accudisc_toc_default_audio_session()` correctly returns session 1,
`accudisc_toc_session_range()` returns the whole session, and
`accudisc_check_audio_range()` then correctly refuses it as `data_track`.

Every step is individually right and the outcome is that **`accudisc read` with
no arguments fails on a Mixed Mode CD** as of 35ba94c. The guard is not wrong —
that range genuinely contains unreadable sectors — but *session*-level
selection is too coarse for a format that mixes types *within* a session.

Fix: track-level selection (`--track N` / `--tracks A-B`), and a default that
selects the audio *tracks* of the chosen session rather than the whole session
span. Roughly 60 lines. **Untested — no Mixed Mode disc has been in the drive.**

---

## 7. CD+G, in a little more detail

Graphics ride in the **R–W** subchannel: 6 bits x 96 symbols = 72 bytes per
sector, packed as 4 packets of 24 bytes, giving 300 packets/second. That packet
stream *is* the `.cdg` file format. **unverified** in its packing details —
worth confirming before implementing.

What we already have: `--sub raw` captures all 96 subchannel bytes per sector,
so the transport half is done.

What is missing: deinterleaving R–W out of P–W, pack-boundary alignment, and
emitting the packet stream.

**Scope line agreed with cdda2img:** AccuDisc deinterleaves and emits packets;
cdda2img renders packets to images/video. Rendering is presentation and belongs
on their side of the "AccuDisc only moves bits" rule.

**Integrity caveat.** The subchannel has no C1/C2 protection — a per-frame
CRC-16 is its only check — so CD+G capture inherits the Q-failure populations
already characterised in RECOVERY.md. Packet loss must be **reported**, never
interpolated.

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

1. **Close the Mixed Mode gap** (`--tracks`). Opened by 35ba94c; should not sit.
2. **CD+G packet extraction.** Highest value per line on the list, already in
   CLAUDE.md's scope, and the capture half is done.
3. **Defensive pass for malformed/protected TOCs.** Cheap, and the session
   model is new enough that its failure modes are untested against hostile
   input.

Deliberately *not* recommended: SACD (impossible), HDCD (nothing to do), XRCD
(nothing to do), CD-i (no audio), multi-session *writing* (large, and no
demand yet).

---

## 10. What would retire the unverified marks

Most of §4 and parts of §3 and §7 rest on recall. The **Orange Book**
(CD-R/CD-RW, Recordable) is the normative source for session structure and the
overhead constants, and is filed as [P3] in TODO. Red Book is already held in
a cdda2img private sub-dir. Both are licensed documents: `private/code/`
handling, never redistributed, summaries only.
