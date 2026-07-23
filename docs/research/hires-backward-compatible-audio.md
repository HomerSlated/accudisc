# High-resolution audio and backward-compatible carriers

> **SCOPE — read first.** This note is **out of scope for AccuDisc** and parked
> here deliberately. It is exploratory research toward a *possible separate
> project* (a backward-compatible hi-res audio container, the `[P4]` "vanity"
> idea in `docs/reference/TODO.md`). AccuDisc's role in any of it would be
> **carriage only** — writing/reading a container bit-exactly — never the audio
> codec design, which is the "AccuDisc only moves bits" far side of the line in
> CLAUDE.md. Expect this file to be **extracted to a new repo**. It is kept
> public/shareable; it contains no licensed or private material.

Written 2026-07-23, from a design conversation. Confidence marks follow the
`disc-formats.md` convention: **spec** / **measured** / **derived** /
**unverified** / **evidence** (peer-reviewed listening tests).

---

## 1. The six formats, condensed

| Format | Encoding class | Native config | Bitrate (stereo) | 3-min size |
|---|---|---|---|---|
| **CDDA** | uncompressed LPCM | 16-bit / 44.1 kHz | 1,411.2 kbps | 30.3 MiB |
| **DVD-Audio** | LPCM or MLP lossless | ≤24/192, ≤6ch | ≤9.6 Mbps (LPCM cap) | ~99 MiB @24/96 |
| **HDCD** | LPCM (CDDA) + in-band coding | 16/44.1 | 1,411.2 kbps | 30.3 MiB |
| **DTS-HD MA** | lossless, core+extension (VBR) | ≤24/192, ≤7.1 | ≤~24.5 Mbps peak | content-dep. |
| **Dolby TrueHD** | lossless MLP (VBR) | ≤24/192, ≤7.1(+Atmos) | ≤~18 Mbps peak | content-dep. |
| **SACD** | 1-bit DSD + DST lossless | DSD64 1-bit/2.8224 MHz | 5.6448 Mbps (pre-DST) | ~121 MiB (~60 after DST) |

Two structural facts reorganise the table:

- **Five are PCM-family; SACD (DSD) is not.** DSD is 1-bit pulse-density
  modulation — amplitude encoded as *density of ones*, not a number per sample.
  Any DSD↔rest transcode is a resample/remodulate, never a repack. **spec**
- **Three are "compatible base + hi-res on top"** — the `[P4]` pattern: HDCD
  (LSB control), DTS-HD MA (lossy DTS core + lossless residual), Dolby TrueHD
  (MLP + embedded AC-3 core). **spec**

**Endianness note relevant to a carrier tool:** CDDA PCM is little-endian
(`s16le`); DVD-Audio/-Video LPCM is **big-endian** (`s24be`). Moving PCM between
the two media requires a byte-swap. **spec**

### Open-tool asymmetry worth remembering

- **Dolby TrueHD / MLP** has a real open-source *encoder* (FFmpeg `mlpenc`,
  `-c:a truehd`/`-c:a mlp`) and full decode. It is the only lossless
  multichannel format here open on **both** ends.
- **DTS-HD MA** decodes losslessly in FFmpeg (`dca`, XLL), but the only open
  *encoder* is the lossy **core** (`-strict experimental`). No open lossless-MA
  encoder.
- **HDCD** decode is open (FFmpeg `af_hdcd`, reverse-engineered); no open
  encoder ever existed.
- **DSD** decode/convert is open (`dsd2pcm` by Sebastian Gesemann; FFmpeg DSD +
  DST decoders); real SACD *authoring* is not (encryption + pressing).
- **CDDA** is fully open end to end (cdrdao, cdrtools, libburn, AccuDisc).

---

## 2. DSD / 1-bit ΣΔ, and why it stayed niche

A sigma-delta modulator trades amplitude resolution for time resolution: a
feedback loop (**delta** = input − fed-back output, **sigma** = integration)
drives a 1-bit comparator at high oversampling (DSD64 = 64 × 44.1 kHz =
2.8224 MHz). Output:

```
Y(z) = STF(z)·X(z) + NTF(z)·E(z)     STF≈1,  NTF(z)=(1−z⁻¹)^N  (high-pass)
```

Noise shaping **relocates** quantisation noise out of the audio band; it does
not remove it. Every DSD problem follows from "out of band" being real,
energetic, and only partly harmless. **spec**

**Advantages:** inherent 1-bit DAC linearity (2 levels = a line — no element
matching); gentle analog reconstruction filter; wide nominal bandwidth.

**Problems:**

- **Rising ultrasonic noise floor.** For DSD64 the shaped noise climbs above
  ~20–25 kHz and by 50–100 kHz can exceed the signal. It is real energy → drives
  downstream **intermodulation distortion** (nonlinear amps/tweeters fold it back
  into band), tweeter stress, measurement trouble. The cure is a ~50 kHz analog
  low-pass — which quietly reintroduces the reconstruction filter DSD was sold as
  avoiding.
- **Conditional stability / lost headroom.** High-order 1-bit loops (needed for
  steep shaping) can go unstable; practical peak is ~−6 dBFS (~50 % modulation),
  not full-scale.
- **Idle tones / limit cycles.** Constant/DC-ish input → periodic bit patterns →
  in-band tones. Dithering a 1-bit stream is hard (little range to spare). So
  low-level noise is *tonal and correlated*, the opposite of benign.
- **No native DSP.** No gain/EQ/mix/filter is possible on a PDM stream. All
  production converts DSD → wide PCM (**DXD**, 24/32-bit @352.8 kHz) → back. Each
  hop is lossy and modulator-dependent.
- **DSD↔PCM artifacts.** DSD→PCM decimation must remove the huge ultrasonic
  noise first (else it aliases in); filter design imprints character. PCM→DSD is
  a fresh modulator with its own noise/idle tones. DSD→PCM→DSD is **not**
  bit-transparent.
- **Not really 1-bit at the ends.** Recording ADCs and playback DACs use
  *multibit* ΣΔ internally; 1-bit exists only in delivery. (Lipshitz &
  Vanderkooy's critique: a 1-bit quantiser cannot get wide range *and* low
  distortion without shaping that creates the problems above.) **evidence**

---

## 3. Dynamic range: the assumption that DSD "wins" is false

Theoretical dynamic range from bit depth = `6.02·N + 1.76` dB:

| Format | Theoretical DR | Character |
|---|---|---|
| 16-bit PCM | ~96 dB (98) | flat across band |
| 20-bit PCM | ~122 dB | flat |
| 24-bit PCM | **~144 dB** | flat |
| DSD64 | **~120 dB, 0–20 kHz only** | rises then collapses with frequency |
| DSD128/256 | ~130 dB+ in-band | more OSR → more scooped out |

- **DSD's DR is frequency-dependent; PCM's is flat.** DSD64's ~120 dB is a
  band-limited figure that is already gone by the top octave. 24-bit PCM's number
  needs no asterisk. **spec/derived**
- **The question is moot anyway:** real converters cap at ~120–123 dB
  (A-weighted) — thermal/analog noise. That wall sits *below* 24-bit's 144 dB
  ceiling (unreachable) and right at DSD64's in-band figure (why it *measured*
  competitively, not superiority). Both deliver ~120 dB usable. DSD also loses
  headroom off the top (−6 dBFS limit). **evidence/derived**

**Corollary (well-established engineering):** a cheap CD transport + a quality
outboard DAC can equal SACD/DVD-A in practice and beat a one-box player whose
cost bought transport bulk. The bits off a clean disc are identical regardless
of transport price; all audible quality is decided in the DAC + analog stage at
that ~120 dB wall. A good async/reclocking DAC also neutralises transport
jitter. The master, not the format, drives most audible difference
(see §4). **evidence**

---

## 4. Blind-test evidence on bit depth / hi-res

- **Meyer & Moran (2007), *JAES* 55 — null.** A 16/44.1 A/D/A loop inserted into
  SACD/DVD-A playback; ABX; ~60 listeners, ~550 trials, **~49.8 % (chance)**. A
  CD-standard bottleneck was inaudible. Criticised (source provenance, levels)
  but the cleanest "does the 16-bit ceiling matter?" test. **evidence**
- **Reiss (2016), *JAES* 64 — small positive.** Meta-analysis, ~18 blind
  studies, 400+ subjects, ~12,500 trials: discrimination of hi-res from standard
  is **small but significant**, growing with training. But it measures the whole
  bundle (rate + filter + often different master), not isolated bit depth.
  **evidence**

Reconciliation: the surviving audible hi-res effect lives in **sample-rate /
anti-alias-filter** territory, **not bit depth**. **16-bit dithered delivery is
perceptually transparent**; 24-bit's genuine value is **production headroom**
(summing, editing), not playback. **20-bit was never a delivery format** and has
essentially no isolated blind-test corpus. Methodology decides everything:
<0.1 dB level-matching, identical master, instantaneous synced ABX. **evidence**

**Implication for AccuDisc's premise:** if 16-bit dithered delivery is
transparent, everything audible on a Red Book CD is contained in a *bit-exact*
16/44.1 capture — exactly what AccuDisc produces. The value it adds is *provable
bit-exactness* (all samples correct, C2/consensus to prove it); that is the
ceiling of what is audibly there.

---

## 5. Cloning the HDCD paradigm (a new, incompatible standard)

The HDCD **decoder** is reverse-engineered (FFmpeg `af_hdcd`), so the paradigm
is understood; only the encoder was never opened. Dropping "standalone hardware
must play it" removes the only hard part → pure software DSP.

**What HDCD actually is:** not LSB data-hiding but a **reversible psychoacoustic
transform** — Peak Extend (reversible compression of loud transients, masked by
their own energy) + Low-Level Extend, steered by a **sparse LSB control code**
(low duty cycle, negligible legacy penalty). Copy *that*, not LSB-stuffing.

**Two walls decide whether a clone is worth building:**

1. **Counting argument.** In-band + backward-compatible + *lossless* hi-res is
   impossible past LSB capacity: 24 bits don't fit in 16 without an 8-bit/sample
   side channel (705.6 kbps) that must live somewhere. HDCD sidesteps by going
   **lossy/psychoacoustic** (recovers *perceived* headroom, not bit-exact 24-bit).
2. **Audibility + Nyquist.** On a 44.1 kHz carrier, LSB/companding tricks touch
   only **amplitude resolution** (the *inaudible* axis, §4), never **bandwidth**
   (Nyquist fixed at 22.05 kHz — the axis with the only blind-test signal). So an
   in-band HDCD clone can only recover what nobody can hear. Both HDCD and MQA
   faded for essentially this reason.

**Escape hatch:** go **out-of-band** (store the real residual in a subchannel or
second session, keep the 16/44.1 carrier bit-perfect). That can carry genuine
24/96 — but it is no longer "the HDCD paradigm"; it is the container question
(§6). HDCD's identity is *in-band*.

---

## 6. Out-of-band container ledger (the version with something audible)

Reference program: **45-min stereo album (2,700 s).** The out-of-band residual
must convey the master *minus what the carrier explains*. The carrier explains
the top ~16 bits of 0–22.05 kHz and **nothing** of (a) bits 17–24
(dither-dominated, near-incompressible) or (b) the 22.05–48 kHz band. So the
residual is dominated by its **least compressible** parts. **derived**

**Container capacities:**

| Container | Rate | Over 45 min |
|---|---|---|
| R–W subchannel | 72 B/sector × 75 = 43.2 kbps | ~14.6 MB (less after RS overhead) |
| CD 2nd session (80-min disc) | ~146k sectors × 2048 B | ~300 MB |
| DVD single layer (ref.) | — | ~4,700 MB |

**Residual sizes (carrier already present):**

| Target | Uncompressed | Lossless standalone | Out-of-band residual |
|---|---|---|---|
| 24/48 | 778 MB | ~428 MB | **~200–240 MB** (≈8 LSBs; ~no new band) |
| 24/96 | 1,555 MB | ~855 MB | **~500–850 MB** (8 LSBs + 22–48 kHz) |
| 24/192 | 3,110 MB | ~1.7 GB | **~1.0–1.7 GB** |

**Fit matrix:**

| Residual ↓ / Container → | R–W (~15 MB) | CD 2nd session (~300 MB) | DVD (~4.7 GB) |
|---|---|---|---|
| 24/48 (~220 MB) | ✗ | **✓ (tight)** | ✓ |
| 24/96 (~500–850 MB) | ✗ | **✗ (~2–3× short)** | ✓ |
| 24/192 (~1–1.7 GB) | ✗ | ✗ | ✓ |

**Verdicts:**

- **R–W is metadata-only** (lyrics, CD+G graphics, cover art, control code) —
  15–40× short of any hi-res residual.
- **CD 2nd session reaches 24/48 but not 24/96.** And a *standalone* lossless
  24/96 album (~855 MB) does not even fit on a blank CD — no cleverness puts real
  24/96 on Red Book media.
- **DVD density is where carrier + residual + overhead all fit**, comfortably.

**The decisive tension:** on a CD, container capacity and audibility pull
opposite ways — the only residual that fits (24/48, depth-only) recovers the
**inaudible** axis; the residual that would recover the **marginally audible**
axis (bandwidth, ≥24/88.2) does not fit. Cross the tension only at DVD density —
where the master fits *outright* (~855 MB ≈ 18 % of a DVD) and the 16/44.1
carrier is preserving compatibility with a CD transport that cannot play the
disc anyway. That is `[P4]`'s own kicker, quantified.

**AccuDisc's slice (if ever built):** the *containers* are in scope — a second
session is **multi-session DAO** (second lead-in/lead-out + session reservation;
today only single-session DAO exists); the R–W path is the **R–W encoder + 8×
interleaver** (CD+G decode maths run backwards). The *residual codec* is out of
scope (audio processing).

---

## 7. Real-world "base audio + optionally-additive side channel"

Systems where a base stream plays on its own and a side channel can be
**enabled or disabled at will** to augment it. Grouped by what the side channel
adds. The sharpest design distinction is **legacy-transparent** (base plays on
hardware that has never heard of the side channel) vs **toggle-within-codec**
(one decoder, choose to render the enhancement or not).

**Adds channels / spatialisation:**

- **FM stereo — the archetype.** Mono base (L+R) + a 38 kHz DSB-SC subcarrier
  carrying (L−R). Mono radios play L+R and ignore the subcarrier; stereo radios
  *add* it to recover L and R. Purpose-built for backward compatibility with
  mono receivers — the conceptual root of the whole family. Legacy-transparent.
- **Dolby Surround / Pro Logic.** Surround matrix latent in an ordinary stereo
  pair; play in stereo → normal, enable the decoder → center + surround extracted.
  Legacy-transparent.
- **Quadraphonic matrix (SQ/QS; CD-4 on an ultrasonic vinyl carrier).**
  Stereo-compatible; extra channels matrixed/carried on top. Legacy-transparent
  (the CD-4 carrier needed a special cartridge).
- **MPEG Surround / Spatial Audio Coding.** Stereo downmix core + small spatial
  side data reconstructs 5.1/7.1. Toggle the spatial layer. Base is a valid
  stereo mix.
- **Dolby Atmos / DTS:X.** Object + height *metadata* carried on a 7.1/5.1 bed
  (TrueHD/DD+ for Atmos; DTS-HD MA for DTS:X). Legacy decoder plays the bed; an
  Atmos/X renderer *adds* objects. Enable/disable = exactly this.

**Adds bandwidth:**

- **Spectral Band Replication (HE-AAC / aacPlus).** AAC-LC core carries the low
  band; small SBR side data reconstructs the high band. A plain AAC-LC decoder
  ignores SBR and plays a band-limited but valid signal. **Parametric Stereo**
  is the same idea for the stereo image (mono core + tiny side info). This is the
  closest modern analogue to the HDCD *philosophy*, done properly and in-standard.

**Adds bit-exactness / resolution (core + lossless extension):**

- **DTS-HD Master Audio** — lossy DTS core (any DTS decoder) + lossless residual.
- **Dolby TrueHD** — MLP + embedded AC-3 core for legacy.
- **MPEG-4 SLS (Scalable Lossless).** AAC lossy core + lossless enhancement that
  makes it bit-exact; truncate the stream for lossy, keep it for lossless.
- **HDCD / MQA** — in-band versions (recover headroom / folded ultrasonics).

**Adds an alternate / additional program (consumer-facing toggle):**

- **Dolby AC-4 Dialogue Enhancement.** Dialogue carried separably; the *listener*
  can boost it. A shipped, user-toggleable additive side channel.
- **Broadcast audio description (receiver-mix).** Main audio + a separate
  narration track + mixing metadata, combined at the receiver; the viewer toggles
  it. Additive by construction.
- **TV MTS/BTSC SAP** — a Second Audio Program subcarrier alongside stereo.
- **Karaoke vocal stems / CD+G.** The lead-vocal as a separately muteable
  channel; "voice cancel." Enable/disable the vocal.
- **Interactive game audio (Wwise/FMOD).** Adaptive music *layers/stems* mixed in
  or out at runtime — optional-additive audio as a first-class design tool.

**Takeaways for a `[P4]`-style design:**

- The **legacy-transparent** members (FM stereo, Pro Logic, AC-3/DTS cores,
  AAC-LC+SBR) are the true model for "compatible base + optional hi-res": the base
  must be *independently valid* to a decoder that ignores the side channel.
- **SBR is the strongest precedent** for adding the one axis that matters
  (bandwidth) via a side channel — and it does so with a *separate* side stream,
  i.e. out-of-band, which is exactly §6's conclusion.
- The **counting/audibility walls (§5)** still bind: a side channel can add
  bandwidth or channels cheaply (parametric/relational), but adding *lossless
  bit-exact resolution* still costs its full incompressible entropy and needs a
  container sized for it (§6).

---

## 8. Where a separate project would start

1. Pick the **deliverable axis** honestly: bandwidth (has blind-test signal) vs
   dynamic range (does not). §3–§4.
2. Pick **in-band vs out-of-band**. In-band ⇒ HDCD/MQA-class, lossy, inaudible-
   axis-only. Out-of-band ⇒ real hi-res, needs §6 capacity ⇒ **DVD density**.
3. Pick the **side-channel model** from §7 — SBR-style relational (cheap,
   bandwidth) vs core+lossless-extension (full entropy, needs room).
4. Decide whether **backward compatibility is the constraint or the goal** — on
   DVD density it may buy nothing (§6 kicker).

AccuDisc contributes **carriage** only, and identically regardless of whether the
payload proves worth hearing.

---

## 9. The destination: a personal archival container (drop compatibility entirely)

§8's four questions all resolve the same way once you make one decision:
**abandon backward compatibility outright.** There is nothing to be compatible
*with* — the disc is read by the user's own software, not a legacy player — so
every mechanism in §5–§7 becomes unnecessary. No side channel, no in-band
smuggling, no counting argument, no Nyquist wall. You just store the files.

The complexity in §5–§7 was, in its entirety, **the tax on backward
compatibility.** Remove the constraint and the problem collapses to "a UDF disc
with files on it and an app that reads them" (§6/§7 carrier internals; a
*data* container needs none of the AV navigation or DRM apparatus).

### The pipeline (all open, all the user's own bits)

```
AccuDisc      → bit-exact CD-DA read/write            (the transport)
cdda2img      → CD → RBI image + lookups/provenance/  (the imaging layer)
                verification
[new project] → 24-bit RBI variant + metadata bundle  (the archive)
                on a UDF disc
appliance/app → GUI player reading the layout          (the presentation)
```

Because the source is **the user's own owned material**, there are no licences,
distribution rights, or manufacturing to arrange. The user builds an appliance
from spec (SBC + DVD/BD drive + OLED; a Volumio/moOde-class base does the
bit-perfect-output hard parts), installs the software, and imports their
catalogue. The "audience of one, replicated independently" model sidesteps the
ecosystem/chicken-and-egg problem that killed comparable ventures (e.g. Pono).

### 24-bit is a *container* decision, not a fidelity claim

A 16/44.1 CD rip promoted to 24-bit is a **lossless zero-pad** — 16 bits of
information in a 24-bit box. Harmless, but no audible gain (§4: 16-bit dithered
delivery is already transparent). It is justified not as an improvement to CD
material but as a **uniform lossless superset** for a heterogeneous catalogue:
16/44.1 CDs, 24/96 DVD-A, DSD→PCM SACD, all under one format.

What keeps that honest — and answers the "upscaled placebo?" objection — is
**provenance recorded per track**: the archive states the *true* source lineage
("16/44.1 CD", "24/96 DVD-A", "DSD64 SACD→PCM"). A bare FLAC filename cannot
carry that truth; a provenance-bearing format can. Honesty about origin is the
feature, and it is a stronger position than any format that merely asserts
"hi-res" on the label.

### The load-bearing spec decision: preserve native sample rate

RBI is Red Book — **44.1 kHz**. If the 24-bit variant extends only *depth* and
stays at 44.1 kHz, importing a 24/96 DVD-A or DSD→PCM SACD forces a **downsample
to 44.1**, discarding the 22–48 kHz band — the one axis with any blind-test
signal (§4). The hi-res source is then reduced to 24/44.1 and the
"not-a-placebo" justification collapses. Therefore:

- **Depth:** promote 16→24 (free, lossless zero-pad).
- **Rate:** **preserve native, per track** — 44.1 (CD), 96 (DVD-A), 88.2/176.4
  (DSD→PCM). Never normalise to 44.1.

Depth promotion is lossless; rate reduction is lossy and defeats the purpose.
This single distinction determines whether the SACD/DVD-A rationale holds.

Note DSD→PCM is itself lossy and filter-dependent (§2): an SACD source is a
*faithful rendering*, not a bit-exact copy. Record the conversion
("source: DSD64, converted via <filter>") so it is honest and reproducible.

### Why an RBI *image*, not a folder of FLACs

An RBI preserves the full Red Book **structure** — exact pregaps, index points,
ISRC/MCN, CD-TEXT, subchannel — which a track-split FLAC folder discards. That
is provenance at the *structural* level, and cdda2img already captures it. The
archive is therefore "the complete disc, structure intact, at native
depth/rate, under unified metadata," not "24-bit FLACs in a directory" — which
is why reusing RBI beats inventing a bare container.

### The through-line

Every layer's virtue is **fidelity to what the user already owns**, and none
makes a fidelity *claim* it cannot back (a 24-bit archive of a CD is honestly
labelled a CD). It is the anti-SACD: **open** where SACD was encrypted,
**honest** where MQA was a marketing axis, **permanent** (UDF + FLAC + JSON +
JPEG/PNG + CUE — all open, stable formats, readable by anything for decades)
where both are orphaned. AccuDisc's role remains carriage of the CD-DA source
only; the archive, the 24-bit container, and the appliance are a **separate
project** that this note is written to seed.
