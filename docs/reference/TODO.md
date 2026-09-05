# AccuDisc — deferred work

Ideas parked for a later session. Not scheduled; not commitments. Recovery
methods are considered complete (see `docs/reference/RECOVERY.md`); this is
everything else worth remembering.

Completed work is kept as one- or two-line summaries with any durable lesson
attached; the blow-by-blow reasoning that produced it is not retained.

## `0x5B` CLOSE TRACK/SESSION — ANSWERED 2026-09-03: `0x35` alone finalises

> **CLOSED.** The discriminator below was run on the CD-RW, 2026-09-03, reading
> **both** fields of `0x51` byte 2 (they are separate 2-bit fields and can
> disagree; `accudisc disc` shows only Disc Status, so this needed a raw read):
>
>     BLANK    byte2=0x10   Disc 0 empty      LastSess 0 empty
>     BURNED   byte2=0x1E   Disc 2 complete   LastSess 3 complete
>
> DAO + `0x35` SYNCHRONIZE CACHE finalises the disc **and** the session, with no
> CLOSE TRACK/SESSION issued. Full TOC read back, lead-out placed. **Removing the
> constant on 2026-09-02 cost nothing and nothing needs adding back.**
>
> **Scope:** CD-RW, DAO, single session. **CD-R is NOT verified** — its lead-out
> behaviour differs, and there is no CD-RW left to re-test with, so the next CD-R
> burn should re-read both fields before this is generalised. TAO and
> multi-session are out of scope for this project either way.
>
> Detail: `private/research/incoming/2026-09-03-first-cd-rw-live-burns.md` §3.

### The original question, as recorded 2026-09-02

`ADSC_OP_CLOSE_TRK_SES 0x5B` was defined in `src/mmc/cdb.h` and referenced
nowhere in the tree — no builder, no wrapper, no consumer. Removed on Keith's
instruction, **with the usability question explicitly deferred rather than
answered**: a deleted constant is not a finding, and "we don't issue it" is not
"we don't need it".

**What is actually established.** Only two things. The opcode is in the firmware
enumeration (`OPCODES.md` §B, provenance FW). Our DAO path closes the disc with
`0x35` SYNCHRONIZE CACHE alone, and that has burned correct discs read back
byte-exact — so *for single-session DAO audio*, `0x35` is empirically sufficient.
Nothing beyond that was ever tested, because the constant was never wired.

**What is NOT established, and is the research question.**

1. **Is `0x35` sufficient, or merely sufficient-so-far?** MMC defines CLOSE
   TRACK/SESSION as the command that writes the lead-out and finalises. If this
   drive's firmware performs the finalisation as part of the DAO cache flush,
   `0x5B` is genuinely redundant here — which by Keith's redundancy rule
   (SELECTOR SWEEP §3.1) puts it out of scope entirely. If it does not, then
   every disc we burn is being closed by a side effect we never asked for, and
   that is worth knowing before it stops being true on some other drive.
   **Discriminator:** burn a disc, then read `0x51` READ DISC INFORMATION and
   check the Disc Status and Last Session Status fields. Complete/finalised
   after `0x35` alone answers it. This is cheap, read-only after a burn we
   already do, and needs no new opcode.

2. **Does it become necessary the moment we are not single-session DAO?** The
   Close Function field (CDB byte 2, bits 2-0) selects close-track vs
   close-session vs finalise. Multi-session, or a TAO/SAO path, would need it.
   Both are out of scope today, so this is a *conditional* need, not a gap.

3. **Is it even implemented on the PX-716A?** Presence in the enumeration means
   the dispatch chain names it, not that it succeeds on a given medium — the
   `0xD9`/`0xF4` lesson: the command surface is media-dependent and an opcode can
   cease to exist under a different profile.

**Verification route, when this is taken up.** Item 1 first, because it is
read-only and can be answered from a burn we are doing anyway. Only if item 1
says `0x35` does *not* finalise does `0x5B` become a live candidate, and it is a
write-path command against writable media, so it is gated on §F's medium rules
like anything else. Do not issue it speculatively against a disc that matters.

**If it is later wired**, the constant comes back with a builder and a
`test_cdb.c` case in the same commit — the state this entry exists to prevent is
a third round of "there is a constant for it, and nothing uses it".

## `eject` has no MMC fallback, and the ioctl path is the one that fails — `[P2]`, raised 2026-09-03

**Measured, not hypothesised.** On a drive that had stopped answering data-in
commands, `accudisc eject` timed out three times (`rc=124` at 60 s, 90 s, 60 s),
and the drive then accepted a raw MMC eject in **1.7 s**:

    ALLOW MEDIUM REMOVAL (0x1E, prevent=0)   st=0x00 host=0x00      0 ms  OK
    START STOP UNIT stop (0x1B, LoEj=0)      st=0x00 host=0x00      6 ms  OK
    START STOP UNIT eject (0x1B, LoEj=1)     st=0x00 host=0x00   1722 ms  OK

**We do not issue `0x1B` for tray control at all.** `src/device.c:415` calls
`adsc_transport_eject`, which is the kernel's `CDROMEJECT` ioctl
(`src/transport/sgio.c:128-139` — it already unlocks with `CDROM_LOCKDOOR` first,
so the door lock is not the gap). `OPCODES.md` §B had already flagged that
eject/load bypass `0x1B` and that **the spin-down form we ship therefore has no
production record at all**; this is that gap being paid.

**The question, which is genuinely open:** should `eject` fall back to
`START STOP UNIT` when `CDROMEJECT` fails or times out? It is not free —

- the ioctl exists partly because it is **unprivileged for cdrom-group members**
  (`src/device.c:415` says so); `0x1B` through SG_IO needed an `O_RDWR` fd. Both
  were available to this user, but that is not a general guarantee.
- a fallback that fires on a *timeout* has to pick a timeout, and the ioctl's own
  is not ours to set.
- **it is a recovery path, so it must be tested by making it fire** — the house
  rule. A fallback nobody has watched trigger is not a fallback.

**Diagnostic note worth keeping regardless of the decision:** the useful signal
that night was **which class of command still worked**. No-data commands (TEST
UNIT READY, `0x1E`, `0x1B`) answered in 0-6 ms while every data-in command timed
out. That is a cheap triage step and it is what found the working exit. But note
what it did NOT do: **a timeout carries no attribution.** The fault was the
medium, and only a command that returned a real sense key (`3/02/00` MEDIUM ERROR,
from cdrecord) said so. See [[destructive-media-loops]].

### ANSWERED 2026-09-04: the two paths send the SAME CDB, and the ioctl's safety checks are NOT on our path

Keith's question was whether raw `0x1B` skips safety checks the ioctl performs.
**Read out of the kernel source at tag `v7.1` (the running kernel is 7.1.12), not
recalled.** The answer inverts the premise, and the inversion is the useful part.

**1. `CDROMEJECT` on an `sr` device IS `START STOP UNIT`, byte for byte.**

`sr_block_ioctl` (`drivers/scsi/sr.c:552`) deliberately does *not* route these two
commands through the CD-ROM layer:

    if (cmd != CDROMCLOSETRAY && cmd != CDROMEJECT) {
            ret = cdrom_ioctl(&cd->cdi, bdev, cmd, arg);
            if (ret != -ENOSYS)
                    goto put;
    }
    ret = scsi_ioctl(sdev, mode & BLK_OPEN_WRITE, cmd, argp);

`scsi_ioctl` (`drivers/scsi/scsi_ioctl.c:925-928`) then does:

    case CDROMCLOSETRAY:  return scsi_send_start_stop(sdev, 3);
    case CDROMEJECT:      return scsi_send_start_stop(sdev, 2);

and `scsi_send_start_stop` (`scsi_ioctl.c:250-257`) builds `cdb[0] = START_STOP`,
`cdb[4] = data`. Our own builder, `adsc_cdb_start_stop(cdb, start=0, loej=1)` at
`src/mmc/cdb.c:14-19`, computes `cdb[4] = (1 << 1) | 0 = 2`. **Identical CDB.**
There is no separate, safer kernel eject mechanism to lose.

**2. The checks Keith had in mind are real, but they are bypassed.**

`cdrom_ioctl_eject` (`drivers/cdrom/cdrom.c:2274-2288`) does carry genuine guards:

    if (!CDROM_CAN(CDC_OPEN_TRAY))          return -ENOSYS;
    if (cdi->use_count != 1 || cdi->keeplocked) return -EBUSY;
    if (CDROM_CAN(CDC_LOCK)) { ret = cdi->ops->lock_door(cdi, 0); ... }

Refusing to eject while another process has the device open, and while the door is
held locked, is exactly the protection worth wanting. **It is not on our path and
has not been since Linux 5.18.** Verified by tag: absent in `v4.19`, `v5.10`,
`v5.15`; present in `v5.19`, `v6.0`, `v6.2`, `v6.6`, `v7.1`.

The bypass is deliberate, not an accident. It was intended by `2e27f576abc6`
("scsi: scsi_ioctl: Call scsi_cmd_ioctl() from scsi_ioctl()", 2021-07-29), which
shipped with a typo — `ret != CDROMCLOSETRAY` instead of `cmd != ...`, and `ret`
is 0 there, so the condition was always true and the restrictive path kept
running. `bc5519c18a32` (2022-03-30, Kevin Groeneveld, Reviewed-by Christoph
Hellwig) fixed the typo and says why in as many words:

> This changes the behaviour of these ioctls as the cdrom_ioctl handling of these
> is more restrictive than the scsi_ioctl version.

So the kernel maintainers chose the **less** restrictive path for `sr`, knowingly.

**3. What the ioctl path DOES give us — and SG_IO gets it too.**

Two protections live in `sr_block_ioctl` above the dispatch, not in `cdrom_ioctl`:

- `scsi_ioctl_block_when_processing_errors()` (`scsi_ioctl.c:975-988`) — returns
  `-ENODEV` rather than queueing while the SCSI error handler owns the device.
- `scsi_autopm_get_device()` — resumes a runtime-suspended device first.

**A raw `SG_IO` on our `/dev/sr0` fd reaches the same function and therefore gets
both.** Traced: `SG_IO` does not appear anywhere in `block/ioctl.c` (v7.1), so
`blkdev_common_ioctl` returns `-ENOIOCTLCMD` and `blkdev_ioctl` falls through to
`bdev->bd_disk->fops->ioctl` (`block/ioctl.c:795-797`), which is `sr_block_ioctl`
via `sr_bdops`. We open the block node (`src/transport/sgio.c:23`), not `/dev/sg*`,
so this is our actual path.

**Therefore: an MMC fallback loses no safety check whatsoever.** The cdrom-layer
guards are already bypassed for us, and the two SCSI-layer guards are shared.

**4. Where the paths genuinely differ — and this is the design input.**

| | `CDROMEJECT` ioctl | our `SG_IO` `0x1B` |
|---|---|---|
| CDB | `1B 00 00 00 02 00` | `1B 00 00 00 02 00` — same |
| timeout | `START_STOP_TIMEOUT` = **60 s** (`scsi.h:139`, read from the running kernel's own header at `/lib/modules/7.1.12_1/build/include/scsi/scsi.h`) | `ADSC_TIMEOUT_CTRL_MS` = **30 s** (`src/transport/transport.h:18`) |
| retries | `NORMAL_RETRIES` = **5** (`scsi_ioctl.c:30`), so up to 6 attempts | **none** — one attempt |
| sense data | **discarded.** `ioctl_internal_command` (`scsi_ioctl.c:69-124`) inspects the sense, prints it to the kernel log, and returns a bare errno | **returned to us**, key/ASC/ASCQ intact |
| door unlock | none on this path (`lock_door` was in the bypassed `cdrom_ioctl_eject`) | none — we call `CDROM_LOCKDOOR` ourselves first (`sgio.c:133`) |

The ioctl is the *more patient* path, not the more careful one: up to ~360 s of
aggregate attempts against our single 30 s. That disposes of "the ioctl gave up
too early" as an explanation for anything.

**The decisive difference is the sense data.** `ioctl_internal_command` swallows
it and hands userspace an errno. This project already has "**a timeout carries no
attribution**" written down as a lesson paid for in hardware — and here is the
kernel enforcing exactly that loss of attribution on our only tray-control path.
An MMC fallback that surfaces `3/02/00 MEDIUM ERROR` instead of `EIO` is worth
having **on diagnostic grounds alone**, independently of whether it opens trays
faster.

**What this does NOT establish.** It does not explain the three timeouts of
2026-09-03. That incident has several uncontrolled variables — a disc that was
failing, a recovery sequence that sent `0x1E` and a spin-down *before* the eject,
and elapsed time between the attempts — and there is no CD-RW left to re-run it
on. The table above is a fact about the code. Attributing the incident to it would
repeat the same night's actual error, which was reasoning from a timeout to a
cause. **Left unexplained, deliberately.**

**Still open after this, and unchanged by it:** whether to add the fallback at all,
what trigger to use (the privilege argument above is unaffected — `0x1B` via SG_IO
still needs an `O_RDWR` fd), and the house rule that a recovery path must be tested
by making it fire.

---

## CD-RW MEDIA SAFEGUARDS — what the 2026-09-03 disc actually establishes — raised 2026-09-04

Research file: `private/research/incoming/2026-09-04-cdrw-rapid-cycling-failure-modes.md`.
Every claim below marked **[verified]** was read this session out of the cited
primary source; **[inference]** means exactly that.

### The verdict is three mechanisms ELIMINATED and one PLAUSIBLE — not a diagnosis

| mechanism | status | why |
|---|---|---|
| PCA / OPC exhaustion | **refuted** | count and sense code both wrong |
| Phase-change alloy fatigue | **refuted** | off by ~30x on cycle count |
| Drive laser / optics overheating | **refuted** | drive verified healthy after |
| Lead-in / PMA / ATIP destruction | **plausible, unproven** | fits the signature; no direct evidence |

**Do not let the fourth row inherit the confidence of the first three.** They are
refuted from standards; it is a hypothesis that survived.

### PCA exhaustion — refuted twice over

- **[verified, ECMA-395 §5.3]** Each PCA has a Test Area of **100 partitions**,
  and a disc carries **two** (PCA1 and PCA2). We ran ~37 calibrations. Even if one
  OPC consumed several partitions we would not be close. *(That one OPC consumes
  exactly one partition is an [inference]; the arithmetic survives either way.)*
- **[verified, ECMA-395 §5.3 and the partially-recorded-disc definition]** *"once
  all partitions have been used, the total PCA1 or PCA2 must be CW-erased, after
  which it is available for the next sequence of power calibration procedures."*
  **On CD-RW, a full PCA is a recycle event, not death.** This leg depends on
  firmware honouring the standard; the count above does not.
- **[verified, MMC-5 ASC/ASCQ table]** The condition has its own sense codes —
  `1/73/01` ALMOST FULL, `3/73/02` IS FULL, `3/73/03` ERROR, plus `3/73/10` and
  `3/73/11` for the *current* PCA. We got `3/02/00` NO SEEK COMPLETE.

### Alloy fatigue — the "1000 cycles" figure, and why comparing 37 to it is INVALID

**[verified, ECMA-395 §18.4b]** requirement "Number of DOW cycles **at PW = PWO**:
**> 1000**". That is the traceable origin of the rated figure quoted on packaging.

**An earlier version of this section then said "37 is not in the same order of
magnitude", and that comparison is wrong.** It compares two different quantities.
Read what the rating actually specifies:

- **[verified, §18.4b]** the cycles are **DOW** — Direct OverWrite, the modulated
  write/erase pass — **at PW = PWO**, optimum write power.
- **[verified, §18.1]** measured with Random EFM, i.e. ordinary data.
- **[verified, §18.5]** "over one disc **at a fixed CLV**" — distributed, not
  concentrated on one band.

`cdrecord blank=fast -force` is none of those:

- **[verified, §13.1.4]** it is (or may be — see below) a **CW erase**: continuous
  unmodulated laser, and the standard says outright that it *"may reduce the
  maximum number of DOW cycles"* and should be confined to PCA1/PCA2.
- **[verified, §18.2.4]** CW-erase power is **PECW ≥ 1.15 × PEO** — a *higher*
  power operation than the erase half of a DOW cycle.
- **[verified, MMC blanking type 001b]** it touches only PMA / lead-in / first
  pregap — one narrow band, taking every single operation.

**So the honest statement is: the 1000-cycle rating never covered this usage.** It
is a distributed, optimum-power, modulated-overwrite figure; we ran up to 37
concentrated, higher-power, unmodulated erases into one band. The standard does
**not** quantify how much CW erase reduces DOW life, so this explains **how
destruction at 37 operations is possible** — it does **not** establish that this
is what happened. Alloy fatigue *in the sense Keith proposed* (whole-disc, rate-
driven, cool-down would help) remains unsupported; localised degradation of the
lead-in band by a method the standard warns against does not.

**[inference]** Whether this firmware's `blank=fast` actually uses CW erase or
logical erase on the PMA is still unknown, and it is the hinge.

### What DOES survive from Keith's "burns it out" intuition — and it is primary source

**[verified, ECMA-395 §13.1.4 Physical Erase]**, quoted:

> By writing with a continuous laser power of about PEO, the overwritten track
> will be left in the high-reflective state... **The maximum number of DOW cycles
> may be reduced by this procedure**, therefore it is recommended to use this
> erase method only for erasing the PCA1 and PCA2 areas, where the presence of
> previously written marks could disturb the OPC procedure.

**[verified, §13.1.5]** Logical erase (overwrite with zero-content EFM) is
recommended everywhere else and *"will cause less reduction of the maximum number
of DOW cycles."*

So the standard states outright that **erasing has a cost to cycle life beyond the
cycle itself** — but the variable is the **erase method** and the **region**, not
the rate. Nothing in the standard supports a cool-down period.

**[inference, NOT verified]** Whether `cdrecord blank=fast` triggers physical or
logical erase of the PMA/lead-in on PX-716A firmware is **unknown**, and it is the
hinge of the surviving hypothesis. We cannot test it: no CD-RW.

### Bricking rewritable media in a handful of cycles is DOCUMENTED, and the field mechanism is WRONG WRITE POWER — not wear-out

**[verified 2026-09-04 by reading the source pages directly, not via the agent's
summary]** Gough Lui's 2026 DVD±RW endurance testing (measurement class:
automated back-to-back cycling on a Lite-On iHAS120, with quality scans and
visual inspection) recorded permanent, unrecoverable media death at cycle counts
of **1** and **6**:

- *SWTechnology 2.4x DVD+RW* — "it burned at 2.4x just fine, but then, the disc
  was **completely unrecognisable by the drive** afterwards. **The first burn
  killed the disc** and there was nothing visual about the disc that would
  explain why."
- *Imation/JVC* — "after the **6th run**, the drive simply would not write to the
  disc anymore claiming of an **Illegal Medium Format**... the claim that the
  **embossed section** of the DVD-RW disc would somehow fail after rewriting. We
  can see that there is some sort of **dark areas developing**".
- *Prodisc DW06* — "the drive appears to have **murdered the disc**, causing the
  disc to have an **unrecognised format**. Visually... the **inner tracks** appear
  to be written in such a way to be unusually bright compared to the rest of the
  disc. **It seems the wrong power has been used and may have damaged the
  recording layer permanently.**"
- *Philips* — "the disc's formatting is **permanently damaged**"; the author
  speculates a "firmware bug or perhaps some vulnerability where **incorrectly
  read disc metadata could result in incorrect burning of media**".

**Why this matters more than everything above it.** The failures are not
cycle-count fatigue — they happen at 1 and 6 cycles. The observed mechanism is
**mis-calibrated write power damaging the recording layer permanently**, the
damage is concentrated at the **inner diameter** (where the PCA, lead-in and PMA
live), and in one case the **embossed pregroove itself** degraded. That is the
same region our 37 operations were concentrated on, and it makes the failure a
**calibration** failure rather than a wear-out.

**[inference]** It also supplies a positive-feedback path that our incident's
shape fits: a marginal lead-in yields bad calibration input, OPC then selects a
wrong power, the wrong power further damages the lead-in, and the next cycle is
worse. That accounts for a *progressive* decline (which we measured — burn times
went bimodal before the fatal erase) ending in *abrupt* unrecognisability. It is
not proven and we cannot prove it without media.

> **SCOPE GAP, stated plainly:** these are **DVD±RW on a Lite-On**, not CD-RW on a
> Plextor. Different standard, different layer stack, different drive. What
> transfers is the *class* of failure (phase-change rewritable, OPC-calibrated,
> inner-diameter control area) and the demonstration that low cycle counts brick
> media. The specific numbers do not transfer.

### THE PHYSICS: what writing at the wrong power does to the recording layer

All quotations verified this session from ECMA-395 (`docs/research/`) and Ohta
2001 (retrieved via Wayback; see the research file's Sources).

**Normal operation is a controlled melt-and-quench.** **[verified, §13.1.1]**
Recording heats the sensitive layer in the groove with a modulated laser. A
*mark* is a region of reflection `<< Rtop`; erased areas sit at `Rtop`.
**[verified, Ohta]** The written portion "is melted and after the laser spot moves
away from that portion, it is **quenched immediately**. This quenching process
changes the portion to be an amorphous mark". Erase raises the temperature "over
the crystallizing temperature" *without* melting, and the mark recrystallises.
Ohta puts the critical cooling rate at **3.4 K/ns** — amorphisation is a race
between cooling and crystallisation, so the whole scheme depends on landing the
deposited energy inside a narrow band.

**The band is ±10%. [verified, §18.4a]** "Write power window of a disc: for
`0.90 PWO < PW < 1.1 PWO` … disc must be recordable within specifications". The
standard specifies a *window*, not a maximum — outside it, nothing is guaranteed.
That window is what OPC exists to find, and it is why a mis-calibration of a few
tens of percent is not a rounding error.

**Too little power:** the layer does not fully melt (incomplete amorphisation,
weak modulation), or on erase does not stay above the crystallisation temperature
long enough (incomplete recrystallisation, residual marks). The result is a
region that is optically neither properly written nor properly blank.

**Too much power** drives the failure modes Ohta names, all of them thermal:

1. **Grain growth in the dielectric.** "Grain growth in the ZnS layer was one
   reason the phase-change optical disks degraded after many rewrites."
2. **Sub-nanometre displacement of the active layer**, from asymmetric thermal
   expansion along the scan direction: "The space deformation becomes the motive
   force of the sub-nanometer displacement of the **liquid phase** active layer
   components."
3. **[verified, US 6,091,698]** repeated overwriting causes "a gradual **flow of
   the phase change material** from one portion of the medium to another,
   resulting in a degradation of the material".

**Note "liquid phase" in (2) — it is the whole answer to why POWER rather than
COUNT is the governing variable.** Mass transport happens while the material is
molten. More power means more energy above the melting point, more time spent
molten, and more irreversible transport *per pass*. A cool-down between cycles
does nothing about it, because the damage is done in the nanoseconds while the
spot is hot, not in the seconds afterwards.

**Important caveat on (1) and (2): these are the SOLVED mechanisms.** Ohta is
describing what took cyclability from thousands to >10⁶ — grain growth was fixed
by the ZnS-SiO₂ mixture, space deformation reduced by an added SiO₂ layer. They
explain **why power matters at all** in a disc operating *within* spec. They are
not, by themselves, an account of our failure.

**Why layer damage BRICKS a disc rather than merely corrupting data — the link to
our sense code.** **[verified, §13.1.2]** Tracking is derived from the push-pull
signal: "An off-track position of the scanning spot results in a diffraction
pattern that is asymmetrical in the radial direction… Subtraction of the powers
diffracted into the two halves of the aperture… yields a servo signal for track
following." **[verified, §13.5, normative]** the Normalized Push Pull Ratio —
push-pull before recording over push-pull after recording — is required to be
**0.5 – 1.3**, and the stated reason is:

> because the **servo electronics have to deal with both recorded and unrecorded
> parts** of a partially recorded disc, and so with two different Push Pull
> signals. As the **dynamic range of the servo electronics is limited**, the
> allowed ratio in Push Pull signals should be specified.

The normalising quantity after recording is `Iga`, "the averaged groove level
after recording… actually used by the servo electronics for tracking in a
recorded groove."

**[inference — this joint is ours, the links above are all sourced]** A region
written at the wrong power has a post-write groove level far from where the servo
expects it. Push it far enough and NPPR leaves the 0.5–1.3 window, the servo's
limited dynamic range is exceeded, and the drive cannot follow the track — which
is what `3/02/00` NO SEEK COMPLETE looks like from the host. Damage the region
that carries the lead-in and PMA and the drive cannot even classify the medium,
which is our observed `profile=0x0000`.

This also makes Gough Lui's visual finding and the NPPR requirement **the same
fact seen two ways**: his inner tracks "written in such a way to be **unusually
bright** compared to the rest of the disc" is a region sitting near or above
`Rtop` where it should be carrying marks — a gross shift in exactly the quantity
`Iga` normalises against.

**What this does NOT establish.** We have no read of our own disc's layer. The
physics explains how wrong power destroys a disc permanently; it does not show
that this is what happened to ours. The ATIP read below is still the only
measurement that would localise it.

### The counter-case: what real PCA exhaustion looks like, and it is RECOVERABLE

**[verified in the report's sources]** Launchpad bug #66710 records `cdrecord`
blanking a CD-RW and returning `3/73/03` POWER CALIBRATION AREA ERROR with "OPC
failed" and "Cannot blank disk, aborting" — **and the disc was not destroyed.**
That is the signature we did *not* see. Field reports also span "hundreds of
successful rewrites" against "failures after ~9" (VideoHelp, anecdote class), a
spread far too wide for cycle count to be the governing variable.

### `3/02/00` is a documented CD-RW *write-refusal* code, not necessarily a servo fault

**[verified, MMC-5 r04 line 9125-9130]**, on High Speed CD-RW media:

> Upon a write attempt to the High speed CD-RW media using a Drive that is only
> compliant with [CD-Ref9], some Drives return CHECK CONDITION Status and set
> SK/ASC/ASCQ values to either ILLEGAL REQUEST/WRITE PROTECTED **or MEDIUM
> ERROR/NO SEEK COMPLETE**. The recommended SK/ASC/ASCQ values for this case are
> ILLEGAL REQUEST/CANNOT WRITE MEDIUM – INCOMPATIBLE FORMAT.

**Scope, honestly:** the passage describes a *legacy* drive meeting high-speed
media. Our PX-716A is Ultra-Speed capable and pinned 24x, so the literal scenario
does not apply. **[inference]** What transfers is that `3/02/00` is a known,
sloppy vendor way of saying **"this drive has concluded it cannot write this
CD-RW medium"** — a classification/compatibility answer, not proof that the
servo failed to seek. Read our sense code that way and it stops being anomalous.

### Safeguards — what is actually implementable

**1. Name the `0x73` family in the write path. `[P2]`**
`src/write/burn.c` special-cases only `2/04/08`, `5/24/00` and `5/20/00`
(`burn.c:212, 335, 484`); a PCA sense would abort as a generic `ACCUDISC_ERR_SENSE`
— right outcome, no diagnosis. Naming `73/01`, `73/02`, `73/03`, `73/10`, `73/11`
costs nothing and turns an opaque failure into a stated cause. This is the
"a timeout carries no attribution" lesson applied before the fact.

> **Verify first, or it may be dead code:** `1/73/01` is sense key 1, RECOVERED
> ERROR. The Linux midlayer dispositions RECOVERED ERROR as SUCCESS for its own
> retry logic (`drivers/scsi/scsi_error.c:615`, `case RECOVERED_ERROR: return
> SUCCESS`). Separately, SG_IO sets `SG_INFO_CHECK` whenever the status byte is
> CHECK CONDITION (`scsi_ioctl.c:400-409`), and our transport returns
> `ACCUDISC_ERR_SENSE` on that with the sense intact (`sgio.c:74-81`), so it
> *should* still reach us. **[inference]** — untested on hardware. Establish it
> before relying on an ALMOST FULL warning.

**2. PCA remaining capacity is NOT readable over standard MMC. [verified]**
The Count Area is not LBA-addressable, and READ DISC INFORMATION's OPC tables hold
cached per-speed calibration *values*, not a consumed-partition counter. **Do not
design a budget around a counter we can query — there isn't one.** Any budget is
session-scoped and covers only our own writes; an external tool's erases are
invisible to us (22 of the 37 that night came from `cdrecord`).

**3. Never force-blank a disc whose state you could not read.** The 2026-09-03
loop fired `blank=fast -force` eight times into a disc that no longer answered.
Already a rule in `LIVE_BURN_QUEUE.md`; it belongs in code, not only in prose.

**4. PROPOSAL, needs Keith's decision — make `--simulate` the default for harness
and repeated-run work.** `burn.c:475` already skips SEND OPC in simulate, so a
simulate run costs the medium nothing. This is a user-facing behaviour change and
must not be implemented unilaterally.

### Correction applied elsewhere

`8a4f9a0` landed the PCA-exhaustion explanation into tracked docs. Corrected
2026-09-04 in `LIVE_BURN_QUEUE.md`, `src/write/burn.c`, and the
`destructive-media-loops` memory. **The operational rules in all three are
unchanged** — they were derived from what the loop did, not from why the disc died.

### Open, and cheap enough to be worth it

- Does `blank=fast` physically or logically erase the PMA on this firmware? Needs
  CD-RW. **Blocked, permanently, unless media reappears.**
- Does a RECOVERED ERROR sense reach userspace through our SG_IO path? Testable on
  **any** disc with a command that returns one — free, no media consumed.

---

## POST-BURN VERIFICATION — three tiers, built 2026-09-05 (0.35.0)

Keith's instruction: two methods, one generic and one advanced, "otherwise we
end up building a tool that only works with Plextor drives." Built as **three**
tiers rather than two, because the C2 middle rung is standard MMC yet still has
to be *probed* — a drive can claim C2 it does not honour, and `accudisc_features`
already carries the functional verdict that tells the two apart.

| tier | question answered | requires |
|---|---|---|
| 0 `compare` | does the disc read back as the bytes I sent? | nothing — **any** drive |
| 1 `c2` | did the drive have to work for it? | the C2 capability **probe**, not the claim |
| 2 `counters` | how much margin is left? | an attached vendor driver |

**They are not one question at three resolutions, and that is the point.** Tier
0 is post-CIRC and read-side: the drive has already spent every correction it
has by the time we see the audio, so a disc can pass tier 0 perfectly while
sitting on the edge of its budget. CIRC hides exactly the degradation a
post-burn verify exists to find. `accudisc_verify_result.tier` therefore states
which question was answered, and the CLI prints a separate `quality=` token
(`not_measured` / `unrated` / `rated`) so those three states cannot be
collapsed into "PASS".

**Tier 0 stands alone.** That is the requirement, restated as a property: a
byte compare against the source needs no driver, no capability and no vendor.

### Alignment is MEASURED, never applied — and it nearly went wrong twice

A read-back is displaced from the source by the drives' combined write and read
offsets, so a naive byte compare reports total mismatch on a flawless disc. The
library will not silently shift to fix that: **"AccuDisc REPORTS offsets and
never applies one"** (`accudisc.h`, offsets section) exists so no consumer gets
a second correction site, and double correction is invisible — well-formed PCM,
wrong by twice the offset. So the verify *locates* the displacement, states it
in `shift_samples`, and compares at it. `aligned == 0` reports no comparison at
all, because an unaligned compare is not a failed compare.

Two defects found by falsifying the guards rather than by reading them:

1. **The sign convention was contradictory** — the header said positive meant
   the read-back ran LATE, the implementation comment said EARLY. Settled by
   writing the equation instead of the prose: `disc[j] == source[j + shift]`,
   so positive means EARLY, matching the offsets section. `test_verify` case 2
   pins it at a known +667, and a sign flip fails that case.
2. **The uniqueness rule was a RATIO and it was wrong for damaged discs.** The
   first version required the runner-up shift to be ten TIMES worse, which is
   fine while the best shift is near-perfect and arithmetically unreachable
   once it is not: with 30% of the anchor differing, ten times worse is 300%,
   so a badly-burnt disc — the one a verify exists for — would have come back
   "cannot align" instead of "aligned, and 30% wrong". Replaced with a
   SEPARATION rule: the runner-up must be worse by a fixed fraction of the
   window. Silence gives `best == second == 0` and separates by nothing
   (refuse); heavy damage separates by most of the window (accept).
   `test_verify` 8d and 8e pin both ends.

### What the falsification pass established

Every load-bearing rule was broken deliberately and watched fail: the shift
sign, the separation rule, the absolute "is this even the same audio" limit,
the unaligned-means-no-comparison rule, `require_tier`, the census-failure
degrade, the arm-is-not-a-readout check, and the C2 UNVERIFIED-is-not-SUPPORTED
gate. **One guard did NOT register: the anchor entropy check.** That is
correct rather than a gap — the separation rule catches silence on its own —
and the source now says so explicitly, so nobody reads the early-out as the
guarantee or deletes the margin thinking it redundant.

### The threshold that is deliberately absent

`verify` ships **no default quality limit**. A pass/fail line across C1 counts
is a judgement, and a judgement shipped without a cited source is a judgement
wearing the costume of a measurement — the failure this project is named
after. `--max-bler N` takes one from the caller and yields
`result=marginal`; without it the counts print and the disc is left
`quality=unrated`. **[open]** Establishing a defensible default means citing a
primary standard, which has not been done this session.

### The verdict lives in the CLI, not the library

`API_PLAN.md` §5.3 pins only the census cadence, so this was open. Decided by
CLAUDE.md's invariant that relative checks never outrank absolute gates: a
threshold on C1 is a relative check being promoted to a gate, and freezing one
into the public ABI makes every consumer inherit our judgement. The library
reports; `cli/main.c` renders. Same split as exit codes and terminal output
(API_PLAN §3).

## MEDIA — the Ritek CD-R stock, ATIP read 2026-09-05

50 blanks, JVC-branded, **Ritek-manufactured**. ATIP read off one on hardware:

```
atip leadin=97:15:17 leadout=79:59:70 type=CD-R manufacturer=Ritek
```

**This is the reassuring result, and it is worth stating why.** The CD-RW
post-mortem's surviving hypothesis is lead-in destruction by a mis-calibrated
write power, and the route by which that happens to an honest tool is FAKED
ATIP — a disc declaring a manufacturer code it is not, so the firmware resolves
a strategy for the wrong dye. Here the ATIP declares Ritek and the disc *is*
Ritek: JVC-branded Ritek media is ordinary, not a substitution. The stock is
self-consistent with its branding, which is the opposite of the counterfeit
case. **[verified]** on hardware; it says nothing about the other 49 discs,
which are presumed the same spindle but not measured.

`97:15:17` is not a literal row in `media_atip_db.inc` — the nearest is
`97:15:10`. That is **not** a partial match dressed as a definite one: the
lookup matches on the frame DECADE by design (`media_db.c:8-12`), because a
manufacturer owns a whole `97:SS:Fx` range and real discs carry frames like
`:17` against a `:10` entry. cdrecord uses the same rounding rule. Checked
rather than assumed, because "manufacturer=Ritek" from a table with no such row
is exactly the shape a silent wrong answer takes.

**Ritek is not Taiyo Yuden and the difference is real** — thinner dye
tolerances, more strategy sensitivity — but nothing here suggests the stock is
misdeclared, and misdeclaration was the specific risk. Ordinary care applies;
alarm does not.

## SELECTOR SWEEP — close the multiplexer gaps, then wire what qualifies — PLAN ONLY, agreed 2026-09-01, to run at the weekend

Keith's five steps, in his order: (1) find which opcodes and pages are still
undocumented, (2) devise tests to query them, (3) decide which are actually
useful here, (4) wire those, (5) test unit/fixture **and** production. Added
2026-09-01 during planning: **document the opcodes that are redundant because
they duplicate standard MMC** — §3.1, which is a disqualifier in step 3 and a
deliverable in its own right.

Full status matrix and the gap list: **`docs/reference/OPCODES.md`** — §E is the
multiplexer audit this plan executes, §G the gaps. Do not duplicate its tables
here; that file is the source of record.

**Nine opcodes carry a selector byte and we occupy one value of most of them.**
`0xE9` is not special, only mapped. Excluded from every phase below, by the §F
safety classes: `0x3B` WRITE BUFFER, `0xE3` PlexEraser, `0xEE` reset, `0xF2`,
`0xF8`, `0xDE`/`0xDF` (all three on `pxfw`'s own blacklist).

### The two rules that govern the whole plan

**A. Nothing is swept until its discriminator is established.** A drive that
returns eight well-formed bytes for a page it does not implement is
indistinguishable from a page that is implemented and reads all zero. That is
this project's dominant failure mode — an output too well-formed for anything
downstream to reject. So every sweep is preceded by a control run proving the
drive can return **both** answers, exactly as `0xD9`/`0xF2`/`0xF4` were gated on
`0x12` (positive) against `0xC1`/`0xC5` (negative). No controls ⇒ the sweep does
not run.

**B. Every result is scoped to the medium it was taken on.** `0xD9` and `0xF4`
*cease to exist* under a DVD — `5/20/00`, the same sense an unassigned opcode
returns. The command surface is media-dependent, so "page N is not implemented"
is only ever "not implemented on this profile". Every deliverable below is a
table indexed by **(selector value x medium)**. Three rules about these opcodes
were withdrawn in one session for exactly this, all the same shape: a rule
inferred from N media states, falsified at N+1.

Standing: `flock /var/tmp/sr0.lock` throughout (contention collapses Q 99%->13%
while audio stays clean, i.e. it reads as a bad disc); trace every CDB to stderr
**unbuffered before issuing**; before/after disc-state snapshot; GET/read forms
only during discovery, never SET.

---

### Phase 0 — discriminators and static RE. No drive for the RE half.

**0.1 Establish the `0xE9` page discriminator.** The GET response is a fixed
8-byte data-in with `resp[0]` = page echo and `resp[1]` = constant `0x06`. The
echo is the candidate discriminator. Prove it before trusting it: run `0xBB`
(known implemented) against at least two pages expected absent, and record
whether an absent page gives sense, or good status with a zero/absent echo. **If
both cases produce identical well-formed blocks, the sweep is not possible by
this route** — say so and stop, rather than producing 256 rows of nothing.

**0.2 Establish the `0xED` discriminator.** Mode code 0 (POWEREC) is the positive
control. **There is no negative control yet** and one is required before mode
codes 1..255 mean anything. Same stop condition as 0.1.

**0.3 Static RE on `PTPXL.exe` — zero drive risk, runs in parallel.** Bind the
`0xDF` selector bytes (`ec`/`f1`/`f6`/`f2`, four builders, one dialog cluster
around `0x728250`-`0x728f19`) and `0xDE`/`0xE1`/`0xE2`. This is the standing
"live lead" and it is the only route to `0xDF` that does not touch a blacklisted
opcode. Feature strings sit several levels up the C++ dialog hierarchy.

Deliverable: a discriminator verdict per opcode (works / does not work / stop),
plus whatever `0xDF` selector bindings the RE yields.

---

### Phase 1 — what the changeable bits MEAN. No drive at all.

Presence and changeable masks for the 11 mode pages are already measured
(`PROTOCOL.md` "Mode pages — 11 present"). Re-reading the payloads adds nothing.
**The missing thing is what each host-changeable field does**, and MMC-5 declares
these pages legacy and defers to MMC-3.

**1.0 Reference material — RESOLVED 2026-09-01, no acquisition needed.** We have
MMC-3: `private/research/mmc3r10g.pdf` in this repo (cdda2img holds an
independent copy of the same file — same size, different inode, not hardlinked).
The gap was narrower than "we need MMC-3": MMC-5 ships with a pre-extracted
`.txt` beside its PDF and MMC-3 did not, and it was filed under
`private/research/` rather than `private/code/MMC/` where the spec material
lives, so a search of the obvious place missed it. **Fixed:** `pdftotext -layout`
output now sits at `private/code/MMC/mmc3r10g.txt` (1.3 MB, §6.3.6 verified
present at line 16095) beside `mmc5r04.txt`. The PDF was left where it is.
MMC-6 (`mmc6r02g.pdf`) exists in cdda2img's research tree if a third opinion is
ever wanted. All of it stays under git-ignored `private/` — licensed T10
material, summaries fine, the source never leaves.

**1.1 Extract MMC-3 field definitions** for the changeable bits of pages
`01 02 07 08 0d 0e 1a 1d` from `private/code/MMC/mmc3r10g.txt`, into a table of
(page, byte, bits, name, semantics, values). Precedent: page `0x0D`'s only
changeable field was read out by hand on 2026-09-01 and turned out to be the
**Inactivity Timer Multiplier** — *hold track state* after a seek, 125 ms to
32 min — **not** a spindown control, which is what this file and `FEATURES.md`
had loosely implied. The changeable mask `00 0f` matches MMC-3's 4-bit field
exactly, so the mask independently corroborates the spec layout. Expect more of
these; expect at least one more mislabel.

**1.2 Then, and only then, read the payloads** of those 8 pages on the drive as a
cheap confirmation of 1.1. Standard MMC, read-only, no vendor opcode, no medium
precondition. This is the confirmation step, not the substance.

**1.3 `0x43` READ TOC formats 1 (multi-session info) and 3 (PMA)** — the two
formats we never issue. Zero risk, same reasoning.

Deliverable: a field dictionary for the eight pages, plus a note per page saying
whether it is a lever we could use or a legacy field with no CD-DA meaning.

---

### Phase 2 — vendor selector sweeps, medium-indexed. Gated on Phase 0.

Runs only for opcodes whose discriminator passed Phase 0. Media axis, decided up
front: **pressed audio CD, pressed data CD-ROM, blank CD-R** — the three CD
profiles already characterised. A DVD arm is optional and read-only; its value is
showing which selectors vanish, not what they do.

| # | opcode | selector | space |
|---|---|---|---|
| 2.1 | `0xE9` | CDB[2] page | 10 identified of 256; GET only, `CDB[1]=0x00` |
| 2.2 | `0xED` | CDB[2] mode code | 1 known of 256; GET form only |
| 2.3 | `0xEA` | CDB[2] scan type | `0x00` and `0x10` documented; sweep the rest |
| 2.4 | `0xF3` | CDB[1]/CDB[2] | the **6-call-site builder** `PROTOCOL.md` calls "likely a get/set dispatcher" — never probed |
| 2.5 | `0xF1` | CDB[1] sub-cmd | `0x01` known (EEPROM read); others unknown |
| 2.6 | `0xE4` | CDB[1]+CDB[2] | 3 builders; MQC arm is DVD-only |

`0xF3` (2.4) is the highest-value item here: a six-site builder with a
runtime-supplied byte 1 is the same shape `0xE9` turned out to have, which makes
it the best candidate for a second feature cluster.

**A GET is not automatically safe.** `0xF2` with a non-zero parameter ran a
minutes-long physical operation and needed a power cycle, with the drive
answering nothing meanwhile. Give unknown selectors minutes-long SG_IO timeouts,
**do not read a timeout as a hang**, and gate on the medium regardless of
direction — an opcode's behaviour on read-only media says nothing about its
behaviour on writable media.

Deliverable: per opcode, a (selector x medium) table with sense codes, and a
verdict of implemented / absent / **undetermined**. "Undetermined" is a permitted
and expected outcome; do not round it to either neighbour.

---

### Phase 3 — relevance. Narrow criterion, decided now so the weekend does not relitigate it.

**Qualifies only if it serves one of: CD-DA read, the DAO write path, or the
frame-accurate status surface.**

- **DVD-only ⇒ out.** Book Type, Test Write, `0xE4` Media Quality Check, the
  `0xEA` DVD arms (PI/PO, PI sum8, POE, PIF).
- **Write-time-only ⇒ deferred** while the burn path is paused: GigaRec,
  VariRec. Identified and GET-verified already; their *effects* need a burn.
- **Expected to qualify:** Silent Mode main (`0xE9` page `0x08`), Single Session
  / Hide CD-R (page `0x01`), mode page `0x01` byte 3 read retry count, and
  `0xEA` `CDB[2]=0x10` Jitter/Beta.

A GET-verified page is not a qualified feature: it means the exchange works, not
that the feature was proven to do anything.

#### 3.1 Redundancy — the "do not build this" list

**A second disqualifier, alongside the DVD and write-time filters: an opcode that
duplicates a standard MMC command we already issue.** This is the direct
counterpart of the vendor-isolation rule — a vendor opcode earns its place only
by doing something generic MMC cannot, and one that merely re-implements a
command already in `src/mmc/` adds a driver dependency for nothing.

**Keith's framing, 2026-09-01, and it upgrades this table from documentation to
architecture: the redundancy table IS the gate between the generic MMC core and
the vendor driver.** It is not merely a "do not build this" list; it is the
decision rule that says which side of the `libaccudisc` / `accudisc-drv-*.so`
boundary a capability belongs on, and it makes CLAUDE.md's vendor-isolation
constraint operational rather than aspirational. Read as a rule:

> **If a standard MMC command achieves it, it belongs in `src/mmc/` and the
> driver must not reimplement it. Only what generic MMC cannot reach may live in
> a vendor driver.** A capability reached by a *standard* opcode carrying
> *vendor-specific values* is a CORE feature with a vendor data table, not a
> driver feature — the same status the read-offset and ATIP tables already have
> ("factual data tables are not features and may live in the core").

That third case is the one this gate exists to catch, because it is the one that
looks like a vendor feature and is not. A worked example arrived the same day:
**Yamaha's AMQR appears to be set through mode page `0x05` Write Parameters**,
i.e. `0x55` MODE SELECT — a command the core already issues — which would put it
in the core with a vendor value table, *not* in a driver. See the drive-research
item below; that reading is a lead, not yet a finding.

Deliverable: a **redundancy table in `OPCODES.md`**, each row naming the vendor
or legacy opcode, the standard command that supersedes it, and the evidence.
Seed rows, all already established:

| redundant | superseded by | evidence |
|---|---|---|
| `0xD8` READ CD-DA | `0xBE` READ CD | **all five** C2/sub-channel combinations work on this drive, so `0xD8` buys nothing. Already the documented reason it is unused. |
| `0x08` READ(6), `0xA8` READ(12) | `0xBE` READ CD | legacy CDB widths of the same operation; `0xBE` is the only form that returns raw CD-DA with C2 and sub-channel |
| `0x0A` WRITE(6), `0xAA` WRITE(12) | `0x2A` WRITE(10) | same, write side |
| `0x1A` MODE SENSE(6) | `0x5A` MODE SENSE(10) | 6-byte form; we use the 10-byte form throughout |
| `0xB9` READ CD MSF | `0xBE` READ CD | MSF-addressed sibling; we address by LBA |
| `0x28` READ(10) | `0xBE` READ CD | cooked 2048-byte sectors; CD-DA needs raw |
| `0xEB` speed LIST readout | `0xAC` GET PERFORMANCE **(candidate — verify)** | both report speed capability; whether `0xEB` carries anything `0xAC` and feature `0x0107` do not is **unverified** and is a Phase 2 question |

**Redundancy is a property of THIS drive, not a universal one — say so in every
row.** `0xD8` is redundant here *because* all five `0xBE` combinations work on a
PX-716A. On a drive whose `0xBE` C2 forms are broken or absent, `0xD8` could be
the only raw-audio path, and a row written as an unqualified fact would then be
wrong in exactly the way this plan's rule B guards against. Each row states the
drive and firmware it was established on.

The last row is deliberately marked a candidate: it is an inference from two
descriptions, not a measurement, and it is precisely the shape of claim this
project keeps having to withdraw.

---

### Phase 4 — wire what qualified.

**Two items need no discovery at all and should land first — they are not gated
on any phase above.**

**4.1 The `uncr`/`e32` offset ambiguity in code we already ship.** This is a
correctness question on a shipped path and ranks **above** discovering new
features. QPxTool decodes eight fields from the 26-byte CD Q-Check block (`bler`
at 10, `e31 e21 e11` at 12/14/16, `uncr` at 18, `e32 e22 e12` at 20/22/24);
`plextor.c` takes three and reads **CU at 20** where QPxTool reads `uncr` at 18.
Neither side is authoritative — QPxTool's own source carries
`// check where drive returns E32` and `// and where is UNCR` at
`qscan_cmd.cpp:273-274`. **Settle it on hardware.** Requires a disc with a
**known non-zero uncorrectable count**; without one the two readings are
indistinguishable and the test proves only that the disc is clean — the same
trap as `0x5C`'s Block=1 form.

**4.2 `0xEA` `CDB[2]=0x10` Jitter/Beta, CD arm.** Framing already pinned from
QPxTool (`cmd_cd_jb_init`, `CDB[1]=0x15`, `CDB[2]=0x10`, `CDB[3]=0x01` for CD).
No sweep needed: relevance + wiring + test only.

**4.3 Anything Phase 3 qualified**, then:

**The two ABI gates, which are the real cost of this phase.**

- **New vendor features mean `ACCUDISC_DRIVER_ABI` 3 -> 4.** POWEREC was
  *appended* in ABI 3 (`include/accudisc/driver.h:90`); the same mechanism
  applies, and every driver `.so` must be rebuilt in step.
- **The free-to-break window is spent.** It was never "soname is `.so.0`" — what
  made breakage free was that nothing outside this repo linked the library, and
  that expired when the Python binding shipped. Any public-header addition must
  align with `API_PLAN.md` and regenerate the binding, whose cdef is verified at
  **import**, not at build.

Do not promote process conventions (exit codes, `--progress-fd`, rendering) into
the library; document the mapping instead.

---

### Phase 5 — test, both levels.

**5.1 Fixture, for anything wired.** CDB-layout cases in `tests/test_cdb.c`, plus
response-decode cases against captured vectors where a response is parsed. Note
that `tests/test_burn_flow.c` stubs the whole MMC layer, so a passing flow test
is **not** coverage of an opcode's bytes.

**5.2 Backfill the two genuinely bare opcodes while here** (`OPCODES.md` §G.2):
`0x2A` WRITE(10) — whose stub discards `lba` and `nblocks` outright, so no
hardware-free test observes even which LBA the audio-writing command is given,
including the two's-complement -150 lead-in address — and `0x54` SEND OPC.
Hardware-free and cheap.

**5.3 Production.** Real PX-716A under `flock`. Read-side items settle on any
disc; `0xEA` Jitter/Beta wants a disc with known error content; 4.1 requires the
known-non-zero-uncorrectable disc named above. Record results at opcode level —
"a burn passed" is `exercised`, not `observed`, and the distinction is the point
of `OPCODES.md`'s production column.

**5.4 Update `OPCODES.md`** — it is the status matrix and goes stale first. It
gains: the (selector x medium) tables from Phase 2, the MMC-3 field dictionary
from Phase 1, and the redundancy table from 3.1. `FEATURES.md` gains rows only
when semantics are established, never on a sense code alone.

### Stop conditions

Any of these ends the relevant thread rather than escalating it: a discriminator
that cannot separate implemented from absent (Phase 0); a selector whose meaning
is undetermined after the media axis is exhausted; a drive that stops answering
(power-cycle, then **stop** — do not retry the parameter that did it). None of
`0x3B`/`0xE3`/`0xEE`/`0xF2`/`0xF8`/`0xDE`/`0xDF` is probed at any point.


## DRIVE PORTFOLIO — a batch of simple vendor drivers, and which drives to acquire — PLAN ONLY, raised 2026-09-01

Keith, 2026-09-01, two related asks recorded as one item because the second
decides the first:

1. **Build a batch of simple drivers** modelled on those shipped with `cdrtools`
   and `cdrdao`.
2. **Research which drives are worth pursuing for special features**, so they can
   be **marked for acquisition and testing**. Named target: at least one drive
   capable of **Audio Master Quality Recording (AMQR)** — a high-redundancy write
   strategy producing unusually robust media. "Nearly all Yamaha, but at least one
   Plextor also supports it."

**Everything below is gated on the redundancy rule** in the selector-sweep plan
§3.1 above: a capability reachable by standard MMC belongs in the core, and only
what generic MMC cannot reach earns a driver. Expect that rule to delete several
candidate drivers before they are written.

### A. What the reference tools actually ship — enumerated 2026-09-01

`cdrdao/dao/` (12 drivers): `CDD2600`, `GenericMMC`, `GenericMMCraw`,
`PlextorReader`, `PlextorReaderScan`, `RicohMP6200`, `SonyCDU920`, `SonyCDU948`,
`TaiyoYuden`, `TeacCdr55`, `ToshibaReader`, `YamahaCDR10x`.

`schily-2024-03-21/cdrecord/` (9): `drv_7501`, `drv_bd`, `drv_dvd`,
`drv_dvdplus`, `drv_jvc`, `drv_mmc`, `drv_philips`, `drv_simul`, `drv_sony`.

**Most of that list is dead weight for this project, and the classification is
the first deliverable.** `CDD2600` (Philips), `SonyCDU920/948`, `YamahaCDR10x`
(= CDR100/CDR102), `TeacCdr55`, `RicohMP6200`, `drv_7501`, `drv_philips`,
`drv_jvc`, `drv_sony` are **pre-MMC 1990s SCSI writers** — they exist because
those drives predate a common command set, which is precisely the problem MMC
solved. Porting them buys nothing unless such a drive is physically acquired,
and they are not the drives worth acquiring.

Note what `cdrdao`'s own table already says: every modern Yamaha
(`CRW2100`…`CRW8824`) is mapped to **`generic-mmc`**, not to a Yamaha driver.
Only the ancient `CDR100`/`CDR102` get `yamaha-cdr10x`. That is the redundancy
rule stated by a reference tool, in its own data, twenty years ago.

**The live vendor surface is QPxTool's, not cdrdao's**: `lib/qpxplextor` (ours,
shipped), `lib/qpxpioneer`, `lib/qpxyamaha`. Three vendors, and the second and
third are the actual candidates for a "batch of simple drivers".

Deliverable A: a table classifying all 21 reference drivers as **pre-MMC legacy
(do not port)** / **superseded by generic MMC (do not port)** / **live vendor
capability (candidate)**, with the reason per row. This is the same evidence
shape as §3.1's redundancy table and should be merged into it.

### B. Candidate vendor drivers, seeded from QPxTool

| vendor | features QPxTool implements | mechanism | verdict |
|---|---|---|---|
| Plextor | Q-Check, SpeedRead, GigaRec, VariRec, SilentMode, SecuRec, AutoStrategy, BookType, PoweRec, PlexEraser | vendor opcodes `0xE9`/`0xEA`/`0xED`/`0xE4`/`0xE5`/`0xD4`/`0xD5` | **shipped** |
| Pioneer | `pioneer_get_quiet` — quiet/speed control | see `lib/qpxpioneer` | candidate, unresearched |
| Yamaha | **AMQR**, Force Speed, **DiscT@2** (`f1tattoo`, laser etching) | see C below | candidate |

**Force Speed is a redundancy-rule casualty and a useful worked example.**
`yamaha_check_forcespeed()` reads mode page `0x2A`, takes the max read and write
speeds from offsets +14 and +28, and issues **`MMC_SET_SPEED` (`0xBB`) with
`CDB[1]=0x01`** — a standard MMC opcode we already build, with the rotation-control
field set to CAV. `cdrdao` carries the same thing as a *flag*,
`OPT_MMC_YAMAHA_FORCE_SPEED`, not a driver. **This is core behaviour with a
vendor quirk bit, not a driver feature** — exactly the third case §3.1 exists to
catch. Verify the CDB[1] reading before acting on it.

### C. AMQR — what is established, and what is not

**Established, from primary source on disk 2026-09-01:**

* QPxTool implements AMQR only as `yamaha_check_amqr()` in
  `lib/qpxyamaha/yamaha_features.cpp`. It does `MODE SENSE` of
  **page `0x05` Write Parameters**, walks to the page offset, and issues a
  `mode_select()` of the same length.
* **Every byte-modification in that function is commented out**, and
  `yamaha_set_amqr()` is literally `{ return 1; }`. **QPxTool does not implement
  AMQR.** What ships is a MODE SENSE/MODE SELECT round-trip that tests whether
  the page can be written back — a weak probe that may well return true on drives
  with no AMQR at all. Treat its "AudioMaster Q.R.: YES" output as unproven.
* **Keith's Plextor recollection is corroborated by QPxTool's structure**:
  `console/cdvdcontrol/cdvdcontrol.cpp:127` calls `yamaha_check_amqr(dev)` from
  **inside the `isPlextor()` branch**, deliberately, in addition to the Yamaha
  branch at :128. The author expected at least one Plextor to have it.
  **QPxTool never names the model.**
* The commented-out bytes are the only mechanism hint we have: writes to page
  `0x05` at offsets +2, +3, +4 and +8. **This is abandoned experimentation by
  QPxTool's author, not a specification.** It is a lead. Do not build on it.
* **`AMQR` appears nowhere in `cdrtools` or `cdrdao`** (searched
  case-insensitively across `private/code/`, zero hits).

**Consequence for the gate, and it is the interesting one:** if AMQR really is
mode page `0x05`, then it is **standard MMC `0x55` MODE SELECT with vendor
values** — a CORE feature with a vendor data table, *not* a vendor driver. That
would make AMQR the flagship worked example of §3.1's third case.

**The two models, from Keith 2026-09-01: Yamaha CRW-F1 and Plextor Premium II.**
"Both high value drives. I'll take either one." All `CRW-F1<any>` are the same
drive with different interfaces, so the suffix is an interface code, not a
variant. Both are already present in our own data:

| drive | our offsets DB | QPxTool capability flags |
|---|---|---|
| `YAMAHA CRW-F1E` / `CRW-F1S` | **+733**, 829 / 42 submissions | n/a (Yamaha lib: AMQR, ForceSpeed, DiscT@2) |
| `PLEXTOR CD-R PREMIUM2` | **+30**, 1932 submissions | `CHK_ERRC_CD \| CHK_JB_CD \| CHK_FETE` |

Two consequences worth having before buying:

* **The whole Yamaha CRW family reads +733** (CRW-70, 2100, 2200, 3200, F1E,
  F1S), except CRW8424/8824 at +117. Since every F1 is the same drive, a
  `CRW-F1UX` is +733 too — but **no `UX` row exists in `offsets_db.inc`**, so an
  exact-string lookup would miss it and fall through to no offset. Check the
  INQUIRY string on arrival and add the row; the value is not in doubt.
* **The Premium II's Q-Check surface is a strict SUBSET of the PX-716A's** —
  CD-only, no `CHK_ERRC_DVD`/`CHK_JB_DVD`/`CHK_TA_DVD`. It adds **no diagnostic
  capability we lack.** Its value is write-side, which sharpens the case below
  rather than weakening it, but it does retire the "a second Plextor would widen
  our diagnostic surface" argument.

**GigaRec 0.6x may be the real reason to buy the Premium II — and it is a better
argument than AMQR, because it is locally sourced.**

Our own RE decoded the full GigaRec rate-code table from `PTPXL.exe` at HIGH
confidence (it matches QPxTool's `gigarec_tbl` byte-for-byte). The `0x80` bit
marks the sub-1.0 **compression** rates, and our own table labels the bottom one:

| code | rate | our note |
|---|---|---|
| `0x81` | 0.8x | compress |
| `0x82` | 0.7x | compress |
| `0x83` | **0.6x** | **compress (max reliability)** |

QPxTool's GUI gates those rates **per model**
(`gui/src/devsettings_widgets.cpp:131-155`): `0.7` and `0.8` are enabled
unconditionally, `0.9`/`1.1` need `ratio_11` (PX-755/760/Premium II), and
**`0.6` and `1.4` need `ratio_14`, which is `startsWith("CD-R   PREMIUM")` —
Premium and Premium II only.**

So, if that gating reflects firmware acceptance (**inferred from a GUI, not
measured — verify it**): the PX-716A already reaches **0.7x and 0.8x**, i.e. the
*same direction* AMQR goes — lower density, longer pits, more robust media — and
**0.6x, our own table's "max reliability", requires exactly the drive Keith is
considering.**

**Therefore the cheap experiment comes first, and it may decide the purchase.**
GigaRec is `0xE9` page `0x04`, already identified, framing pinned, GET-verified,
and it is one of the eight unwired pages in the selector-sweep plan §3.1/Phase 3.
Wire it, burn one CD-R at 0.8x and one at 1.0x, and measure both with `0xEA`
Q-Check — which we already ship. That answers, on hardware we own, for the price
of two blanks: **does lower recording density actually produce measurably more
robust media here?** If it does not, neither AMQR nor GigaRec 0.6x is worth a
drive purchase. If it does, the purchase is justified by a measured curve rather
than by marketing copy.

**Do not conflate GigaRec with AMQR.** Both reduce density, and that is the whole
of the established similarity. Whether they are the same technique, related, or
merely adjacent is **not established** and should not be assumed while designing
the experiment above.

**A documentation correction to check while doing this:** `FEATURES.md` row 4
describes GigaRec on the PX-716A as "CD-R density 0.6-1.4x". If QPxTool's
per-model gating is right, this drive reaches only 0.7-0.8 and 1.2-1.3, and the
row is stating the *feature's* range inside a *per-drive* table. Also stale:
`re-tools/gigarec_ratemap.py`'s docstring still says "page 0x06", corrected to
`0x04` everywhere else.

**Open, and the research to do:**

1. **Confirm AMQR on both models with a real source.** Keith names Yamaha CRW-F1
   and Plextor Premium II; **no local source names any AMQR-capable model**, so
   this is currently recollection plus QPxTool's structural hint, not a citation.
   Try the Wayback Machine for Yamaha's own product pages (`wayback` skill).
2. **Search the PX-716A manual and PlexTools CHM first** — both are on disk. If
   our own drive has it, no acquisition is needed at all. **They are CP1252 and a
   UTF-8 `grep` silently skips lines**, so any negative taken without
   `iconv -f CP1252 -t UTF-8` is void.
3. **The actual mechanism** — page `0x05` bits, or something else. Primary
   sources to try: Yamaha service/OEM documentation, `dvd+rw-tools`, MyCE/CDFreaks
   archaeology, and the Wayback Machine for Yamaha's own product pages. The
   `wayback` skill exists for exactly this.
4. **Whether AMQR is even reachable on media we can still buy.** It reportedly
   trades capacity for pit length; if it needs media characteristics no current
   CD-R has, a drive purchase buys a demonstration and not a capability.

### D. Acquisition list — the actual output

A table of **capability → drive model(s) → why this project wants it → source**,
each row marked **acquire / watch / reject**, so Keith can buy against it.

Ordered by what the capability buys *this* project (CD-DA read fidelity, DAO
write fidelity, the frame-accurate status surface). Candidate rows to research,
none yet justified:

* **AMQR** — robust audio media. The named target.
* **Yamaha DiscT@2** — laser etching, and it ships free with a CRW-F1 rather than
  being a reason to buy one. **Reject as a purchase driver**, on two independent
  grounds: it has no bearing on audio fidelity, and per Keith it needs special
  discs that are **out of production**, so the capability would arrive with no
  media to exercise it. Worth one line in the docs as a known vendor feature we
  deliberately do not implement, nothing more.
* **Pioneer** vendor features — unresearched; `lib/qpxpioneer` is the entry point.
* **Plextor Premium II** — AMQR (claimed), **GigaRec 0.6x and 1.4x** (gated to
  this model per QPxTool), and it would test whether our driver's gating
  generalises past one drive. **Its Q-Check surface adds nothing** — CD-only, a
  strict subset of the PX-716A's. Buy it for the write-side density range and the
  architecture test, not for diagnostics, and **only after the 0.8x experiment
  above shows density buys robustness on hardware we already own.**
* **A drive with a known-broken `0xBE` C2 path** — would falsify §3.1's `0xD8`
  redundancy row, which is currently scoped to "this drive". Probably not worth
  buying deliberately, but worth recognising if one turns up.

**Standing caution for every row: one drive is not a population.** Every
generalisation this project has made from the single PX-716A has needed
withdrawing at N+1 — three in one session on the vendor opcodes alone. A second
drive is worth more as a falsifier of what we already believe than as a source of
new features.


## READ BUFFER ("AccuBuffer") — the slow-sink experiment, and why the page cache already is one (2026-08-26)

Keith asked for a read buffer before the powered C2 test, and asked the right
question with it: *where would it actually be useful?* System built `slowdisk`
(a cgroup write-rate-capped ext4 container, currently 2 822 400 B/s = 16x) so
the question could be measured rather than argued.

### 1. The experiment — UNINFORMATIVE, and that is the honest verdict

Interleaved fast-sink / capped-sink whole-disc passes, 4 pairs. Prediction and
falsifiers written first (`scratchpad/PREDICTION3.md`): a starved sink should
provoke MORE Mode 2, because our engine is synchronous and a blocked `fwrite`
delays the next READ CD until the drive must stop and **re-position** — the
operation that produces the slip.

| pair | fast arm | capped arm |
|---|---|---|
| 1 | clean | *(comparison produced no output — treated as MISSING)* |
| 2 | clean | clean |
| 3 | **BAD** 3 sectors, bit errors, 289503..289565 | clean |
| 4 | clean | clean |

**Zero Q-mispositions in either arm.** The only corruption was Mode 1, on the
*fast* arm, in the outer region — consistent with the radial read-margin
explanation and unrelated to the sink.

**This does not falsify the prediction; it fails to test it.** Mode 2's base rate
this evening was ~1 in 8 passes, so `P(zero events in 7) = 0.393`. Seeing nothing
is unremarkable whether or not the sink matters. Recorded as underpowered rather
than negative — the same trap as §2.19b, and the third time today that a small
sample of a ~10-25% intermittent event has invited a conclusion it cannot carry.

### 2. What the timings DO establish — the kernel is already the buffer

Pure sink time for the image at the cap: `825 552 000 / 2 822 400 = 292.5 s`.

| capped pass | elapsed | vs pure sink time | excess |
|---|---|---|---|
| 1 | 306.5 s | 104.8% | 14.0 s |
| 2 | 307.1 s | 105.0% | 14.6 s |
| 3 | 301.2 s | 103.0% | 8.7 s |
| 4 | 301.1 s | 102.9% | 8.6 s |

The steady-state disc read alone takes 191.9-192.3 s. **If read and write
serialised, a capped pass would take 484.4 s. It took 301.1.** So **95.5% of the
disc read is hidden behind the sink.**

The mechanism is not ours: `fwrite` returns into the page cache and the kernel
drains asynchronously via kworkers, so the drive keeps streaming while writeback
is throttled. Corroborated independently by System, who measured the sink at
**100.0% of cap** mid-pass from `stat` deltas — the sink is the binding
constraint continuously, and our 4.7% residual is startup/seek/per-command time
when it is not.

**So for a page-cached file sink on Linux, a user-space read buffer adds almost
nothing.** That is the configuration every measurement in this project has used,
and it is why the buffer has looked like a "just in case" feature.

### 3. Where a read buffer IS worth having

The cases the page cache does *not* cover — i.e. where there is no kernel buffer
between us and the slow thing:

1. **Work inside the sink callback.** This is the real one, and it is AccuDisc's
   own API shape: `accudisc_read` streams chunks to a caller-supplied function.
   FLAC encoding, hashing, AccurateRip/CTDB checksumming, a GUI update — every
   millisecond spent there is a millisecond the drive is not being read, with no
   page cache to hide it. A consumer doing 1 ms of work per 24-sector chunk adds
   ~15 s to a whole-disc rip and, more importantly, inserts a stall exactly where
   the engine is synchronous.
2. **Sinks with no page cache behind them** — a pipe or socket to a slow reader,
   `O_DIRECT`, a synchronous network filesystem.
3. **Bursty interference** — writeback storms, another process hammering the
   disc. The page cache absorbs these too, up to the dirty limit; a bounded ring
   raises that ceiling under our control rather than the kernel's.

Note what is NOT on this list: **making a sustainably-slower-than-the-drive sink
keep up.** Nothing can do that. A buffer converts a *burst* into a delay; it
cannot create bandwidth.

### 4. Sizing and placement — answering "malloc from paged memory?"

Yes, and it is already the default: a large `malloc()` is anonymous pages, RAM
first, swap under pressure, nothing committed until touched. `mlock()` would be
needed to *prevent* paging. But relying on that as the policy is wrong here:

- **Swap is slower than the sink we are buffering for.** A buffer that grows
  into swap replaces a slow sink with a slower one, and turns steady backpressure
  into an unpredictable page-fault stall on the producer. On a burn that is the
  underrun we were preventing.
- **Linux overcommits.** A 4 GiB `malloc` succeeds instantly; the OOM killer
  arrives later, mid-burn, aimed by heuristic. "Unless both RAM and swap are
  exhausted" is not a boundary the allocator enforces.

Design of record: a **bounded ring with explicit backpressure** — fixed capacity
chosen at open, `malloc`ed (so it *is* pageable; we simply do not depend on it),
touched once at allocation so a shortfall fails at startup rather than at 60% of
a burn, and a producer that blocks when full. Bounded means the failure mode is
"the drive waits", which this session measured the drive handling gracefully
(chunk-size sweep: 11.48 s for 24 000 sectors at chunk 8, 16, 24 and 27 alike).

Sizing has a clean answer on the write path: cover the longest sink stall worth
surviving. At a 16x burn (2.8 MB/s) a 64 MiB ring rides out 23 s.

### 5. Consumer-visible contract — two points that are NOT transparent

The buffer sits between engine and sink, so chunk callbacks arrive in the same
order and consumers need no change. Two exceptions must be documented:

- **Chunk lifetime.** `chunk.data` currently points into the engine buffer and is
  valid for the callback only. With a ring it points into the ring — same rule,
  but a slow sink now *holds a ring slot*, so a consumer that retains the pointer
  stalls the producer rather than merely reading stale bytes. The Python binding
  already pins this as `test_sink_zero_copy_retained_slice_is_a_KNOWN_HOLE`.
- **Cancellation ordering.** A sink returning non-zero cancels at the next chunk.
  With a buffer the engine may be N chunks ahead when the sink refuses, so cancel
  becomes "stop producing, then drain or discard what is queued" — and which of
  those it is has to be specified, not left to the implementation.

### 6. Loose end

`slow 1`'s comparison produced no output and its stderr went to a buffered pipe
that could not be recovered. Cause unknown; the data point is recorded as
MISSING, not clean. Any rerun must capture stderr per-arm.

### 7. THE ACCUBUFFER — built 0.22.0, and measured before and after

`src/read/accubuf.{c,h}`: a bounded chunk ring with a consumer thread, between
the engine and the caller's sink. Opt-in via `accudisc_read_req.buffer_bytes`;
**0 by default and the default path is byte-for-byte unchanged**. Surfaces in
step: engine, CLI (`--buffer`, a human line, `buffer_peak=`/`buffer_stalls=` on
the machine summary), Python binding (`buffer_bytes=` on `read`,
`ReadStats.buffer_peak_chunks/.buffer_stalls`, `ReadStats.buffer_helped`), man
page, `cli-machine-interface.md`, ABI pins, golden usage. 45/45.

**Hardware verification, whole disc:**

| run | sink | buffer | elapsed | ring |
|---|---|---|---|---|
| A | capped (2.82 MB/s) | none | 316.0 s | — |
| B | capped | 64 MiB | 301.3 s | peak **1056** chunks, **373** stalls |
| C | fast (`/var/tmp`) | 64 MiB | 261.5 s | peak **1**, **0** stalls — "never the constraint" |

Read carefully, because two of these three numbers say less than they look:

- **A vs B is n=1 each and proves nothing.** B (301.3 s) sits inside the range of
  the four *unbuffered* capped passes measured earlier the same evening
  (301.1-307.1 s); A (316.0 s) is worse than any of them. The apparent 14.7 s
  "win" is within the drift. And a win was never expected here: the capped sink
  is *sustainably* slower than the drive, and a ring cannot create bandwidth —
  §1 of this section says so, and B's 373 stalls are the ring filling and then
  bounding the read exactly as designed.
- **C is the honest-reporting mechanism working.** Peak 1 chunk, 0 stalls, and
  the CLI says "never the constraint". A fast file sink does not need this
  feature and the API says so rather than implying a win.

**C's 261.5 s against a ~192 s baseline looked like 36% buffer overhead, and it
is not.** Interleaved, same span, fast sink, 6 pairs:

```
  nobuf  12.139  11.631  11.623  11.601  11.569  11.496
  buf    11.621  11.636  11.519  11.617  11.618  11.679
```

**~0.3% difference.** Scaled to a whole disc, a 36% overhead would have shown as
~4.8 s on this 11.6 s span and is nowhere in the data. C's number is a single
unexplained outlier — recorded as such rather than attributed.

**The test that would catch a fake.** Ordering, payload-copy and bounds tests all
pass on a ring that is secretly synchronous. `tests/test_accubuf.c` therefore
measures wall clock: 8 chunks into an 8-slot ring against a 20 ms sink must
return in far less than 160 ms. **Falsified** — patching `push` to deliver inline
aborts the test; restored, it passes.

**Two mistakes made and fixed during the build**, both worth keeping:

1. The new stats fields went in **before** `subq_misposition`, moving a field
   0.21.0 had already shipped. Appended instead, and the offset is now pinned at
   **140** — measured by compiling against `git show 9679b3c:...` rather than
   assumed.
2. The `--buffer` usage text went into a C string literal as raw multi-line text.
   Same mistake as an earlier session; the usage block needs per-line `"...\n"`
   fragments.

**Sizing — start small, and this is measured, not guessed.** System reported the
container's dirty budget as **3.8 MiB hard-capped**, working set under 2.6 MiB,
against **1.6 GiB** on this machine's ordinary storage — a factor of 432. So the
95.5% read-hiding in §2 above was achieved by a *few megabytes* of kernel buffer
in the hostile case. There is no evidence that hundreds of megabytes buy
anything, and the ring is touched at allocation, so an oversized one costs
resident memory up front. Single-digit MiB first; raise only if `buffer_stalls`
says the ring was actually the constraint.

**Still unproven.** The case the buffer was built for — work inside the sink
callback — has no hardware test here, because every sink used in this session is
an `fwrite`. `tests/test_accubuf.c` proves the overlap synthetically. A real
demonstration wants a consumer doing per-chunk work (hashing, encoding), which
is a consumer-side experiment rather than one this repo can run alone.

## WRITE OFFSET measured on real media, and the silent read displacement it exposed (2026-08-25/26)

One of Keith's five blanks (Taiyo Yuden CD-R) was spent on this. The measurement
itself succeeded; the read-back turned up a separate defect that is **not ours**
and that **defeats our own consensus defence**. Both are recorded here because
neither was in git.

### 1. The measurement — DONE

`accudisc write-offset` (0.20.0) burns a known signal and finds it again.
Procedure: generate the locator signal, burn DAO at 16x, read the disc back,
locate the burst in read-offset-corrected coordinates.

**PX-716A write offset = −30 samples**, agreed by both pulses (the 1 s and the
60 s marks) independently. The burn itself was byte-perfect: three of the five
whole-disc read-backs compare **byte-identical** to the source image across all
78 minutes.

### 2. The read anomaly — REAL, and it is the drive, not us

Two of the five whole-disc read-backs returned **real disc data from the wrong
LBA**, with C2 reporting zero errors and no SCSI error of any kind.

Ground truth is `/var/tmp/woff_disc.pcm` — the exact image that was burnt. Keep
it; the pass files are regenerable, that one is not.

#### 2.1 Exact geometry

Expressed as byte-stream segments against the source (sector = 2352 B):

| pass | byte offset | = sector + off | length | source delta |
|---|---|---|---|---|
| woff_read | 528849456 | 224850 + 2256 | 7152 B | −2048 sec |
| woff_read | 809527936 | 344187 + 112 | 44576 B | −2048 sec |
| woff_read | 809572512 | 344206 + 0 | 1133664 B | **+2 sec** |
| passB | 208921064 | 88826 + 2312 | 44728 B | −2048 sec |
| passB | 528849456 | 224850 + 2256 | 7152 B | −2048 sec |

Four structural facts, each of which constrains the explanation:

1. **Every displacement is a whole number of sectors** (+0 byte remainder) —
   the substituted data is sector-aligned *content*.
2. **Every corrupt segment starts at an arbitrary, sample-aligned byte offset
   INSIDE a sector** (2256, 112, 2312), and **ends exactly on a sector
   boundary**, after which data is correct with delta 0.
3. **−2048 sectors, three times out of four events.** 2048 = 2^11. That is an
   index/tag aliasing constant, not a random seek error.
4. **The 224850 + 2256 event is byte-for-byte identical in two independent
   passes** — same splice byte, same length, same delta. It is
   *position-deterministic*, not transient.

The `+2 sec` segment means two sectors went missing from the stream and
everything downstream shifted early for 482 sectors, then re-synchronised. It
spans far more than one READ CD command (`ADSC_CHUNK_MAX` is 32).

#### 2.2 Why it is not ours — proven structurally, not sampled

- **No sub-sector copy exists in the engine.** Every `memcpy`/`memset` in
  `src/read/engine.c` moves `r->sector_len`, `r->audio_len`, `r->c2_len`,
  `r->sub_len`, or an `n * sector_len` multiple. A splice at byte 2256 *inside*
  a sector is therefore not something this code can produce.
- **The requested LBA and the buffer position come from one counter.** The loop
  holds a single `uint32_t lba`; every read is `lba + s` paired with
  `buf + s * r.sector_len`, and the delivered chunk carries `.lba = lba`. There
  is no second, independently-advanced counter — including on the retry, rescue
  and consensus paths — so "we asked for L+2 and wrote it at L's offset" is
  impossible.
- **Short transfers are already caught.** `sgio.c` records `resid` and
  `adsc_exec_check_short` promotes it to `ACCUDISC_ERR_SHORT`, so a partially
  filled buffer cannot silently keep stale bytes in its tail.

#### 2.3 What it probably IS — SETTLED by §2.9; hypothesis (b) is dead

They are not mutually exclusive and the data so far cannot tell them apart:

- **(a) Drive-side positional substitution.** The drive loses sync mid-sector,
  re-acquires at an aliased address (−2048), and delivers valid, correctly
  ECC'd audio from the wrong place without raising C2. Mid-sector start,
  sector-boundary recovery and the 2^11 constant all fit.
- **(b) A marginal spot in OUR burn.** This is a CD-R that AccuDisc wrote. A
  link or a weak region gives exactly a location-locked defect that is
  borderline rather than fatal. If so this is a **write**-path finding, not a
  read-path one.

(a) and (b) compose: a physical marginal spot at a fixed location, whose
*recovery* is a silent positional substitution.

**Resolved 2026-08-26 (§2.9), then PARTLY RETRACTED the same day — see §2.14.**
The original wording said "(a) is right and (b) is dead", on the grounds that a
marginal burn cannot produce a sector in which **all 96** wrong bytes come from
exactly −2048 sectors and **zero** are bit errors.

That argument is sound about the **substitution** and was wrongly applied to the
**trigger**. Fetching data from −2048 sectors is certainly drive-side; a marginal
burn cannot do it. But nothing in that argument shows what made the drive lose
position in the first place, and a physical discontinuity we wrote is a perfectly
good candidate for that. **(b) is not dead — it was never tested.**

#### 2.4 The pass table — speed is NOT isolated

| pass | speed | eject/load first | result |
|---|---|---|---|
| woff_read | default (max, reports 32x) | no | 2 bad regions |
| passB | default (max) | no | 2 bad regions |
| passC | capped 8x | no | clean |
| passD | 8x | yes | clean |
| passE | 32x (cap reset to max) | yes | clean |
| passF | max, chunk 24 | no | 2 bad regions, 15 sectors |
| passG | max, **chunk 16** | no | 5 bad regions, 40 sectors |
| passH | 8x | no | clean, 0 C2 |
| passI | 8x | no | clean, 0 C2 |

Nine whole-disc passes. **All four 8x passes are byte-identical to the burnt
image with zero C2 sectors; four of five max-speed passes carry events.** See
§2.12 — the speed effect is now measured rather than asserted, and `passE` (32x,
clean) is the one max-speed pass that does not fit. `speed 8` persisted across eject/load on this
drive; `speed 40` reports back 32x.

#### 2.5 THE IMPORTANT PART — this defeats our consensus defence

`src/read/engine.c`'s header states the trust model: a fired C2 flag reliably
marks a bad byte, **a clear flag is only a claim** — "hence verify passes
compare actual audio bytes between independent reads rather than trusting 'no
C2' alone, and consensus demands two byte-identical reads."

The 224850 event **reproduces to the byte across independent reads**. Had
`woff_read` and `passB` been the two consensus passes, they would have agreed
byte-for-byte, C2 would have been silent, and the engine would have returned
wrong audio as verified-good. **Consensus is not a defence against a
position-deterministic defect.**

Two further gaps, both real:

- `overlap_sectors` checks the *seam* between chunks. Every event here begins
  mid-chunk, so the boundary check is looking in the wrong place.
- `accudisc_probe_c2_lag` was never run on this drive before trusting the C2
  silence. Do that first. Note a lag shifts *where* pointers land, not how many,
  so it cannot by itself explain zero C2 bits over a 482-sector displacement —
  and it is the wrong order of magnitude for a sector-scale event.

Candidate fix, already available in the API: **vary `speed_x` between verify
passes** instead of repeating an identical read. A position-deterministic defect
survives repetition; it may not survive a speed change. That is the one lever
the current design has that was not used.

#### 2.6 Caveats on the experiment itself

- The read was run through the **undefended** path — no `--retries`,
  `--verify`, or `--overlap`. That plausibly explains why weeks of ordinary
  ripping never surfaced this, and my first "silent corruption" framing was
  overstated.
- The 78-minute test signal was AccuDisc's 75 s locator head followed by
  **numpy-generated white noise (scaffolding, not the feature)**. Full-scale
  white noise is pathological EFM and a plausible reason the anomaly does not
  appear on real music.
- cdda2img's own write-offset test reads only the first 75 s (~5625 sectors).
  Every event here is past sector 88826, so it never looked there. There is no
  contradiction between the two projects' results.

#### 2.7 Next steps, cheapest first — costs no disc

1. Run `accudisc_probe_c2_lag` on this drive.
2. Hammer the reproducible spot: repeated *targeted* reads of ~224840..224870
   at full speed. Cheap (seconds), and it measures the reproduction rate.
3. Read the same disc with an **independent tool** (cdda2img). If the anomaly
   appears there too, hypothesis (a) is confirmed from outside our code.
4. Re-run the whole disc through the **defended** path and confirm the engine
   flags rather than silently passes.
5. Only then decide whether §2.5 warrants an engine change.

#### 2.8 2026-08-26 — reproduced a third time, and the mechanism split in two

`passF` (whole disc, max speed, chunk 24, no eject/load) differs from the source
at **byte 528849457 — the identical byte as `woff_read` and `passB`**, same
7152-byte length, same −2048 delta. Three of four max-speed whole-disc passes
now carry a byte-identical defect at sector 224850 + 2256.

But it does **not** reproduce on demand:

- **0 / 20** targeted reads of 224592..224879 (chunk grid aligned to the
  original, cache-defeated between reads).
- **0 / 10** reads with a 10 032-sector run-up at full speed through the spot.

So the trigger is neither location nor streaming speed on its own. Whole-disc
streaming from LBA 0 is so far the only condition that produces it.

**Two distinct failure modes, separated by counting which source position each
wrong byte came from:**

| LBA | true bit errors | bytes from −2048 | C2 |
|---|---|---|---|
| 324083 (passF) | **104** | 49 | fired — 68 bits, the only C2 sector on the disc |
| 224850 (passF, passB, woff_read) | **0** | 96 | silent |

- **Mode 1 — detected error, then silent drift.** At 324083 there is a genuine
  uncorrectable read error: 104 bytes match neither the correct sector nor the
  displaced one, and C2 fires on 68 of them. From byte 2304 of that same sector
  onward the data switches to −2048 and stays there for 10 more sectors,
  stopping at chunk offset 22. The drive's *error recovery* is what returns
  wrong-position data, and it does not flag it.
- **Mode 2 — no error at all.** At 224850 every one of the 96 wrong bytes comes
  from −2048. Zero bit errors, zero C2, and it reproduces to the byte across
  three independent passes. This is the dangerous one: nothing in the sector is
  physically wrong, so no relative check has anything to fire on.

All four −2048 events (three passes, four distinct sites) end at **chunk offset
22** of a 24-sector transfer. *(Refuted in part by §2.9 — the extent does track
our transfer geometry, but the constant is not "−2"; under `--chunk 16` the same
event runs to the very end of the transfer.)*

**Engine consequence for Mode 1 — actionable now.** C2-guided rescue re-reads
the *flagged sector*. Here the flagged sector is the one with real errors, and
the ten silently-displaced sectors after it are C2-clean, so rescue repairs the
one sector it can see and passes ten wrong ones through. The rule that would
have caught it: **a C2 hit (or a hard error) invalidates the remainder of that
transfer, not just the flagged sector.** That is cheap and it is squarely inside
the trust model already stated in `engine.c` — a clear flag is only a claim, and
after this drive has erred once in a transfer the claim is worth less still.

Mode 2 remains uncaught by anything we currently have.

#### 2.9 2026-08-26 — chunk 16 + C2 capture: two bugs, not one, and C2 fires at run position 0

`passG` = whole disc, max speed, **`--chunk 16`**, C2 captured to a file. It
differs from the source at **byte 528849457 — the same byte again**, a fourth
time.

**The prediction made before looking** (kept, with its falsifiers, at
`scratchpad/PREDICTION.md`) was half right:

- **Start is invariant.** Sector 224850 + 2256 B, identical under both chunk 24
  and chunk 16, across four passes. It is a fixed physical position on the disc.
- **Extent tracks our transfer geometry** — confirmed, and this is the half I
  got right. Under chunk 24 the run is 4 sectors (224850..224853); under chunk
  16 it is **14** (224850..224863, to the very end of that transfer).
- **The "chunk_end − 2" rule is refuted.** It held 4/4 on the 24-sector grid and
  did not survive a second grid. Four events on one disc were never independent.
- **`--chunk` is not a mitigation.** Chunk 16 made it *worse*: 40 corrupt
  sectors in 5 runs, against 15 in 2 runs for chunk 24.

**C2 correlation, measured against a real C2 file rather than a summary line:**

| run | sectors | C2 fires at | per-sector composition |
|---|---|---|---|
| 224850..224863 | 14 | **none** | all −2048 |
| 236359..236367 | 9 | 236359 (position 0) | true errors, then 8 × −2048 |
| 236629..236639 | 11 | 236629 (position 0) | true errors, then 10 × **−1024** |
| 237071 | 1 | 237071 (position 0) | true errors only |
| 267387..267391 | 5 | **none** | all −2048 |

Three C2 sectors on the whole disc, and **every one of them is position 0 of a
corrupt run**. Never anywhere else in the run. The mechanism is now clear and it
is not one bug but two:

- **Mode 1 (3 runs, 21 sectors).** A genuine uncorrectable read error. C2 fires
  on that sector and that sector only. The drive's recovery then returns
  wrong-position data for the rest of the transfer, silently. **Displacement is
  −2048 or −1024** — it varies, and both are powers of two, which points at an
  index/tag aliasing inside the drive rather than a seek error.
- **Mode 2 (2 runs, 19 sectors).** No bit errors anywhere, no C2 anywhere, and
  the 224850 site reproduces to the byte across four independent passes. The
  earlier suspicion that Mode 2 might collapse into Mode 1 once C2 was actually
  captured is now **ruled out by measurement**.

**Scored against the rule proposed in §2.8** — a C2 hit invalidates the
remainder of the transfer — it would catch **21 of 40** corrupt sectors in
passG: all of Mode 1, none of Mode 2. Worth doing, and not sufficient.

#### 2.11 Site recurrence — ONE site recurs, the other five do not

Tabulated across the four bad passes, so the "invariant" claim is not carried
further than it goes:

| site | woff_read | passB | passF | passG (chunk 16) |
|---|---|---|---|---|
| **224850** | yes | yes | yes | yes |
| 88826 | — | yes | — | — |
| 324083 | — | — | yes | — |
| 344187 / 344206 | yes | — | — | — |
| 236359, 236629, 237071 | — | — | — | yes |
| 267387 | — | — | — | yes |

**224850 fires in every bad pass, always at the same byte. Every other site
fires exactly once.** So the correct statement is *"when 224850 fires it fires
identically"*, not *"the sites are deterministic"* — five of six are not. Do not
carry the stronger claim into later reasoning; it would predict a determinism
the data does not show. (The commit message for this work, e4a3cd3, states the
invariance without that qualifier and is overstated on this point.)

The 21-of-40 score in §2.9 is likewise **one pass, not a rate** — passG is the
only pass with a captured C2 file. On passF the same rule scores about 11 of 15.
The rule's *shape* is what is established (C2 at run position 0, three for
three), not its yield.

#### 2.10 What actually catches Mode 2 — vary the READ, not just repeat it

Repetition cannot see Mode 2: four passes agree byte-for-byte and are all wrong.
Two levers exist, and both are already expressible in the current API.

1. **Speed.** Both 8x passes (`passC`, `passD`) are byte-identical to the
   source. Four of five max-speed passes carry events. The 8x sample is only
   n=2, so this is directional rather than established — but it is consistent
   with the physical story, and `accudisc_read_opts` already carries the
   reread-speed machinery. **Verify passes should differ in speed rather than
   repeat an identical read.** That is the single highest-value change here.
2. **Chunk size.** The extent moves with the transfer geometry, so a chunk-24
   pass and a chunk-16 pass disagree over 224854..224863 — but they *agree*, and
   are both wrong, over 224850..224853. A partial detector only.

Neither is a substitute for the absolute gates (AccurateRip / CTDB) that live in
the calling application, per `docs/reference/RECOVERY.md`. This is squarely the
invariant that document already states: a relative check never outranks an
absolute one. What is new is a measured case where **every** relative check we
own — C2, repetition-consensus, and the chunk-seam overlap test — fails
simultaneously on the same sectors.

#### 2.12 2026-08-26 — the speed lever, tested against the site that recurs

§2.10 recommended varying speed between verify passes off n=2. Two more 8x
whole-disc passes take that to n=4, and the discriminating question is not "is
8x clean" but **does site 224850 fire at 8x** — it is the only site that recurs
(§2.11), and it fires in every bad max-speed pass.

- `passH`, `passI` (8x, whole disc, C2 captured): **byte-identical to the burnt
  image, zero C2 sectors.** With `passC` and `passD` that is **4/4 clean at 8x**.
- Site 224850: **4 of 5 max-speed passes, 0 of 4 at 8x.**
  Fisher exact, one-tailed, **p = 0.0397**.

So the recommendation stands, and it is now measured rather than directional.
Honest limits on it:

- p ≈ 0.04 on n=9 is a real signal, not a strong one. It is one disc, one drive,
  one signal — full-scale white noise, which is pathological EFM.
- **`passE` is the counter-example and has not been explained.** 32x, after an
  eject/load, byte-clean. If speed alone drove this it should have fired. Either
  the eject/load matters, or the effect is probabilistic at max speed rather
  than deterministic. Do not describe the max-speed behaviour as deterministic —
  only *site 224850, when it fires, is byte-deterministic*.
- This says nothing about other drives or ordinary music, and nothing about
  whether 8x is a *sufficient* defence. It is evidence for varying the read, not
  for trusting a slow one.

#### 2.13 Not damage, and probably not the noise either — site 224850 is MSF 50:00:00

Keith asked the right question: is this white noise being hard to read, or is
the disc damaged? Measured answer: **neither, for the mode that matters.**

**The disc is physically sound.** Four whole-disc passes at 8x are byte-identical
to the burnt image with **zero** C2 sectors between them. Damaged media does not
read perfectly four times running with no error pointers anywhere.

**Mode 1 is marginal high-speed reading, and it MOVES.** C2 fired at four
distinct sites across two max-speed passes and never at the same site twice
(324083; 236359, 236629, 237071). A fixed physical defect would fire in the same
place every time. This is the drive at 32x sitting at the edge of its ability,
with which sectors tip over varying pass to pass. Full-scale white noise is a
plausible aggravator — maximum transition density, minimum DC-balance headroom,
the worst case for EFM — but that is **untested**: nothing here compares this
disc against music, so it stays a hypothesis.

**Mode 2 is not a read-quality problem at all.** At 224850 there are *zero* bit
errors — the audio came off the disc perfectly — and it recurs at the identical
byte. The drive delivered correct data from the wrong address. Data quality is
not involved.

**And the site is exactly a minute boundary:**

    LBA 224850 + 150 = 225000 frames = MSF 50:00:00.00

Of the nine sites observed, that is the only one on a minute boundary, and it is
the only one that recurs. Expected number landing on a boundary by chance is
9/4500 ~ 0.002, so this is very unlikely to be coincidence — the drive tracks
position by Q-subchannel MSF, and 49:59:74 -> 50:00:00 is a multi-digit BCD
carry.

**Do not promote this to a mechanism yet.** The disc crosses 78 minute
boundaries and seven tens-of-minutes boundaries; exactly one of them misbehaves.
So "minute rollover" is not sufficient on its own. What is established is that
the recurring site sits on a boundary, not that boundaries cause the fault.
n = 1 site.

**Rejected the same session, recorded so it is not re-derived:** the
displacements are −2048, −1024 and +2, all powers of two, which invites "a
single-bit error in the drive's binary LBA". Tested against all eleven events —
does the LBA actually have that bit in the state a flip would require?
**7 of 11, against 5.5 expected by chance.** No signal. The hypothesis is dead.

#### 2.14 Keith's reframe — a pristine CD-R should never error, so look at what WE wrote

Keith, 2026-08-26: comparing a freshly-burnt CD-R against a played pressed disc
is apples and oranges, and more to the point *"a pristine disc should not produce
errors, ever. The fact that we're seeing errors is deeply suspicious."*

That is right, and it exposes a conflation in §2.3 (now corrected there):

- **What produces the wrong bytes** is drive-side. Settled.
- **What triggers the fault** is a separate question that was never asked. I
  treated "reads clean at 8x" as proof the disc is sound. It is not. A disc that
  is readable at 8x and fails at 32x is the textbook signature of a **marginal
  burn**, not of sound media — pristine dye plus a bad write gives exactly this.

**So this may be a WRITE-path finding, and the burn was never examined.**

Two concrete gaps in our own write path, found by reading it rather than by
measurement:

1. **BURN-Proof is enabled unconditionally** — `src/write/wparams.c:41` sets
   `p[2] |= 0x40`. Every buffer underrun therefore becomes a **link point**: a
   place where the laser stopped and restarted, i.e. a physical discontinuity in
   the spiral at a fixed location, benign at low read speed and marginal at high
   speed. That is the shape of what we are chasing.
2. **Nothing counts or reports underruns.** `write_chunk` loops only on
   "buffer full" (SK 2 / ASC 04 / ASCQ 08), which is the drive telling the *host*
   to wait — the opposite condition. The write loop is synchronous
   (`pread` then `WRITE(10)`, no prefetch), so a stalled `pread` can starve the
   drive, and BURN-Proof would mask it silently. **A burn can underrun and report
   complete success, and we would never know.** That is a genuine gap independent
   of this investigation.

**Do not promote links to the leading explanation yet.** The burn ran at 16x
(~2.8 MB/s) from a local file and reported success, so the link theory currently
predicts underruns for which there is **zero evidence** — none was recorded,
because nothing records them. If a census shows diffuse errors rather than a few
sharp fixed spikes, links are wrong and "this drive mis-tracks at high speed on
CD-R media generally" moves ahead of them.

**The instrument that settles it was available all along and was not used:**
`accudisc cxscan` — the Plextor C1/C2/CU hardware census. It must be run on the
burnt CD-R **at both 8x and 32x**, because the whole anomaly is speed-conditional:
a spike at 224850 present at 32x but absent at 8x means "the drive cannot handle
this spot fast"; present at both means a physical defect, full stop. Those are
different diagnoses. A pressed disc scanned on the same drive is the yardstick —
a C1 count with nothing to compare it to is not evidence.

**Limits of the pressed-disc comparison, stated in advance.** There is no oracle
for a pressed disc, so passes can only be compared against each other and against
a slow reference. That detects Mode 1 and *non-reproducing* displacement, and is
**blind to reproducing displacement** — precisely the Mode 2 shape. The disc's
lead-out is also 204143, so it never reaches LBA 224850: the one recurring site
is unreachable there. A clean result on the music CD is therefore much weaker
evidence than it will look.

#### 2.15 The pressed-disc comparison — flawless at 40x, and what that does and does not show

Keith supplied the newest pressed CD in his collection (11 tracks, lead-out
204143). Same drive, same session, same read path.

| what | result |
|---|---|
| `cxscan` @32x (C1/C2/CU hardware census) | C1 **2–14 per sample interval**, C2 **0**, CU **0** |
| reference read @8x | clean, 0 C2 |
| three reads @**40x** | clean, 0 C2 |
| all three fast passes vs the 8x reference | **byte-identical** |
| C2 bits set across all four passes | **0** |

Note the drive offers **40x** on the pressed disc and caps at **32x** on the
CD-R — it derates CD-R reading itself. So the pressed disc was read *faster*
than the CD-R ever was, and was flawless; the CD-R carried events in four of
five max-speed passes.

A C1 rate of 2–14/s is a healthy pressed disc (Red Book tolerates far more), and
it is the yardstick the CD-R scan needs. Without it a C1 count off the CD-R is a
number with nothing to compare it to.

**The confound, stated plainly: this comparison changes TWO variables at once** —
media (CD-R vs pressed) *and* content (full-scale white noise vs music). It
cannot separate them. What it does establish is that neither the drive nor our
read path is broken in general at maximum speed: 612 429 sectors read at 40x
across three passes with zero C2 bits and byte-exact agreement.

The blindness declared in §2.14 still applies — no oracle for a pressed disc, so
a reproducing displacement would be invisible, and the disc never reaches LBA
224850 anyway.

**Content is NOT what selects the failing sites.** The source audio at all eight
CD-R failure sites is statistically indistinguishable from random control sites
on the same disc: RMS ~18 900–19 100 of 32 767 at every one, full ±32 7xx range,
no zero runs, longest constant run 1–2 samples everywhere. So the noise does not
explain *where* the failures land. It remains possible that near-full-scale white
noise (~−4.8 dBFS RMS — maximum transition density, minimum DC-balance headroom)
makes the disc harder to read *as a whole*; that is a whole-disc property and is
still untested.

**To separate media from content costs one blank:** burn a CD-R containing music
(or any ordinary-crest-factor audio) and run the same protocol. Keith's call —
four blanks remain.

#### 2.16 The census — THE BURN IS GOOD, and the recurring site is one of the cleanest places on the disc

`accudisc cxscan` on the burnt CD-R, whole disc, 4680 samples of 75 sectors,
at both speeds. (Verified first that `census.c` reads the counters after exactly
`cadence` sectors at either speed, so the counts are per-sector normalised and
the two are comparable — the "/s" in the summary label is 75 sectors, i.e. one
second of *audio*, not of wall clock.)

| | C1 total | C1 mean/sample | C1 max | C2 | CU |
|---|---|---|---|---|---|
| **32x** | 30 523 | 6.5 | 70 | **16** (4 samples) | **0** |
| **8x** | 12 221 | 2.6 | 30 | **0** | **0** |
| pressed CD @32x (outer region) | — | 2–14 observed | — | 0 | 0 |

**The marginal-burn hypothesis is refuted, and refuted where it matters.**

- **CU = 0 across the whole disc at both speeds.** Nothing uncorrectable anywhere.
- **C2 = 0 across the whole disc at 8x.**
- C1 is comparable to Keith's pressed disc.
- **At site 224850 — the recurring Mode 2 site — C1 = 1 at 32x and 0 at 8x, C2
  and CU zero.** It is one of the *cleanest* places on the disc. There is no
  physical defect where the fault occurs.

So §2.14's link/underrun theory is dead as an explanation of Mode 2. It was
worth testing and it failed. (The two write-path gaps it turned up —
unconditional BURN-Proof and no underrun accounting — are still real and still
worth fixing; they just are not the cause here.)

**What the census DOES establish: speed-conditional read margin, measured.**

- C1 rises **2.5x** from 8x to 32x on the same physical disc. The disc did not
  change; the drive's ability to read it did.
- C2 appears **only at 32x** — 16 errors in 4 sample intervals (122775, 143025,
  319800, 319875). None coincides with any observed read-failure site, which is
  what a stochastic process should look like.

**Where that leaves the two modes:**

- **Mode 1 — explained.** The drive's read margin on CD-R at 32x is marginal.
  C2 fires, our engine sees it, and the recovery that follows is what silently
  substitutes. The trigger is real and now measured.
- **Mode 2 — NOT explained by disc quality, and the alternatives are exhausted
  on our side.** The spot is physically clean, the burn is good, our read engine
  provably cannot splice sub-sector, and the pressed disc is flawless at a higher
  speed. What remains is a drive positioning fault at a specific address under
  sustained high-speed streaming. The MSF 50:00:00 coincidence (§2.13) gets
  *stronger* now that no physical explanation competes with it — but 60:00:00 and
  70:00:00 are on the same disc and do not fire, so "minute boundary" is still
  not sufficient, and the mechanism stays inside firmware we cannot see.

**Practical conclusion for the library.** On this drive, CD-R media at maximum
speed is not trustworthy, and *none* of our relative checks can see the worst of
it. The 8x evidence is now strong on both instruments — 4/4 whole-disc passes
byte-exact, and a census with zero C2 over the entire disc. This is the
strongest form yet of the §2.10 recommendation: **verify passes must differ in
speed.** Two reads at the same speed on this drive can agree byte-for-byte and
both be wrong; a 32x read and an 8x read cannot.

**Not done, and Keith's call.** Burning a second CD-R with the same content
would separate "this disc at this address" from "this drive at this address" —
one blank, four remain. Nothing else on our side is unexhausted.

#### 2.17 Keith's tests 1-3 — and the anomaly STOPPED REPRODUCING, which voids the comparison

**Test 1 as run:** `cdrdao read-cd --rspeed 32 --paranoia-mode 0 --fast-toc`,
twice, same CD-R, same drive. Both passes **byte-perfect** against the burnt
image and identical to each other.

(cdrdao writes **big-endian** audio in its `.bin`. The raw compare fails from
sector 75 — the first locator pulse — matching wherever the source is digital
silence and differing everywhere it is not. That is the byteswap signature, not
a read fault. Byteswapped: 0 differing sectors. Anyone repeating this without
the swap will "find" 345 375 bad sectors.)

**I read that as "cdrdao is clean where we are not" and it was wrong.** The
control run settles it:

| pass | config | result |
|---|---|---|
| passJ | `--no-c2` (2352 B/sector, like cdrdao) | **byte-identical** |
| passK | default, **C2 requested** (2646 B/sector) | **byte-identical** |
| passL | `--no-c2` again | **byte-identical** |

`passK` is the exact configuration that failed four times out of four this
morning, and it is clean. **The anomaly is not reproducing at all any more**, so
cdrdao ran in a state where our own reads are also clean. *The tool comparison
measures nothing.* Test 3 (a cdrdao write/read pipeline) would have been
confounded identically and would have cost a blank to prove nothing — do not
spend one on it in this state.

What test 1 and the control DO establish: **the C2 request is not the trigger.**

**The timeline is the actual finding:**

```
23:50  burn ends
00:30  woff_read  max   BAD    <- first max read after the burn
00:49  passB      max   BAD
00:59  passC      8x    clean
01:05  EJECT/LOAD
01:11  passD      8x    clean
01:18  passE      32x   clean
       ~8.5 h idle, disc loaded, drive spun down
10:09  passF      max   BAD    <- first max read after the long idle
10:15  passG      max   BAD
10:26  passH/I    8x    clean
11:55  disc OUT (pressed CD tested)
12:38  disc BACK IN
12:45  cxscan 8x        C2 = 0 whole disc
13:22  cdrdao x2  32x   clean
13:31  passJ/K/L  max   clean   (K requests C2)
13:46  EJECT/LOAD
13:49  passM      max   clean   <- see the falsified prediction below
```

**4 of 4 bad before 11:55; 7 of 7 clean since 12:39.**

#### 2.18 A prediction, written in advance, and FALSIFIED

Hypothesis: a slow whole-disc pass *trains* the drive (adaptive servo
calibration) and a fresh load discards it, so the first max-speed read after a
reload should fail. Recorded at `scratchpad/PREDICTION2.md` with its falsifiers
before running.

**Test: eject, load, immediately read whole disc at max with C2. Predicted BAD
at or near LBA 224850. Result: byte-identical.** The prediction is wrong and the
training hypothesis is unsupported. Recorded rather than quietly dropped.

Also ruled out while looking: `census.c` calls `accudisc_counter_scan_end` on
every path out, so `cxscan` did not leave the drive armed in a modified state.

#### 2.19 Honest position

**The anomaly is currently not reproducible and its trigger is unknown.**
Everything that separates "then" from "now" is confounded — elapsed time,
thermal state, and how much this disc has been read since.

What IS established, and survives:

- The disc is physically excellent (§2.16): CU zero disc-wide, C1 comparable to
  a pressed CD, and the recurring site among the cleanest places on it.
- The burn is good. Links/underruns are not the cause (§2.16).
- Our read engine cannot splice sub-sector, and LBA and buffer position share one
  counter (§2.2). The substitution is drive-side.
- The C2 request is not the trigger (§2.17).
- When it does occur, it is invisible to every relative check we own (§2.9,
  §2.10).

**The one distinguishing factor not yet tested.** Both bad episodes were max-speed
reads with **no reload since the disc was last inserted or burnt, and a
substantial gap since the drive last touched it** — 40 minutes after the burn,
and 8.5 hours of idle. Every clean max-speed pass came within minutes of a
reload. A plausible firmware story: on eject/load the drive performs a full disc
calibration, but on spin-up after an idle with the same disc still loaded it
reuses stale parameters.

**Test that would settle it, costing no disc:** leave the disc loaded and the
drive idle for ~45-60 minutes with no eject, then read the whole disc at max.
Predicted BAD. That is the only untested factor that separates the two
populations.

#### 2.24 Test 1 — the powered C2 test: UNINFORMATIVE, as its own pre-written falsifier requires

Keith's test 1. 40 whole-disc passes at 32x, **interleaved** C2 / `--no-c2`,
`--sub raw` throughout so the 0.21.0 Q-position check ran on both arms, each
pass compared to the burnt image immediately. Design, prediction and falsifiers
written before running (`scratchpad/PREDICTION4.md`).

| arm | passes | misposition events | audio BAD |
|---|---|---|---|
| C2 requested | 20 | **1** | 0 |
| `--no-c2` | 20 | **0** | 0 |

Fisher one-tailed **p = 0.5**. This is the third falsifier as written: *"FEW OR
NO EVENTS IN EITHER ARM -> uninformative, full stop."* **The C2 question is not
resolved and remains open.** It is not resolved in either direction — this says
nothing about whether the C2 request matters.

**The base rate collapsed, which is the actual finding.**

| when | rate |
|---|---|
| 2026-08-26 morning | 3/4 = 0.75 |
| 2026-08-26 evening | 1/8 = 0.125 |
| 2026-08-26/27 overnight | **1/40 = 0.025** |

**A 30x swing in under 24 hours, same disc, same drive, same command.** This
outranks the C2 question in importance: any A/B on this fault needs its arms
interleaved (they were) *and* enough events to be worth comparing (there were
not). At 1/40, resolving a 4x effect would need hundreds of passes per arm —
days of drive time — so **the C2 question is not answerable by this method at
tonight's rate**. That is a fact about the fault, not a scheduling problem.

Note also the power arithmetic, computed BEFORE the run rather than after: even
at n=20/arm with a true 4x effect, p<0.05 requires the counts to land near
expectation (8 vs 2 -> p=0.032; 6 vs 2 -> p=0.118; 5 vs 2 -> p=0.204). The test
could confirm a large effect; it could never rule one out.

#### 2.25 What the run DID establish

**1. The Q-position check works unattended, at scale, with zero false
positives.** 40 whole-disc passes, 14 040 000 sectors, one fault detected and
**39 clean passes reporting nothing.** Combined with §2.20's 1 404 000 sectors,
the check now has ~15.4 M sectors of hardware evidence and has never once fired
on a clean read. That is the strongest support the feature has.

**2. The repair path works — audio was CLEAN on the pass that carried the
fault.** The event was the familiar shape:

```
  C2-flagged       : 0 sectors, 0 bits          <- C2 silent, as always
  subchannel Q     : 349829/351000 CRC-ok (99.67%), 1171 bad
  Q MISPOSITION    : 17 sectors
  audio            : CLEAN vs the burnt image
```

Same 17 sectors as §2.23. Detected, repaired by `qpos_rescue`, and the delivered
audio was byte-perfect — the second independent hardware confirmation of the
whole path, and this one arrived unattended in the middle of a 40-pass run.

**3. Q CRC failure remains useless as a signal, quantitatively.** 1171 bad
frames on the pass that carried the fault — 0.33% of sectors — against 17
mispositions. Anyone tempted to gate on Q CRC health would be swamped 69:1 by
benign failures. Position disagreement is the clean signal; CRC failure is not.

**Consequence for the two-lever recovery model.** Nothing here changes the
absolute-gate invariant, but it sharpens the practical advice: on this drive the
fault is rare enough that a single rip will almost always be clean, and rare
enough that *testing* for it needs the Q-position check rather than repetition.
A caller who wants protection should request `--sub raw` and read
`subq_misposition`, not run more passes.

#### 2.19b Keith's 20-read test — the cdrdao difference is NOT significant, and cdrdao fails too

10 AccuDisc reads at 32x then 10 cdrdao reads at 32x, same disc, no eject,
each compared to the burnt image immediately.

| | bad | clean |
|---|---|---|
| AccuDisc | **4** / 10 | 6 |
| cdrdao | **1** / 10 | 9 |

**Fisher exact, one-tailed: p = 0.1517. Not significant.** And cdrdao *failed*,
which ends the "cdrdao reads this disc correctly" reading of §2.17 outright.

| pass | sectors | span | true bit errors | displaced |
|---|---|---|---|---|
| accudisc1 | 4 | 224850–224853 | 0 | **4 × −2048** |
| accudisc3 | 64 | 319291–321113 | 64 | 0 |
| accudisc9 | 185 | 288401–291336 | 185 | 0 |
| accudisc10 | 2530 | 311113–317889 | 2530 | 0 |
| **cdrdao9** | 64 | 345818–347637 | **64** | 0 |

**Mode 1 is radial.** Every Mode 1 event in the session, both tools, lies beyond
**LBA 288 000 (~64 min)** — the outer 14 minutes of a 78-minute disc. That
matches the census exactly: the C1 peaks at 32x were at 346875, 347550 and
350775, the outermost samples. At 32x CAV the outer edge runs at the highest
linear velocity the drive ever sees, and that is where its read margin runs out.
Physical, tool-independent, and not our defect.

**Mode 2 remains AccuDisc-only but weakly.** Across the session: 5 displacement
events in 19 AccuDisc max-speed passes, 0 in 12 cdrdao passes. Fisher one-tailed
p ~ 0.068 — a lead, not a finding, and it pools passes across conditions.

**A correction: the C2 hypothesis was NOT ruled out.** §2.17 declared "the C2
request is not the trigger" because passJ/K/L were clean. With Mode 2 running at
roughly 26% per pass, three clean passes has probability 0.74^3 ~ 0.40 — that
test could not have detected anything. The claim was underpowered and is
withdrawn. **C2 is still the most conspicuous difference between the two tools:
we request C2 pointers, cdrdao does not.**

The design that could settle it: `--no-c2` vs default, **interleaved** rather
than sequential (the failure rate drifts, so sequential arms are confounded),
**20 passes per arm**. ~2 hours unattended, no discs. Note §2.20 may make this
moot — a Q-position check detects the fault regardless of what triggers it.

#### 2.20 SOLVED — the drive genuinely mis-positions, and its own Q subchannel says so

Keith asked for the Q subchannel at 224850. It answers the question completely.

Four whole-disc reads at 32x capturing audio + C2 + **raw P-W subchannel** +
the `subq_map` lane. `q1` caught a live Mode 2 event at the usual site. Decoding
Q per sector and comparing its **absolute MSF against the LBA we commanded**:

```
    LBA    expect | Q crc  Q abs MSF   Q lba  delta  subq_map
 224851  50:00:01 |   OK   50:00:01  224851     +0    1 (OK)
 224852  50:00:02 |  BAD          -       -      -    2 (CRC fail)
 224853  50:00:03 |   OK   49:32:55  222805  -2048    1 (OK)   <-- !!
 224854  50:00:04 |   OK   49:32:56  222806  -2048    1 (OK)
   ...          17 sectors, continuous, all CRC-VALID, all -2048
 224869  50:00:19 |   OK   49:32:71  222821  -2048    1 (OK)
 224870  50:00:20 |   OK   50:00:20  224870     +0    1 (OK)
```

**The drive is not handing back the wrong buffer. It genuinely believes it is
2048 sectors back.** The Q it returns agrees with the audio it returns, carries a
valid CRC, and is internally consistent across the whole run. There is one Q CRC
failure at 224852 — the transition sector, the moment position is lost — and the
run ends as cleanly as it began.

That kills the cache/buffer-aliasing reading of §2.9 entirely. This is a servo
positioning slip: the drive loses lock, re-acquires 2048 sectors earlier,
streams correctly from there, and re-acquires the true position ~17 sectors
later. Everything it reports about itself is self-consistent and wrong.

**Measured across all four whole-disc passes (1 404 000 sectors):**

| pass | audio-bad sectors | Q CRC failures | **Q position mismatch** |
|---|---|---|---|
| q1 (Mode 2 live) | 20 | 1599 | **17** — one run, all −2048 |
| q2 (clean) | 0 | 1670 | **0** |
| q3 (clean) | 0 | 1560 | **0** |
| q4 (Mode 1, 2052 bad) | 2052 | 5494 | **0** |

- **Zero false positives.** Three passes with no positional fault produced no
  position mismatch at all, despite ~1600 benign Q CRC failures each (the
  drive's normal subcode error rate at 32x on this disc — CRC failure alone is
  far too noisy to use as a signal).
- **Complete sensitivity to Mode 2.** 17/17 displaced sectors flagged. The 2
  remaining audio-bad sectors (224850, 224851) carry *correct* Q with wrong
  audio — the leading edge of the slip, where audio and subcode are offset in
  time — and lie within **2 sectors** of a flagged one.
- **q4 confirms the check is specific.** Mode 1 is not a positioning fault, and
  it correctly produced no position mismatches.

#### 2.21 THE FIX — and a concrete defect in what we already ship

`ACCUDISC_SUBQ_OK` is documented as "CRC-16 verified, ADR=1 position". It does
**not** compare that position against the LBA the caller asked for. So in the
table above, **the library labelled all 17 displaced sectors `1` (OK)** — a
sector whose own subcode proves the drive was in the wrong place is reported as
healthy. Verified directly from `q_1.qmap`.

**The check to add:** for every delivered sector, decode Q; if the CRC is valid
and ADR == 1, require `q_abs_lba == commanded_lba`. A mismatch means the drive
silently mis-positioned. On this data that is 100% sensitive to Mode 2 with zero
false positives over 1.4 M sectors.

Design notes for when it is implemented:

- It needs a **new `subq_map` state** (or a flag bit) — `OK` cannot keep meaning
  two different things. `BAD` is wrong too: the frame is perfectly valid, it
  just describes somewhere else.
- **Extend suspicion by a small margin either side of a mismatch.** The leading
  edge of the slip has correct Q with wrong audio; measured worst case here is
  2 sectors, so a margin of a few sectors covers it. Combine with the §2.8 rule
  (a fault invalidates the remainder of the transfer).
- It requires subchannel capture, which costs transfer bandwidth
  (`sector_len` 2742 vs 2646, chunk 23 vs 24). That is a caller choice, not a
  default to force.
- **This is the first check we have that catches Mode 2 at all.** C2 is silent,
  repetition-consensus agrees with itself, and the chunk-seam test looks at
  seams. §2.10 said only "vary the read"; this is better — it is a direct
  contradiction between what we asked for and what the drive says it did.

**One earlier claim narrowed.** §2.9 said the splice byte was invariant. Under
subchannel capture (chunk 23) it landed at byte **2264**, not 2256, and the run
was 20 sectors. The site recurs; the exact byte and extent move with the
transfer geometry. "Byte-identical" held only within one chunk size.

#### 2.22 IMPLEMENTED 0.21.0 — the Q-position check, and the defect I nearly shipped inside it

`ACCUDISC_SUBQ_MISPOSITION` (0x5) and `read_stats.subq_misposition` are live.
The counter went into the 4 bytes of tail padding the 0.9.0 note reserved, so
the struct did not grow. Surfaces updated in step: engine, CLI (human line +
`summary subq_misposition=` token), Python binding (`SubQState.MISPOSITION`,
`ReadStats.subq_misposition`, `ReadStats.positional_fault`), man page,
`cli-machine-interface.md`, and the two pin tests. 44/44.

**Detection** is a comparison, not an inference: decode Q for every delivered
sector and require `q_abs_lba == commanded_lba` whenever the frame is CRC-valid
with ADR=1. Verified live on hardware — a whole-disc read reported
`Q MISPOSITION : 22 sectors`.

**THE DEFECT, found before shipping and worth recording in full.** The first cut
routed a Q mismatch into `consensus()`. Reading that function settles it:

```c
memcpy(r->samples, sec, r->sector_len);   /* sample 0 = the copy in hand */
count = 1;                                 /* alt was NULL, so that is ALL */
...
if (adsc_audio_diff(r->scratch, r->samples + i * r->sector_len) == 0) {
        memcpy(sec, r->scratch, r->sector_len);
        return 1;                          /* "agreed" -> RECOVERED */
```

For this fault, sample 0 is the known-bad displaced copy and **the slip
reproduces** — the same site returned the same displaced data across five
independent whole-disc passes. A faithful reproduction would therefore "confirm"
the corruption, and the sector would be relabelled RECOVERED. The check that
found the fault would have laundered it. Worse than not checking.

This is the project's recorded dominant failure mode in a new place: *a check is
only worth what its inputs can distinguish.* The **detector** was sound — it uses
the drive's own position claim, which the fault cannot fake — but the **repair**
was validated by byte-agreement between rereads, the one signal Mode 2 defeats.
Detection and repair must not share a weakness.

**The fix — `qpos_rescue()`.** The bad copy is never a vote. A replacement is
accepted only when its **own Q frame** agrees with the commanded LBA, i.e.
validated by the same independent signal that detected the fault. A reread whose
Q is unreadable is rejected too: with no claim there is nothing to check, and
silence is not consent. No position-correct copy after the ladder is exhausted
=> the sector is marked SUSPECT, honestly, rather than recovered.

**Counting.** `subq_misposition` counts only sectors that actually disagreed.
The engine additionally treats `ADSC_QPOS_MARGIN` (4) sectors either side as
suspect — the leading edge of a slip carries correct Q with already-wrong audio,
measured worst case 2 — but those are NOT counted, so the number means what its
name says.

**Known gap.** The detector is unit-tested (`tests/test_map.c`, including a
falsification run: with the position comparison removed the test aborts). The
**repair path needs a drive** and has no desk test. A green suite is not
evidence that `qpos_rescue` behaves on hardware.

**Why `subq_map` can read 0 while the counter is non-zero.** The map's referent
is the sector as DELIVERED (documented in the header). A detected-and-repaired
sector is delivered correct, so its Q matches and the lane says OK; the
`status_map` shows RECOVERED. MISPOSITION appears in the lane only for sectors
the rescue could not fix. That is coherent, and it is why the counter — not the
lane — is the thing to watch.

#### 2.23 VERIFIED ON HARDWARE — detected, repaired, and the audio came out right

Eight whole-disc passes at 32x on the fixed build. One carried the fault:

```
Q MISPOSITION       : 17 sectors whose valid Q named a DIFFERENT LBA
status_map RECOVERED: 22 sectors  224849..224870 (one run)
subq_map MISPOSITION: 0
audio at 224800..224899: CORRECT
```

Every number checks out against the independent offline analysis of §2.20:

- **17 detected** is exactly the 17 Q-position mismatches measured offline in
  `q1` (224853..224869). The detector's calibration is corroborated by a
  separate measurement rather than by itself.
- **22 recovered** is those 17 widened by `ADSC_QPOS_MARGIN`, clipped to the
  transfer: 224849..224870. The margin picked up 224850..224852 — precisely the
  leading-edge sectors that carry *correct* Q with already-wrong audio, which is
  the case it exists for and the case a bare mismatch test would have missed.
- **subq_map 0** because every sector was repaired, so the delivered Q agrees.
  The counter is the signal; the lane shows only what could not be fixed.
- **Zero suspects** — `qpos_rescue` found a position-correct replacement for all
  22, so nothing had to be given up.
- **The audio is byte-correct at the site.** In four earlier passes this exact
  run came back displaced and silently wrong.

So the whole path works: detect by contradiction, repair by an independent
signal, deliver correct data, and report it.

**Rate note.** 1 of 8 passes carried it here, against 3 of 4 this morning. The
fault is intermittent and its rate drifts substantially over hours — which is
why every arm of a comparison has to be interleaved, not sequential (§2.19b),
and why "N clean passes" means nothing until N is large.

**Still no desk test for the repair path.** This is hardware evidence, one
occurrence. It is the right evidence, and it is a single sample.

### 3. Keith's ruling on the interface — NOT YET DONE

The measurement must run **end-to-end in the API**, in RAM, with no files:

- The audio is generated **inside AccuDisc**, never by an external tool.
- `accudisc write-offset` alone must suffice — no `--signal <file>`.
- The signal is 13.23 MB, so it is too large to bake in as a build-time hunk;
  generating into RAM is the fix.
- **Python AND Rust bindings are required.** "No consumer will be using the CLI
  at all, for any purpose."

This overrules the mechanism-only design that shipped in 0.20.0 (library
supplies signal + locator, caller orchestrates the burn and read-back).

### 4. A silent-truncation path found while diagnosing the above — `[P2]`, OUR DEFECT — **DONE 2026-08-28 (0.30.0)**

`read_sink` in `cli/main.c` makes **five `fwrite` calls and checks none of
them** (audio, C2, subchannel, CD+G packs). `ferror` is never consulted either,
and the sink always returns 0. So ENOSPC, a full filesystem, or any write error
part-way through a rip produces a **short or truncated output file and exit 0** —
"the rip succeeded" while the data is wrong.

This is NOT the cause of the displacement in §2 — the pass files are all exactly
the right length, and a writer cannot invent audio from 2048 sectors away — but
it is the same failure shape the project already treats as its dominant one: a
well-formed output that nothing downstream can catch.

Fix: check each `fwrite` return, latch the failure in `struct read_ctx`, and
have `cmd_read` fail loudly. The sink's only current signal back to the engine
is a non-zero return, which the engine maps to `ACCUDISC_ERR_CANCELLED` — the
wrong name for an I/O error, so the exit-code mapping in
`docs/reference/cli-machine-interface.md` needs a look at the same time.

**DONE.** The write/close checking moved to `cli/sink.c` + `sink.h`, split out
of `main.c` for the reason `format.c` was — so `/dev/full` can stand in for a
full filesystem and the truncation path is testable with no drive and no disc
(`tests/test_cli_sink.c`, 8 cases, mutation-tested).

The count was five, not four: the fifth is `dump_to_file`, which serves
`fulltoc FILE` and `cdtext FILE` and announced `N bytes -> path` over a write
and a close it had never checked. Fixing only `read_sink` would have left it.

`cmd_read` keeps the device's own message for a real device error but reports
the latched write error where the engine merely relayed our sink saying stop —
`ERR_CANCELLED` names the messenger, not the cause. Exit 2, plus an
`error write <lane> <strerror>` token on `--progress-fd`; both documented.

Two findings worth keeping from doing it:

- The first version of the report was guarded on `ret != 1`, meaning "argument
  validation already owns the exit code". But `cmd_read` INITIALISES `ret = 1`,
  so the guard was true on exactly the path it existed for: `--pcm /dev/full`
  gave a **silent exit 1**. Well-formed condition, wrong referent. Only the
  end-to-end run found it — the unit test does not exercise `main.c`.
- `cli_sink_close` must set `errno` even when handed no latch, because
  `dump_to_file` has `strerror(errno)` as its only way to say why. Without it
  the message read `writing /dev/full FAILED: Success`.

## OFFSET DICTIONARY — REDUMP is AccurateRip, frozen in 2022 (2026-08-22) — DONE

**Supersedes the 2026-08-19 section below wherever the two disagree.** That
section reasons about REDUMP and AccurateRip as two corpora that happen to
overlap heavily. They are one corpus at two dates, and several conclusions below
were fitted to the wrong model — the six "unresolvable" conflicts most of all.

### The finding

REDUMP's offset table is AccurateRip's published list, imported once and frozen.
Established from redumper's own git history, which is a full clone in
`private/code/redumper`:

- The table has two commits ever. `15f369e` (2025-05-28) *created* `offsets.ixx`
  by deleting `driveoffsets.txt` and an 80-line converter, `generate_offsets.cc`.
- `driveoffsets.txt` has three commits: imported 2022, line endings normalised,
  deleted. **It never grew a row.**
- Its format is AccurateRip's own four columns —
  `TEAC - DW-224E-CN\t+120\t2\t50%` — including AccurateRip's `[Purged]`
  markers, which the converter skips.
- **The converter reads columns 1 and 2 only.** The submission count and the
  agreement percentage were discarded at import.
- Set-compared, normalising the `" - "` separator and the vendor renames:
  **4595 rows each way, zero unique to either.** The only transformation is
  marketing vendor names rewritten to INQUIRY ones — `LG Electronics` →
  `HL-DT-ST`, `Panasonic` → `Matshita`, `Lite-On` → `JLMS`.

Corroborated externally: redump.org's own dumping guides tell dumpers to take
the drive read offset from EAC or the AccurateRip list. There is no independent
read-offset corpus on that side. (redump.org's *disc write* offsets are their own
measurements; nothing here touches those.)

### What it cost, and the rule that fixes it

Every REDUMP-vs-AccurateRip disagreement was AccurateRip correcting itself, with
the live counts against the 2022 ones lopsided the same way every time:
1065/3, 652/3, 554/1, 477/7, 263/6, 198/2, 23/2, 7/2. **All six of the
"unresolvable" conflicts listed in the 2026-08-19 section are in that list.**

`tools/gen_offsets.py` now takes `--redump-provenance` (REQUIRED — optional
would let a future run silently produce a table with the rule not applied) and
drops REDUMP values on two arms, kept apart in the report:

    RETRACTED   in the 2022 import, absent from the live list        8 rows
    SUPERSEDED  still live, different offset now                     9 rows
    unknown provenance — kept, listed, rule NOT applied             15 rows

No agreement threshold: absence is the signal. A threshold would keep
`Philips DVD-ROM PCDV632` (+116, one submission, 100% agreement, withdrawn).

**That row is a real PC drive, and the rule drops it anyway. State the trade
rather than hiding it.** A 2006 snapshot of `cclo.ntu.net/mac/Mac.htm` lists it
in a Pentium III build — `光碟機: Philips PCDV632 6X DVD`, an optical drive
beside an Adaptec AHA 2940 U2W and IBM SCSI disks — so it is a genuine Philips
6X DVD-ROM, consistent with the `PCDV` family still live in AccurateRip
(`PCDV5016L2` +6/13 subs, `PCDV5016P1` +738/15, `PCDV6116` +691/2). An earlier
identification of it as a standalone consumer player (DVP630/632/642) was Keith's
and he withdrew it; this note carried that claim for one commit.

So the case against the row is narrower than "it is not a drive", and it still
holds: ONE submission, and AccurateRip withdrew it. We do not republish what the
source retracted, and a real drive does not make a lone measurement of it good.
What the rule costs here is a plausible offset for a rare drive; what it buys is
that no caller receives a number its own publisher has taken down.

The agreement figure remains useless for gating and this row shows why, whatever
the hardware turns out to be: **100% on a single submission is agreement with
oneself.** Agreement measures consistency AMONG submissions. It cannot rank a
row that has only one.

And a caution on method: name-shape reasoning was wrong three times over this
rule — the box read `PCRW` not `CDRW`, `CDRW` turned out to be a legitimate
Philips INQUIRY prefix (12 live rows, 403 submissions), and `PCDV632`'s
three-digit model number proved nothing. Only the counts and the retraction were
ever load-bearing.
The 15 unknowns are the rows a whole-field vendor rewrite cannot reach, where
AccurateRip prints the vendor run into the product (`SATA LG ELECTRONICSBD-RE B`).
They are named on every run — a join that resolves everything by letting misses
fall through to the safe branch is a guard measuring its own scope.

The withdrawn rows are listed **in `offsets_db.inc` itself**, so the deletion is
reviewable in `git diff` forever rather than in generator output that scrolls
away.

### Consequences already applied

- **The table has no conflicting keys left.** `ERR_AMBIGUOUS` is unreachable
  from the shipped data; the code path stays, because a corpus refresh can
  revive it. `F_CONFLICT` and `F_ADJUDICATED` are carried by zero rows.
- Two vendor aliases added, each measured: `JLMS` → `LITE-ON` (9/9 agree, 0
  differ) and `CENDYNE_` → `CENDYNE` (1/1). Alias coverage moved
  4554/33/245 → **4564/23/235**.
- 0.12.0, and the bump is the point: 8 keys that returned an offset now return
  `ERR_NOTFOUND`, 9 return a different offset.
- `ATTRIBUTION.md`, the `sources` comment in the public header, and the file
  comment in `src/drive/offsets.c` all said or implied "two collections". All
  three corrected.

### Left open

- ~~`gen_offsets.py` keeps only the highest-submission row of an AccurateRip
  duplicate.~~ **DONE 2026-08-22 (0.12.1).** Rows are pooled by (key, offset) —
  submissions summed, agreement as the submission-WEIGHTED mean — and only then
  does the largest pool win the key. 69 keys pooled, **933 submissions**
  recovered against the previous behaviour, 91 table rows changed, exactly one
  agreement percentage moved (`TSSTCORP CDDVDW SE-218GN`, 193 at 100% plus 4 at
  75% -> 197 at 99%; a flat mean would say 88), and NO offset changed.

  Two figures in the section above were quoted as 66 keys / 930 submissions.
  Those were measured on the RAW (vendor, product) key; the generator keys with
  `fold()`, which also pools rows AccurateRip split differently between the two
  fields. 69 / 933 is the same finding counted the way the code counts it.

  The recovery figure needs its baseline stated or it is nonsense: measured
  against the FIRST row rather than the LARGEST — which is what a naive counter
  does, and what the first version of this one did — the identical change
  reports **12955** submissions. Arithmetically correct, and it describes a
  behaviour the generator never had.
- **What AccurateRip keys on is unknown.** 4878 rows carry 4802 distinct
  vendor+product names; 75 names appear more than once, and the counts are not
  always lopsided (`DVD RW` 298|267, `SLIMTYPE DVD A DS8A4S` 136|197|12). It
  does not normalise near-identical strings — `SIimtype` survives beside
  `Slimtype` — so the display name is not the key. Firmware revision is the
  obvious candidate and is **not established**.
- **A. UNDERSCORE-INSENSITIVE KEYING in `fold()` — DONE 2026-08-24 (0.13.0).**
  Landed as specified: `fold()` folds underscore to space AFTER the alias
  lookup, every spelling is still emitted, `redump_retracted` fell 8 -> 7 and
  `redump_unknown_provenance` 15 -> 14. Table 5879 -> **5881 rows**, a number
  PREDICTED before regenerating and decomposed: +1 `HL-DT-ST DVDRAM_GHA2N`
  rejoining, +1 `TSSTCORP CDDVDW` (below). Zero rows removed, **zero offsets
  changed on any name that already resolved**, 96 rows carry a higher
  `ar_submissions` because the two spellings pool their evidence, and every
  merged group's spellings now agree on every field. `conflicting_keys` stayed
  0, which IS the "26 agree, 0 disagree" prediction checked at generator level.

  Two things the brief did not anticipate:

  1. **The fold exposed a pre-existing defect it did not create.** `read_ar()`
     returns ONE pooled row per key, so every other spelling of that key died
     between the pool and the table. That already cost `("TSSTCORP", "CDDVDW")`
     — a drive answering `ERR_NOTFOUND` while its identical measurement shipped
     under `("", "TSSTCORP CDDVDW")`. Underscore folding would have widened the
     loss to 25 names, so the carry-through fix is part of this change:
     `read_ar()` now hands `merge()` every spelling that fed the key, across all
     of its offsets rather than the winning one, since a spelling is a NAME a
     drive reports and not evidence for a value. The three keys that look like
     they survived this before (`DVDROM GO-D1600B`, `TSSTCORP BDDVDW`/`CDDVDW`)
     survived only because REDUMP happened to carry the other spelling. Luck.

  2. **The guard for it was VACUOUS on its first version, and was measured to
     be.** `assert_every_name_survives()` originally built its requirement from
     `read_ar()`'s own output. Sabotaging the spelling carry-through shrank the
     requirement and the table together, so it PASSED on a table missing 25
     names. It now re-reads the AR JSON itself; sabotaged at two independent
     points it exits 1 and names all 25. **A guard whose reference is derived
     from the thing it guards cannot distinguish the failure it exists to
     catch** — and the row count matching its prediction would have read as
     corroboration. Scope is `kept` (post-retraction) plus raw AR: widening it
     to the raw REDUMP table would fire on the 16 rows the retraction rule is
     meant to remove, and a guard that cries wolf gets weakened.

  Three follow-ups landed on top of it, each falsified before being trusted:

  - **The guard checks BOTH directions now.** `emitted - required` must also be
    empty: a table may not carry a name no source reported. That is precisely
    B's stated condition, so **B's rescue-only rule is now structural** — a
    rebadge line that synthesised `MEMOREX MD6032` aborts the run and names it
    (verified by doing exactly that). Reviewing a rebadge line is therefore "is
    this mapping true?", never "did this mapping invent anything?".
  - **`apply_retractions()` asserts conservation** — every input row leaves
    kept or dropped, exactly once. The coverage guard re-reads the AR file but
    takes its REDUMP half from `kept`, this function's own output, so a row lost
    here would shrink the requirement and the table together and the guard would
    stay silent: the vacuous-guard shape again, one function upstream.
  - **`read_provenance()` pools duplicates instead of keeping the larger**,
    matching `read_ar()`. The two arms had diverged silently at 0.12.1. 53
    (key, offset) pairs collide — only 24 from the fold, the other **29 are
    names the 2022 file simply lists twice** (`ASUS - DRW-24B1ST` at +6 on 2
    submissions and on 686), so this predates the fold. No currently withdrawn
    row is among the 53, so the table is byte-identical: the fix is correct and
    inert today, which is the moment to make it rather than when a future corpus
    makes one of the printed "2022: N subs" figures wrong.

  Original brief, kept for the measurements:

- **A (as specified). UNDERSCORE-INSENSITIVE KEYING in `fold()` — Keith said do it, 2026-08-22.**
  `HL-DT-ST DVDRAM_GHA2N` (+667) was dropped as RETRACTED while
  `LG Electronics DVDRAM GHA2N` (+667, 71 subs) is live: the same drive, an
  underscore against a space, so the provenance join missed it. Measured before
  being proposed, and DO NOT re-derive:

        distinct keys today                       4822
        underscore-folding would merge      26 groups / 52 keys
           groups agreeing on the offset          26
           groups disagreeing                      0

  Zero disagreements — the same shape the case-fold change measured. 59 live
  AccurateRip products contain underscores (Lenovo `USB_SATA_Burner3`, LG
  `BD-RE_BT20N`). **Every distinct spelling must still be EMITTED**, exactly as
  with case: the fold pools evidence and joins provenance, it never collapses
  what a drive can report. Expect `redump_retracted` to fall 8 -> 7.

  Measured again on landing, and the variant matters: underscore -> SPACE merges
  26 groups / 52 keys; underscore DELETED merges only 2. Substitution is the
  rule that matches how the source spells these names. The fold also subsumes
  the trailing-underscore vendors `VENDOR_ALIAS` was naming one at a time — the
  2022 import's `Generic_ - DVD-ROM` joins with no alias line, which is what
  moved `redump_unknown_provenance` 15 -> 14. `FREECOM_`/`CENDYNE_` are left in
  the alias table: now redundant, but removing them widens the diff for nothing.

- **B. A REVIEWED REBADGE TABLE — DONE 2026-08-24 (0.14.0).** `REBADGE` in
  `tools/gen_offsets.py`: exact whole-key mappings in `fold()` form, one line per
  cited human decision, consulted ONLY where a row is about to be dropped as
  RETRACTED and only ever to KEEP it. One entry so far —
  `PHILIPS DVD-ROM PCDV632` -> `TOSHIBA DVD-ROM SD-M1212`. Table 5881 -> **5882**
  (predicted first), `redump_retracted` 7 -> 6, zero rows removed, zero offsets
  changed, zero names invented.

  **It ships as REDUMP-only with zero AccurateRip figures**, and refusing to
  attach SD-M1212's 38 submissions is the honest half of the design: nobody
  measured a drive under the Philips name. But that makes the shipped row
  indistinguishable from any uncorroborated REDUMP row, so the evidence lives in
  a `RESCUED BY REBADGE` block in `offsets_db.inc` — parallel to the WITHDRAWN
  block, naming both sides and both counts, and **stating the trade rather than
  only the case for it**: this republishes a row the publisher took down, on a
  human judgement.

  Five guards, each MADE TO FAIL before being trusted:

  | branch | behaviour | proven by |
  |---|---|---|
  | rows AGREE | rescue, and say so | the shipped run |
  | rows DISAGREE | report, do NOT apply | retarget to SD-M1222 (-472) |
  | target NOT LIVE | report stale mapping, do NOT apply | retarget to SD-M9999 |
  | entry fires on NOTHING | report the untested line | rename the key |
  | rescue cites a twin absent from THE TABLE | **exit 1** | move SD-M1212 to +999 |

  Two things worth keeping:

  - **`assert_rescues_are_corroborated()` exists because "live upstream" and "in
    this table" are different claims.** `apply_retractions()` checks the OEM row
    against AccurateRip's pooled map, which is the right test for agreement and
    the WRONG one for what the shipped comment then asserts. They come apart the
    moment anything downstream drops the OEM side, leaving a rescued orphan.
  - **A GUARD CAN BE SHADOWED BY AN EARLIER GUARD.** The first attempt to falsify
    the corroboration check DELETED the OEM row — which trips the coverage guard
    first, so the run failed for the wrong reason and proved nothing about the
    check under test. Changing the offset instead (name still emitted, coverage
    still green) is what actually exercised it. **A sabotage that trips two
    guards proves only the first.**

  The rescue is RETRACTED-only: a SUPERSEDED row is one where AccurateRip
  corrected the value rather than removing the name, and republishing it would
  restore a number the publisher has replaced.

  Original brief:

- **B (as specified). A REVIEWED REBADGE TABLE — Keith said build it WITH the rescue-only
  guard, 2026-08-22.** Rebadged drives report the REBADGE string over INQUIRY,
  so dropping a rebadge row strands its owner even when we hold the right offset
  under the OEM name. The worked case:

        Philips PCDV632   +116   1 sub, RETRACTED and dropped
        Toshiba SD-M1212  +116  38 subs, live       <- the same drive

  `archive.rpc1.org/farzeno/club-internet/dvd/dvdfi.htm`: "Philips PCDV632
  (6X/32X) (Known firmware : 1P16) Toshiba SD-M1212 OEM drive RPC-1". The
  relation is predictive where it can be checked — `Philips PCA532` (+116, 1 sub)
  is a Toshiba SD-M1202 rebadge and SD-M1202 is +116 on 19 submissions; both
  survived and both ship.

  Shape: product-level alias, exactly parallel to `VENDOR_ALIAS` — EXACT
  whole-field mapping, one line per human decision, never a similarity match.
  Applied only when the two rows AGREE on the offset; a disagreement means the
  mapping or the data is wrong and gets REPORTED, not applied.

  **THE GUARD, and it is the whole of Keith's condition: rescue only, never
  synthesise.** A rebadge alias may keep a row that EXISTS in the corpus. It may
  never emit a row for a rebadge nobody submitted — the same page names
  `Memorex MD6032` and `DVD-632` as SD-M1212 rebadges and neither is in either
  corpus, so emitting them would publish an offset nobody measured under a name
  nobody reported. That is the "researched data must not share a field with
  reported data" hazard recorded further down this file.

  Do NOT try to rescue by "a live row shares this offset" — measured and
  useless: +116 has 43 live rows, +6 has 1888 rows across 206302 submissions.
  The link is the rebadge, which is human knowledge and not in the numbers.

- ~~**`ERR_AMBIGUOUS` has no test exercising it.**~~ **DONE 2026-08-24.**
  `tests/test_offsets_ambiguous.c` + `tests/offsets_ambiguous_db.inc`.
  `src/drive/offsets.c` now takes its table through `ADSC_OFFSETS_DB` (defaulting
  to `offsets_db.inc`, so the library is unchanged) and the test COMPILES THAT
  FILE against a 10-row fixture — a second copy of the matcher would only assert
  that the copy works. No version bump: nothing observable changed.

  **Every row of the fixture is built to discriminate**, because most of the
  obvious assertions here are vacuous otherwise. `ar_submissions == 0` proves
  nothing unless the first matching row carries a NONZERO count (it carries
  1234/99); `sources == 3` is indistinguishable from "took the first" unless the
  rows carry DIFFERENT bits (1 and 2). Covered: the sentinel `read_offset`
  surviving, `values[]`/`value_sources[]` in table order, the AccurateRip figures
  being cleared rather than inherited, normalisation being what MAKES a key
  ambiguous (`"fixture"/"SPACED  KEY"` vs `"FIXTURE"/"SPACED KEY"`), `n_values`
  clamping at 5 rows against `MAX_VALUES` 4 with the dropped value appearing
  nowhere, the fifth row's sources/flags STILL reaching the caller, and the
  `accudisc_offset_for_device` / `accudisc_read_offset` arms.

  Seven falsifications, each aborting on the RIGHT assertion: not clearing the
  AR figures, taking the first `sources` instead of OR-ing, reporting `n_values`
  unclamped, filling `read_offset` on a contested key, breaking the scan once
  `values[]` fills — plus two WIRING failures, omitting the `-D` and naming the
  fixture `offsets_db.inc`. **The second of those is not hypothetical**: the
  quoted `#include` form searches `src/drive/` first, so a fixture wearing the
  real name IS silently shadowed by the real table. Both wiring failures are
  caught by the test's first assertion, which exists for exactly that reason.

  It also carries an **ABI TRIPWIRE**: `assert(ACCUDISC_OFFSET_MAX_VALUES == 4)`.
  Not a live defect — the macro has been 4 since the struct was introduced whole
  in 0.10.0 (`24ac59e`, verified across every commit that touched the header), so
  no conforming caller has a smaller `values[]`. It fires at the moment the
  capacity note below stops being latent, which a comment cannot do.
- **PRODUCT-ONLY KEYING (2026-08-19 point 5) — DONE 2026-08-24 (0.15.0).**
  The lookup keys on the product identifier and the vendor only narrows; a
  vendor matching no row no longer rejects.

        product match -> candidates
        -> some candidate's vendor matches?  narrow to those
        -> otherwise                          keep them all
        -> one DISTINCT offset: OK  |  more: ERR_AMBIGUOUS with values[]

  Measured on the shipped table before writing it, and the second line is the
  one a rewrite gets wrong:

        distinct products                            4562
        products with >1 DISTINCT offset               13   (vendor resolves ALL 13)
        products matched by >1 ROW                   1242
           ... whose rows AGREE on the offset        1229
        worst product ('CD-ROM')                        4 offsets = MAX_VALUES

  **Count DISTINCT OFFSETS, not matching rows.** The table deliberately carries
  a row per spelling, so multi-row matches are the normal case; counting rows
  would report `ERR_AMBIGUOUS` for 1229 products whose offset is not in doubt.

  **Exactly ONE answer changes across all 5882 keys already in the table**:
  `DVDROM` with an EMPTY product, now `ERR_NOTFOUND`. An empty product is not a
  weak identifier but the absence of one, and keyed on the product alone it
  would have answered +564 for every drive reporting no product string. Zero
  AccurateRip figures changed. Everything else the change buys is on queries
  whose vendor string is NOT what a submitter sent — which is the point.

  **What it cost — CLOSED 2026-08-24 (0.16.0), Keith called it.** A generic
  product answered for any vendor. Most self-identify by colliding (`CD-ROM`
  holds four offsets and comes back ambiguous), but seven did not: the empty
  product (refused categorically in 0.15.0), and `DVD` +48 FUJITSU, `COMBO` +6
  E-ELEI, `DVDRW` +6 DEXPRESO, `DVD+RW` +1292 ATAPI, `OPTICAL DRIVE` +6 BUFFALO,
  `CD-ROM DRIVE` +12 900 40X — one row each, nothing to collide with.

  `GENERIC_PRODUCTS` in `tools/gen_offsets.py`, alongside `VENDOR_ALIAS` and
  `REBADGE`: exact whole-field, one line per human decision, **never a pattern**
  — a regex over product names is the `nnXnnX` rule that was measured
  overzealous, because some drives really are called `16XDVD-ROM-AMH`. The
  generator marks matching rows `ACCUDISC_OFFSET_F_GENERIC` (0x08) and reports
  any entry that fires on nothing. Exactly six rows changed, flags 0 -> 8;
  nothing else moved.

  **THE BLOCK IS ON THE PRODUCT-ONLY PATH, NOT THE ROW**, and that is the whole
  design. `BUFFALO OPTICAL DRIVE` rests on **85 submissions** — dropping it would
  throw away a real measurement to fix a matching rule, the opposite of Keith's
  "genuinely useful, not ignore data without good justification". So the row
  ships and still answers for its own vendor; what it may no longer do is answer
  for a vendor it has never been seen with. Two rules of deliberately different
  strength: an EMPTY product can never answer, a GENERIC one answers when the
  vendor narrows.

  **NOT extended to the generic names that COLLIDE.** `CD-ROM` is at least as
  generic and stays reachable: it refuses to pick rather than picking wrongly,
  which is a different and safer failure, so blocking it would remove
  information without removing a hazard. A stated choice, not an oversight.

  Falsified in four directions, each caught in BOTH tables: the block absent
  (0.15.0 behaviour); **the block TOO STRONG** — the row unreachable even for its
  own vendor, which is how the 85 submissions would have been lost silently; the
  generator never setting the flag; and an entry that fires on nothing. Carried
  through to `DriveOffset.generic_product` and a CLI `generic_product 1` line
  printed only when set, so an ordinary drive's output is byte-identical.

  Consequences carried through: `ACCUDISC_OFFSET_F_TRUNCATED` (0x02) says
  `values[]` could not hold every distinct offset — unreachable today, since the
  worst product holds exactly `MAX_VALUES`; `accudisc offset --product P` alone
  is now a complete CLI query (`--vendor` alone stays a usage error); the Python
  binding gained `DriveOffset.truncated`; the generator emits an `AMBIGUOUS ON
  THE PRODUCT ALONE` block naming all 13; the usage golden was regenerated after
  reading its one-line diff.

  The AccurateRip figures on an OK answer come from the SINGLE best-evidenced
  contributing row, as a pair. 232 products have several rows backing one offset
  with different counts (`""` at 3 submissions beside `"SHARK"` at 1); summing
  would multiply the spelling variants of a single entry, which the generator
  gives identical figures, and taking whichever row the scan reached first is
  arbitrary.

- **THE EIGHT-BYTE VENDOR FIELD — CLOSED 2026-08-25 (0.17.0), Keith spotted
  it.** He read the generated `AMBIGUOUS` block and said the two `ROM` lines were
  wrong: not one product held at two offsets, but two drives. He was right, and
  the cause reaches further than those lines.

  The SCSI INQUIRY vendor field is **eight bytes**. A drive whose name is longer
  simply continues into the product field, and AccurateRip publishes the two
  halves separately, so the corpus records the cut faithfully:

  | joined name | vendor (8 chars) | product |
  |---|---|---|
  | `ATAPI CD-ROM` | `ATAPI CD` | `-ROM` |
  | `16X DVD-ROM` | `16X DVD-` | `ROM` |
  | `DVDROM 8X` | `DVDROM 8` | `X` |
  | `DVDROM 10X` | `DVDROM 1` | `0X` |

  Mid-word cuts put the mechanism beyond doubt — `ATAPI 16` + `X DVDROM VA100`,
  `hp BD RO` + `M BC-5550H`. **The split is not ours**: `split_drive_name()` cuts
  on whitespace-hyphen-whitespace, and AccurateRip's own file prints
  `DVDROM 8 - X`. 8-char-ness is NOT a detector — `TSSTcorp`, `Slimtype`,
  `Verbatim`, `GoldStar` are genuine 8-char vendors, 618 AR rows have a full
  vendor field, and what separates a spill is that concatenating the halves
  WITHOUT a separator yields a real name. That is a human judgement per row.

  **The lookup was never wrong for `ROM` or `16X`.** Both real drives resolve
  (`ATAPI CD`/`ROM` -> +12, `16X DVD-`/`ROM` -> +738): the vendor carries the
  identity here — the inverse of the design's assumption — and the collision
  forces `ERR_AMBIGUOUS` where it cannot. What was wrong was the generated
  commentary calling a fragment a product; the block header now says so.

  **What DID answer wrongly: `X` and `0X`** — the two judged indefensible, not
  the only fragments reachable. A one-character product returned
  +564 for any vendor at all — the `GENERIC_PRODUCTS` hazard from a second cause,
  so it gets the same remedy. Both are unbranded drives whose entire reported
  identity is a category plus a speed, so there is no model to recover by
  rejoining. **The rows stay**: +564 is what that generation measures (14 other
  rows hold it, `HITACHI DVD-ROM GD-2500` on 48 submissions, `SAMSUNG CD-ROM
  SN-124` on 48), and the string still answers when the caller supplies the whole
  of it. `GENERIC_PRODUCTS` is therefore no longer only category words.

  **NOT extended to every fragment, and the boundary is stated rather than
  implied.** Five others still answer for any vendor — `-ROM` +12, `-952E-AKV`
  and `-956E-AKV` +691, `-ROM JOYBEE610` +691, `ROM DRIVE 50MAX` +12 — because
  each is DISTINCTIVE enough that nothing but the drive it was cut from will
  report it, so answering on the product alone answers for the right drive. The
  test is "could another drive plausibly report this string?", not "is this a
  fragment?": being a fragment is a fact about the firmware, not by itself a
  hazard. Pinned in `tests/test_offsets.c` beside the `CD-ROM` collide choice.

  **A RETRACTED ROW THAT ESCAPED, found while checking the family.**
  `("DVDROM", "")` at +564 was in AccurateRip's 2022 list and absent from the
  live one, so it should have been dropped in 0.13.0. AccurateRip writes a vendor
  with no product as `DVDROM -`, and `read_provenance()` required whitespace on
  BOTH sides of the separator, so the name keyed as the PRODUCT `DVDROM -` and
  joined nothing. **A provenance miss is not loud**: `apply_retractions` takes its
  unjoined branch and `continue`s, so the live-AR check never runs and the row is
  KEPT, merely counted as unknown provenance. It was harmless only because the
  empty-product guard refuses it — luck, not design. Two parsers read the same
  ` - ` convention (`split_drive_name()` and `read_provenance()`) and BOTH
  mishandled a trailing separator, so they agreed with each other and disagreed
  with REDUMP; agreement between two implementations of one mistake looks exactly
  like corroboration.

  Predicted before regenerating, exact on all four: **entries 5882 -> 5881**,
  `redump_retracted` 6 -> 7, `redump_unknown_provenance` 14 -> 13,
  `products_generic` 6 -> 8. Three data-row changes and one new comment line,
  nothing else moved. Falsified in both directions: the parse arm neutered
  (guard fires, names `DVDROM -`, first failure not a shadowed one) and the
  `F_GENERIC` flag removed (`test_offsets.c:337` fires, the product-only
  assertion rather than a later one).

  **Consequence worth knowing: the shipped table now has ZERO empty-product
  rows**, so `test_offsets.c`'s `("DVDROM", "")` assertion can no longer tell the
  guard FIRING from the key being ABSENT. It is kept as a regression pin on the
  retraction and says so; the guard's discriminating test is `EMPTYP` in
  `tests/test_offsets_ambiguous.c`, against a fixture that really holds one.

- **CLOSED 2026-08-25 (0.18.0) — the same parse bug on the AccurateRip side,
  and Keith confirmed the row live on the website.** `LG Electronics -` really is
  published: a vendor with an EMPTY product. `split_drive_name()` required
  whitespace on BOTH sides of the separator, so it parsed as the PRODUCT
  `LG ELECTRONICS -` and shipped as `{ "", "LG ELECTRONICS -", +103, 1, 100, 2,
  0 }` — a phantom string no drive reports, answering +103 to anyone who sent it.
  Now `{ "LG ELECTRONICS", "", +103, ... }`, unreachable, which is what a
  measurement with no product identifier deserves.

  Fixed in TWO places for one reason: the fetcher owns the parse, but the split
  is baked into the committed `ar_offsets.json` AT FETCH TIME, so the fixed
  parser changes nothing until a network re-fetch. `repair_ar_trailing_separator()`
  repairs what is on disk and reports a count that goes to **zero** after a
  re-fetch — the signal that it can be deleted. It is ONE function called from
  both `read_ar()` and `assert_every_name_survives()`, not two implementations:
  the guard's reference must describe the input the pipeline consumed, or it
  reports the correction as BOTH a lost name and an invented one.

  **It restores a discriminating test.** `("LG ELECTRONICS", "")` is now the
  shipped table's only empty-product row, so `test_offsets.c` can assert the
  empty-product guard against a row that actually exists — replacing the
  `("DVDROM", "")` pin that 0.17.0 had to downgrade to "cannot tell the guard
  firing from the key being absent".

- **KEY_ALIAS — one drive, several names the fold cannot reach. NEW 2026-08-25
  (0.18.0), Keith supplied the case.** AccurateRip lists Lenovo's Ultraslim DVD
  four ways:

  | key | offset | subs |
  |---|---|---|
  | `LENOVO ULTRASLIM DVD` | +6 | 44 |
  | `LENOVO ULTRASLIMDVD` | +6 | 21 |
  | `THINK PLUSULTRASLIMDVD` | +6 | 20 |
  | `THINKPAD ULTRASLIM DVD` | +6 | 339 |

  Two badges, the ThinkPlus brand, and a spelling missing its space. `think` +
  `plusUltraslimDVD` is the eight-byte field again. All agree on +6, so the
  offset was never in doubt — what was wrong is that a caller querying the second
  was told the evidence was **21 submissions when 424 stand behind the drive**.
  All four now report 424, and **every spelling is still emitted as its own row**:
  the alias pools at build time and never lets the runtime answer for a name no
  source sent. Same asymmetry the underscore fold obeys.

  **The mechanism is worth more than this case.** Where a family DISAGREES,
  merging lets `read_ar()`'s existing rival-offset resolution pick the
  better-evidenced value, so a one-submission row stops answering on its own
  authority. Measured hazard, live before any fix: `--product 'DVD-RAM GH24NS95'`
  returns **+667 on 1 submission** while `DVDRAM GH24NS95` returns **+6 on
  1315** — one hyphen apart, same vendor, same model. They do not collide, so
  nothing comes back ambiguous; the drive is simply handed 661 samples of
  misalignment, exit 0.

  **A RULE WAS MEASURED AND REJECTED.** Squashing spacing and punctuation
  collapses 146 groups in this corpus: **132 agree on the offset, 14 do not**,
  and some of the 14 are two genuinely different drives (`MAD DOG 56X CDROM`
  +691 beside a bare `56X CD-ROM` +12). A sweep would assert identities the data
  refuses — the `nnXnnX` lesson again. `KEY_ALIAS` is therefore exact whole-key,
  post-fold, one line per human decision, the same contract as `VENDOR_ALIAS`,
  `REBADGE` and `GENERIC_PRODUCTS`.

  Applied LAST in `fold()`, so an alias is written in the form every other key
  already has; written in raw INQUIRY form it would never match, and silently.
  Predicted before regenerating, exact: entries **5881 unchanged**, AR keys
  4775 -> 4772, rows held by REDUMP 5647 -> 5649, BOTH 5633 -> 5635, pooling
  recovery 1580 -> 1665 (= 424 - 339). Falsified both ways: the repair neutered
  (the `stranded` guard fires FIRST, naming the row) and one `KEY_ALIAS` line
  removed (`test_offsets.c:331` fires, the pooled-count assertion).

- **KEY_ALIAS group A+B — CLOSED 2026-08-25 (0.19.0). Four wrong answers fixed,
  and the count was 14 not 13.** I had said "13 others" by subtracting Ultraslim;
  Ultraslim was in the AGREEING set (all four spellings at +6), so nothing was
  subtracted. Naming the miscount because the number was what Keith was asked to
  review.

  **Group A — a one-submission row answering against hundreds.** Same vendor,
  same model, one punctuation mark apart, and nothing collided so nothing
  refused:

  | spelling | was | now |
  |---|---|---|
  | `DVD-RAM GH24NS95` | +667 / 1 | **+6 / 1315** |
  | `DVDRAM GSA-E60L` | +667 / 1 | **+102 / 247** |
  | `DVDRAM- GP65NB60` | +102 / 1 | **+6 / 1151** |
  | `CDDVDW SE -218GN` | +102 / 1 | **+6 / 197** |

  GH24NS95 and GSA-E60L point OPPOSITE ways — hyphenated is the minority in one
  and the majority in the other. That is the concrete reason this is a reviewed
  list rather than a rule.

  **Group B — agreeing spellings, evidence only.** `HP DVDROM DT30N` +103 (9+3 =
  12) and the two `ATAPI CD` spellings of ATAPI CD-ROM +12 (1+1 = 2).
  `16X DVD-` + `ROM` stays its own key, so the product `ROM` alone is still
  ERR_AMBIGUOUS — asserted, as the check that the alias did not over-reach.

  **TWO ARMS HAD TO BE ADDED, and the first was found by predicting it.**

  1. *An aliased REDUMP row must be KEPT by the retraction rule.* The rule asks
     "does AccurateRip still list THIS NAME at THIS value?"; once aliased,
     `live[key]` is the merged DRIVE's value, so a mismatch reads as SUPERSEDED.
     It dropped three rows and with them the ONLY copies of
     `HL-DT-ST DVD-RAM GH24NS95`, `DVDRAM GSA-E60L` and `DVDRAM- GP65NB60` —
     AccurateRip files those under "LG Electronics", so no AR spelling replaced
     them. **`assert_every_name_survives` could not see it**: its reference is
     `kept` plus AR spellings, and a dropped row is in neither, so the
     requirement shrank exactly as far as the table did. The vacuous-guard shape,
     third occurrence. Now guarded directly: no dropped row may have an aliased
     key, which fires the moment the arm is removed (falsified — it names all
     three).
  2. *An aliased REDUMP row contributes its SPELLING but no VALUE.* REDUMP's
     table IS AccurateRip's 2022 import, so its +667 for that name is the same
     datum `read_ar()` already discarded for +6/1315. Letting it claim an offset
     re-entered the discarded row by the other door and `merge()` read one datum
     as two sources disagreeing: `conflicting_keys` 0 -> 3, three rows shipping
     `ACCUDISC_OFFSET_F_ADJUDICATED` — telling a caller the sources disagreed
     about a disagreement the alias itself created. Both back to 0.

  Final: **5881 rows either way, zero names lost or gained**, `redump_superseded`
  back to 9, `conflicting_keys` 0, AR keys 4772 -> 4765, rival-offset keys
  9 -> 13. Falsified in both directions. **A build error was nearly missed** —
  `grep -c error` returned 1 while the stale test binary still reported PASS; the
  compile had failed on a wrong constant name. Check the build, not the binary.

  **The remaining TEN families are deliberately untouched**, reviewed and
  recorded rather than left unexamined:
  - *Do not merge:* `56X CDROM` (different vendors, plausibly two drives);
    `ATAPI CDROM 48X`/`52X` — the dot form is +12 and the space form +691 across
    **two independent model numbers**, so the punctuation is a working
    discrimination, not a typo; `DVD RW` (category words, two already in
    `GENERIC_PRODUCTS`); `DVDROM DH60N` (crosses vendors, no confident read).
  - *No clear winner — CLOSED as a decision, not an open question (Keith,
    2026-08-25): leave them as uniques.* `CDRW 52X32` (+738/88 vs +6/2),
    `CD-RW 52XMAX` (+688/31 vs +6/10 — not lopsided enough to read as a
    mis-submission), `LG ELECTRONICSDVD-RAM` (+102/14 vs +6/1). Each spelling
    keeps its own row and its own value. "Chasing the nth degree is subject to
    the law of diminishing returns" — the process is as refined as it needs to
    be, and a merge nobody can defend on the evidence is worse than two rows.

- **DOES REDUMP STILL EARN ITS PLACE? YES — for NAMES, not values. Measured
  2026-08-25.** Zero rows carry an offset AccurateRip does not also hold, which
  is exactly what "REDUMP IS AccurateRip frozen in 2022" predicts. What it holds
  alone is **1079 spellings**, and they are the load-bearing kind:

  | REDUMP spelling | rows | AccurateRip publishes |
  |---|---|---|
  | `HL-DT-ST` | 649 | `LG Electronics` (699 rows, **0** as HL-DT-ST) |
  | `MATSHITA` | 375 | `Panasonic` (390 rows, **0** as MATSHITA) |
  | `FREECOM` | 30 | `Freecom_` |
  | `JLMS` | 9 | `Lite-On` (111 rows, **0** as JLMS) |
  | glued-name rewrites | 13 | `PanasonicBD-CMB U` -> `MATSHITABD-CMB U` |
  | rescued rebadge | 1 | `PHILIPS DVD-ROM PCDV632` |

  **AccurateRip publishes MARKETING vendor names; REDUMP publishes what the drive
  reports over INQUIRY.** The runtime matches literally, so those 1064
  alias-vendor rows are the only ones real hardware can hit — and the inversion
  is total: AccurateRip has **zero** rows saying `HL-DT-ST`, the commonest
  optical-drive vendor string there is. Drop REDUMP and every Hitachi-LG and
  every Panasonic drive answers ERR_NOTFOUND.

  The 13 "REDUMP-only values" are not measurements either: they are the same
  drives under names a whole-field alias cannot reach, where AccurateRip prints
  the marketing vendor GLUED into the product (`PanasonicBD-CMB U` +103 is our
  `MATSHITABD-CMB U` +103 — same offset). REDUMP substituted the INQUIRY vendor
  inside the glued string, which `VENDOR_ALIAS` structurally cannot do.

  Synthesising them from AccurateRip is NOT an option: emitting `HL-DT-ST` from
  an `LG Electronics` row would be inventing a name no source reported, which
  `assert_every_name_survives` forbids in its second direction, and for good
  reason. REDUMP stays.

  **The survey's scope, stated so it is not mistaken for the class.** It finds
  PRODUCT strings differing only by spacing or punctuation. It cannot see a
  family whose VENDOR also differs — Ultraslim, which Keith found and the survey
  did not — nor one whose product WORDS differ: `HL-DT-ST` GHA2N is listed as
  `DVD+-RW GHA2N` (169), `DVD-RAM GHA2N` (69) and `DVDRAM GHA2N` (71), one drive
  with 309 submissions split three ways, all agreeing at +667. Invisible here.

- **`ACCUDISC_OFFSET_MAX_VALUES` IS FROZEN BY THE ABI — and the note this
  replaces was wrong about the remedy.** It said to derive the `values[]` write
  bound from `out->size` before growing the macro. That is not enough:
  `value_sources[]` FOLLOWS `values[]`, so growing the array MOVES it. Measured:
  at 4, `sizeof` is 36 with `value_sources` at offset 32; at 8, `sizeof` is 56
  with it at 48. An old caller's struct is therefore not a PREFIX of the new one
  and `size` cannot describe it — the caller would read its `value_sources` from
  bytes the library used for `values`. Bounding a write does not fix a field
  that has moved. Reporting more values needs a field APPENDED after
  `value_sources[]` (which `size` does handle) or a new call.
  `tests/test_offsets_ambiguous.c` carries the tripwire.

- Not touched, and pre-existing rather than introduced here: the SCALAR fields
  of `accudisc_offset_info` are written without consulting `out->size`, so a
  caller passing a size smaller than the struct would have `read_offset`,
  `n_values` and the rest written past its buffer. The ABI check accepts any
  `0 < size <= sizeof`. Unreachable today for the same reason as above — one
  layout has ever shipped — and orthogonal to the keying change, so it was left
  alone rather than folded in. `values[]`/`value_sources[]` ARE now guarded.

## OFFSET DICTIONARY — the design Keith settled (2026-08-19)

**Supersedes the 2026-08-16 section below on two points.** The `nnXnnX` question
is closed — the rules *are* overzealous, but for a reason nobody had yet stated —
and "drop the bad entries" has been replaced by "consolidate and warn". Read this
first; the older section is kept for the evidence in it.

### The design, in Keith's words

> *"For actual drive detection, we will use only the unique product identifier
> (which is why it can't be missing or generic), and only verify the vendor if
> present and not conflicting (unless it's a recognised alias)."*

and, the same evening, the correction that changes the goal:

> *"I really only need to drop entries that are genuinely unusable, because there
> are multiple identical entries with different offsets. But we already agreed
> that, in those cases, we simply print a warning and the list of possible values.
> So maybe I don't need to drop any entries at all, I just need to consolidate
> genuine duplication. […] The tool should be genuinely useful, not ignore data
> without good justification."*

### Sequence of work he set

1. Accumulate vendor names, verified by dumping the vendor-less list and reading
   it (`name_reduce.py --no-vendor`, added 2026-08-19).
2. Then identify entries with no product name, or only a generic one.
3. **no vendor AND no product** is the only candidate set for dropping.
4. Vendor-less rows that *do* carry a real product: research the vendor by hand
   and attach it **for information only** — never for matching, since no drive
   reports it.
5. Detection keys on the **product identifier alone**; the vendor only narrows,
   and only when present, non-conflicting, or a recognised alias.

### THE LOOKUP IS ALREADY BUILT — check before designing it again

`accudisc_offset_info` (public header, shipped 0.10.0) already carries everything
"consolidate and warn" needs:

- `ERR_AMBIGUOUS` (-14) + `n_values` + `values[]` + `value_sources[]` — the
  warn-and-list path, already the behaviour on a disagreeing key.
- `ar_submissions` — the confidence signal, so a caller can see a row rests on
  one measurement without the table having to judge it.
- `ar_agree_pct` — AccurateRip's WITHIN-source agreement, valid even though the
  two corpora are not independent of each other.
- `sources` — carries the non-independence warning in its own comment.

So point 5 changes the **key** (`src/drive/offsets.c`, `tools/gen_offsets.py`),
not the API surface. Shape:

    product key -> candidate set -> narrow by vendor (aliases apply)
                -> 1 left: OK   |   >1 left: ERR_AMBIGUOUS with values[]

### Measured 2026-08-19 — do not re-derive

**Product-only keying is safe.** Redump's structured product column, the only
corpus half with a real vendor/product split:

    distinct RAW product strings : 4349
    ambiguous on offset          :   20   (0.46%)
       vendor differs (resolvable):  14
       vendor same  (unresolvable):   6

**But the key must be the RAW product, never the reduced string.** The reduction
strips tokens that are occasionally the only discriminator:

    AOpen 12X DVD-ROM-AMH  -> reduced "DVD-ROM-AMH"  +691
    AOpen 16XDVD-ROM-AMH   -> reduced "DVD-ROM-AMH"  +102

Two different readers, 589 samples apart, merged into one key by the dictionary.
Keith: *"16XDVD-ROM-AMH is most likely the actual product identifier, despite its
partially generic appearance. So my nnXnnX rule is overzealous. Some products just
have very literal and unimaginative identifiers."* `tests/test_offsets.c` already
asserts both values — that test is the regression guard, keep it.

This closes the `nnXnnX` question from 2026-08-16. **Reduction is for AUDITING and
for deciding whether a product string is generic; it is never the detection key.**
The `\d{1,3}X` -> `\d{1,2}X` narrowing measured on 2026-08-16 stays unapplied and
is now low-priority rather than wrong.

**The 6 that the vendor cannot resolve** — pre-existing conflicts, not
consequences of the new rule; today's vendor+product key does not fix them either:

    PIONEER  BD-RW BDR-206      +0  / +667
    PIONEER  DVD-RW DVR-221L    +6  / +667
    SONY     CD-RW CRX175E      +0  / +120
    SLIMTYPE DVD A DS8A1H       +0  / +594
    PLEXTOR  DVDR PX-740A       +6  / +618
    ATAPI    DVD A DH16AFSH     -12 / +6    (AR holds +6, 9 submissions)

`+667` was checked for being a systematic artefact and is NOT: it is the third
most common offset in the corpus (926 rows, behind +6 and +102) — the
LG/HL-DT-ST family value. A pattern found by looking only at conflicts describes
conflicts, not the corpus.

**`ATAPI CD-ROM`, the entry that started the vendor discussion:**

    Redump  ATAPI | CD-ROM   +680
    AR      ATAPI CD-ROM     +680   1 submission

One entry, one offset, held by both tables — but the tables are not independent,
so that is one witness, and `ar_submissions = 1` already says so to any caller.
Under today's vendor+product key it is unique and usable. Under point 5's
product-only key the product `CD-ROM` alone is held by FIVE Redump rows carrying
FOUR offsets:

    52XATAPI CD-ROM  +108      BCD 32XH CD-ROM   +12
    ATAPI    CD-ROM  +680      BCD 44XH CD-ROM   +12
                               BCD24XHM CD-ROM  -1164

so it becomes ERR_AMBIGUOUS and the vendor narrows it back to one. That is
Keith's rule working as designed — and it means **generic product strings
self-identify by colliding**, so they may need no filter at all.

**The no-vendor population** (`--no-vendor`): 1591 of 9504 rows, 811 distinct
names, 676 of which still reduce to a model string. Dominated by drives whose
FIRMWARE reports no vendor, not by junk — the most-submitted vendor-less rows are
Lite-On's iHAS/iHBS family reporting vendor `ATAPI`:

    1556  +6    ATAPI iHAS124 F        1023  +6  Dell DVD+--RW DW316
    1182  +696  ATAPI iHOS104           739  +6  Slimtype DVD A DS8A5SH
    1172  +6    ATAPI iHAS124 B         418  +6  TS8XDVDS TRANSCEND

An empty vendor field measures whether the firmware author filled it in, never
the quality of the drive. `SLIMTYPE` alone is 232 rows — 14.6% of the vendor-less
set from one missing keep rule.

**Submission counts separate the populations but cannot gate:**

    subs      all AR   no-vendor
    1          12.6%      25.7%
    100+       21.4%      10.5%

Twice as likely to rest on a single submission, half as likely to have 100+ — but
86 vendor-less rows have 100+. Use it as an ORDERING for the audit, so real
missing vendors float to the top; never as a filter.

### Corruption forensics — three mechanisms, told apart by arithmetic

XOR the suspect against the real name; the popcount says which:

    'PLEXTOR'   vs 'PLEXTOB'    1 diff,  XOR 0x10        single bit flip
    'HL-DT-ST'  vs 'HL)DP-ST'   2 diffs, XOR 0x04 twice  same bit, twice
    'Slimtype'  vs 'SIimtype'   1 diff,  XOR 0x25        3 bits - not electrical
    'Nakamichi' vs 'Nakamich'   0 diffs, 9 -> 8 chars    T10 field width

Single-bit differences are bus or media corruption during a real probe, so those
rows are corrupt COPIES of drives already in the table — deletions, not new
vendors. A 3-bit XOR at an `l`/`I` position is a human reading a rendered name
(`SIimtype eBAU108 7 L`, 1 submission, sitting beside `Slimtype eBAU108 7 L` at
120). Truncation is not an error at all: measured on Redump's columns, vendor
max = 8 (1561 rows at exactly 8) and product max = 16 (919 rows at exactly 16) —
the T10 INQUIRY field widths. `Nakamich` is `Nakamichi` truncated, and Nakamichi
genuinely made PC drives (MJ-4.8 / MJ-5.16 five-disc ATAPI and SCSI changers), so
the vendor is real; "Dragon 05" as a model is still unverified.

Also present and **not hardware**: five virtual-device rows — `Oh!Soft
VirtualDVD-ROM +667`, `DiscSoft VirtualWritable +6` (DAEMON Tools), `NECVMWar
VMware SATA CD02` and `CD05` (+6). An emulator returns image bytes and has no
read offset at all; +667 on one of them is a measurement artefact that would shift
real output by 2668 bytes. A category error rather than a name-quality judgement,
and the cleanest deletion rule available.

### `values[]` holds 4 and product-only keying already overflows it — measured

`ACCUDISC_OFFSET_MAX_VALUES` is **4** (public header). `accudisc_offset_for_inquiry`
appends one value per MATCHING ROW and **does not dedup identical values**
(`src/drive/offsets.c:102-131`); it clamps `n_values` to the array while `n` keeps
counting, so an over-wide conflict reports `conflict 4`, lists four values, and
says nothing about the ones dropped. Silent truncation of the very list
`cli/main.c:217` tells the user to choose from.

Today that is unreachable: of 5888 table rows there are 5887 distinct
(vendor, product) keys and exactly **one** key held twice — the single
ERR_AMBIGUOUS drive. The generator already merges rows carrying the same value
(0 identical (vendor, product, offset) triples in the built table).

Under point 5's product-only key, measured on the built table:

    5 rows  4 distinct  "CD-ROM"          <- OVERFLOWS values[4]
    4 rows  3 distinct  "DVD-ROM"
    4 rows  2 distinct  "DVD RW"
    4 rows  1 distinct  "DVDRAM GT20L"    <- 4 rows, ONE value: pure duplication
    3 rows  2 distinct  "GCE-8483B"

Exactly one product overflows, and three more sit at the limit. Note the last
shape: 4 rows agreeing on one offset would today be reported as a 4-way CONFLICT,
because the lookup counts rows rather than distinct values. **Deduping identical
values in the lookup fixes the overflow and the false conflict at once**, and is
Keith's "consolidate genuine duplication" expressed in code rather than in the
table — `DVDRAM GT20L` is four vendor spellings of one drive, not a disagreement.

After deduping, the widest real conflict is `CD-ROM` at 4 distinct values, which
fits exactly. That is zero headroom, and AccurateRip's rows carry no vendor/product
split, so whatever derives their product identifier can only add. Decide: dedup in
the lookup (needed either way), and then either grow the array (a layout change —
minor bump, the `size` field handles it) or add a "list truncated" flag. Growing
it silently is the wrong fix alone: a caller must be able to tell a complete list
from a clipped one.

### Two API decisions implied by point 5, neither settled

1. **What comes back when the vendor conflicts.** Under "reports, never applies"
   the offset should still return, flagged as not corroborated. A silent
   `ERR_NOTFOUND` is indistinguishable from an unknown drive, so the caller
   cannot tell "no data" from "data we distrust". Likely a new
   `ACCUDISC_OFFSET_F_*` bit.
2. **Researched vendors must not share a field with reported ones.** The point-4
   vendors are, by construction, strings no drive will ever report. Put them in
   the column the matcher reads and a future change starts matching on them,
   sending every affected drive to NOTFOUND — well-formed, silent, and invisible
   to tests. Separate field, or a per-row flag.

### Tooling added 2026-08-19

`name_reduce.py --no-vendor` — rows no vendor `keep` rule claims, grouped by
LEADING token. The grouping is structural, not a guess: `offset_dump_all.py`
builds column 4 as `f"{vendor} {product}"`, so token 0 is the vendor position.
`--limit 0 --examples 0` lifts both caps. Reduction output and `--stats` totals
verified byte-identical after the change.

Two traps closed while adding it, both worth keeping:

- **The vendor set is DECLARED, not inferred.** Two of the 55 keep rules guard
  structure (`[A-Za-z0-9]`, `V\d+`), so "a keep rule fired" is not "a vendor was
  recognised" — it over-counts by 567 rows, with a well-formed number nothing
  downstream could catch. Marked `struct:` in the note column; `vendor_keeps()`
  excludes them. Inferring from `keep` vs `keepre` works today and rots the first
  time a brand needs a regex spelling.
- **`check_note()`** rejects a note carrying both `cs:` and `struct:` (only the
  leading prefix is honoured, so the other would be silently ignored) and
  `struct:` on a non-keep rule. All three rejection paths were made to fail before
  being trusted.

## OFFSET DICTIONARY — Keith's redirection: the test is a VENDOR, not a speed string (2026-08-16) — PARTLY SUPERSEDED, see above

**Superseded in part by the 2026-08-19 section above** — the `nnXnnX`
proposal below is settled there, and "drop the bogus entries" has become
"consolidate and warn". The evidence in this section still stands.

The exclusion dictionary
(`tools/drive_name_terms.tsv`, 159 rules) transferred from cdda2img today and is
verified working in this tree. Before it drives anything, Keith redirected the
approach:

> *"I'm starting to lean towards the idea that maybe we should drop the nnXnnX
> type rule. The real test for whether an entry is bogus, or at least unusable,
> is if there's no recognisable vendor name, e.g. the ones that merely identify
> as 'ATAPI CD-ROM', etc."*

### The change of principle

The dictionary as inherited asks *"what tokens are not identifiers?"*, strips
them, and blanks a row if nothing survives. Keith's test asks a different and
simpler question: **does this row name a manufacturer we recognise?** If not, the
entry cannot bear an authoritative offset whatever else is in the string, because
many unrelated drives report `ATAPI CD-ROM` and they do not share an offset.

Consequences to work through, none of them decided:

- The ~92 `token` drops and the speed `regex` rules may become largely
  unnecessary — if the gate is vendor recognition, stripping `52X32X52` earns
  nothing. **Dropping the `nnXnnX` rules is the specific proposal.**
- The 53 `keep` rules (vendor names) stop being a guard and become **the whole
  mechanism**. That list then becomes the highest-value artefact in the tool and
  needs reviewing as such, together with the vendor ALIAS pairs it interacts with
  (`tools/gen_offsets.py:86-90`).
- `blank-if-all-kept` inverts its justification: today a row of nothing-but-vendor
  blanks because a bare vendor is not a drive. Under the new test that is still
  true, so `ASUS` / `PIONEER` / `Lenovo` should still go — the rule survives but
  for a different reason, and that should be stated rather than inherited.
- Whether the reduced string still exists at all. If the gate is "recognisable
  vendor, yes/no", the output may be a **filter decision** rather than a reduced
  name — which would retire the raw-first/reduced-fallback machinery and the
  "reduced string is never a key" hazard along with it.

### Evidence from tonight, so it is not re-derived

- **The list is 356 rows / 187 distinct names.** A reviewable dump with offsets,
  submission counts and per-corpus provenance was written to
  `/var/tmp/accudisc-offsets-handover/blanked_review.txt` — regenerate rather
  than trust it, `/var/tmp` is not backed up. `name_reduce.py --blanked` is the
  flag; the join back to offsets is by ROW POSITION (output is 1:1 row-aligned
  with the input TSV), not by string match.
- **One proven false positive under the current rules:** `HP DVD Writer 840x`,
  +102, 25 AR submissions. `drive_name_terms.tsv:122` is `regex \d{1,3}X`, and
  the three-digit bound eats the model number. Minimal pair, measured:

  ```
    HP DVD Writer 840x   -> (blank)
    HP DVD Writer 840    -> HP 840
    HP DVD Writer 1260x  -> HP 1260x     (4 digits, escapes the rule)
  ```

  Narrowing to `\d{1,2}X` was tested and is surgical: 356 -> 354 blanked rows,
  **exactly one** name changes, the rule keeps 151 of its 153 hits, and nothing
  else moves across 9,504 rows. **NOT APPLIED** — it may be moot if the rule is
  dropped entirely, which is the proposal above. Note the rule's own examples
  (`52X, 16X, 40X, 48X, 56X`) are all two-digit, and no optical drive speed has
  ever been three-digit (CD 56x, DVD 24x, BD 16x).
- The `split (\d{1,3}X)([A-Z][A-Z0-9-]+)` rule carries the identical bound. Inert
  on this corpus, same latent bug.
- **165 of the 187 blanked names are held by BOTH corpora and all 165 agree on
  the offset.** This is NOT evidence the entries are sound — it is the
  non-independence finding again (documented on `accudisc_offset_info.sources` in
  the public header). Two tables agreeing about `ATAPI CD-ROM` is one guess
  counted twice, and it argues *for* dropping them.
- Four judgement calls left with Keith, unresolved: `HL-DT-ST` /
  `LG Electronics LENOVO BURNER` (+102, 11 subs — a product name, not a model
  number); `Memorex CD-ROM 52X v2` (a revision marker with no model to qualify);
  truncated trailing letters (`Optiarc DVD RW A`, `PIONEER DVD-RW D`,
  `Slimtype DVD A`), which are the same shape the `keepre` guard exists to
  protect on `DRW-24B1ST`; and bare vendors, which should go.

### Also outstanding on offsets

- **Integration shape, still undecided:** (a) build-time filter dropping
  identifier-less rows, (b) runtime raw-first with reduced-form fallback, or
  both. Keith's redirection may retire (b) entirely — settle the principle first.
- **Point 3 of Keith's eight — write-offset measurement — DONE 2026-08-25
  (0.20.0).** The last of the eight. `accudisc_write_offset_signal()` and
  `accudisc_write_offset_locate()` + `accudisc_write_offset_info`; CLI
  `write-offset --signal`/`--measure`; `src/write/write_offset.c`.

  **The library supplies the signal and the arithmetic, NOT the procedure.** The
  burn is `accudisc_write` and the read-back is the ordinary read path, so no
  library call destroys a disc and the CLI verb touches no device. Same rationale
  as `accudisc_ctdb_repair`: what crosses is the part every consumer would
  otherwise reimplement and get subtly wrong.

  **THE READ OFFSET IS A REQUIRED INPUT.** The read-back carries both offsets
  summed. A defaulted 0 returns a confident number wrong by exactly the drive's
  read offset, and 0 is legitimate for hundreds of drives, so nothing downstream
  could tell — the CLI refuses with exit 1 rather than assuming.

  **THE CDEmu TEST CANNOT VALIDATE THE ARITHMETIC, and that is why the unit test
  exists.** On CDEmu `write_offset == 0` and `read_offset == 0`, so the
  correction term drops out and a sign error in it is invisible; a green run
  proves the plumbing and nothing else. `tests/test_write_offset.c` builds the
  discriminating case with no drive — pulses at `expected + R + W`, `read_offset
  = R`, result must be exactly `W` — with R and W of OPPOSITE SIGN and
  |R| != |W| so no compensating error survives (adding R where it should be
  subtracted gives W + 2R). Falsified: negating the term and dropping it both
  fire, at the discriminator rather than somewhere downstream.

  **A vacuous assertion was caught in the same file.** The isolated-click check
  originally ran against a SILENT buffer and asserted `ERR_NOTFOUND` — which
  comes back whether the click is rejected or not, because pulse B is missing
  either way. Measured: with the run requirement weakened to 1 the test still
  passed. Now run against an intact signal, so rejecting the click is the only
  thing that can produce the expected answer. Shadowed guard, fourth occurrence.

  Hardware-adjacent verification, CDEmu `/dev/sr1`: generate -> burn (5625
  sectors) -> `read --pcm` -> measure = **+0**, and the round-trip is
  byte-identical (`cmp`, not the return code). The read-offset term was then
  shown to move the answer on that real read-back: `--read-offset 30` -> `-30`,
  `--read-offset -102` -> `+102`. A real disc is still owed.

  CDEmu is itself a good demonstration of the ambiguity machinery: it reports
  product `CD-ROM`, the four-offset key, so `accudisc offset` refuses to answer
  and `--read-offset 0` has to be supplied from knowledge (a virtual drive does
  no seeking) rather than from the table.

  Reference was cdda2img's `src/cdda2img/write_offset.py`. Only the geometry,
  the signal and the locator crossed; its XDG paths, TOML store, cycle summaries
  and interactive loops are caller policy and stayed there.

  **Not carried through to the Python binding** — `DriveOffset` has no
  write-offset counterpart yet. cdda2img drives this through the CLI today, so
  nothing is blocked, but the project's habit is every surface in step.

- **A PHANTOM NAME in the public header — fixed 2026-08-25 (0.20.0).** Both
  `accudisc.h` and `tools/gen_offsets.py` cited `accudisc_measure_write_offset`
  as though it shipped. It never existed: a name for work not yet done that
  leaked into the installed contract, found while surveying open work. Both now
  name the two functions that do exist.
- **Uncommitted in the tree:** the case-folding change (Keith's ruling —
  upper-case the table, upper-case the query) across `accudisc.h`,
  `src/drive/offsets.c`, `offsets_db.inc`, `tests/test_offsets.c`,
  `tools/gen_offsets.py`; 42/42 green; verified lossless (0 of 5888 rows differ
  only by case). Deserves **0.11.0** — a lowercase INQUIRY string that returned
  `ERR_NOTFOUND` now returns `OK`, the same "meaning changed, layout did not"
  case that justified 0.10.0. Plus four untracked handover tools. `24ac59e` is
  still unpushed underneath all of it.
- **Device path not hardware-verified since the case change.** Run
  `accudisc --device /dev/sr0 offset` against the real PX-716A under
  `flock /var/tmp/sr0.lock`.

## READ SPEED — ONE SESSION, THEN PERMANENTLY ABANDONED (Keith, 2026-08-08)

> *"We will set aside a session, not today, where the only topic we discuss is
> read speed. At the end of that session, anything not completed will be
> permanently abandoned. So that means no hours-long tests."*
> — and, on the general subject: *"We need to move past this, quickly."*

**The rule is a guillotine, so what goes into that session matters.** The
discriminator is **does this need the drive?** — not whether the word "speed"
appears in the title. Filing desk work into the session kills fixes that never
needed hardware; filing hardware work outside it re-opens the open-ended
investigation the ruling closed.

**DRIVE-BOUND — this is the session, and items 3–5 cannot be advanced on the
hardware we own, so expect them to be abandoned by default:**

1. ~~`--speed 16` silently honoured as 8×, unreported.~~ **DONE 2026-08-09 —
   and it was never drive-bound.** The ladder detection had existed since
   2026-07-28 (`ACCUDISC_RUNG_QUANTIZED`); what was missing was a notice on
   `speed X` / `read --speed X`, which needs a MODE SENSE, not a timed read.
   Filed here as drive-bound on the strength of this entry's own stale
   paragraph — **the second time in two days that a wrong TODO nearly cost a
   fix its session.**
2. The read-engine throughput cap (~5× vs 12–19× raw) — "Speed control" below.
   The one most likely to be *felt*: every whole-disc read goes through it.
   **This is the ONLY remaining drive-bound item this rig can advance.**
3. ~~Task 5's A-vs-B uncap discriminator~~ — **DISSOLVED 2026-08-09.** Dropped
   first (*"Is this another SpeedRead test? Because if it is, please remove
   it."*), then the whole inference behind it was removed in 0.8.0 on Keith's
   follow-up ruling, which took the question with it. Nothing to document and
   nothing to settle.
4. Phase 3 ranged SET STREAMING — needs **other drives**; this one cannot
   advance it.
5. The `device.c` latch bug — needs a drive that genuinely lacks 0xB6, i.e. not
   this one. Falsifiable nowhere on this rig.

**So the session is now ONE item.** 3 is dropped; 4 and 5 need hardware that
does not exist here and will lapse by the abandonment rule. Unless something
new appears, "the read-speed session" means item 2 and the desk list below.

**DESK WORK — no drive, NOT session-bound, and must not be abandoned with it:**

- ~~Task 1, remove the SpeedRead guards.~~ **DONE 2026-08-09, version 0.6.0.**
  Keith ruled option (b): removed outright, not kept as a reserved byte. Being
  desk work is what let it land the same day the ruling came, rather than
  waiting on a session — which is the whole point of this split.
- Task 5c, the self-contradicting `--driver auto` advice. Pure logic.
- Task 5d, document the `speed` / `speed-uncap` availability split.
- ~~Task 5b, the `LIKELY_OFF` asymmetry.~~ **MOOT 2026-08-09 — and note it took
  TWO checks to become so.** After task 1 I checked and it was still live: the
  classifier survived, so `OFF` could still arrive by inference while being
  documented as authoritative. The 0.8.0 removal deleted the inferring branch
  entirely, so every value the enum can now return really is authoritative or
  `UNKNOWN`. The asymmetry the item described no longer has a source.
- Task 4b's residual: the cross-rung timed-window **length** term is still
  undocumented (the *radius* term was documented 2026-07-28).

**CLOSED — do not re-open, and do not spend session time re-deriving:** the
`speeds` min/max sweep (hardware-validated 2026-07-28), the
`probe_speed_ladder` binding (`ea11d56`), task 4a's phantom 48× rung (an answer
to hand out, not work), 4b's radius half, and — as of 2026-08-09 — the
SpeedRead guard removal (0.6.0), the quantized-speed CLI notice, and the
`speed_requested_x`/`speed_honoured_x` API signal (0.7.0).

**One thing the API signal deliberately does NOT cover, recorded so it is not
mistaken for done:** those two fields are the **pass** speed. `speed_ladder`
moves the speed per rung mid-read and no per-rung record of what was honoured
exists. cdda2img raised exactly this (§163.3) and was told so plainly rather
than being left to infer it from a field that looks wider than it is. The
pre-flight answer is `probe_speed_ladder`'s `ACCUDISC_RUNG_QUANTIZED`. If a
per-rung *record* is ever wanted it needs per-rung storage and is a new item —
**not** a widening of these fields.

## NEXT SESSION — PLAN (agreed 2026-07-16). Execute Phase 0 first.

Phases 0/1/3 are a chain (0 gates the rest); Phase 2 is independent and can be
done any time. All of it was designed against measurements taken 2026-07-16 —
see "disc-init governor — SOLVED" below for the evidence.

### GOVERNING PRINCIPLE — probe, don't bake
Everything we learned about the PX-716A is **runtime-derivable from generic
MMC**. Nothing may be hardcoded, or we ship "PlexTools for Linux" instead of
AccuDisc:

| fact | scope | handling |
| --- | --- | --- |
| performance curve | generic MMC, **but CDEmu rejects it** | probe; failure => report *unknown*, never infer |
| CLV/CAV/P-CAV/Z-CLV | derived from curve SHAPE | generic derivation from drive-supplied data |
| curve endpoints (17-40x) | per-drive, per-medium | comes from the drive |
| speed ladder {4,8,24,32} | per-drive | probe by set->readback; never a table |
| governor ceiling | Plextor-documented; unknown elsewhere | read current_x/max_x; **report the fact, do NOT interpret** |
| SpeedRead (0xE9) | Plextor vendor | already isolated in drivers/plextor |

The PlexTools risk is **semantics, not opcodes**: shipping "current_x < max_x =>
this disc is damaged" bakes a Plextor firmware behaviour into a general tool.
CLAUDE.md forbids it ("no analysis; AccuDisc only moves bits") — report the raw
values, let the calling app decide.
**Test both drives every phase:** /dev/sr0 = PX-716A, /dev/sr1 = **CDEmu**
(virtual; advertises Real-Time Streaming then rejects GET PERFORMANCE — free
negative control for anything that trusts a feature bit over a smoke test).

### PHASE 0 — fix SET STREAMING so the drive actually accepts it — DONE 2026-07-17
Two bugs fixed, hardware-verified on the PX-716A.

**Durable lessons:**
1. **SET STREAMING's Parameter List Length lives at CDB bytes 9-10, not 8-9** —
   it is the one MMC command that shifts its length field off the normal Group-5
   slot (schily: `/* Sz not G5 alike */`, cdrecord/scsi_mmc.c:991). At 8-9 the
   drive reads 0x1C00 = 7168 expected bytes, gets 28, and answers **4/1b
   SYNCHRONOUS DATA TRANSFER ERROR** — which reads exactly like "this drive does
   not implement 0xB6". Portable correctness fix, not a Plextor quirk.
2. **Descriptor flag bits are RA=0x01 / Exact=0x02 / RDD=0x04**; a normal ceiling
   uses 0x00. This PX-716A **rejects RDD** (5/26/00) — it has no "restore
   defaults", so any restore must set an explicit prior-speed descriptor. That is
   also what the push/pop restore-to-prior SOP wants.
3. 0xBB (SET CD SPEED) stays the portable read-speed lever (libcdio uses it);
   0xB6 adds the LBA-ranged ceiling.

Follow-up still open: **device.c latch bug** — `accudisc_set_speed` latches
streaming off only on ERR_IO or sense key 0x05; HARDWARE ERROR (key 0x04) never
latches. Moot while 0xB6 works, but wrong on drives that genuinely lack it.
**Read-speed session, item 5 — and unfalsifiable here**: the bug only presents
on a drive without working 0xB6, so no test on the PX-716A can reach it. The
fix is three lines and obvious from the sense-key list; what cannot be done is
*confirming* it. Decide in that session whether to take it unverified or drop
it.

### PHASE 1 — speed + rotation — DONE 2026-07-17 (commits 7e4aced, 702b5ac)
Hardware-verified on the PX-716A. Shipped: `accudisc_get_performance` +
`accudisc_classify_rotation` (CLV/CAV/P-CAV/Z-CLV/UNKNOWN, discriminated on
intra-segment slope so a Z-CLV step-up is not read as CAV; PX-716A CD curve is
one rising 17x..40x segment = CAV); the `speed [X] [--exact] [--start L --count
N]` subcommand over `accudisc_set_speed` (0xB6, 0xBB fallback) and
`accudisc_set_speed_range` (0xB6 only); the `features` flag split; and the
empty-tray `features --c2` false negative (now C2_UNVERIFIED, not UNSUPPORTED —
medium-not-present is sense 0x02/0x3a). `speed-report` removed.

Still open (small, deferred from Phase 1):
- **--exact discrimination**: it was accepted at 8x, where the drive runs CLV
  anyway. Test `--exact` at 24-32x to see if it forces CLV where the drive
  prefers CAV, or refuses. Not needed for correctness; a characterization nicety.
- **Push/pop for `read --speed X`**: standalone `speed X` persists (done). The
  read engine's per-read restore-to-prior and the `speeds` probe's stale "never
  auto-restored" header were NOT touched this pass — revisit with the engine.
- **Re-run the Q-vs-speed sweep** now that cap'd 0xB6 actually commands speed
  (last session's numbers were the 0xBB fallback).

### DISC-KIND GUARD — DONE 2026-07-22, fully hardware-verified

`accudisc_probe_disc()` + `accudisc_disc_probe` + the `disc` subcommand; verdict
logic isolated as the pure `adsc_disc_classify()` and unit-tested
(`tests/test_disc.c`, 14/14). Interface settled with cdda2img (§17.2) and frozen
in `cli-machine-interface.md`, which is now the reference for the token line,
the precedence order and the `reason=` slugs.

Scope (confirmed 2026-07-22): no medium (tray open/closed), CDDA including
CD-R/RW CDDA, blank CD-R/RW, unknown media. Nothing finer — CD-ROM layouts, the
Mixed Mode data half and DVD/BD need filesystem support and all land in
`NEITHER` with a slug saying which.

All five paths hardware-verified on the PX-716A (audio CD-R, blank CD-R, DVD-R,
no-medium tray-open, no-medium tray-closed). No open verification items.

**Durable lessons:**
- **The profile gate must precede the track census.** The DVD-R *did* answer
  READ TOC, reporting `data_tracks=1`. Without the gate it would have classified
  from its census, and a medium whose CTRL bits happened to read as audio would
  have reached the CD-DA rip path. Keep the order.
- **AUDIO outranks BLANK** so a written-but-appendable audio CD-R classifies
  rippable, not blank; the reverse would offer to burn over music.
- **`disc_status`/`erasable` emit -1 when not obtainable**, never 0 — 0 means
  "empty" and would read as blank.
- `tray=open|closed|unknown` comes from the ASC 0x3A qualifier and is the only
  branch depending on sense extraction rather than command output.

### PHASE 2 — media identification (independent of 0/1/3)
Two layers; the profile is physical, the logical type is not:
- **Layer 1 — profile:** GET CONFIGURATION current profile (bytes 6-7).
  `adsc_mmc_get_configuration` **already exists** (features.c uses it for
  CD_READ 0x001E). Add a profile->name table (codes are facts => MIT, same
  precedent as the ATIP DB): 0x08 CD-ROM, 0x09 CD-R, 0x0A CD-RW, 0x10 DVD-ROM,
  0x11 DVD-R seq, 0x12 DVD-RAM, 0x13/0x14 DVD-RW, 0x15/0x16 DVD-R DL, 0x1A
  DVD+RW, 0x1B DVD+R, 0x2A/0x2B +DL, 0x40 BD-ROM, 0x41/0x42 BD-R, 0x43 BD-RE,
  0x50-0x52 HD DVD, 0x0000 no disc/unrecognised, 0xFFFF non-conforming.
- **Layer 2 — CD logical type:** from data we already read — track CTRL bit 2
  + full-TOC session disc-type byte (0x00 CD-DA/CD-ROM, 0x10 CD-I, 0x20 XA) +
  READ DISC INFO (0x51, already implemented). All audio => CD-DA; all data =>
  CD-ROM; mixed => Mixed Mode; multi-session w/ data session 2 => CD-Extra.
  **MUST be gated on a CD profile (0x08/09/0A)** — `tools/mediaprobe.c`
  demonstrates the bug it prevents: it calls a DVD-R "CD-ROM (data)" by running
  the CD classifier on a DVD's synthetic single-track TOC.
- **Filesystem = a lookup table, and that is where we stop.** Volume
  Recognition Sequence at fixed sectors 16+: 5-byte magic `CD001` (ISO9660),
  `BEA01`/`NSR02`/`NSR03`/`TEA01` (UDF). Read 2 sectors, memcmp constants =>
  ISO9660 / UDF / UDF-Bridge. **Needs a READ(10)** (2048B data sectors) —
  confirmed absent from the codebase; our read path is READ CD (0xBE, 2352B).
  **DVD-Video vs DVD-Audio vs DVD-ROM is NOT done** — all are profile 0x0010
  and only a root-directory walk (VIDEO_TS/AUDIO_TS) separates them, which is
  filesystem parsing, not a lookup. Report physical type only. Revisit if/when
  enhanced/mixed-mode work needs it (user decision 2026-07-16).
- **SA-CD: OUT OF SCOPE, not deferred** (user decision 2026-07-22; CLAUDE.md
  amended). The DSD layer is DVD-density, read at 650 nm, and encrypted — a
  CD/DVD drive cannot address it at all, so this is not a matter of effort.
  (Known SACD rips came from specific Blu-ray players, never a PC drive.)
  A hybrid SACD's CD layer is a *genuine* Red Book CD: the drive reports CD-DA
  and is **correct** about the layer it can see, and the HD layer is invisible
  to every generic command, so there is no "unrecognised" signal to trust.
  Only a single-layer SACD trips 0x0000/0xFFFF. Reporting CD-DA for a hybrid
  SACD is not a bug — it is the whole of our SACD story.
- **Restructure `media`:** profile primary, ATIP supplement. Today it keys on
  ATIP, which only exists on CD-R/RW — a pressed CD-DA or any DVD gets nothing.
- Optional, no scope breach: READ DISC STRUCTURE (0xAD) fmt 0x00 -> DVD/BD
  Physical Format Info incl. Book Type, layers, disc size.

### PHASE 3 — scope the streaming contract to a damaged span — DEFERRED 2026-07-17
`accudisc_set_speed_range(dev, speed_x, start, end, flags)` was built and the
ranged contract was tested on hardware. **Result: on the PX-716A the range is
applied whole-disc, not locally** (measured with `tools/rangeprobe.c`: a 4x
contract over a mid-disc span slowed reads everywhere; a 3-descriptor payload
honoured only the first descriptor, globally).

**Ranged sub-disc throttling is a REAL, documented MMC-5 feature** (§6.39.1, full
text + field defs in git-ignored `private/code/MMC/SET_STREAMING_findings.md`). We
were simply UNSUCCESSFUL enabling it on the one drive tested. Cause undetermined
(single whole-disc GET PERFORMANCE extent? still-wrong CDB framing? firmware?).
No open-source tool uses ranged reads (redumper/cdrdao/libcdio use whole-disc
SET CD SPEED 0xBB; schily uses 0xB6 with start_lba=0), so there is no precedent
to copy. **Status: UNKNOWN — investigate further later (more drives,
GET-PERFORMANCE-derived extents), NOT "impossible".** (See memory
`dont-conclude-impossible`.)

**Read-speed session, item 4 — and not advanceable on this rig.** The one
experiment that would move it is "try another drive", and we have one drive. So
the session cannot resolve this, and under its abandonment rule it will lapse by
default. That is an acceptable outcome for a deferred feature; what would not be
acceptable is letting the lapse be recorded as *"impossible"* rather than
*"unknown, never retested"* — the distinction this entry exists to protect.

**Interim (agreed 2026-07-17):** the CALLER (cdda2img) owns the "repeat reads
across an LBA range on a speed ladder" loop, invoking AccuDisc per iteration with
a WHOLE-DISC speed. AccuDisc already supports this: `read --start L --count N
--speed X` sets whole-disc speed, reads the span, and does NOT auto-restore
between invocations (a cdda2img hard requirement). Nothing new to build for the
interim; `accudisc_set_speed_range` stays (spec-legitimate, whole-disc-effective
on single-extent drives). Revisit the ranged feature in a future session.

### Carried over — Q recovery (Task 2, was mid-flight)
- **Resolve the lone UNKNOWN boundary (t16)** by model reconstruction: frames
  below the dead L-1 are t15 index-1 counting up in abs-MSF, the frame above is
  t16 index-1. If abs-MSF is continuous across the gap and no rel countdown
  appears, the dead frame was prev-body => upgrade UNKNOWN->NONE. Pure inference
  from surviving CRC-good neighbours; **no re-read needed**. Capture for offline
  work without the disc: `tests/data/abba_t16_unknown_boundary.sub`
  (LBA 281333..281762, decode with `tools/pregap.py`).
- **Unified re-read predicate:** a sector fails if C2-dirty OR Q-CRC-bad, both
  from one READ CD; status map gains a second dimension (audio | Q). Harvests
  the transient Q population (proven: ~half the failures clear on re-read).
- Needs a **more damaged disc** to exercise the lost-anchor-at-boundary regime.

### Known bugs to fix along the way
- ~~Default read range dropped track 1's program-area pregap~~ **FIXED
  2026-07-23** (cdda2img §30 → §31). **Lesson:** ECMA-130 §20 makes a Pause part
  of the track that *follows* it, so extents built from INDEX 01 alone leave
  hidden-track-one audio owned by nobody — the default read started past it,
  shifting every LBA against the stream and computing a wrong disc ID. New
  `accudisc_track.pregap` records it (non-zero only for session 1's first
  track); `toc` emits `pregap <n>`.
- ~~`features` no-disc false negative (C2_UNSUPPORTED/exit 1) -> UNVERIFIED.~~
  **DONE** in the Phase-1 `features` split; this line was a stale duplicate.
- ~~`--speed 16` silently honoured as 8x, unreported. [P1]~~ — **DONE
  2026-08-09, and most of it had been done weeks earlier under a different
  entry.**

  Keith: *"do we not already determine the real speed ladder from throughput
  measurements? Again, I thought this was done weeks ago."* Correct on both
  counts, and this entry was the thing that was wrong.

  **What already existed.** `ACCUDISC_RUNG_QUANTIZED` (`accudisc.h:1213`) and
  its detection at `src/drive/speeds.c:163-166` — and note *why* that clause is
  the good one: it is the **only exact clause** in the whole verdict function.
  No measurement, no cross-rung comparison, no radius term; page 2A came back
  below the request and the drive has told us directly. It was hardware-
  validated in the 2026-07-28 `--sweep` acceptance run, which produced the
  admitted ladder `40,32,24,8,4` with **48 duplicate:40 and 16 quantized:8** —
  i.e. this exact defect, named, in a run that was signed off.

  **What was actually missing**, and it was small: `speed X` printed the asked
  and adopted figures on adjacent lines and left the reader to compare them
  (the data was always there; the *notice* was not), and `read --speed X`
  reported nothing at all, because `engine.c:447-448` applies the speed
  best-effort and discards the outcome. Both closed now — a `quantized` stdout
  line on `speed X`, a stderr notice before the read on `read --speed X`.

  **Neither needed the drive time this entry demanded**, which is the lesson:
  page 2A read-back is a MODE SENSE, not a timed measurement, so the whole
  "thresholds must be validated on the drive" objection applied to a design
  nobody ended up needing. The read-back was already trusted everywhere else in
  the file. Verified on hardware anyway, all four cases: `speed 16` warns and
  names 8x; `speed 8` and `speed 24` are silent (no false positives);
  `read --speed 16` warns and the summary's 58.2 sectors/s independently
  confirms the 8x ceiling; `-q` does **not** suppress it. Drive restored to 40x.

  The `-q` decision is deliberate and follows the `if (!quiet)` audit item
  below: a read running at half the requested rate is a data-integrity notice,
  and quiet means no human is watching — which strengthens the claim on being
  told, not weakens it.

  Both messages are stderr/stdout prose and therefore **not** a stable
  interface; `speeds --sweep`'s `verdict=quantized:<x>` token remains the
  parseable form. Said so in `cli-machine-interface.md` rather than leaving it
  to be assumed.

  **Completed the same day by an API signal — 0.7.0.** Keith: *"it's up to the
  calling app to produce its own notices, but AccuDisc must send some kind of
  machine readable signal via the API."* Correct, and CLI prose was never a
  substitute: every non-CLI consumer would have had to issue its own MODE SENSE
  and reimplement the comparison. `accudisc_read_stats` now carries
  `speed_requested_x` / `speed_honoured_x` (136 → 144 bytes, additive at the end
  of an **OUT** struct, so a shorter caller is refused rather than truncated).
  **The CLI's notice is now derived from those stats** rather than measured
  separately — one source of truth, so the CLI and an API consumer cannot
  disagree about the same read. The duplicate set/read-back the CLI briefly had
  is gone.

  **The trap is the zero, and it is why this needed two fields rather than one
  "honoured" number.** `speed_honoured_x == 0` means *no answer* — nothing
  asked, the set failed, or page 2A did not read back. A bare `honoured <
  requested` reports that as quantized (`0 < 16`); treating zero as "fine"
  reports a genuinely failed set as honoured. Both wrong, opposite directions,
  same zero. The test is `honoured && honoured < requested`, stated in the
  header, implemented in `ReadStats.speed_quantized`, and pinned by a
  device-free test covering all four states plus the adopted-more-than-asked
  case.

  Also: the read-back only happens when the set **returned OK**. Page 2A always
  reports something, and after a failed set that something describes the prior
  state — exporting it would report a quantization that never occurred.

  Verified on hardware through the binding: `asked 16x -> requested=16
  honoured=8 quantized=True`, `asked 8x -> honoured=8 quantized=False`, `asked
  none -> 0/0 quantized=False`. Drive restored to 40x.
- ~~**`cdtext` with no FILE reports itself as an unknown command. [P2]**~~ —
  **DONE 2026-08-28.** Dispatches on the name, validates inside, and emits
  `accudisc: cdtext: FILE argument is required` plus usage, still exit 1. The
  ambiguous usage line is reworded too: FILE is stated as required, and the
  disc-carries-no-CD-Text case (writes nothing, exit 3) is named separately and
  left untouched. The rule is in the code comment so it is not reintroduced.
  Original analysis:
  Keith 2026-07-26: *"If a value is mandatory but not provided, and you continue
  anyway, that's a silent failure."* `cli/main.c:1686` dispatches on
  `!strcmp(command, "cdtext") && nrest > 0`, so a missing argument makes the
  branch fail and control falls through the whole chain to the catch-all at
  `cli/main.c:1723`, printing `accudisc: unknown command 'cdtext'`.
  **The exit code (1, usage/argument) is already correct — the defect is the
  diagnostic.** A known command is reported as unknown, so anyone debugging it
  hunts for a typo or a version/feature mismatch rather than a missing argument;
  the device is also opened before dispatch, so the drive spins up only to emit
  a false message. This is the house failure mode in miniature: well-formed
  output, right exit code, wrong referent, and nothing downstream can catch it
  because "unknown command" is exactly what a script expects from an older
  binary.
  **Fix:** dispatch on the command name alone, validate the argument inside, and
  emit e.g. `accudisc: cdtext: FILE argument is required` plus usage; still
  exit 1. **Name the rule so it is not reintroduced: never fold "missing
  mandatory argument" into "unknown command".**
  Scope: `cdtext` is the only instance of this shape — `fulltoc` uses a ternary
  because its FILE is genuinely optional (bare form runs `cmd_fulltoc_parsed`),
  and every other command dispatches on the name alone. **Do not disturb** the
  separate and correct behaviour where a disc carrying no CD-Text writes no file
  and exits 3 (data absent). While there, disambiguate the usage text at
  `cli/main.c:25` — "(no file if absent)" reads as if it might mean the FILE
  argument; it means the disc's CD-Text.
- ~~**A value-taking option given no value silently consumes the NEXT FLAG as
  its value. [P1]**~~ — **DONE 2026-08-28.** One helper, `opt_val()`, replaces
  all 46 `&& i + 1 < argc` guards across all 8 parser loops; a missing or
  flag-shaped value now names the option and exits 1.

  **The scope in the original note was too narrow — it said `cmd_read`, and it
  was every command.** 46 sites, 8 loops: `offset`, `cxscan`, `speeds`,
  `c2lag`, `speed`, `write-offset`, `write`, `read`, plus `main()`'s global
  flags. Confirmed by running the malformed form against each.

  Three things found while fixing it, none of which the note predicted:
  - **`--driver --help` exited 0.** The complaint printed, then `--help` was
    re-parsed as its own request and succeeded. `main()` now checks the bad-flag
    before honouring a later `--version`/`--help`.
  - **`--track`/`--tracks` share one value**, so it needed two `opt_val` calls
    ORed rather than the mechanical conversion (safe: a non-matching call
    touches neither `i` nor the value).
  - **`cmd_features` takes only bare flags**, so it gets no helper at all —
    there is no value for a next argument to be eaten as.

  A lone `-` is deliberately NOT treated as flag-shaped: it is the conventional
  stdin/stdout filename. Pinned in both directions in `tests/cli_surface.sh`
  (24 checks; reverting the fix fails them), which is the right home because
  the rejection happens before any device is opened.

  **The test had the same bug it was testing.** `grep -qF "$opt requires..."`
  with `$opt` starting `--` was read by grep as an option, so eight checks
  failed against output that matched. Fixed with `grep -qF --`. And the first
  draft used a nonexistent device, which short-circuits in `main()` before the
  per-command parser runs — it would have passed while checking nothing.

  Original analysis: Every option in
  `cmd_read`'s parser (`cli/main.c:1102-1194`) tests
  `!strcmp(a, "--chunk") && i + 1 < argc`, so:
  1. **Trailing** (`read --chunk`): the guard fails, control falls to the final
     `else` at `:1191`, which dumps the whole usage text and exits 1 **with no
     message naming the offending argument**.
  2. **Mid-command** (`read --chunk --map`): `i + 1 < argc` is *true*, so `--map`
     is consumed as the value. `strtol("--map")` returns 0, and **0 is the
     sentinel for "use the default"** (`accudisc.h:1152`), so the command runs
     clean, exits 0, and silently (a) does not apply the chunk the user asked
     for and (b) never renders the map, because `--map` was eaten.
  **This is the house failure mode in its purest form: a sentinel meaning
  "default" makes a parse failure indistinguishable from an unset option.**
  Worst instance is `--progress-fd --map` → fd **0**, i.e. progress tokens
  written to *stdin*.
  Affects `--start --count --session --chunk --retries --c2-retries --verify
  --overlap --speed --progress-fd` and the path-taking options (which at least
  fail visibly later, on open). **`--tracks` is already correct and is the model
  for the fix** — it captures `strtol`'s `end` pointer and rejects trailing
  junk (`cli/main.c:1113-1126`). Fix: a shared helper that requires the next
  argv, rejects one beginning with `-`, and validates the full parse, emitting
  `accudisc: --chunk: expected a value` rather than a bare usage dump. Check
  `cmd_write`, `cmd_speeds`, `cmd_c2lag`, `cmd_cxscan` and `cmd_features` for
  the same shape.
- **The man page says `--chunk`/`--retries` are "off by default"; they are
  always in effect. [P2] — DONE 2026-08-28.** The section now says these tune a
  single-pass capture, names the four that are genuinely off unless set, and
  states that `--chunk`/`--retries` cannot be off because their 0 means *use the
  default*. Original analysis: `docs/man/accudisc.1`, "Accuracy and recovery":
  *"All of these are off by default"* is true for `--c2-retries` (0 = off),
  `--verify` (1 = off), `--overlap` (0 = off) and `--ladder` (unset), but
  **false for `--chunk` and `--retries`**, whose 0 means *use the default*
  (`engine.c:402`, `:414`), not *disable*. There is no state in which chunking
  or per-sector retries are off. Split the group, or reword to "these tune a
  single-pass streaming capture; the last four are off unless set". Keith
  2026-07-26: as written it reads as a contradiction — a default value on a
  flag that is simultaneously off.
- ~~**`cxscan`'s options are undiscoverable. [P3]**~~ — **DONE 2026-08-28.**
  The usage text now lists `[--start LBA] [--speed X]` and the output shape.
  The man page already documented both — and carried a note saying the usage
  text did not, which was itself the stale half; that note is gone. Worth
  recording that the man page also USED `--speed` in an example while the usage
  text denied it existed, which is worse than silence: a reader cannot tell
  whether the example is wrong or the option is undocumented.
  Original analysis: `cmd_cxscan` accepts
  `--start` and `--speed` (`cli/main.c:189-199`), but the usage line
  (`cli/main.c:61`) documents none, and any argument it does not recognise falls
  to `usage(stderr)` + exit 1. So the two supported options exist only in the
  source. One-line usage fix; also cover them in `docs/man/accudisc.1`.
- **The `--progress-fd` summary line emits nine keys; two documents say six.
  [P2]** — `cli/main.c:1537-1549` writes `hard c2 recovered suspect rereads
  slips subq_total subq_ok subq_bad`. Both the usage text (`cli/main.c:~103`)
  and `docs/reference/cli-machine-interface.md:109` list only the first six.
  **Not a contract break** — the keys are additive and the documented rule is
  token-primary parsing, which cdda2img follows — but
  `cli-machine-interface.md` declares this format *stable* and is the authority
  a binding is written against, so the authority is currently behind the code.
  Fix both, and add the three `subq_*` counter meanings to the existing table.
  **HALF DONE 2026-08-28.** `cli-machine-interface.md` was brought current
  during the 0.30.0 work (all twelve keys plus the new `error write` token) and
  the usage text now lists them too. What REMAINS: the doc's per-counter meaning
  list still stops at `slips` — `buffer_peak`/`buffer_stalls` are described in
  the AccuBuffer prose but not in the summary-token table where a binding author
  would look.
- ~~**`ATTRIBUTION.md:25` still calls the DAO write path "upcoming". [P3]**~~ —
  **DONE 2026-08-28.** Now records it as shipped in 0.20.0 and hardware-verified
  (burned DAO, read back bit-exact, CD-Text included).
- Logical type must be gated on a CD profile. [P2]
- `accudisc_eject`/`accudisc_load` header comments describe START STOP UNIT
  (LoEj), but the implementation uses block-layer CDROMEJECT/CDROMCLOSETRAY
  (device.c explains why). One-line comment fix; contract vs implementation.
- ~~`adsc_toc_parse_cue` directive-injectable through a quoted string~~ **FIXED
  2026-07-24.** A newline inside a `CD_TEXT TITLE "…"` value spilled its tail
  onto the next physical line, where a column-0 keyword parsed as a real
  directive — phantom track, forged ISRC, changed lead-out, all returning
  `ACCUDISC_OK`. The line-scan now tracks quote context and rejects an
  unterminated quote at EOL (matching cdrdao's lexer: a string may not span a
  line). Regression in `tests/test_tocparse.c`. **Owed: notify cdda2img** —
  their `escape_toc_string` was our only guard until this landed.
- ~~`toc` reported `leadin_unreadable` on roughly half of all discs~~ **FIXED
  2026-07-24 (field report).** READ TOC's second transfer was sized from the
  returned data-length header; a full TOC is `37 + 11*ntracks` bytes, i.e. **odd
  on every disc with an EVEN track count**, and ATAPI moves 16 bits at a time, so
  the host adapter rejected it before the drive answered. Fix: shared
  `adsc_alloc_even()` (`src/mmc/mmc.h`), applied to READ TOC and to
  `mode_sense10`, which had the identical latent defect. Regression:
  `tests/test_alloc_even.c`.
  **Two lessons worth keeping.** (1) A *transport* fault was being reported as a
  *disc health* verdict — the tool blamed the media. That is the dangerous half:
  the failure was well-formed and pointed at the wrong subject. (2) It went
  unattributed for months because `ACCUDISC_ERR_IO` discarded
  `errno`/`host_status`/`driver_status`; there was a bare DID_ERROR (0x07) and no
  sense to read. Hence `accudisc_last_io()`.
- **[P2] Parser hostile-input sweep** (RECORDING_PLAN.md §11.9 REVIEW QUESTION,
  adopted from cdda2img §40.3): apply *"what does this accept if the producer is
  hostile, or merely wrong?"* to every parser fed from a boundary we don't
  control — drive responses in `mmc/`, `cdda/subq.c`, `meta/cdtext.c`, `toc/`.
  The `adsc_toc_parse_cue` injection above was one instance of a pattern that
  surfaced three times in one session across three codebases (each trusted its
  boundary because the *usual* producer is well-behaved); do this as one sweep,
  not three separate fixes as they bite.
- **[P2] `.toc` FILE start/length: accept the bare-integer forms.** cdrdao's
  grammar (`private/code/cdrdao/trackdb/TocParser.g`, rules `samples` and
  `dataLength`) accepts *either* an MSF *or* a plain integer for the two fields
  of `FILE "x" <start> <length>` — and the units differ per field: a bare start
  is **samples**, a bare length is **bytes** (`dataLength` multiplies only the
  MSF form by the block size). `adsc_toc_parse_cue` requires MSF for both and
  returns `ACCUDISC_ERR_INVAL` otherwise, so a perfectly legal cdrdao .toc is
  refused as `write: invalid argument` with no indication of which line lost.
  Refusing is the *right* failure mode (misreading a sample count as frames
  would silently mislocate every track), so this is a compatibility gap, not a
  correctness bug — but the diagnostic is useless and the input is legal.
  Two parts: accept both forms with the correct per-field units, and report the
  offending line number instead of a bare INVAL. Matters for interop: cdda2img
  generates .toc files and may well emit the integer form.

#### Bug audit 2026-07-23 (full report: `private/bugs/2026-07-23-bug-audit.md`)
Correctness sweep of the whole tree, 7 findings, 0 critical. `rw.c` RS/GF math
and the pregap/extent/guard interaction both audited **clean**. Top three
verified against source before recording. **All seven fixed 2026-07-23**, each
with a regression test; suite clean under ASan+UBSan.

| id | defect | where |
|---|---|---|
| F-001 [P1] | SG_IO `resid` never checked — silent corrupt audio | `transport/sgio.c` |
| F-002 [P2] | `accudisc_rw_feed` desynced the de-interleave when `max < 4` | `cdda/rw.c` |
| F-003 [P2] | DAO cue buffer 32 bytes short of the 400-entry worst case | `write/burn.c` |
| F-004 [P3] | `--cdg` open failure leaked the status map / open files | `cli/main.c` |
| F-005 [P3] | c2lag pass-2 window exceeded `ADSC_MAX_XFER` | `drive/c2lag.c` |
| F-006 [P3] | `accudisc_q_parse` decoded payload before the CRC gate | `cdda/subq.c` |
| F-007 [P4] | `accudisc_lba_to_msf` cast a negative quotient to `uint8_t` | `cdda/subq.c` |

**Durable lessons:**
- **F-001 is the worst class: a short read that *succeeds*.** Any GOOD status was
  treated as a full transfer, so a short one left stale bytes in the chunk tail,
  streamed to `--pcm` marked `MAP_OK`, with the C2/consensus net never running.
  Fixed at the correct seam: the transport *reports* the residual and does not
  judge it (allocation-length commands legitimately transfer short); only
  `adsc_mmc_read_cd`, which alone knows its exact expected length, promotes
  `resid != 0` to `ACCUDISC_ERR_SHORT`. Deferred nicety: re-read only the missing
  tail sectors rather than the whole span.
- **F-002/F-006: a sink or a decoder must never silently drop or half-decode.**
  Both were fixed by rejecting the impossible request up front rather than
  truncating (`max < ACCUDISC_RW_PACKS_PER_SEC` → `ERR_INVAL`; CRC-failed frame
  → `ERR_CRC` before any BCD/ISRC decode, leaving those fields zeroed).

### Meta — a caution for next session
Four confident spec-derived claims were overturned by hardware today: "setcap is
an artifact of the RO open", "page 2A is a placebo", "MMC has no rotation
lever", and "SpeedRead defeats the governor". **Measure first; the drive wins.**

---

## (superseded) NEXT SESSION — real read-speed control, then Q-channel preservation

Agreed 2026-07-15, executed, and superseded by the plan above. The plan itself is
gone; what follows is the durable result of running it. Where a finding is still
open it says so.

### Speed control — DONE + live-verified 2026-07-15

SET STREAMING (0xB6) works: commanded 4x/8x delivered 4.01x/8.01x at outer
windows where CAV would give ~30x — a binding ceiling `CDROM_SELECT_SPEED` never
could. Details in `drivers/plextor/PROTOCOL.md`.

**STILL OPEN — read-engine throughput cap.** The recovery `read` engine sustains
only ~5x on a *clean* disc where raw streaming (`speeds`, bare READ CD) reaches
~12–19x at the same radius: roughly 70 ms/command of per-chunk overhead. Not a
speed-control bug, a ripping-throughput one. Suspects: default `chunk_sectors`,
per-chunk cache-defeat, status-map write cost, a synchronous stall between
commands. Matters because whole-disc Q baselines run through this path.

**Read-speed session, item 2 — and the one most likely to be felt as a rough
edge**, because every whole-disc read goes through it and the gap is a factor
of 2–4 in wall-clock time on a *clean* disc, where no recovery work is being
done to justify it. It is also the only drive-bound speed item that is
plausibly fixable in a short session: the four suspects are separable by
instrumenting the per-chunk loop rather than by long comparative rips, which
the "no hours-long tests" rule forbids anyway. Isolate the 70 ms first — a
timing breakdown of one chunk answers which suspect it is, and costs one read
of a few hundred sectors.

### SET STREAMING is a streaming *contract*, not a speed knob

Setting *any* ceiling — even 40x, above natural — collapses the recovery engine's
throughput to ~5x: the drive tunes for constant-rate delivery, and the engine's
~37 ms/chunk of C2+Q processing reads as "host not consuming", so the drive
throttles. **Rule: first pass free-runs (no `--speed`); slow a damaged span with
SET STREAMING low (4x/8x are exact). Never use it to set a high ceiling for a
bursty read.**

Ceiling on the PX-716A for a Q-preserving read: **~18.7x with subchannel** (the
drive cannot deinterleave subcode faster), 25.9x with C2 but no sub, 21.4x
audio-only.

### The PX-716A disc-init governor — SOLVED (2026-07-16)

The long-standing "stuck at 32x, but 40x after an eject" mystery is **not** a mode
we armed: **the drive profiles the disc at init and pins a quality-appropriate
ceiling.** Repeatable across eject/load cycles — ZZ Top (pristine) inits at 40x
(= page 2A max), ABBA (scratched) at 32x. The disc is the only variable.

- **Page 2A was never a placebo.** `current_x` is real, readable state: it tracks
  SET STREAMING exactly and reports the governor's init ceiling before we set
  anything. Earlier "always 40x" readings were because no set had ever *succeeded*
  (silent fallback to 0xBB).
- **Free damage triage.** At init, `current_x < max_x` is the drive's own quality
  verdict on the medium — an absolute, vendor-authored signal costing zero reads.
  **Caveat:** it only holds with the read-speed uncap OFF, or a pristine disc
  reads 40 < 48 and is falsely flagged. `accudisc_speed_uncap_probe` answers
  **ON/OFF/UNKNOWN** (`LIKELY_ON` removed 0.8.0), and **without a driver it can
  now only say UNKNOWN** — so triage built on it must treat UNKNOWN as "do not
  flag", never as "off", which since 0.8.0 is the common case rather than the
  edge one.
- **SpeedRead does NOT defeat the governor.** Measured on both discs including a
  full eject/load re-init with SpeedRead on: the ceiling holds (40x pristine, 32x
  scratched). The two act on **orthogonal axes** — the governor caps DATA RATE,
  SpeedRead raises RPM (nominal curve scales ×1.2, 17–40x → 20–48x).
  **Why Q dies:** at the inner radius the natural rate under SpeedRead (20x) is
  below the cap, so the cap never binds, the drive spins at full SpeedRead RPM,
  and Q — which has no CIRC — fails. At the outer radius natural 48x exceeds the
  cap, RPM is throttled, and Q survives. The governor guards the wrong axis and
  never protects the inner tracks. This predicts the measured dead zone exactly
  (curve crosses 32x at ~44% into ABBA; measured inner 10–60% dead, 70–100%
  clean).
  **SpeedRead is therefore a pure liability for CD-DA:** it cannot raise the rate
  ceiling, so its only headroom is the inner radius — exactly where its RPM kills
  Q. The whole-disc A/B showed no throughput gain at all (both 24.2x) while Q
  fell 99.2% → 40.6%.
  **ALL OF THE ABOVE IS SUPERSEDED — 2026-08-09, and the guard it describes no
  longer exists.** Keith's ruling: the drive is physically incapable of reading
  CD-DA above 40x, the governor ignores SpeedRead for CD-DA entirely, and page 2A
  reports the request rather than the governed throughput — so **that A/B's two
  arms both ran at 40x**, and "its RPM kills Q" cannot be what separated them.
  cdda2img has since measured Q degradation as a property of the DISC, not the
  speed. The refusal, `allow_unsafe`, `ACCUDISC_ERR_UNSAFE_COMBINATION` and the
  CLI warning were all removed in 0.6.0 (item 1 of the 2026-07-26 outstanding
  list, now DONE). The escalation contemplated here — "refuse the uncap for any
  CD-DA read" — is dead rather than open. **No further speed tests on this
  question**, by the same ruling.
  **Independently confirmed by the vendor's own tool, 2026-08-11.** PlexTools
  Professional XL V3.16 lists the `20–48X CAV` SpeedRead band in the **Mode 1
  column only** — Mode 2, Audio CD and CD-RW are blank on that row, while every
  other rung is offered to all four. So Plextor does not offer the SpeedRead
  band for audio either. This does not merely agree with the ruling, it splits
  it: the ×1.2 scaling above is **real as a mechanism** (it is a published row)
  and false only in its *applicability to CD-DA*. Mechanism confirmed, scope
  denied — the guards were removed correctly rather than by luck. Transcribed in
  `docs/research/cav-read-speed-geometry.md` §1.2, which also settles that
  `req=16 → page2a=8` is a rung that does not exist rather than a refusal.
- **Honoured speed ladder is discrete: {4, 8, 24, 32}.** 1–3 → 4; 6 → 4; 9–23 →
  8; 28 → 24; 40/48 → 32. Two disjoint regimes, explained by the nominal CAV
  curve starting at 17.00x: {4,8} are CLV (a ceiling below 17x binds at every
  radius, so the curve is flat), {24,32} are CAV (clamping only the outer
  region). The 9–23 dead zone is the gap between the top CLV rung and the CAV
  floor. **40x is not settable** — it is only reached by free-running at the outer
  edge. ~~Still open: `speed X` reports what was asked, not what the drive
  honoured~~ — **CLOSED 2026-08-09**: `speed X` now prints a `quantized` line
  naming both figures, and `read --speed X` warns on stderr. See the
  `--speed 16` item above.
- **GET PERFORMANCE nominal is RPM-derived, not medium-measured.** Identical
  across no-disc / ABBA / ZZ Top — `end_lba` is always 359999, never the real
  lead-out — but it *does* track SpeedRead. Constant across discs, not across
  drive state. DVD case untested.

### Q subchannel — architecture, findings, and what is left

**Key architecture fact.** C1/C2/CU protect only the main (audio) channel, via
CIRC Reed-Solomon. The subchannel is **not** inside CIRC: Q's only integrity
check is one CRC-16/CCITT per frame — **detection, no correction.** So a sector
can carry pristine audio (0 C2) and a dead Q frame. Confirmed on real damage: an
800-sector window read 0 C2-flagged sectors and 22 CRC-bad Q frames. **Q and C2
are orthogonal domains.**

**Q's compensating advantage** is that it is near-deterministic — abs MSF +1 per
sector, track/index piecewise-constant, rel MSF counting down in a pregap and up
in a track. Enough CRC-valid *anchors* fit the model and interpolate the gaps.
The one non-interpolable event is the index 0→1 transition, which needs at least
one clean anchor near each boundary. **So target re-reads at index boundaries,
not uniformly.**

**Two failure populations — the key result.** Three cache-defeated re-reads of the
same region: ~6 LBAs bad in *all* passes (static physical defect; re-reads never
fix), ~6 bad in only 1–2 passes (transient; recovered by re-read + consensus).
Neither lever alone suffices — consensus for the transient population,
deterministic-model interpolation for the static one.

**Speed barely affects the Q error rate inside the drive's governor envelope**
(4x → 9 bad, 8x → 7, free-run → 7–9 on the same window): defect-driven, not
RPM-driven. *Outside* the envelope — with the uncap defeating the ceiling — speed
matters enormously. Both datasets are correct; they sample different regimes.
**Lever = re-reads + model. Never defeat the governor on a damaged disc.**

**Clean-disc baseline (ZZ Top, new).** Radius sweep, 3000-sector windows:
uniform ~99.9% Q-CRC-ok at every radius, **no dead zone**. Max speed does not
corrupt Q on a clean disc, so the dead-zone Q loss and cdda2img's §9 missing
pregaps are **damage-driven, not speed-driven**.

**Whole-disc pregap census (ABBA, all 19 boundaries) — "9 of 19" RESOLVED.**
9 tracks have real pregaps (t2–t7, t9, t14, t18) of 47–50 frames, plus track 1's
33-frame lead-in gap; the other 9 are **genuinely gapless** — index-1 → index-1
with no index-0 frames, confirmed even where the boundary region was damaged. The
starting hypothesis ("all tracks have pregaps, damage hides them") is **refuted
for this disc**. Dead frames *inside* a pregap are fully covered by the surviving
countdown anchors, so zero pregap information is lost.

**The CRC gate is load-bearing.** Rejected frames decoded as abs142:38, index19,
adr9 — garbage a non-CRC-checking reader would splice in as phantom indices.
Damage does not merely lose Q metadata, it **injects false Q metadata**; the
per-frame CRC-16 is the only thing between the two. (Related: only ADR=1 frames
are position — ADR=2 is MCN, ADR=3 is ISRC — or the ~1-in-98 MCN frames
masquerade as index-0 boundaries and manufacture phantom pregaps.)

**Damage is localized, not radius-graded.** Sweep of 2000-sector windows across
ABBA: 96–100% Q-ok everywhere, with a few damaged spots (inner ~LBA 40k, outer
~LBA 340k, pinpoint hits such as t16@281733). Surface blemishes at specific
places, not an inner dead zone. This disc therefore needs no Q recovery to get
pregaps right — the recovery machinery is for a **worse** disc that loses a
boundary anchor entirely.

**Index/pregap decoder — DONE 2026-07-16.** `accudisc_index_map_decode`
(`src/cdda/index_map.c`) + `accudisc pregaps` + `test_index_map`. CRC-gates every
frame and classifies each TOC boundary PRESENT / NONE / UNKNOWN / NO_DATA.
**Key rule: a pregap ABUTS index-1**, so gaplessness is decided by the
boundary-abutting frame (walk down from L-1, skipping MCN/ISRC frames), never by
any bad frame in a wide window — that over-flags. Live on ABBA: 9 pregaps, 9
gapless, exactly 1 UNKNOWN (t16, whose L-1 frame 281732 is physically dead),
correctly separated from t19 which the manual pass had lumped with it.
`pregaps` exits 3 if any boundary is UNKNOWN.

**Still open — Q recovery.** Same items as "Carried over — Q recovery" above:
model reconstruction across a dead abutting frame (t16 is the standing test case,
captured offline as `tests/data/abba_t16_unknown_boundary.sub`, LBA
281333..281762); targeted re-read + consensus for boundaries whose neighbours are
also damaged; and the unified re-read predicate — a sector fails if C2-dirty OR
Q-CRC-bad, both from one READ CD, with the status map gaining a second dimension
(audio | Q). Q-CRC counters themselves are **done**
(`subq_total`/`subq_ok` in `accudisc_read_stats`, printed by the read summary and
mirrored on `--progress-fd`).

## Eject feature — DONE

`accudisc eject` / `accudisc load` ship (block-layer `CDROMEJECT` /
`CDROMCLOSETRAY`, not START STOP UNIT — see the header-comment item above).

## Investigate how Plextor drives handle speeds — SOLVED

The "permanently stuck at 32x, but 40x after an eject" report is the disc-init
governor, explained in full above. The original hydrogenaudio thread
(<https://hydrogenaudio.org/index.php/topic,28739.0.html>) had it right: the
drive determines an optimal read speed at disc init and imposes it in hardware.
The `0xEA arm refused` seen alongside it was the driver selftest failing on a
read-only open, not a speed matter.

## ATIP / media identification

- ~~**Wire the ATIP catalog into a lookup + CLI.**~~ **DONE 2026-07-12.**
  `accudisc media` reads the disc ATIP (READ TOC/PMA/ATIP fmt 4) →
  manufacturer + code + capacity + CD-R/RW; `accudisc_read_atip()` /
  `accudisc_atip_manufacturer()` in `src/drive/media_db.c`, unit-tested and
  live-verified on a Taiyo Yuden blank. **Lookup keys on `sec` + frame-decade**,
  so 97:24:01 resolves the 97:24:00 entry and the decade separates makers
  sharing a `sec`. *Optional follow-up:* surface spiral length (record+0xDC) and
  the ATIP reference-speed / indicative-power fields.
- **Public ATIP cross-reference pass.** *Largely done 2026-07-12:* diffed against
  cdrecord's `diskid.c` (107/123 agree) and Nero 2026 — both carry the same
  effectively-frozen registry, so the feared "post-2007 gaps" are moot.
  cdrecord's 3 high-confidence uniques folded in (`gen_media_db.py` → 134 codes).
  *Remaining (optional):* a broader web ATIP database diff for obscure codes none
  of the three list.

## Recording

- **CD-Text on write — two modes; design of record is `RECORDING_PLAN.md` §11.**
  - **v0 — PASS-THROUGH: SHIPPED 2026-07-24.** `write --cdtext FILE` consumes the
    raw READ TOC format-0x05 blob byte-for-byte and injects the packs into the
    SEND CUE SHEET lead-in (dataForm 0x41 + R-W subchannel packs, ring-filled).
    No encode step, so it handles re-burns of a captured disc. Verified on CDEmu
    (§11.7) and on physical media (§11.8, PX-716A + Taiyo Yuden): 760 bytes in,
    760 out, `cmp` identical, alongside 19 correct ISRCs and the MCN.
    **Lesson from the first, failed attempt: one wrong byte in the cue sheet
    produced perfect audio/MCN/ISRC and *no CD-Text at all*, with every command
    returning success.** A byte-for-byte read-back compare is the only assertion
    that catches that class; decoding for a human diagnostic is not.
  - **v1 — AUTHORED (design settled 2026-07-24; NOT IMPLEMENTED — `adsc_toc_parse_cue`
    still ignores `CD_TEXT`, and there is no strings→packs encoder in the tree).**
    strings / `.toc` `CD_TEXT` blocks → 18-byte packs → the blob v0 already knows
    how to lay down.
    **INPUT SURFACE IS SETTLED, and it is the `.toc`.** An extracted RBI holds one
    s16le PCM stream and one ASCII `.toc`; there is no CD-Text blob block, so v0
    has no reachable input in this pipeline and the Step C/D fixture was
    manufactured via a CDEmu detour the workflow cannot reproduce. First task is
    therefore `adsc_toc_parse_cue`, which ignores `CD_TEXT` today — a new parser
    fed from a boundary we don't control, so it lands inside the `[P2]` hostile-
    input sweep above.
    **Authoring is also the stronger path on charset**, not a compromise: a blob's
    payload is never inspected (by design), so an illegal codepoint reaches the
    disc and fails to render on standalone hi-fi equipment — `cdemu_utf8__33packs`
    in our corpus is UTF-8 declared as charset 0x00. Mapping strings to bytes
    forces every character to be confronted, which is why the rule below is
    enforceable here and unenforceable in v0.
    Needed because
    cdda2img has NO strings→packs encoder (their `cdtext.py` is decode-only) and
    cdrdao is what encodes for them today — so once cdrdao leaves, a *fresh* disc
    authored from metadata has no CD-Text unless AccuDisc encodes it. cdda2img's
    interim until this lands is re-burn-only (their option (a); they are NOT
    porting their own encoder). **Locked first-cut scope (cdda2img §43):** block
    0, single language, single-byte, pack types 0x80 title (disc+track) + 0x81
    performer (disc+track) + 0x86 disc-id (disc-level, conditional) + mandatory
    0x8f size-info. NB 0x86 is NOT in our current decoder (cdtext.c does 0x80/0x81
    only) — new to us but cheap (encoder is pack-type-agnostic); consider adding
    0x86 *decode* too for the `cdtext` display (separate, off critical path). OUT:
    0x82 songwriter (never authored), 0x8e UPC/ISRC (that's Q-subcode via
    CATALOG/ISRC → cuesheet, not a CD-Text pack). Unencodable codepoint fails
    BEFORE the burn (§11.9 INVARIANT rule 4). Reference: cdrdao `CdTextEncoder` /
    `writeCdTextLeadIn`, libmirage `cdtext-coder.c` — rewrite, never copy.
    Sequenced after v0.
    - **UNENCODABLE CODEPOINTS — the failure mode is the requirement** (adopted
      from cdda2img §55.2, whose evidence is a disc we both handled). cdrdao,
      given `"Voulez‐Vous (edit)"` from a `.toc`, **silently discarded the
      whole TITLE and renumbered the following packs**, so the gap decodes as a
      well-formed "track 13 has no title" — indistinguishable from a disc
      genuinely authored that way. It cost a day of wrong theories about where
      the gap came from. Our encoder must not reproduce that.
      **MANDATORY: never drop silently.** An unencodable field is either
      substituted *with a report* or refused *with a report* naming the field and
      the codepoint. `ACCUDISC_WROTE_WITH_CAVEATS` / exit 3 already exists and
      fits the substitution case exactly. A silent drop is a well-formed lie,
      which is worse than an error — nothing downstream can detect it.
      **OPTIONAL: the transliteration itself** (U+2010 HYPHEN → `-`, U+2019 →
      `'`, typographic quotes/dashes/ellipsis, NBSP). Deliberately *not*
      mandatory, per cdda2img §56.2: transliteration tables are not
      standardised, so two tools folding independently can disagree and make a
      *verifier* report phantom CD-Text mismatches between the `.toc` and the
      disc. Callers that fold upstream (cdda2img's `fold_cdtext` does) make ours
      a no-op; ours is a net for callers that don't, never a second authority on
      someone else's strings.
      **Licensing: do NOT pull in a transliteration dependency.** AccuDisc is
      MIT throughout; a GPL-3 folding library would break that, and the 95 % case
      is one small punctuation class (U+2010–2015, U+2018/19/201C/201D, U+2026,
      NBSP) — a few dozen lines, with everything else reported rather than
      discarded.
      Applies to the *decoder* display too: nothing should ever present "no
      title" without a way to tell "absent on the disc" from "we couldn't
      render it".
  Note: CD-Text does NOT affect the Disc ID (pure TOC) — content fidelity,
  separate from the pregap item below.
- **THE WRITE PROGRESS SURFACE — `accudisc_write_stats`. [P2], PLAN, agreed
  with Keith 2026-08-28.** Research and the full spec survey are in
  `private/research/incoming/2026-08-28-write-progress-surface.md`; this is the
  execution plan.

  **The gap.** A read hands the caller `accudisc_read_stats` — ~20 fields
  driving both a progress bar and a disc map. A burn hands back
  `void (*)(void *user, uint32_t done, uint32_t total)`. Two integers. There is
  no write-side stats struct at all.

  **And most of it is already measured.** `struct write_flow` (`src/write/burn.c`)
  carries, for the whole burn: `stalls`, `stall_ms`, `worst_ms`, `worst_lba`,
  `stalling_chunks`, and the drive-buffer `cap_total` / `min_fill_pct` / `polls`
  / `low_samples` / `cap_varied`. The FIFO separately counts `starved`, the
  low-water slot count and producer waits. **All of it goes to log lines and
  none of it reaches the caller.** So step 1 is export, not measurement.

  **What the SPEC adds beyond bytes/sectors/time** (MMC-5, surveyed 2026-08-28):

  - **NEXT WRITABLE ADDRESS, READ TRACK INFORMATION (0x52) — the one genuinely
    new signal.** MMC-5 §6.27.3.14: when streaming, NWA is *the next user data
    block the Drive expects to receive*. That is the DRIVE'S position, against
    our host-side `done`, which counts what we handed to its BUFFER. The gap
    between them is the burn's true in-flight depth — precisely the quantity a
    progress bar over a buffered pipeline gets wrong. One 0x52 per existing poll
    interval, so no new spin or seek behaviour.
    **Validity is conditional** (Table 509, RT/Blank/Packet/FP bits): it must
    report UNKNOWN where those say so, on the same principle as `cap_live` — a
    drive that will not answer must never read as "at sector 0".
  - **GET PERFORMANCE type 03h (Write Speed descriptors)** — the drive's ladder
    of admitted write speeds for the mounted medium. Belongs beside
    `accudisc_probe_speed_ladder`, NOT in burn stats: it is a choosing aid, and
    today's work shows it is an ACCEPTANCE list, not a delivery one.

  **What is NOT available, recorded so nobody hunts for it:**
  - **No write-side C2 or error count.** Correction is *generated* on write, not
    detected. The only way to know a burn was good is to read it back — which is
    why `LIVE_BURN_QUEUE.md` B1 exists.
  - **No per-sector write status.** MMC gives no write counterpart to the read
    engine's frame-accurate map: the drive accepts a WRITE(10) or refuses it. A
    write "map" could only record what the host sent and what was refused —
    worth building, worth not overselling as the read map's equal.
  - **No delivered-rate field.** Established repeatedly on 2026-08-28: page 2A
    echoes the REQUEST. Elapsed time is the only instrument for a rate.

  **Build order (each step independently shippable):**
  1. `accudisc_write_stats` carrying ONLY what is already measured. No new
     commands, no new risk on the burn path, and it closes most of the gap.
  2. Buffer fill into the progress callback, so a bar can show it LIVE rather
     than only in the end-of-burn tally. This is the single most useful thing a
     burn bar can show beyond percentage — a buffer trending empty is the early
     warning, minutes before an underrun.
  3. NWA last, because it is the only part that adds a command to the burn path,
     and the burn path is where this project has found its sharpest defects.

  **Two API constraints, both learned the hard way this week:**
  - **The struct needs a `size` field from its FIRST version.**
    `accudisc_features` has none, so 0.32.0 had to lean on the version macro as
    the sole signal that it grew 16 -> 24 bytes. `accudisc_read_req` and
    `accudisc_write_opts` carry `size` and negotiate properly. Be born with it.
  - **Changing `adsc_burn_progress`'s signature is an ABI break** for anything
    using it, and the Python binding wraps it. Either add a second callback type
    and keep the old one, or take the break at a minor bump and say so loudly —
    the project has done the latter (0.26.0, 0.27.0) without harm because the
    consumers are known. Decide deliberately; do not discover it.

    **IT COLLIDES WITH 8TRAX'S ITEM 1 — but do NOT wait for it.** That item is
    `[P4]` (Keith 2026-09-02: 8trax has no code, and may be months away), so
    coupling this work to it would drag this down rather than pull that up. The
    constraint that survives is narrower than "do them together": *do not move
    the write signature in a way that forecloses a matching read one.* Their
    request (§"Consumer requests — 8trax", item 1) is a read-path progress
    callback with the *identical* signature to the write one, because the whole
    value to them is the two paths becoming interchangeable behind one
    abstraction. So step 2 here (adding buffer fill to the write callback)
    would, if done alone, move the write signature AWAY from the read one we
    have not yet added — creating the divergence their request exists to
    prevent, and doing it in the window before they can notice.

    Resolution: settle ONE callback shape that serves both paths before
    changing either. Their `done`-counts-ATTEMPTED rule (a bar fed from
    `sectors_read` stalls short of 100% on a damaged disc) applies to the burn
    equally: whatever an unwritable sector is, it still advances `done`.

  **Step 3 is gated on a live burn.** NWA during `--simulate` may track the
  buffer, or be frozen, rather than meaning what it means during a real burn —
  queued as `LIVE_BURN_QUEUE.md` A4. Do not ship an NWA-derived depth whose only
  validation is a laser-off run.

- **Disc-ID round-trip mismatch = pregap/TOC, upstream (cdda2img).** Root cause
  pinned via 3-way compare: original disc track 1 @ LBA 33 with a 33-frame
  pregap, lead-out 347208; cdda2img's RBI + our burn both track 1 @ 0, lead-out
  347175 (Δ = 33). Our burned Disc IDs are byte-identical to the RBI mount, so
  AccuDisc reproduced the .toc *exactly* — the loss is in cdda2img's extract
  (its .toc drops track 1's pregap and must match the original TOC's index-1
  offsets AND lead-out, not just track 1). AccuDisc verified ready: a declared
  track-1 `START` yields the right index1_lba. Consider an AccuDisc read-back
  verify (`--verify-toc`) that diffs the burned TOC vs the source .toc and warns
  on any offset delta.

## Probes / diagnostics

- **A per-sector Q-CRC map, alongside the audio one. [P3], NICETY — Keith
  2026-07-26, "just a nicety, if possible, not an absolute necessity".** Today
  `--map`/`--map-file` cover audio only; Q health is reported solely as the
  `subq_total`/`subq_ok`/`subq_bad` summary. Wanted: the same per-sector picture
  for Q.

  **Possible, and cheaper than it looks — the verdict already exists and is
  discarded.** `src/read/engine.c:569-580` runs
  `accudisc_sub_extract_q` → `accudisc_q_parse` per delivered sector and
  collapses `qd.crc_ok` straight into the counters. This is a *store*, not a new
  measurement: no extra drive I/O, no second pass. It is also the "status map
  gains a second dimension (audio | Q)" line already promised under the unified
  re-read predicate — building it here would discharge that.

  **Four design points, each with a trap:**
  1. **Do NOT pack Q into the existing map byte.** A sector can be audio-clean
     *and* Q-dead at once — that orthogonality is the core Q finding (CIRC
     protects the main channel; Q has only a CRC-16, detection without
     correction), and the byte holds one state, so the two dimensions would
     compete for one slot. Use a second byte array mirroring `--map-file`'s
     existing contract (one byte per sector, mmap-able, `--qmap-file`).
     Incidentally there IS room in the byte — low-nibble states 6..15 are
     unused (`accudisc.h:1072-1077`) — which is the tempting wrong answer.
  2. **"Q ok" must mean CRC-valid, not "position present".** ADR alternates
     1=position / 2=MCN / 3=ISRC, roughly one MCN frame per 98. A CRC-good
     ADR=2 frame is healthy but carries no position; treating it as missing
     position is precisely the bug that manufactured phantom pregaps in the
     early ABBA analysis. Encode ADR as well as CRC state — the second nibble
     is the natural home.
  3. **"Not captured" needs a state distinct from "ok".** Q is only meaningful
     with `--sub raw`; the engine's own comment notes `SUB_Q` is CRC-gated
     inside the drive, so there is no verdict to report on that path. Without a
     distinct value, absence renders as health — the same defect as
     `disc_status` returning 0 instead of -1 when unobtainable.
  4. **`--map` rendering needs its own legend**, reusing the worst-state-in-
     bucket condensation. Do not reuse the audio glyphs for different meanings.

  Interface note: `--qmap-file` is purely additive, so
  `cli-machine-interface.md`'s stability guarantee is untroubled. The public
  `ACCUDISC_MAP_*` constants stay as they are; a Q map wants its own
  `ACCUDISC_QMAP_*` set rather than extensions that make the audio encoding
  ambiguous.

- ~~**`speeds`: report min/avg/max as well as the current single figure.**~~ —
  **DONE AND HARDWARE-VALIDATED 2026-07-28; the bands became first-class and
  the DEFAULT at 0.9.0, 2026-08-10.** `--sweep` / `points=3` landed first: each
  rung timed at inner, middle and outer, `min=`/`max=` appended, `measured=`
  kept as the middle band. The acceptance-test result is at the end of this
  item — read it rather than this line, since three numbers appearing is not
  evidence the instrument measures what it claims.

  > **This header said "NOT YET HARDWARE-VALIDATED … the acceptance test has
  > not been run" until 2026-08-10**, contradicting the passing table 150 lines
  > below it inside the same item, and pointing at a "What is still owed"
  > section that had been deleted when the test passed. Recorded rather than
  > quietly fixed: an item long enough to disagree with itself is one where the
  > **summary at the top is the part that goes stale**, because the evidence
  > gets appended at the bottom where it was produced. A reader who stopped at
  > the header would have concluded the instrument was unproven.

  **What 0.9.0 changed** (`accudisc_speed_rung` 14 → 20 bytes, API_PLAN §8 row
  13): the probe was measuring three per-band rates and then discarding them,
  publishing only `measured` (middle), `min` and `max`. `band_cx[3]` keeps
  them, the CLI prints `inner=`/`middle=`/`outer=`, and the sweep is now what
  a bare `speeds` does — `--quick` is the old single-band probe.

  **Why the sweep had to become the default**, in one observation: Keith's bare
  `speeds` on 2026-08-10 reported `req=40 measured=17.46` against
  `req=32 measured=18.23`. A default that shows 32× beating 40× is displaying
  the phantom-rung radius artefact this entire item exists to remove, and the
  library already refuses to draw a `verdict=` from it. The confounded
  measurement should not be the one you get for free.

  **`min`/`max` are kept beside the bands, and are not redundant.** They are
  order statistics; the bands are locations. `min == inner` only while the
  curve rises monotonically with radius — the healthy CAV case, hence the case
  where confusing them is invisible. The very first run after the field landed
  produced the counter-example without being asked to: `req=8` came back
  `inner=8.73 middle=8.02 outer=8.01`, `max` on the **inner** band. Had the CLI
  printed `inner=min_cx` it would have passed every test written from the
  recorded (monotonic) PX-716A vector.

  Decisions taken, with the reasoning, since each closed off an alternative:

  - **`measured=` is the MIDDLE band, not the mean of three.** The token is
    declared stable in `cli-machine-interface.md`; a mean would have kept
    every parser working while changing the quantity underneath it — the
    referent-drift class, arriving through the exact door the "do not rename
    to `avg=`" trap was built to guard. Consequently there is no `avg=` key
    at all, and the item's own title is now a slight misnomer.
  - **No `size` field on `accudisc_speed_rung`; the break was taken instead.**
    6 → 10 bytes (measured, both compiled). A per-element `size` on an OUT
    *array* is structurally wrong under API_PLAN §7.1's own OUT rule — it
    means trusting N independent caller claims — and §7.1 explicitly warns
    that widening the guarded set "turns a decidable problem into an
    unbounded one". `points` is a plain parameter, so this is one deliberate
    break rather than two. **The window is now spent**; see task 6.
  - **Windows are laid out by `adsc_speeds_layout` (`src/internal.h`), which
    is pure and tested device-free** (`tests/test_speeds.c`, 33rd test).
    Extracted precisely because the failure is silent: a guard that did not
    scale with `points` would overlap the bands, the re-reads would be
    cache-served, and every rung would report a FLAT gradient — which is
    also the signature of a genuinely CLV-clamped rung. The bug would have
    presented as the instrument's own diagnostic firing correctly. The test
    was verified by injecting that exact bug (guard left at `count/ncand`):
    69 failures, including the named case.
  - **Cross-rung radius bias is now documented rather than fixed** (item 4b's
    option (a), header + man + machine interface). Reversing the rung order
    between bands would have cancelled the bias in the *mean* — but we do not
    report a mean, so it would have bought nothing.

  **ALL THREE CLAUSES NOW IMPLEMENTED (2026-07-28).** Keith restated the
  full specification — "read the page 2a settings on the ladder, look at the
  actual throughput at all 3 sections of the disc, then decide which of the
  page 2a readings represent real, actual speeds that are achievable under
  the many conditions that influence the governor's choice, discard the
  readings that are obvious duplicates/unachievable, then use the remaining
  settings as the *real* speed ladder" — and the deciding half landed too.

  **Where the verdict lives, decided by Keith:** the library decides and
  reports; the read engine does **not** apply it. `accudisc_speed_rung`
  gains `verdict` + `equiv_x` (ADMITTED / DUPLICATE / QUANTIZED / UNKNOWN),
  the CLI prints `verdict=` per rung plus a `ladder admitted=` line, and
  `accudisc_read_req.speed_ladder` is never rewritten. Rationale: putting it
  in the library is what stops every consumer reimplementing the rule —
  `RECOVERY.md:196`'s `drive_speed.admitted_ladder` on cdda2img's side is
  now redundant, which is the reference-consumer principle working.

  **How the rule survives the cross-rung radius term**, which was the real
  design problem: it never compares raw rates against a modelled curve. A
  rung's own `max_cx - min_cx` is the speed change across the whole span,
  and adjacent rungs sit exactly one window apart, so that spread *measures*
  the per-neighbour radius effect on this disc — self-calibrating. A gap
  must beat `K` radius-steps to count. **This is what min/max are for
  beyond reporting, and why clause 1 had to land first.**

  Three findings worth keeping, all from making the test fail:
  - **`K` does not manufacture duplicates.** The obvious reading is wrong.
    `req=48` measured *slower* than `req=40` (23.01 vs 23.73) — the radius
    bias made directly visible — so it fails "faster than the rung below"
    outright and never consults the margin. `K`'s only load-bearing job is
    the other direction: not collapsing genuinely distinct rungs.
  - **`K` is therefore uncritical, and that is asserted, not asserted-about.**
    The binding constraint is the closest genuine pair (40-vs-32: 4.18x gap
    against a 0.72x radius-step), so any `K` below 5.8 keeps it. The test
    sweeps `K` 1..5 for an identical ladder **and requires `K=6` to differ**,
    so the sweep cannot pass by the rule ignoring `K`.
  - **A first version of the stability check was wrong.** It widened the
    intervals rather than varying `K` — which fabricates a disc with twice
    the gradient, where adjacent rungs genuinely *are* less distinguishable.
    Correct physics, wrong test. `K` was made a parameter of the internal
    entry point precisely so the sweep tests the threshold itself.

  `points == 1` yields UNKNOWN for every rung and no ladder line, rather
  than judging on point samples — the refuse-don't-narrow rule.

  Verified against the recorded PX-716A run as a test vector (not invented
  numbers, since the difficulty is exactly the radius term): admitted ladder
  **`40,32,24,8,4`**, with 48 duplicate:40 and 16 quantized:8.
  Measure each rung at
  the inner ring, the middle and the outer ring instead of one location, so a
  rung reports a range rather than a point. Wanted for its own sake and as a
  **sanity check** that our timing arithmetic and the drive are both behaving.

  **It is a good check, and here is why it works:** on a CAV drive the spread
  *is* the CAV curve — inner slow, outer fast — so a rung with no spread on a
  CAV drive means our timing is wrong, the cache is serving us, or the rung is
  actually clamped. Conversely the {4,8} rungs are CLV (a ceiling below the
  17.00x curve floor binds at every radius) and **must come out flat**, while
  {24,32} are CAV and must spread. That makes this an independent cross-check on
  `accudisc_classify_rotation` and on the GET PERFORMANCE nominal curve, which
  today are the only things asserting rotation mode.

  **Four confounds must be controlled or the instrument reports the wrong
  thing** — this is a measurement, so it inherits every lesson the Q work
  learned:
  1. **The disc-init governor** pins a per-disc ceiling at load (clean 40x,
     scratched 32x). A damaged disc's spread reflects the governor, not
     geometry.
  2. **SpeedRead / the uncap** scales the nominal curve x1.2 and is persistent
     drive state a previous session may have left on. Record its state with the
     result.
  3. **Drive contention** — another process on the same device collapses
     measurements. Take `flock /var/tmp/sr0.lock` and write `/var/tmp/sr0.owner`.
  4. **Media dependence**, which Keith raised: the curve is per-medium. A figure
     is only comparable against another run on the *same disc*.

  **Two design decisions, both with a trap:**

  - **DO NOT rename `measured=` to `avg=`.** `cli-machine-interface.md:176`
    declares the `speeds` line stable and token-primary, and explicitly permits
    *appending* new keys — appending `min=`/`max=` is therefore free, but a
    rename breaks any parser keyed on `measured`. cdda2img is on our live tree,
    so a rename additionally owes an API_PLAN §8 ledger row *before* the commit.
    Separately, **"avg" would be a false label**: the mean of three spot samples
    is not the disc average, and naming it so claims more than the measurement
    supports. Keep `measured=` as the existing single-window figure, append
    `min=`/`max=`, and document all three.
  - **`accudisc_speed_rung` is a caller-allocated transparent OUT struct with no
    size field** (`include/accudisc/accudisc.h:1041`; `{requested_x,
    reported_x, measured_cx}`, 6 bytes, filled through `out`). Adding
    `min_cx`/`max_cx` grows it — **exactly the API_PLAN §7.1 hazard**, on a
    struct that never got the `uint32_t size` treatment `read_req`/`read_stats`
    did. Either give it a size field in the same change, or take the break
    knowingly while nothing outside this repo links the library — and note that
    window **closes when the Python binding ships** (§7.1's own reasoning).

    **Status 2026-07-28: SPENT.** The break was taken; the struct is 10 bytes
    and carries no size field. Nothing outside this repo linked the library at
    the time, and `accudisc_probe_speed_ladder` was still unbound. It is now
    free to bind — and binding it is what makes the next growth of this struct
    a real version break rather than a free one.

  **Implementation notes:** cost is 3x the timed windows per rung (default
  ladder is up to 8 rungs after page-2A filtering). Each of the three locations
  needs its *own* cache-fresh window per rung, so the window allocator must not
  collide across rungs *or* radii — the existing guarantee is only per-rung.
  Prefer keeping the current single-location behaviour as the default and
  putting the three-point sweep behind an opt-in flag, so the default output and
  probe time are unchanged; `--start` then keeps its present meaning instead of
  being silently reinterpreted as a centre point.

  **As built** (all of the above honoured): opt-in `--sweep`, default path and
  its probe time untouched, `--start` unchanged. Windows are indexed
  `band*ncand + rung` over the span, so all `points*ncand` are disjoint across
  rungs *and* radii; the fit guard scales with `points` and refuses rather
  than overlapping. `--sweep` widens the default span from the middle half to
  the whole disc, since three bands of the middle half would be three samples
  of the same neighbourhood rather than inner/middle/outer.

  **HARDWARE-VALIDATED 2026-07-28 — the acceptance test PASSED.** PX-716A
  rev 1.11, Tracy (11 audio tracks, leadout 162892, clean), under
  `flock /var/tmp/sr0.lock`. Confounds recorded: governor NOT pinned below
  max (page 2A `max 48x / current 48x` at init — the
  [[plextor-speedread-subq]] triage signal is `current < max`, absent here);
  SpeedRead uncap found ON and left ON; single-agent, lock held; same disc
  throughout.

  The CLV/CAV discriminator came out exactly as the reasoning above
  required, and it is the whole point of the item:

  | req | page2a | measured | min | max | spread |
  |---|---|---|---|---|---|
  | 48 | 48 | 23.01 | 17.15 | 27.67 | **10.52** |
  | 40 | 40 | 23.73 | 18.08 | 28.28 | **10.20** |
  | 32 | 32 | 19.55 | 15.14 | 23.09 | **7.95** |
  | 24 | 24 | 14.98 | 11.75 | 17.57 | **5.82** |
  | 16 | **8** | 8.01 | 8.01 | 8.01 | **0.00** |
  | 8 | 8 | 8.01 | 8.00 | 8.01 | **0.01** |
  | 4 | 4 | 4.01 | 4.01 | 4.01 | **0.00** |

  The CAV rungs spread by 5.8–10.5x; the CLV-clamped rungs are flat to
  within 0.01x. **A flat result was the ambiguous one** — it is equally the
  signature of an overlapping-window bug — so the fact that flatness appears
  *only* on the rungs independently predicted to be clamped, and never on
  the CAV rungs, is what makes this a pass rather than three plausible
  numbers. `req=16` snapping to `page2a=8` is the drive quantizing, which is
  exactly what the `page2a` token exists to expose.

  **The uncap does not affect it on this disc, measured A/B/A rather than
  assumed** (Keith's prediction, confirmed). Matched ladder
  `40,32,24,16,8,4` so `ncand` — and therefore the window layout — is
  identical across states; without pinning it the uncap changes the
  page-2A max, filters the 48 rung out, and silently re-lays every window.
  Largest ON-vs-ON difference (run-to-run noise) **0.40x**; largest
  OFF-vs-mean-ON difference (the effect) **0.21x**; ratio **0.53**, i.e.
  the effect is half the noise. **The robust reason is that nothing in this
  run came near either ceiling** — the fastest single measurement anywhere
  was 28.28x, so 40x versus 48x cannot bind whatever the limiter is. That
  statement does not depend on knowing *why* we top out at ~28x, which is
  the honest position: the nominal curve predicts ~32.7x at this lead-out,
  we measured below it, and whether the shortfall is geometry or a separate
  CD-DA read cap is a further question (Keith's `readcd` data-disc test).
  **Do not generalise to a full-length disc** either way — an 80-minute
  disc runs further out the curve.

  Not re-run under `--sub`: [[plextor-speedread-subq]] applies unchanged and
  this probe reads audio-only (`ACCUDISC_SUB_NONE`, `speeds.c`), so the Q
  cliff is out of its path by construction.

- **Timed-read cache detection.** Borrowed from libcdio-paranoia
  (`cdrom_cache_handler`): a re-read that returns implausibly fast was
  served from the drive's cache, not the platter. Paranoia flags a backseek
  read completing in under ~6 ms. Use it as a probe that *verifies our
  cache-defeat actually works* on a given drive — time a normal read, then
  time a re-read at our 5000-sector flush distance; if the re-read is not
  meaningfully slower, the flush distance is too small for this drive's
  cache and every "independent" reread (c2_retries, verify, consensus,
  c2lag) is silently reading cache. Report-only, per-drive, like the other
  probes. Would also let the flush distance auto-tune instead of being a
  fixed constant.

## TOC reading

### Full-TOC → TOC automatic fallback — DONE + HARDWARE-VERIFIED 2026-07-21/22

`accudisc_read_toc_src()` prefers READ TOC format 0x02 (the raw lead-in
Q-channel, which carries session structure) and degrades to format 0x00,
reporting `source=` and `degrade=` (`none` / `leadin_unreadable` /
`leadin_absent` / `leadin_malformed`). `toc` exits **0** on a degrade — the
command promises track geometry and a degrade still delivers it in full; failing
would regress exactly the discs the fallback exists to serve. Frozen in
`cli-machine-interface.md`; the pure conversion `adsc_toc_from_fulltoc()` is
unit-tested (`tests/test_tocsrc.c`, 13/13). Both paths hardware-proven on the
PX-716A, and the degrade path's geometry cross-validated **byte-identical**
against the drive's own cooked format-0x00 answer — two independent decodes
agreeing on the MSF→LBA arithmetic, track slotting and extents.

**Durable lessons:**
- **Format 0x02 does NOT carry INDEX 00 / pregap data.** The lead-in TOC holds
  track *starts* (INDEX 01), A0/A1/A2 and session structure, nothing more.
  Pregaps exist solely in the program-area Q subchannel, so a successful 0x02
  supplies no more pregap data than a degraded 0x00 — the degrade costs only
  session structure. Both an earlier note of ours and cdda2img's §26.2 assumed
  otherwise. **Proven empirically, not just read off the spec:** on an MPO CD-R
  whose lead-in is completely unreadable, `pregaps` still recovered all 11
  pregaps (tracks 2–12, every one 149f) with near-perfect Q CRC.
- **A failing lead-in on a healthy program area is a real disc-health signal**,
  and it is disc-specific rather than a CD-R class property (the same session's
  Ritek CD-R read 0x02 clean). That is what `degrade=` exists to carry; callers
  archiving provenance should record it.
- **Cost:** preferring 0x02 is free when it succeeds (~5 ms either way) and costs
  ~166 ms only on a degrade — the drive giving up on the lead-in. Trivial once,
  significant for a caller polling `toc` in a loop on a degraded disc.

- **Phase B (`toc --pregaps`) — DROPPED 2026-07-22 at the requester's request.**
  cdda2img §27 withdrew the ask: their pregaps come from the Q stream and never
  from a TOC, and their rip path always captures `--sub raw`, so this would be a
  second program-area pass over data they already hold. No other consumer wants
  it; `pregaps` stays the standalone diagnostic. The token still ships (always
  `none`), **renamed `pregaps=` → `subq_indices=` on 2026-07-25** because the old
  spelling collided with the per-track `pregap <n>` field and real output read as
  a self-contradiction. **Do not build this without a new requester.**
- **Phase C (retry counter) — WITHDRAWN 2026-07-22 by the requester.** cdda2img
  §26.5 assumed a retry loop existed in the 0x02 path; there is none anywhere in
  `src/mmc/` or `src/transport/`. They then argued the current behaviour is
  *better*: a single-attempt failure means one specific thing, "failed after N
  tries" blurs it, and retries cost time on exactly the discs already failing.
  **Do not add retries to make a counter possible.** Retry *behaviour*, if ever
  wanted, is a separate deliberate decision.

- ~~**[P1] Emit a session COUNT on the degrade path**~~ (cdda2img §28) — **DONE
  and hardware-verified 2026-07-22.** `accudisc_toc_info.session_count` /
  `accudisc_toc.sessions_total`, emitted as `session_count=<n>` on the `toc` line.

  **Why it was needed:** a **multi-session all-audio** disc is otherwise
  undetectable. It passes an "all audio ⇒ safe" test while format 0x00 hands back
  the *last* session's lead-out rather than session 1's — wrong lead-out, wrong
  disc ID, silently. A track census provably cannot see session boundaries.

  **Why it works:** READ DISC INFORMATION answers from the drive's own disc
  model, not by re-reading the groove, so it still speaks when the lead-in will
  not. Confirmed on the MPO CD-R whose lead-in does not read
  (`degrade=leadin_unreadable ... session_count=1`), and the count corroborated
  three independent ways: READ DISC INFORMATION byte 4 (sessions = 1), byte 5
  (first track in last session = 1), and libcdio `cd-info`'s
  `Last CD Session LSN: 0` — a different tool issuing a different command.

  Three behaviours where there was one: **1** = fully reconstructible (one
  session owns every track and format 0's lead-out *is* its lead-out), so the
  model is synthesised and a dead-lead-in disc stays wholly rippable — exercised
  end to end, `read` resolving `session 1, lba 0 count 236435`; **>1** refuses
  with `session_unmapped` (the seams are known to exist, their positions are
  not); **0** falls back to the conservative all-audio walk.

- ~~**[P1] Mixed Mode: session selection is too coarse**~~ — **CLOSED
  2026-07-22.** On a Mixed Mode CD one session holds a data track first, then
  audio. Every step was individually right — the default audio session *is*
  session 1, its whole-session range *does* start at LBA 0, and the guard *does*
  correctly refuse a range containing a data track — and the outcome was that
  `accudisc read` with no arguments failed on the format. **Lesson: correct
  components can compose into a wrong behaviour; the defect was in the seam, not
  in any of the parts.**

  Fix: `accudisc_toc_session_audio_range()` narrows the default to the session's
  **audio tracks**, plus `accudisc_toc_track_range()` and `--track N` /
  `--tracks A-B`. **The guard was not relaxed.** Verified on hardware (PX-716A,
  Taiyo Yuden CD-R: data track 1 of 138230 sectors, audio 2–11, lead-out 342197):
  the default resolves to `lba 138230 count 203967`, `--tracks 2-11` resolves
  identically (independent confirmation, not one code path agreeing with itself),
  and the full rip produced 203967 × 2352 bytes exactly with 0 C2-flagged
  sectors. Still correctly refused: `--track 1`, `--tracks 1-11` (`data_track`),
  and a cross-session span.

  One case is **refused rather than solved**: audio tracks either side of a data
  track within one session cannot be expressed as a single range, so
  `ACCUDISC_ERR_UNSUPPORTED` is returned and the caller must name tracks. Legal
  on the wire, unit-tested, never observed on real media.

- ~~**[P3]** Bindings do not yet expose `accudisc_read_toc_src` or
  `accudisc_probe_disc`~~ — **CLOSED 2026-08-28, and it was wrong in BOTH
  directions.** Python exposes both and has for some time:
  `Device.probe_disc` (`__init__.py:1931`), `Device.read_toc_src` (`:2269`),
  with the cdefs at `build_accudisc.py:385,489` and `read_toc_src` documented in
  the binding README. And `bindings/rust/` contains a README and **no Rust
  source at all** — there is nothing there for a function to be missing from.
  The item described a gap in a binding that does not exist yet and a gap in one
  that had already closed it. Verified by reading both trees, 2026-08-28.

## Formats and specs

- ~~**[P2] CD+G capture and pack extraction**~~ — **DONE 2026-07-22.**
  `src/cdda/rw.c`, public API `accudisc_rw_*`, CLI `read --cdg FILE`.

  Built against the normative Philips/Sony *Subcode/Control and Display System —
  Channels R-W* (Nov 1991). Structure per §5.1: 6 bits = SYMBOL, 24 symbols =
  PACK, 4 packs = PACKET, so one packet per sector, 75 packets/s and **300
  packs/s**; `.cdg` is the 24-byte **pack** stream. Three stages: extract R-W as
  the low 6 bits of each subcode byte; undo the 8-pack convolutional interleave
  and its position permutation (transpositions (1,18), (2,5), (3,23)); RS-decode
  over GF(2^6), `P(X)=X^6+X+1`. Both codes are conventional RS with consecutive
  roots, so one routine serves both parameterised by length and parity count —
  (24,20) across the pack correcting 2, (4,2) over symbols 0–3 correcting 1.

  **Two corrections the spec forced on an earlier version of this entry:** the
  pack/packet nesting was inverted, and R-W *is* Reed-Solomon protected where it
  had been claimed not to be.

  **Findings worth keeping:**
  - **The stray-symbol count VARIES between reads of the same sectors** (39, 66,
    48 on three passes). R-W gets no C1/C2 correction from the drive, so these
    are transient channel errors, not pressed-in bits — which is the empirical
    argument for doing the RS decode at all. Without it every rip of the same
    disc would differ.
  - Measured on a Mixed Mode CD-R with **no** CD+G (still a real test): 1000
    sectors → 3993 packs (= 4n−7, the de-interleave span costing 7 at the tail),
    95,832 bytes exactly, all MODE ZERO. In one read capturing `--subf` and
    `--cdg` together the raw subchannel held 48 stray symbols and the P code
    repaired exactly 48 across 47 packs, one of which had two — so the t=2 path
    fired on real data.
  - Verification is a round trip against an **independent** encoder built by a
    different method (the encoder solves H*V=0 by Gaussian elimination, the
    decoder works from syndromes), so a shared mistake is unlikely to cancel out.

  **Still open: end-to-end verification needs an actual CD+G disc.** The one
  unproven assumption is the order in which the drive returns the 96 subcode
  bytes; that rests on the Q path in `subq.c`, which reads bit 6 of the same
  bytes and is hardware-verified. Worth acquiring a karaoke disc.

  Scope line with cdda2img holds: we deinterleave, RS-correct and emit packs;
  they render. RS correction recovers recorded bits rather than interpreting
  them, so it stays our side of the "AccuDisc only moves bits" rule.

- **HDCD: nothing for us to build.** It is a watermark in the LSBs of ordinary
  16-bit PCM; a bit-exact rip preserves it with zero special handling, and both
  detection (scanning LSBs for the control-code sync pattern) and decoding
  (peak extend, gain, dither) are analysis of delivered audio — explicitly out
  of scope per CLAUDE.md, and communicated to cdda2img as wholly theirs.
  Recorded here so the question is not reopened.

- ~~**[P3] Obtain the Orange Book**~~ — **DONE 2026-07-22.** Orange Book Part II
  (CD-R) Vols 1–2 and Part III (CD-RW) Vol 1 acquired, with the Multisession CD,
  Enhanced Music CD, R–W subcode, CD Text Mode, CD-ROM XA, MMC-3, SCSI-2 and SACD
  specs. **Public** ECMA-130 / ECMA-394 / ECMA-395 are cited but deliberately
  **not committed** — ECMA publishes them for free download, so a citation beats
  megabytes of permanent history.

  All four session-overhead constants are now confirmed:

  | constant | sectors | source |
  |---|---|---|
  | first session lead-out | 6750 | ECMA-394 §5.7.1 — **public, citable** |
  | later session lead-out | 2250 | ECMA-394 §5.7.1 — **public, citable** |
  | second+ session lead-in | 4500 | Multisession CD spec — licensed |
  | pregap | 150 | Multisession CD spec — licensed |

  The measurement (6750+4500+150 = 11400) was right. ECMA-394 being *public* is
  the useful part: those two figures may now be quoted and cited in `docs/`.

  **The 99-session ceiling was searched for and not found** in the Multisession
  spec — the document that would define it. Not proof of absence, but it looks
  like folklore borrowed from the real 99-*track* limit. `docs/research/
  disc-formats.md` §4 now records it as actively doubted. Any firmware ceiling
  is measurement work on our hardware, not a spec question.

- ~~**[P3] Read *CD Cracking Uncovered* (Kaspersky)**~~ and ~~**the defensive
  pass over `adsc_toc_from_fulltoc()`**~~ — **BOTH DONE 2026-07-22.** Taxonomy
  and findings in `docs/research/disc-formats.md` §11.

  **The audit found one real hole, and it was not a crash.** Memory safety was
  already sound (the suite runs clean under ASan + UBSan with
  `-fno-sanitize-recover=all` against `tests/test_toc_hostile.c`). The defect was
  the third failure mode — **silently normalising a contradictory TOC into a
  plausible-looking one.** `toc_fill_extents()` walked tracks in *track-number*
  order and treated that as *address* order; Kaspersky ch. 6's "Incorrect
  Starting Address for the Track" exists to break that coincidence. When it did,
  the out-of-order data track's extent collapsed to zero — invisible to the map —
  while its neighbour's stretched over the region it vacated, and
  `accudisc_check_audio_range()` returned **ok** for a span covering a data
  track.

  **Fixed two ways, deliberately keeping both.** Extents are computed in
  *address* order (`next_by_address()`) — not a hardening measure but the correct
  definition, identical on honest media; and `accudisc_toc.anomalies` records
  structural defects, the three meaning the map cannot be believed (`lba_order`,
  `overlap`, `leadout_before`) making the guard refuse with `toc_untrusted`.
  **The first defence is only as good as our imagination about which orderings
  can be violated; the second does not depend on having predicted the trick.**
  Six further flags are reported only — their discs are still described
  correctly, and over-refusing would break media that reads fine.

  **Deliberately not defended against: "Data Track Disguised as Audio".** CTRL is
  the TOC's only statement about track type; if it lies, no self-consistency
  check catches it. It surfaces at read time as sense key 5 / ASC 0x64, which the
  read engine already stops on. Recorded so nobody later "fixes" it with a
  heuristic that guesses track type from content — that is analysis (out of scope
  per CLAUDE.md), and it would be guessing besides.

  Two observations bounding what our parser can ever see: a drive may **remap
  non-standard point numbers into the legal range** (an NEC unit reporting `0xAB`
  as `0x6F`), so an in-range track number is not proof it was recorded that way;
  and non-standard points are **invisible to READ TOC entirely**, format 2
  included — reaching them needs subchannel reads of a later session's lead-in.

- **[P2] Acquire copy-protected test discs.** Also on cdda2img's TODO; recorded
  here because the need is ours first — the §11 hardening is verified only
  against **synthetic** TOCs built from Kaspersky's descriptions. That proves we
  survive the taxonomy as documented; it does not prove we survive what the
  schemes actually pressed. Real media is the only thing that closes that gap.

  Wanted: at least one disc per scheme, second-hand, cheap. **Regional pressing
  matters** — the same album is frequently protected in the EU and unprotected
  in the US, so a catalogue or matrix number is part of the requirement, not a
  nicety. A research pass to build the acquisition shortlist was launched
  2026-07-22; output lands in `private/research/incoming/`.

  **Survey done 2026-07-22** — `private/research/incoming/` holds the scheme
  taxonomy, a ~35-row title catalogue with catalogue numbers, and a ranked
  shortlist. Three flagged shapes were then modelled synthetically and are
  permanent tests in `tests/test_toc_hostile.c`; **none needed a disc to
  answer**: CDS-200 cross-session duplicate addresses (already covered — the
  overlap check does not filter by session), key2audio's three sessions
  (already handled — the audio session is chosen by content, not position),
  and MediaCloQ track-type inversion (inside the model; see below).

  Top of the shortlist: Natalie Imbruglia *White Lilies Island* (BMG 74321
  891212) and Right Said Fred *Fredhead* (BMG 74321 87262 2), both CDS-200; and
  a genuine **A/B pair** — Handel *Deidamia*, Virgin Classics 5455502 (Copy
  Control) versus 5456692 (Red Book reissue of the identical recording), plus
  Virgin Veritas 5457112 *Serse* as an explicitly-unprotected control from the
  same imprint and era. That triplet is the cleanest possible check that
  `anomalies=` keys on something real rather than on label/era artifacts.
  Charley Pride *A Tribute to Jim Reeves* is worth promoting — it would confirm
  the track-type-inversion report on real media. **Barcode 7 816190222-2 4**
  (from BinaryObjectScanner), which beats a catalogue number for secondhand
  buying.

  **BUY THIS FIRST — "Karaoke Spotlight Series — Pop Hits Vol. 132", Sound
  Choice SC8732.** Both halves now confirmed, so the earlier inference is
  retired: DRML lists it as a **confirmed MediaCloQ V1 sample**, with the
  protection printed on the disc label ("This disc is copy protected by -
  MediaClōQ - By SunnComm, Inc. - V1"), and an eBay listing's item specifics
  give `File Format: CD+G` / `Media Type: Standard CD+G`.

  One disc, two subsystems: the **R-W/CD+G decoder end-to-end** — the
  verification `tests/test_rw.c` structurally cannot provide, being a round trip
  against our own encoder — **and** the track-type-inversion path. That puts it
  ahead of the CDS-200 pair.

  **The family is wide**: Sound Choice shipped ~35 CD+G discs with MediaCloQ
  across their **8700 series** (discontinued 2003-04-14). So if one listing is
  awkward — the one seen says *"May not ship to United Kingdom"* — any
  8700-series Spotlight disc is a candidate, and the label text is checkable
  from a photo before buying.

  **Safe to read.** MediaCloQ installs no driver and no kernel component; that
  was XCP and MediaMax. The privacy allegations in *DeLise v. Fahrenheit
  Entertainment* concern web-side tracking after a user followed the disc's
  download offer to SunnComm's site — not an on-disc payload. Nothing an SG_IO
  tool touches, and those servers are long dead.

  Also gained from BinaryObjectScanner, closing survey gaps: **DocLoc** is by
  **DocData** (not Optimal Media as the survey had it), works via a
  "non-standard second session", and has three titles — *Yorin FM Hitzone 21*
  (Discogs 790336), *Helium Vola* (Discogs 188439), *Wolfsheim — Casting
  Shadows*. **LabelGate CD2**: Redump entry 95010 / product ID SVWC-7185. And
  key2audio's three-session structure is independently corroborated, with an
  unspecific further claim of a "partially invalid TOC" — **uncertain, do not
  act on it**, but a real disc would settle whether key2audio also trips an
  anomaly slug.

  Next research source, unexamined: **DRML** (the DRM Library,
  github.com/TheRogueArchivist/DRML), cited as BinaryObjectScanner's authority
  for MediaCloQ. Likely the best lead for the schemes still thin here —
  Alpha-Audio, DocLoc's mechanism, and SafeAudio's unidentified US titles.

  Not worth buying for TOC work, stated so effort is not wasted: XCP and
  MediaMax discs are Windows kernel-driver attacks on ordinary, well-formed
  Enhanced-CD-shaped discs, useful only as negative controls.

- **[P2] SafeAudio disc — for the RECOVERY engine, not the parser.** Tracked
  separately because it tests a different subsystem. SafeAudio inserts short
  bursts of unrecoverable noise into the audio, sized so a player's
  interpolation hides them; it never touches the TOC.

  The survey found no title at all. A contemporaneous source
  (audiorevolution.com 2001-07-24, relaying *de Volkskrant* 2001-07-20; fetched
  via `cdda2img/tools/wayback/fetch.py` — WebFetch is blocked from
  web.archive.org) names **Volumia! *Puur*** (Netherlands, BMG, 2001). It also
  explains why identification is so hard: BMG confirmed the system was shipped
  **with no notice on the disc**. A second lead, *Groeten uit Salou 4*, is
  explicitly hedged in that source as possibly key2audio instead — do not buy on
  it alone.

  Why it is worth having, which the survey undersold:
    - the errors are **mastered in**, so they are *static* by construction —
      identical on every re-read. Real damage mixes transient and static
      populations, and separating them is exactly the hard part. This is a
      **known-pure static population**, the control the C2/re-read work has
      never had.
    - it is the case where **a player interpolates and we must not**. A
      SafeAudio rip should surface hard errors *by design*; worth establishing
      before such a rip is read as a damaged disc.
    - diagnostically, systematic unrecoverable C2 at **consistent locations
      across re-reads** on visually clean media is a SafeAudio signature rather
      than a scratch — an inference the recovery engine could surface.

  What each disc would actually buy us, in order of value:
    1. a scheme that does something our taxonomy does **not** cover — the only
       way to find out is to meet one;
    2. confirmation that `anomalies=` fires on real protected media and stays
       silent on the unprotected pressing of the same title (the ideal test
       pair, and the reason pressing identity matters);
    3. evidence about whether the `UNTRUSTED_GEOMETRY` refusal is correctly
       calibrated — if a real protected disc rips fine everywhere else and we
       alone refuse it, we are over-refusing and should demote a flag.

- **[P3] Physical-characteristic protection is untouched** (Kaspersky ch. 9):
  deliberate defects, read-timing and inter-sector angle measurement, weak
  sectors. These bind to the medium rather than malforming the TOC, so nothing
  in the §11 pass addresses them. Listed for completeness, not planned — no
  demand, and the recovery engine's existing C2/reread machinery is the part
  that would meet them.

- **[P4] Vanity project: a backwards-compatible hi-res audio disc.** Replicate
  SACD's *audio quality only* in a format readable by drives >= DVD, using a
  technique in the spirit of HDCD / DTS-CD — payload smuggled inside a
  container existing hardware already plays. Options deliberately open.
  Sketch of the design space, to be argued properly later:
    - *Where the extra bits live.* HDCD hides ~1 bit in PCM LSBs; DTS-CD
      replaces the PCM entirely with a bitstream (so legacy players emit
      noise — the thing Enhanced CD was invented to avoid). A middle path
      keeps a valid 16/44.1 downmix audible and carries the residual
      elsewhere: LSB subcoding, the R-W subchannel (~72 B/sector), or a
      second session's data track.
    - *Why >= DVD matters.* A CD's 74-80 min at 44.1/16 has no headroom for
      a meaningful residual; DVD-density media gives ~4.7 GB, enough for
      24/96 outright, and the question becomes what legacy compatibility is
      even worth preserving at that point.
    - *The honest tension.* "Backwards compatible" and "hi-res" pull opposite
      ways: every bit spent staying compatible is a bit not spent on quality.
      Worth deciding early which one is the constraint and which the goal.
  Pure vanity, no schedule, and explicitly not on the critical path.

## Library API completion — PLAN ONLY, do not execute without direction

Raised 2026-07-25. Audit finding: the C API is **complete at the ABI level and
incomplete at the policy level** — `libaccudisc.so` and `accudisc.h` agree
exactly (65/65 symbols, no leakage, no reach-through from `cli/`), but the
*judgement* lives in `cli/main.c`: 1838 lines against a 1199-line header.

**The full design is `docs/reference/API_PLAN.md`** — scope, the guard/policy/
convention split, proposed signatures, the ABI questions gating the bindings,
the cdda2img communication ledger, and the effort estimate. Do not duplicate it
here; that document is the source of record.

Headline items, for grep:

- ~~`[P1]` Two silent-failure guards must move INTO the library~~ — **DONE
  2026-07-25, and the SpeedRead half is now being REMOVED entirely** by Keith's
  2026-07-26 ruling (see "Outstanding" task 1 below). The read-only-fd
  vendor-opcode diagnosis stays: it reports a condition rather than predicting a
  failure, and is correct under both the setcap and default regimes.
- `[P2]` Promote four acquisition strategies to public API — **and rewrite the
  CLI onto them in the same commit**, or the policy exists three times (CLI,
  Python, Rust) and drifts.
- `[P2]` Do NOT promote exit codes, `--progress-fd`, or `render_map` — those are
  process conventions, not library concerns. Document the mapping instead.
- ~~`[P3]` Bindings: settle the transparent-struct ABI hazard and the FFI
  callback design first.~~ **DONE 2026-07-26** (API_PLAN §7.1-7.3, commits
  `638db16` / `41591a9`): `uint32_t size` on `read_req`/`read_stats`,
  `ACCUDISC_ERR_ABI`, `accudisc_chunk` frozen, version single-sourced from the
  header. Note the free-to-break window is now narrower than "soname is `.so.0`"
  suggests — what actually made it free is that **nothing outside this repo
  links the library**, and that expires when the Python binding ships.
- ~~cdda2img is pinned to a snapshot fork of the binary for the duration.~~
  **Pin retired 2026-07-26** — both symlinks point at this tree's
  `build/cli/accudisc` again, so §8 is a *live* obligation: write the ledger row
  before the commit, not at the end of a phase.

## Consumer requests — cdda2img, recorded 2026-08-29

### 1. `accudisc_measure_write_offset` — the WORK, not the tools — `[P2]`

Keith's ruling, relayed in cdda2img §182 §2: *"If there's something you need
where AccuDisc should be doing the work but isn't, ask them to do it, don't ask
for the tools to do it yourself."*

cdda2img withdrew their own §178 framing on the strength of it. They had asked
us to expose `write_offset_signal` and `_locate` so **they** could orchestrate
generate → burn → read back → locate. That is asking for the tools, and it
contradicts the architecture Keith had already given them (*"we call AccuDisc,
you perform the test, you return the value"*) — which they had quoted to us
three paragraphs above the ask.

```c
ACCUDISC_API int accudisc_measure_write_offset(accudisc_device *dev,
                                               int32_t read_offset,
                                               int32_t *out);
```

One call in, `W` or `ACCUDISC_ERR_AMBIGUOUS` out. We generate the signal, burn
it, read it back, locate the pulses, return the offset.

**This resurrects a name this header already had and deleted.** `0.20.0` removed
`accudisc_measure_write_offset` as a PHANTOM — the header and `gen_offsets.py`
both cited it as though it shipped, and it never existed: a name for work not yet
done, leaked into the contract. Deleting it was right. The ruling says the work
should now exist, so the name comes back *behind an implementation* this time.

The three primitives stay public and bound — cdda2img still uses `_locate`
alone for the no-blank cross-check in their §179. What changes is that the
DEFAULT path is one call, and the orchestration lives once here rather than once
in cdda2img, once in 8trax, and once in the next front end.

**Blocked on media, not on design**: it burns, so it cannot be developed against
`--simulate`. Goes in `docs/reference/LIVE_BURN_QUEUE.md` when the blanks
arrive. Not urgent — cdda2img is explicit that it is not a blocker and they are
not burning.

### 2. The ladder driver: sweep-then-gate, NOT a caller predicate — `[P2]`

Settled with cdda2img across §181 §4 / §182 §1, and worth recording because the
losing option is the one that looks more efficient.

They proposed a ladder driver taking a **caller-supplied per-rung predicate** —
we own the sweep, they answer one boolean per attempt (their AccurateRip
verdict). Rejected, and they accepted the rejection: a predicate invoked from
inside our sweep does not put the lookup in their code, it puts a **blocking
network round-trip inside our read loop with the drive spinning**, which is what
`CLAUDE.md`'s "no lookups, no network" boundary exists to prevent.
`accudisc_ctdb_repair` is the model — the caller fetches, chooses and gates; the
library is handed bytes and hands back samples, and never reaches out.

**Shape: we run the whole sweep and return per-rung results; they gate
afterwards.** Cost, stated honestly: we cannot stop early on the rung that would
have satisfied them, so a sweep does strictly more drive work than an
oracle-guided one. Bought with it: the sweep's behaviour stops depending on a
remote service that can time out — their own run2 log shows recurring
`AccurateRip https fetch failed: read timed out` during re-gates, and a sweep
whose control flow depends on that produces null results nobody can reproduce.

### 3. The track-1 pre-gap rule is OURS — Keith's ruling — `[P2]`

Their cuesheet convention (INDEX 01 at LBA > 0 becomes track 1's pre-gap,
`start_frame=0`) is a range-and-geometry decision that `accudisc_plan_read_range`
does not express. Keith ruled it moves to us rather than staying a consumer
policy. See cdda2img §182 §3.

## Consumer requests — 8trax (Rust/FLTK GUI), recorded 2026-08-01

8trax is the third correspondent and the first **GUI** consumer. Keith's ruling
(their §b.1): it is a GUI **alternative** to cdda2img that will never interact
with it — not a peer sharing work, not a front end over it. He also wants
**nothing built until both AccuDisc and cdda2img go gold** (a few weeks), so
**neither item below is scheduled before gold.** They are recorded because 8trax
has written no code against us yet, which will never be true again, and asking
them what they needed while their migration cost was zero was the whole point.

**LOWEST PRIORITY IN THE FILE — Keith, 2026-09-02: "All 8trax requests are
lowest priority. It might be months before we get anywhere near that. 8trax
literally has no code."** Everything here is therefore `[P4]`. It was tagged
`[P2]`/`[P3]` until then, which is what caused a session to surface item 1 as a
near-term candidate on the strength of a `[P2]` grep — the "their top ask"
wording made it worse by carrying *their* ranking with no scale attached. Their
ranking orders these against **each other**; it says nothing about where any of
them sits against our work, and there is no consumer waiting on the other end.
Re-read this paragraph before promoting anything below.

### 1. Read-path progress callback — `[P4]`, their top ask — ours lowest

`accudisc_read_cdda` has no progress callback; `accudisc_write` does
(`accudisc.h:270-275`). One consumer, two shapes.

**The constraint that makes it worth anything: identical signature to the write
callback**, `void (*progress)(void *user, uint32_t done, uint32_t total)`. Their
point is that the value is the two paths becoming interchangeable behind one
abstraction; a differently-shaped read callback is worth much less.

Why not "just scan the map": every progress consumer that is *not* the grid —
window title, taskbar, ETA, an aggregate bar across a batch — would scan ~350,000
bytes per frame at 60 Hz to recover one integer we already have.

**`done` MUST count sectors ATTEMPTED, including `HARD` — do not wire it to
`sectors_read`.** Found by 8trax (§d.1) before the thing existed, and verified:
`src/read/engine.c:558-562` classifies a `HARD` sector, publishes it to the map,
then `continue`s **past** `r.st.sectors_read++`. So `sectors_read` is "delivered",
not "processed" — which is exactly what `accudisc.h:1393` says it is ("returned
by the drive (excludes zero-fills)"), so the field is correct and must not
change. But a progress bar fed from it **stalls short of 100% on a damaged disc
and never completes** — on precisely the disc where the user is already anxious,
where a stuck bar reads as a hung application. If `done` is ever defined the
other way, say so in the header rather than leaving it to be discovered.

Same rule for the burn equivalent: whatever an unwritable sector is, it still
advances `done`.

### 2. Status map on the WRITE path — `[P4]`, below item 1 by their own ranking

`accudisc.h:1218-1219` has said "passes it to a read (later: write) request"
since the map was introduced. 8trax asked explicitly (§b.3.2), which is what
turns it from a parenthetical into a task.

Their justification is better than the header's, which never gave one: **a rip
failure costs time and is repeatable; a burn failure costs a physical disc and is
not undoable.** That is where a user wants to watch it happen rather than read a
percentage. Symmetric Rip/Burn tabs drawing one widget from one byte layout is
the secondary benefit.

### 3. Documentation defects this exposed — `[P4]`, cheap

- **The priority chain is not in the header — DONE 2026-08-08**, landed beside
  `ACCUDISC_MAP_STATE`. `engine.c:584-585` classifies
  `hard > suspect > recovered > C2 > ok`, one byte per sector, so **a higher
  state masks a lower one that also applies**. Reachable case: `recov[s]` is set
  by boundary-overlap consensus (`engine.c:498-517`) *before* `bits[]` is
  computed (`engine.c:520-523`), and `RECOVERED` outranks `C2`, so a
  consensus-recovered sector whose winning copy still has C2 fired displays as
  `RECOVERED` with the C2 invisible. Consequence for any consumer: **counting
  `C2` cells in the map is not the count of C2-flagged sectors** — that is
  `stats.sectors_flagged` (`engine.c:621-622`), accounted unconditionally. Map
  for the picture, stats for the numbers.

  Written up when a **second** consumer reached it from the other direction:
  cdda2img (§159.2) checked whether the masking could affect them, concluded
  correctly that it cannot at their defaults, but enumerated only **two** of the
  three levers — `c2_retries` (`engine.c:527`) and `verify_passes >= 2`
  (`engine.c:551`). The third is `overlap_sectors`, which reaches `recov[s]`
  through `ext` (`engine.c:479`) and `prev_ext_n` with no dependence on the
  other two. Their conclusion survived only because *all three* are zero at
  `ACCUDISC_READ_REQ_INIT`. A consumer enabling overlap alone — a plausible
  profile — gets `RECOVERED` and the masking with it, and the two-lever account
  would not have predicted that. The header now names all three.
- **`RECOVERED` vs `OK` is not an ordering**, and the header does not say so.
  About the *bytes*, `RECOVERED` has strictly more evidence (multiple agreeing
  reads, or a C2-clean copy found — `engine.c:495-508` sets it only when C2 went
  to **zero**). About the *medium* it is worse: a problem was observed there.
  Neither is verification — a deterministic miscorrection passes both, and
  `RECOVERY.md §12.4` has the measured instance where recovery drove C2 to 0–1
  and AccurateRip v2 + CTDB still failed. Guidance given: give `RECOVERED` its
  own hue off the `OK`→bad ramp; severity there is *effort* (extra reads taken),
  and it is the state most likely to differ on a re-rip.
- **Severity is not comparable across states.** Larger is worse within each, and
  that is deliberate so one saturation ramp works — but severity 7 under `C2`
  (~128 fired bits) and under `RECOVERED` (7 extra reads) are unrelated
  quantities. Never sum severity across states.

### 4. The Rust binding is OURS — Keith's ruling, 2026-08-01 — `[P4]`

*"We will build the bindings, and coordinate with the other agents to service
their needs."* 8trax offered to write `accudisc-sys` + the safe wrapper from
their side (their §2.1.2); **declined**, and they have been told not to
scaffold it. Not before gold, like everything else here.

"Service their needs" is the operative half: the wrapper gets designed against
8trax's real call sequence, not against the header's full surface. Four things
to get from them before writing it — asked, with no reply expected before gold:

1. Which calls they actually make, and in what order.
2. Where the subprocess boundary sits. Per their §b.2 they are subprocess-first
   *permanently* for anything privileged (burn, vendor features) because of the
   `CAP_SYS_RAWIO`-binds-to-the-inode point — so a wrapper covering the whole
   API is partly dead weight, and knowing which part matters.
3. What the status map should look like in Rust — raw `&[u8]`, a typed cell
   iterator, or per-state counts without walking it. They redraw at 60 Hz.
4. Whether the sink callback survives contact with FLTK. They confirmed the
   blocking-call-on-a-worker shape is right, but the sink runs *inside* that
   call, and a Rust closure crossing FFI has constraints the C header cannot
   anticipate.

`bindings/rust/README.md` still says "scaffolded once the C API surface
stabilizes"; that condition is now met (see item 1's stability note), and the
blocker is schedule, not readiness.

### 5. Answered, no work — recorded so it is not re-litigated

- **Threading**: blocking `accudisc_read_cdda` on a worker thread is the shape
  8trax wants (FLTK is single-UI-thread; a callback/reactor API would force them
  to marshal every callback onto the UI thread anyway). The status map is a
  particularly good fit *because* it sidesteps marshalling — worker writes bytes,
  UI reads on a redraw timer, nothing crosses the boundary. **Do not "improve"
  this into a callback.**
- **`CAP_SYS_RAWIO` decided their architecture.** It binds to the executable's
  inode, so subprocessing the CLI carries it for free, while linking
  `libaccudisc` would need `setcap` on the **GUI binary itself**. They will not
  ship a setcap'd desktop GUI, so their plan is subprocess-first *permanently*
  for anything privileged (burn, vendor features), library for the unprivileged
  read/probe/parse work. Worth remembering: for a GUI consumer the machine
  interface is not a fallback, it is the primary path for half the product.

## Consumer requests — cdda2img, recorded 2026-08-07

### 1. `subq_map` — a per-sector Q-health lane beside `status_map` — DONE 2026-08-08

**Keith ruled yes on 2026-08-08 and it shipped the same morning**, on every
surface: `accudisc_read_req.subq_map` (56 → 64 bytes, appended last so it is
additive — API_PLAN §8 row 9), the five `ACCUDISC_SUBQ_*` states, version
0.4.0 → 0.5.0, CLI `--subq-map-file`, and Python `read(subq_map=True)` /
`ReadResult.subq_map` / `SubQState` / `subq_state()`.

Verified on the PX-716A with an 11-track audio disc carrying an MCN (which is
what made the acceptance precondition below satisfiable): 3000 sectors gave
2938 OK, 32 BAD, **30 NO_POSITION (1.00%)**, and a separate read of 16
unreadable sectors gave 16 NO_AUDIO. Two things that could not be argued from
the code alone —

- `OK + NO_POSITION == 2968` matched the CLI's independently accumulated
  `subq_ok` exactly. Two code paths, same answer.
- The status lane was **all OK** across those 3000 sectors while the Q lane
  carried 32 CRC failures, which is the independence claim measured rather
  than asserted.
- The counterfactual, also measured: CRC-ing the delivered subchannel for the
  16 unreadable sectors yields **16 CRC failures** — the fabricated damage a
  DIY lane would have recorded, against 16 `NO_AUDIO` from the engine.

Kept below: the reasoning, because the *why* is the part that has to survive,
and the acceptance precondition, because it still binds on any future test.

**One thing this exposed, and it was worse than the feature was valuable.**
`CMAKE_BUILD_TYPE=Release` puts `-DNDEBUG` on the test targets, which compiles
every `assert()` in the suite to nothing. Measured 2026-08-08: `assert(0)` on
the first line of `test_map`'s `main()` exited 0. Most of the suite had been
passing without checking anything, and the green result is what hid it — a
skipped test is loud, a test that runs and passes vacuously is not. Fixed with
`-UNDEBUG` per test target plus `tests/test_assert.c`, which fails if the
mechanism ever goes inert again. With asserts live for the first time, 41/41
still passed, so nothing real had been hiding underneath.

<details><summary>The original case for the feature (kept — the reasoning is the durable part)</summary>

cdda2img §148 (answered in `2026-08-07a/b/c`). They are building the rip progress
bar as a live disc map, and after reading the binding they withdrew two requests
before sending — `status_map` and `read_span(**kwargs)` already gave them the C2
lane and the frontier. **The one thing missing is the Q lane**, and this is the
whole ask: a second `count`-byte array requested by `subq_map=True`, same
allocation, lifetime and live-read semantics as `status_map`.

**Nothing has been implemented.** The three answers they asked for needed no
code; a new field on `accudisc_read_req` is scope, and scope is Keith's call.
They are shipping the C2 lane meanwhile with the Q lane dark (§148.6), so
nothing is blocked either way.

**Why it belongs here rather than in their loop — the argument is correctness,
not cost.** Their DIY path would read `_split_streams` and CRC every sector's Q.
Hard-unreadable sectors are delivered **zero-filled** (`accudisc.h:1371-1374`),
and a zero-filled Q frame **fails** CRC-16 (measured 2026-08-07: `rc == -9`,
`ACCUDISC_ERR_CRC`, for both all-zero and all-ones fills). So a DIY lane paints
fabricated subchannel damage on exactly the sectors whose audio is already gone
— corroborating the real failure beside it, so it reads as confirmation rather
than as a bug. The engine does not have this because it `continue`s at
`engine.c:558-561` before the Q check; the only way to know that is to read the
engine. They confirmed they would have shipped it (§149.1).

The marginal cost in the engine is **not** the CRC — that already runs
unconditionally for every SUB_RAW read at `engine.c:569-580`. The only new work
is one relaxed atomic byte store per sector through the existing `map_store`
(`engine.c:124-128`). Already paid for.

**ABI: additive, no soname bump.** `accudisc_read_req` carries its own `.size`
and the IN rule (`accudisc.h:1289-1294`) treats a shorter caller's tail as zero.
`tests/test_abi.c:49` pins the current 56 bytes and would need updating with it.

**The five states, agreed by both sides** (state in the low nibble, mirroring
`ACCUDISC_MAP_STATE`/`SEVERITY`):

```
0x0  SUBQ_PENDING      not yet attempted (byte untouched)
0x1  SUBQ_OK           CRC-16 verified, ADR=1 position frame
0x2  SUBQ_BAD          CRC-16 failed
0x3  SUBQ_NO_POSITION  CRC verified, ADR != 1 (MCN / ISRC)
0x4  SUBQ_NO_AUDIO     sector was hard-unreadable; no frame delivered
```

`SUBQ_NO_POSITION` is the state neither side would have designed unprompted, and
it is **load-bearing, not a nicety**. Measured 0.98% of frames (1,590 of
162,892) are CRC-good non-position frames — MCN/ISRC, legitimately interleaved,
which `index_map.c:84-85,124-125` already skips as such. Under their worst-wins
aggregation over ~7,000-sector cells that is **every cell flagged on a perfect
disc** (§149.2). A per-frame rate that looks negligible is total after
aggregation. Note the two consumers read it with opposite polarity: healthy for
a health lane, signal for `subq_toc`, which is the argument for it being a state
rather than a boolean.

**The rate varies by DISC, 0% to ~1%, and that makes it more necessary rather
than less** (their §150.4b: an entire pressing with no MCN and no ISRCs, 0.00%
across all fifteen captures). A state whose necessity depends on the disc is the
dangerous kind — validate the Q lane on that disc alone and you prove the state
optional. **So any acceptance test for `subq_map` must name a disc carrying MCN
or ISRCs as a precondition**; otherwise `SUBQ_NO_POSITION` is unreachable and
the arm passes without testing anything.

Two design calls, both agreed (§149.4): **refuse** `subq_map` without
`ACCUDISC_SUB_RAW` (`ACCUDISC_ERR_INVAL`) rather than return a uniform map a
renderer will draw as a lane; and the **severity nibble stays zero**, because Q
integrity is one CRC-16 and anything else there would be a proxy.

A third emerged during implementation and neither side had spotted it:
**`crc_ok` must be consulted before `adr`.** `accudisc_q_parse` fills `adr` from
`q[0]` whether or not the CRC verified (`src/cdda/subq.c:52-53` — it must, that
byte is the frame-type header), so a corrupt frame routinely presents ADR=2 or
3. Classify on `adr` first and such a frame is painted `NO_POSITION`, i.e.
reported as **healthy**, on exactly the frames the lane exists to find. Nothing
downstream could catch it: the byte is well-formed and names a real state.
`adsc_subq_byte()` exists as a named function purely so the order is testable,
and `tests/test_map.c` asserts the CRC really failed *and* `adr` really is still
2 before checking the verdict — without those two the case would pass while
testing nothing.

</details>

### 2. Q lag — MEASURED, no lag on this drive; `tools/qlag.c` shipped

Their question 3 was whether Q lags the audio, which would make an LBA-indexed
map wrong by *k* everywhere. Settled for this drive and left open for the fleet.

**Structural half (certain):** an ADR=1 frame is self-locating, so lag is
invisible if you index by the frame's own address — but `accudisc_q_parse` leaves
every position field zero on CRC failure by deliberate design
(`accudisc.h:1481-1485`), so a CRC-**bad** frame can only be placed by transfer
slot. Lag is irrelevant for the frames you can locate and decisive for the ones
you cannot, which are exactly the frames the lane exists to draw.

**Measured half:** `tools/qlag.c` (new, public-header-only, device-free) over a
whole-disc raw-sub capture on the PX-716A: **157,871 of 157,914 position frames
at delta 0** — no lag, and not a near-zero average. The 43 exceptions are six
short contiguous runs whose deltas are all exact multiples of 512 sectors (a
buffer number, not a disc number). **No mechanism claimed.** What it does prove
is that a frame can pass CRC-16 and still be positionally wrong, at ~0.03%.

Falsified before it was trusted: `LAG +3` on a shifted capture, `SPREAD` (41
deltas) on a jittered one, refusal on a wrong stride, and a base-mismatch warning
on a partial capture. **The SPREAD threshold is 8** (`SPREAD_MAX`), not the
64-entry histogram capacity — `2026-08-07c` corrects that after we sent the wrong
number.

**CLOSED at scale by cdda2img** (their §150.2 and §151 — the per-capture numbers
live in the correspondence itself; their sweep file is under *their* `private/`,
machine-local and in no clone, so it is deliberately not cited as a path):
42 captures, 3 discs, 4×/8×/24×/32×/40×, three passes each —
**NO LAG on every one**, nothing near the threshold. The result we could not have
obtained: two independent Q-collapse events (the vendor speed cliff, and one
degraded pass among identical siblings) drop CRC-good to **47.79%** and
**38.73%** while in-slot stays at **99.988%** and **99.978%**. A Q yield below
half does not disturb slot alignment — damage removes frames without moving the
ones that remain. That rules out the plausible failure (whatever destroys Q CRC
also disturbs delivery timing, so survivors drift exactly when the lane most
needs to be right) and it could not have been argued from first principles.

They also rebuilt the shifted arm independently, and the detail that fell out of
it is worth keeping: the minority deltas **tracked the shift** (`-2048 → -2045`),
which makes the buffer-aligned anomalies positional facts about the capture
rather than artefacts of the measurement.

One defect their sweep exposed, since fixed: `nonpos` was printed only as a
percentage of *all* frames, which moves when Q yield moves — so comparing
captures across speeds suggests the interleave thins under load. It does not;
normalised against CRC-good it is flat to three digits across a 2× change in
yield. Both denominators now print.

If a nonzero lag ever appears the unit is **whole frames** (one Q frame per
sector, not sample pairs) and the sign convention is
`accudisc_probe_c2_lag`'s — positive = the companion stream trails the audio.

## Outstanding — carried from 2026-07-26 (phase 3 landed; these did not)

### 0. RECOVERED sectors were returned WRONG, 9/9 — **DROPPED 2026-08-08 by Keith. Do not chase.**

**Ruling: "If there really is a bug, let it present itself, then we can deal
with it."** Not closed as fixed and not claimed to be a non-bug — deliberately
abandoned as an investigation, with the evidence kept below so a recurrence is
recognisable rather than re-derived.

**The provenance, which is the whole reason it was droppable.** One run, one
disc, one day: Tracy Chapman, five whole-disc reads 2026-07-26 with the **full
recovery ladder** — `--retries 3 --c2-retries 4 --verify 1 --overlap 2 --ladder
40,32,24,8,4`. Never witnessed again.

**It is unreachable on any default read.** The surviving hypothesis after the
discriminator run (their §90.2) is **H1-local**: the ordinary publish loop is
correct — proven by byte 0 and 162,882 correct `OK` bytes — and only the
*recovery* path records at a different index. Every one of the ten anomalous
bytes was state 4 `RECOVERED`; not one was `OK`, `C2`, `HARD` or `SUSPECT`. With
`retries`/`c2_retries`/`verify_passes`/`overlap_sectors` at their
`ACCUDISC_READ_REQ_INIT` zeros, `RECOVERED` cannot occur at all (the three levers
are named at `ACCUDISC_MAP_STATE`), so no default read can hit this.

**Honest limit on "never seen again":** the disc has been read dozens of times
since, but not once with those recovery flags. The absence is unrepeated
conditions, not a repeated negative. That cuts both ways and is exactly why
"let it present itself" is the cheap policy rather than the risky one.

**Keith's AccurateRip point, checked:** `RECOVERY.md` §12.3 records AR v2 ✅ at
40/32/8× and ❌ at 24/4× *in that same run*, so the gate did fire and was noticed.
An undetected silent corruption is not what this was.

**If it ever recurs, the one test worth running** (cdda2img §90.3, needs no drive
time): find a capture where a sector went `RECOVERED` **and** its delivered bytes
were correct. Under H1-local the flag sits at +1 from a *correct* sector, so the
sector it names would be clean. Their data could not supply that case; all nine
of theirs came back wrong.

<details><summary>Original entry, kept for recognisability (superseded)</summary>

#### RECOVERED sectors were returned WRONG, 9/9 — cdda2img §89.5

**The most serious open claim against the read engine.** Keith ran five
whole-disc reads of Tracy at 40/32/24/8/4 (`--retries 3 --c2-retries 4
--verify 1 --overlap 2 --ladder 40,32,24,8,4 --driver plextor`, each with
`--pcm --c2f --sub raw --map-file`), diffed against a CTDB-repaired reference
that then verified 11/11 AccurateRip at confidence 200. Result: **nine corrupt
sectors, ten flagged map bytes, every flag state 4 = RECOVERED, and every flag
sits at exactly corrupt_LBA + 1** — never 0, −1, or +2. At documented indexing
recall is 0/9; shifted one, 9/9 with precision 9/10. All five `.c2f` files are
47,890,248 bytes of **zeros**, so C2 was silent throughout.

Mechanisms, none excluded yet:

- **H1 — storage off by one.** Byte for sector *N* written at *N*+1.
- **H2 — indexing correct, RECOVERED is simply false.** *N*+1 really was
  recovered and *N* was mis-delivered carrying no flag. Requires the recovered
  neighbour to land at +1 nine times running; cdda2img calls that a stretch.
- **H3 — units mismatch at the boundary.** We never apply the read offset
  (`cli/main.c:679`), so our PCM and map are both raw drive-space. If their
  artefact was offset-corrected for CTDB, "corrupt LBA" and "flagged LBA" are
  different coordinate systems. Does not obviously give a clean +1 nine times.

**AUDITED 2026-07-26 — no index error found anywhere in the recovery path.**
Every site checked and correct as written: the classify/publish loop
(`engine.c:554-556`, `idx = cur - req->lba`), the map file
(`cli/main.c:1376,1382,1396` — `ftruncate(count)` / `mmap(count)`, no header or
padding), `read_span`'s per-sector fallback (`engine.c:243-251`, `sec` and `cur`
both derived from the same `s`), `read_sector` (`engine.c:211-215`, a thin
wrapper with no arithmetic), both consensus write-backs (`engine.c:288, 321`),
the seam stash (`engine.c:623-627`) and the chunk advance (`engine.c:629`,
`lba += n`, so the extension is genuinely re-read rather than skipped).

**Still unexamined, and now the only place a whole-sector displacement could
originate:** `adsc_mmc_read_cd` and the SG_IO layer under it. Start there.

**New evidence 2026-07-27 (cdda2img §102.2, then measured properly in §103), and
it sharpens the claim rather than weakening it.** Their A/B control read the
damaged region (LBA 112500 +700) twice through the *same* transport and got **36
differing sectors**; a follow-up four-reads-per-condition run confirmed
fixed-speed determinism is false (3/4 pinned reads distinct, 1–4 sectors apart —
full table and its unseparated confound under "the pit is readable" above).
So same-speed reads on this disc are not generally stable —
which is what makes the 9 corrupt sectors a different phenomenon rather than
more of the same. Where the disc is merely jittery the reads disagree and a
verify pass would flag SUSPECT; the 9 were **stably wrong**, agreed with
themselves, and took the `diff == 0` "confirmed" early-out. The unreachable-
ladder root cause is unaffected — a disagreement is exactly what would have
reached `consensus()` — but "the drive misreads the same way every time" is now
known to be true *of those sectors*, not of the drive.

It also closes off an instrument: byte-level differential comparison cannot
adjudicate anything in a damaged region, because nothing there agrees with
itself. Absolute gates only — the RECOVERY.md invariant, reconfirmed from a new
direction.

**Excluded by measurement, not by argument:**

- *Chunk geometry, the whole family.* A seam-misattribution mechanism was
  constructed and refuted: `sector_len` 2742 → `65535/2742` = 23, capped at 32,
  minus overlap 2 → **chunk 21**, and the corrupt LBAs scatter across residues
  0, 2, 9, 10, 13, 14, 16 mod 21. No C in 2..32 puts them at one residue. The
  adjacent pair 113056/113057 rules it out structurally too. Anything tied to
  chunk position — the seam check, `prev_ext`, the overlap extension — predicts a
  signature that is absent.
- *C2 lag.* 2 sample pairs = 8 bytes (`accudisc.h:987`) against a 2352-byte
  displacement, and it is **report-only by contract** (`accudisc.h:1004`), not an
  omission. Do not re-chase this.
- *Straddle H2* (one defect across the N/N+1 seam, C2 firing above and CIRC
  miscorrecting below) failed the in-sector-position test — 8 of 9 end 582–1776
  bytes short of the boundary. **But the test is weak** (cdda2img §92.2, conceded
  in §bk): CIRC interleave delocalizes a defect across ~100 frames, so byte
  offset is a poor proxy for physical position. Do not bank this negative. The
  sharp version needs the F1/F2 frame index, which neither side has.

**INDEPENDENT FINDING, outlives the whole +1 question — and the ROOT CAUSE of the
data loss, even though it does not explain the +1.**

*Corrected 2026-07-26 by cdda2img §93, which retracted their own §92.3; the
earlier "reproducible across a 10x speed range" reading in this entry was wrong
and is replaced.* The error is stable **per speed**, not per defect:

| LBA | 40x | 32x | 24x | 8x | 4x |
|---|---|---|---|---|---|
| 112612 / 112737 / 112751 | OK | OK | OK | OK | **WRONG** |
| 112765 | OK | OK | OK | **WRONG** | **WRONG** |
| 113043 | **WRONG** | **WRONG** | OK | OK | OK |
| 113056 | OK | OK | **WRONG** | OK | OK |
| 113057 | OK | OK | OK | **WRONG** | OK |

**No sector is wrong at all five speeds**, the failing speeds differ per sector,
and "slower is better" is false at sector granularity. (Caveat: `disc40.pcm` and
`disc32.pcm` are byte-identical disc-wide, so those two requests likely landed on
one rung — the quantised-rung problem. The only clean two-speed case is 112765.)
A plain per-sector **majority vote across the five captures reconstructs all seven
sectors correctly** — no parity, no CTDB, no AccurateRip.

**So the pit is readable and this drive can get the bits.** The failure is
entirely recovery *policy*. `consensus()` is blind because **it largely resamples
the same function**: at a fixed speed the drive mostly returns the same wrong
bytes, so agreement is cheap and carries little information.

> **Weakened 2026-07-27 by measurement (cdda2img §103, retracting their own
> §102.2 gloss).** This paragraph used to say agreement at a fixed speed was
> *guaranteed* and the resampled function *deterministic*. Measured on the
> damaged span (LBA 112500 +700, four reads per condition, PX-716A):
>
> | condition | distinct results | pairwise sectors differing |
> |---|---|---|
> | unpinned | **4/4** | 31, 32, 35, 35, 42, 43 |
> | pinned `--speed 8` | **3/4** | 1, 1, 3, 3, 4 (one identical pair) |
>
> So: **speed is the dominant variable** — pinning drops divergence by roughly an
> order of magnitude, which supports the directional part of §93/§94 from a
> second angle — but **fixed-speed determinism is FALSE**. Repetition at one rung
> has a small non-zero yield, so "more retries only deliver the wrong answer with
> more confidence" was too strong.
>
> **Do not lift that table without its confound**, which cdda2img stated before
> anyone could: a slower rung produces fewer wrong sectors to begin with, so
> "fewer differ at 8x" is what you would see even if erroring sectors were
> equally random at both speeds. Divergence and error rate are not separable from
> these two conditions alone — it needs an AccurateRip/CTDB reference to express
> divergence as a fraction of sectors *wrong at all*. Not done, not claimed.
>
> **None of this moves the root cause.** The ladder is unreachable for structural
> reasons (below), not because rereads are information-free — and the nine
> corrupt sectors still agreed with themselves and took the `diff == 0` early-out,
> which now reads as a sharper anomaly rather than a vaguer one, since same-speed
> reads on this disc are *not* generally stable.

**Why the `--ladder` did not save it (answered from source, §bl.1).** Speed varies
only inside the per-sector rereads — `ladder_speed` is called at exactly two
sites, `engine.c:280` (`c2_rescue`) and `engine.c:311` (`consensus`). Every
whole-span read is preceded by `ladder_restore` (`engine.c:461` primary,
`engine.c:520` every verify pass), which sets `req->speed_x`
(`engine.c:191-197`). For a silently-wrong sector: no C2 fires so `c2_rescue` is
skipped; the verify pass re-reads at the *same* speed, gets identical bytes,
`diff == 0`, and takes the "confirmed" `continue` at `engine.c:531-532`.
**`consensus()` is never called — the ladder is unreachable.** The one mechanism
that would catch this is gated behind the symptom it removes.

**The comment at `engine.c:510-518` is itself a defect.** It claims "speed
diversity against persistent same-speed misreads (RECOVERY_STRATEGY R6) comes from
the consensus/rescue rereads". False for this failure class — those rereads are
unreachable when the misread is silent; the claim holds only for sectors that
already announced themselves. Its *other* claim, that whole-range speed-diverse
sweeps are the caller's layer, is **confirmed** by the 7/7 majority vote. The
split of responsibilities is right; the coverage claim inside it is wrong, and it
currently tells the next reader the hole is covered.

This is RECOVERY.md's invariant demonstrated: relative checks must never outrank
absolute gates, and the absolute gate is the caller's layer.

**`ACCUDISC_MAP_OK` is the worse lie, ahead of `RECOVERED`.** A sector confirmed by
N same-speed passes is marked identically to one that was right first time — every
one of the seven corrupt sectors is `OK` in some capture. Fix candidates, in
order: correct the `510-518` comment; make `MAP_OK` distinguish "confirmed by
same-speed passes" from "clean"; consider **one final verify pass at a different
ladder rung** (per-chunk speed switching is rejected by the recalibration-thrash
objection in that same comment, which is real and presumably measured).

Cross-check from their side: `sector-hammer` (repeat at one setting, max retries)
scored 2/20 while the speed-varying ladder scored 19/20. §93.2 supplies the
mechanism that ranking was missing — repetition at a fixed speed samples a
constant.

**Candidate mechanism for H2, with a known hole.** Sector N holds a *stable*
concealed error → the verify pass sees `diff == 0`, takes the "confirmed"
`continue` at `engine.c:531-532`, and writes `MAP_OK`. Sector N+1 holds a
*marginal* defect from the same damage region → unstable → `consensus()` fires →
`RECOVERED`. One damage region, two sides of the stability threshold, no
off-by-one required. **Hole: it does not explain why the stable sector is always
the lower-numbered one** (9/9 at +1, never −1). CIRC delay-line directionality is
the place to look; it is not an explanation we have. H1-local stays open beside
it.

**The second-drive test is SETTLED and withdrawn** — it was going to decide
whether the pit is readable, and cdda2img's own five captures already answer it:
this drive reads 113043 correctly at 24x, 8x and 4x. A second drive would only
confirm generality. Not worth the time.

**`MAP_OK` on the corrupt sector CONFIRMED 9/9** (cdda2img §94.1): every one of
the nine reads `0x01` at the corrupt LBA and RECOVERED at LBA+1. Not one carries
C2, HARD or SUSPECT. The actual errors are all wearing the state that announces
nothing.

**The +1 adjacency is not small-numbers coincidence** (§94.2): 10 RECOVERED bytes
in 814,460 map bytes — base rate 1.2e-5 — with nine of them at exactly corrupt+1,
and per-capture counts tracking (1/1 at 40x and 32x, 2/2 at 8x, 4/4 at 4x, 1/2 at
24x). **Acceptance test for any eventual explanation: one event, one sector apart,
direction never varies.**

</details>

### 0b. Recovery rereads use the read mode c2lag.c measured as DIFFERENT on this drive — `[P2]`

> **STAYS OPEN — item 0 being dropped does NOT drop this.** Reviewed
> 2026-08-09, because the numbering invites exactly that mistake. Item 0 was
> dropped as a possibly-ephemeral observation from a single run of a single
> disc. **0b is not an observation at all** — it is a design inconsistency
> visible by reading two modules against each other, and the measurement it
> rests on is `c2lag.c`'s own live one on this drive (a streaming pass flagged
> ~40 sectors where per-sector rereads of the same LBAs flagged zero). It would
> still be true if item 0 had never been reported. **Not a read-speed item
> either** — it is about read *mode*, so it does not belong to that session and
> must not be abandoned with it.

Found reading two modules against each other; independent of the +1 and of the
speed result. `consensus()` and `c2_rescue()` both do `cache_defeat(r, lba)` then
`read_sector(r, lba, ...)` (`engine.c:281-282`, `312-313`) — an **isolated
single-sector read immediately after a 5000-sector seek**. Meanwhile
`src/drive/c2lag.c:21-27` states, from live measurement on this same PX-716A:

> Rereads are STREAMING WINDOW reads, not isolated single-sector reads: marginal
> defects fire C2 while the drive streams and decode cleanly on a careful
> post-seek single-sector read (verified live on the PX-716A — **a streaming pass
> flagged ~40 sectors where per-sector rereads of the same LBAs flagged zero**).

c2lag uses `C2LAG_RUNUP` 16 sectors of lead-in specifically to be streaming before
it crosses the damage. So one module designed around a measured mode difference
and the recovery path uses the mode it designed around: **delivery is a streaming
span read, recovery is an isolated post-seek single-sector read**, and we have our
own live evidence on this drive that the two disagree.

This weakens `consensus()` a second way, on top of the speed issue: it is not only
resampling the same speed, it is resampling in the *quieter mode* — the one that
tends not to reproduce the defect. The condition it adjudicates ("did the
streaming read get this sector right?") is not the condition it measures. Fix
direction: give the recovery rereads a streaming run-up, as c2lag already does.

**Still unexplained, and do not let any of the above close it:** the +1 asymmetry.
Nine of nine flags at N+1, never N−1. Neither the speed-stability result nor 0b
touches the direction. One speculative lead, recorded as speculation only: CIRC
delay lines spread symbol errors along the direction of travel, so the leading
sector of an encounter may stay within correction capacity and be silently
miscorrected while the following one exceeds it and announces itself. **No
measurement behind that** — testing it needs the F1/F2 frame coordinate, which
neither side has.

Discriminator requested in §bh.2, costs nothing: a uniform +1 shift never writes
index 0, so **byte 0 of their five existing map files** decides it — `0x01` (OK)
excludes whole-array H1, `0x00` (PENDING) on a capture that read sector 0 fine
confirms it. Byte `count-1` is the same test at the far end.

Independent of the +1, the semantic defect is real and ours: `consensus()`
(`engine.c:317-323`) returns 1 on the first byte-for-byte agreement between any
two reads. **That is a stability test, not a correctness test** — two reads can
agree on the same wrong bytes.

**Scope of the doc fix, corrected 2026-07-26 — it is NARROWER than first
recorded.** The block contract at `accudisc.h:1061-1065` already says the right
thing and always did: "Every state below is a RELATIVE claim … **A drive that
misreads deterministically passes every relative check**; absolute gates
(AccurateRip, CTDB) are the calling application's job and always outrank anything
recorded here." That is cdda2img's entire finding, written down before they
measured it. So the contract does **not** need rewriting — only the per-state
one-liner for `ACCUDISC_MAP_RECOVERED` ("problem seen, clean/agreeing copy won"),
which reads as fidelity and contradicts the block six lines above it.

The real gap was never documentation: `consensus()` implements precisely the thing
the block comment tells consumers not to trust, and we labelled its output as
though the comment did not exist.

Next experiment (needs the drive, Tracy loaded, Keith's call): re-read one of the
nine LBAs and compare delivered bytes against the CTDB reference. Clean ⇒ the
consensus is unstable rather than wrong, a different defect with a different fix.

**Also fixed en route:** §89.6 asserted our span-finder shares
`MAP_NEEDS_RECOVERY = {0x2,0x3,0x5}` (0x4 excluded). **We have no span-finder** —
`recovery_bench.py` is in their tree. Our only map consumer is the CLI glyph
table (`cli/main.c:1049-1054`), where RECOVERED already ranks above OK and
renders `r`. Corrected in §bh.4.

Severity is **not comparable across states** and this should be documented:
C2 = `log2(fired bits)` (`engine.c:50`), SUSPECT = `log2(differing bytes)`
(`engine.c:62`), RECOVERED = raw **attempt count** (`engine.c:57`). That last one
explains §89.5's "unexplained" asymmetry — 40x sev 1 vs 32x sev 3 on the same LBA
with byte-identical PCM is expected, not anomalous.

### 1. Remove all SpeedRead guards — **DONE 2026-08-09, version 0.6.0**

**Keith ruled option (b): remove `allow_unsafe` entirely**, not the reserved-byte
compromise. Done, with everything the inventory below listed, plus four sites it
did not: `__all__` and the `_ERRORS` map in the Python binding, the binding's
`pyproject.toml` version, and the man page's error table.

Ruling verbatim, and it settles the mechanism as well as the scope: *"SpeedRead
cannot possibly ever be 'unsafe', as the hardware is physically incapable of
reading CDDA at speeds >40x, and the governor completely ignores the SpeedRead
setting for CDDA. The Q channel corruption you measured was at 40x, not 48x. The
page2a reading shows the request, not the governor controlled throughput."* Plus
a standing instruction: **no further speed tests** — cdda2img has since measured
Q degradation as a property of the disc rather than the speed, and "higher speeds
= more Q misreads" is already common knowledge.

What went, in full: the library refusal (`src/read/engine.c`),
`accudisc_read_req.allow_unsafe`, `ACCUDISC_ERR_UNSAFE_COMBINATION` and its
`strerror` case, the CLI's `--uncap`+`--sub` interlock and its `LIKELY_ON`
pre-read warning, the guard-specific assertions in `tests/test_uncap.c` and
`tests/test_abi.c`, and the Python `UnsafeCombination` class plus the
`allow_unsafe=` keyword.

What deliberately stayed: `accudisc_speed_uncap_probe/set/get/push/pop`, the
`speed-uncap` subcommand and `--uncap`. **Reporting a drive's configuration is
not the same as refusing to work in it**, and the standing goal is 100 % Plextor
feature coverage. (The stock-ceiling table and `adsc_uncap_classify` were also
kept at this point, and removed a few hours later in 0.8.0 — see task 5. The
distinction that survived both rulings: *reporting what a driver tells us* is
legitimate; *inferring it from an advertised speed* was not.)

**Two things this produced that outlive the task.** `-11` is **retired and must
never be reused** — a consumer built before 0.6.0 maps it to "unsafe
combination", so reassigning it would make that consumer report the wrong
failure for the right return value. `tests/test_uncap.c` now pins the
retirement via `accudisc_strerror(-11) == "unknown error"`, and
`test_binding.py` pins it from the Python side. And `sizeof(accudisc_read_req)`
stayed 64 because the byte was padding — verified by compiling, not assumed —
which made this removal free **by luck of layout, not by the `size` rule**. The
IN rule protects a caller that is SHORT; it says nothing about one that sets a
field this build no longer has. Recorded in API_PLAN §8 row 10 so the next
subtractive change does not generalise from this one.

<details><summary>The task as it stood before it was done (kept: the ABI analysis is the durable part)</summary>

**Re-affirmed 2026-08-08**, with the reasoning stated more sharply than the
original ruling: *"SpeedRead is for CD/DVD-ROM only. Having a guard against
something the hardware is physically incapable of doing doesn't make any
sense."* The guard defends against a throughput increase the drive cannot
produce on CD-DA.

**CORRECTED 2026-08-09 — this paragraph used to say the task "belongs to the
single dedicated read-speed session … anything unfinished when that session ends
is permanently abandoned", and that was wrong in a way that could have destroyed
the task.** It is **DESK WORK**: source-only, no drive, no measurement — see the
banner at the top of this file, which lists it as such. Nothing about it consumes
session time, so nothing about it should be exposed to the session's abandonment
rule. And it is a **standing directive from Keith**, given 2026-07-26 and
re-affirmed 2026-08-08; a scheduling note is not licence to let a directive
lapse.

What is true is that it is **gated**, which is a different thing from scheduled:
it needs the ABI ruling below before anyone writes code. A gate waits for an
answer; a schedule waits for a clock. This one waits for an answer.

**One thing to weigh in that session, because it is the only real cost.**
Removing `allow_unsafe` from `accudisc_read_req` is the first *subtractive* ABI
change we have made — every other has been additive. The `.size` IN rule
(`accudisc.h`) cannot help: a 0.5 caller setting `allow_unsafe = 1` would have
that byte silently reinterpreted as whatever occupies the offset next, which is
the "well-formed data, wrong referent" failure the size field exists to prevent.
Two ways out, and the choice is a design decision rather than a detail:
**(a)** keep the field as a reserved, ignored byte and delete only the
enforcement — zero ABI cost, honours "remove enforcement only" literally; or
**(b)** remove it and bump the soname. (a) is the cheaper reading of the ruling
and does not foreclose (b).

**Original ruling 2026-07-26: "Remove all guards for SpeedRead. CDDA is
completely unaffected by it. Those guards have no reason to exist."**
Manual-backed — `private/drives/Plextor/Plextor-716.pdf` **p.15** publishes three
ceilings for this model by media class: DATA (CD-ROM/CD-RW/CD-R) 48×, **CD-DA
and CD-R audio 40×**, CD-RW audio 32×. The 48× the uncap exposes into page 2A is
a *data-media* figure, so the uncap cannot make an audio disc spin above 40× and
the throughput half of the hypothesis is closed.

Scope agreed before stopping: remove **enforcement only**. Keep
`accudisc_speed_uncap_set/get/push/pop`, the `speed-uncap` subcommand, the
`--uncap` flag and the plextor driver — a queryable state is not a guard, and the
standing goal is 100 % Plextor feature coverage.

Inventory, already built — every site:

| remove | where |
|---|---|
| the refusal block + `adsc_uncap_authoritative` call | `src/read/engine.c:370-391` |
| `allow_unsafe` field | `include/accudisc/accudisc.h:1189` (+ the §7.1 comment at :1109) |
| `ACCUDISC_ERR_UNSAFE_COMBINATION` | `include/accudisc/accudisc.h:68` |
| its `strerror` case | `src/device.c:74` |
| the `--uncap` + `--sub` interlock | `cli/main.c` (~1502 pre-edit numbering) |
| the `LIKELY_ON` pre-read warning | `cli/main.c:1444-1457` |
| guard-specific assertions | `tests/test_uncap.c:96, 226-245` |
| `allow_unsafe` references | `tests/test_abi.c:85, 109-110` |

Notes for whoever does it:

- **`allow_unsafe` sits in existing padding**, so removing it leaves
  `sizeof(accudisc_read_req) == 56` unchanged — but `tests/test_abi.c`'s
  `_Static_assert` and the short-struct case both name that field, so they need
  re-pointing at another field rather than deleting.
- It is still a **public API removal** (a field and an error code), so bump
  `ACCUDISC_VERSION_MINOR` to 3. CMake derives the `.so` version from the header.
- **cdda2img is on our live tree now** — write the API_PLAN §8 ledger row
  *before* the commit, per §8's post-relink discipline, not after.
- `drivers/plextor/FEATURES.md:18` still documents the refuted *mechanism*. Fix
  it with this change. **Keep the observation** — a real ABBA A/B, 40.6 % Q with
  SpeedRead on vs 99.2 % off, 0 % across the 10–60 % radius band — recorded as
  unexplained data with its confounds named (n=1 per arm, damaged media, taken
  before drive contention was known to produce the same signature). Removing a
  guard is not a reason to delete a measurement.

*(Three of these notes proved wrong in detail when the work was done: the size
was 64 not 56, the minor bumped to 6 not 3, and `test_abi.c`'s short-struct case
needed no re-pointing because `cancel`, `status_map` and `subq_map` already
assert the same zero-extension semantic three times over. The ledger row and the
FEATURES.md fix were both done as instructed.)*

</details>

### 2. `setcap` the INSTALLED binary, not the build-tree one — **DONE. Verified 2026-08-08.**

Keith: *"I'm pretty sure that P1.3 was closed weeks ago. Double check the
installer."* Correct — it shipped and the entry was simply never closed. Read
back today rather than recalled:

- `ACCUDISC_SETCAP_ON_INSTALL`, default **ON** (`CMakeLists.txt:74`).
- The install rule is `cli/CMakeLists.txt:28-57`, and its own comment states the
  point this entry was opened for: `install(TARGETS)` copies the binary and an
  xattr does not survive the copy, *"so this is not a duplicate of the post-link
  setcap; it is the only thing that arms what actually gets installed."*
- **DESTDIR-aware**, which the original entry did not ask for: capabilities do
  not survive tar/cpio/rsync, so a staging build skips the cap and prints the
  exact command for the package's post-install script instead of silently
  producing an unarmed package.
- Failure is `FATAL_ERROR`, not a warning — deliberate, because an unarmed
  binary fails *quietly* (the vendor path falls back to generic MMC with no
  error), so a warn-and-continue would ship something everyone believes is armed.
- `ACCUDISC_INSTALL_SETCAP_COMMAND` is bare `setcap` rather than the build-time
  `doas …` form, since `cmake --install` to a system prefix is already root.

Nothing outstanding. The build-tree `setcap` target remains for the
rebuild-disarms-it case, which is a separate convenience and not this item.

<details><summary>Original entry (superseded — the work landed 2026-07-29)</summary>

`CMakeLists.txt:34-38` already documents this and we have not been doing it. The
capability binds to the **inode**, so every rebuild that relinks the CLI drops
it. Three independent reasons now, the third new:

1. It interrupts Keith on every rebuild.
2. It silently disarms the vendor path mid-session (four occurrences).
3. **Since the 2026-07-26 relink, cdda2img executes the same inode**, so our
   rebuild drops the capability from *their* binary too.

> **The mechanism now exists** (`make install`, 2026-07-29):
> `ACCUDISC_SETCAP_ON_INSTALL` caps the installed binary, strip runs first so
> the xattr survives, and `DESTDIR` skips it with the command a package script
> must run. Verified with `getcap` on a real install.
>
> **It is enabled, not closed, and the difference is reason 3.** Nothing is
> fixed until the binaries actually invoked come from the install — ours *and*
> cdda2img's, which still executes the build-tree inode. Until that switch
> happens the rebuild keeps disarming both. **Deciding that is Keith's**, not
> something to change under either project's feet.
>
> cdda2img's §119.2 raises the severity and the argument holds: this is a
> **measurement-validity** defect, not an inconvenience. A rip with the vendor
> path disarmed is a *different configuration*, so any bench run, A/B or ladder
> probe straddling a rebuild silently compares two configurations while looking
> like one series. Their `disc_ab.py` refuses a stale extension and re-hashes
> the engine; neither check can see a dropped capability. The cost is wrong
> answers, not lost minutes.

</details>

### 3. Audit `if (!quiet)` around anything that is not progress — `[P2]`

cdda2img passes `-q` on all three read paths (`accudisc_reader.py:335`, `:439`,
`tools/recovery_bench.py:777`), and the pre-read uncap warning was gated on
`!ctx.quiet` — so the flag that makes a caller a *machine* consumer suppressed
the notice written for machine consumers. The specific warning goes away with
task 1; **the pattern is the task**. Quiet means "no human is watching", which is
the strongest claim on being told something is wrong, not the weakest. Sweep
`cli/main.c` for any other data-integrity notice behind a verbosity flag, and
prefer a machine-readable token over stderr prose — stderr wording is explicitly
not a stable interface (`cli-machine-interface.md`).

**AUDIT RUN 2026-08-09. Six `!ctx.quiet` sites in `cli/main.c`; one new finding,
and it is not the one task 1 removes.**

| site | what it gates | verdict |
|---|---|---|
| `:1118` | the human progress line | **Fine.** `--progress-fd` carries the same information on a machine channel *outside* the `quiet` test — the pattern the other sites should follow. |
| `:1489` | "tracks A-B, lba N count M" | Descriptive, not integrity. Leave. |
| `:1585` | the `LIKELY_ON` uncap warning | The known defect. Removed by task 1. |
| **`:1622`** | **"read-speed uncap: on (was off)"** | **THE FINDING — see below.** |
| `:1632` | a newline | Cosmetic. |
| `:1711` | the CD+G summary | Statistics for a human. Leave. |

**`cli/main.c:1622` is a notice that we MUTATED PERSISTENT DRIVE STATE, and
`-q` hides it.** It is a strictly worse case than the warning that prompted this
item, for two reasons. First, it reports something we *did* rather than a risk
we suspect — and the uncap survives the process, so a machine consumer passing
`-q` gets its drive silently reconfigured for every later invocation, including
other tools' (this is the [[drive-contention-flock]] failure mode arriving by a
different door). Second, **it does not go away with task 1**: that ruling removes
enforcement while explicitly keeping `speed-uncap`, `--uncap` and the queryable
state, so this line survives the change that retires `:1585`.

Fix, in the shape `:1118` already demonstrates: emit the state change on a
machine channel that `quiet` does not gate, and keep the stderr prose as the
human rendering of it. The `(was %s)` prior value is the part that matters — it
is what a caller needs to restore the drive, and the push/pop SOP depends on it.

**There is no anomaly. 48 and 40 are the same physical read wearing two labels.**
Keith ran four whole-disc reads (`--start 0 --count 162892`, uncap on, no vendor
driver) instead of either discriminator — **better than both, because a full-disc
read covers every radius identically at both settings, so the CAV term cancels
exactly rather than being modelled**:

| req | seconds | sectors/s | whole-disc avg | C2 sectors | C2 bits |
|---:|---:|---:|---:|---:|---:|
| 48 | 91.5 | 1780.3 | **23.74×** | 63 | 1160 |
| 40 | 89.8 | 1814.1 | **24.19×** | 54 | 1133 |
| 32 | 113.1 | 1439.8 | 19.20× | 1 | 12 |
| 24 | 150.2 | 1084.8 | 14.46× | 2 | 24 |

48 vs 40 differ by **0.45×, 1.9%**, with 40 marginally the *faster* — noise. 40 vs
32 differ by 20.6%, a real rung. The C2 column corroborates independently and was
not part of the timing. So the residual chased through §97–§99 was ~2%, and the
mid-disc `speeds` delta of 1.19× was almost entirely the radius term from 4b.

**The collapse rule (`accudisc.h:1036-1038`) is vindicated** — 48/40 is exactly the
indistinguishable pair it describes. What was wrong was the *input*: mid-disc
`measured_cx` at per-rung radii cannot support that comparison; whole-disc
throughput can.

**Page 2A is NOT lying** (Keith, explicitly): it correctly reports the requested
speed *ceiling* and needs reading in context. Both sides had drifted toward
treating it as faulty. The ceiling is 48 because the uncap is on; actual CD-DA is
governed to 40. `speed-uncap off` changes only the data-disc maximum and the
`speeds` display.

**`speeds` figures vary with disc degradation** — Keith has seen the governor cap at
32× and as low as 8× on damaged media, with the uncap having zero influence.
Throttled speeds are real and measurable, not a page-2A artefact.

**Consequence for the recovery side, unexpected and useful:** at 40–48× this disc
flags 63 and 54 C2 sectors across a ~3,200-sector span; at 32× it flags **one
sector, 12 bits**. A ~50× reduction in flagged sectors for a 21% throughput cost.
"Slower is better on damaged media" now has a direct hardware measurement, on the
same disc where their bench ranked the speed-varying ladder 19/20 against
`sector-hammer`'s 2/20.

**Neither discriminator will be run** and 4b's instruments are not needed — they
would characterise a confound the settled method avoids by construction. 4b's
*contract* findings stand on their own (see below).

<details><summary>Original entry (superseded)</summary>

### 4a. The phantom 48× ladder rung — (SUPERSEDED — historical, no work outstanding)

API_PLAN §9.3. With the uncap on, page 2A genuinely reports 48, `accudisc speeds`
genuinely returns `req=48 page2a=48`, and a ladder admitting rungs on the strict
`req == page2a` rule carries a 48 label over a measured ~21×. The premise that
retired this ("we cannot set the uncap") died when the capability landed on the
binary. Evidence a monotonicity rule would need: a `speeds` table at three passes
with the uncap on — which the SpeedRead discriminator would produce as a
by-product if it ever runs.

**Mechanism, from Keith 2026-07-26 (cdda2img §96.2) — this is UNDETECTABLE from
the bus, by construction.** Keith, on the drive he owns: *"the drive governor will
cap the speed at 40x for a healthy disc, or less for an unhealthy one. Nothing
anyone does in software alone can ever force a CDDA to read at 48x… That is a
function of the drive which is not exposed via MMC/SG."* So with the uncap on,
`req=48 page2a=48` has **both operands derived from the same advertised ceiling**,
agreeing with each other and both wrong about what the drive will do. **`req ==
page2a` cross-checks the quantiser, never the ceiling**, and no MMC field exposes
the gap.

**Our side needs no change — the field and the rule already ship**, and this is
what to tell anyone who asks: `accudisc_speed_rung.measured_cx`
(`accudisc.h:1043`, a *timed streaming read*, not a report) is emitted per rung by
`cli/main.c:672`. And `accudisc.h:1036-1038` already states the rule: "rungs whose
`measured_cx` collapse to the same value are indistinguishable on this rig (bus or
firmware limited) and one of them suffices in a recovery ladder." A 48 rung
measuring ~21× is exactly that case.

**Confound to carry into any such rule** (`accudisc.h:1034-1036`): `measured_cx` is
the achieved rate **at this radius**, and CAV drives read outer tracks faster, so
rungs probed at different LBAs are not comparable — geometry will manufacture
"violations". This is the same confound behind the `speeds` min/max item
(inner/middle/outer) under "Probes / diagnostics". **See 4b — it is worse than a
caveat, it operates inside a single table.**

</details>

### 4b. `speeds` biases every table against its own fast rungs — `[P2]`, OUR DEFECT

**RECONCILED 2026-08-09 — this entry is now MOSTLY CLOSED, and what is left is
one paragraph of documentation.**

- **The radius term: DOCUMENTED 2026-07-28**, which was fix option (a).
  `accudisc.h:1136-1141` states it outright — rungs are laid out along the span,
  a descending candidate list puts the fast rungs innermost, and *"treat a modest
  cross-rung inversion as unproven, not as a measured fact about the rungs."* The
  header also now says which comparison to trust instead (within a rung, where
  the bands are a fixed distance apart and the timed length is identical). Option
  (b), reversing rung order between bands, was considered and rejected: it
  cancels the bias in the **mean**, and we deliberately do not report a mean.
- **Fix option (c) landed too** — the min/max sweep, hardware-validated
  2026-07-28. See "Probes / diagnostics".
- **The discriminator instruments are moot** (whole-disc reads avoid the confound
  by construction) and will not be run.

**STILL OPEN — the second confound, and it is DESK WORK, not session work.** The
timed-window **length** varies across rungs (`want = req * 75` clamped to
[150, 2250]: 2250 sectors at 32× and above, 1800 at 24×, 1200 at 16×, 600 at 8×,
300 at 4×), and the header does not say so. What it *does* say —
`accudisc.h:1143-1145` — is that the timed length is identical **within** a rung,
across its three bands. That is true, and it is the sentence most likely to be
read as covering the cross-rung case it says nothing about. So 48-vs-40 is
length-clean and the documented radius caveat is complete there, but any rule
spanning 48 down to 4 compares rungs differing in radius **and** in timed span,
and the second effect has never been measured. Fix: one paragraph beside the
radius one. Measuring it is optional; **not claiming it is absent is not.**

Found answering cdda2img §97.2. Bare `speeds` probes the **middle half**
(`cli/main.c:658-662`: `lba = leadout/4`, `count = leadout/2`), so it is *not*
inner-disc — that part of their hypothesis fails. But each rung gets its own
window and **the windows march outward**:

```c
/* src/drive/speeds.c:56, 66 */
uint32_t stride = count / ncand;
uint32_t wlba = lba + (uint32_t)i * stride;
```

The default candidate list is **descending** (`cli/main.c:647`,
`{52,48,40,32,24,16,8,4}`), so **the fastest rungs are measured at the innermost
radii and the slowest at the outermost** — a systematic bias against fast rungs, on
a CAV drive, in exactly the direction of the task-4 phantom rung.

Worked example, their uncap-on run (leadout ≈ 162,892, so ncand 7, stride ≈
11,635): the 48 rung is measured at LBA 40,723 and the 40 rung at 52,358 — **11,635
sectors further out**. Against the drive's own curve (`0..359,997 → 20.0x..48.0x`)
that gap is worth **0.90×** linear-in-LBA or **1.13×** under a radius model
(speed ∝ r, LBA ∝ area). Their observed gap is 1.84×. **So geometry accounts for
~half to two-thirds of the "faster setting measures slower" anomaly**, and a
residual of ~0.7–0.9× survives and may be real. The bias has the same sign as the
effect, which is the worst arrangement for judging it.

**Contract defect, not just a docs gap.** `accudisc.h:1036-1038`'s collapse rule
compares `measured_cx` across rungs, and `accudisc.h:1030-1032` explains the
per-rung window (cache freshness — a sound reason) without ever stating that the
consequence is a radius term in every cross-rung comparison. Fix options: (a) say
so in the header; (b) interleave windows so the bias cancels; (c) the
inner/middle/outer item, which reports the gradient instead of hiding it — best of
the three.

**Zero-build discriminators** (`--ladder` preserves order, no sort, no dedup —
`cli/main.c:622-630`). Cross-rung `measured_cx` is not safe today.

- **`--ladder 48,48,48,48`** — the good one. Windows at 40,723 / 61,084 / 81,445 /
  101,806 (61,083-sector span, nominal gradient **4.75×**), and all four time the
  *same* window length, since `want = req*75` clamps to `SPEEDS_MAX_SECTORS` 2250
  (`speeds.c:70-73`) and 48×75 = 3600 clamps. Speed, length and candidate held
  constant, only radius varies → the correction curve for every default-start
  table.
- **`--ladder 48,40,48,40`** — interleaved, a *within-run* control: a 48 on each
  side of a 40, so the trend differences out instead of needing a model.
- **NOT `--ladder 48,40`** — corrected 2026-07-26, this was wrong when first
  written here. `stride = count/ncand`, so **fewer rungs spreads the windows
  further**: a 2-rung ladder puts them 40,723 apart, a **+3.17× bias — larger than
  the entire 1.84× anomaly**. It would look like a spectacular confirmation of the
  effect while measuring only geometry.
- **`--passes` does not exist.** `cmd_speeds` accepts `--start` and `--ladder` and
  nothing else (`cli/main.c:620-636`); anything else prints usage and returns 1.
  Repeat by re-running the command.

**Second, uncharacterised confound: timed window LENGTH also varies by rung.**
`want = req * 75` clamped to [150, 2250] (`speeds.c:70-73`) gives 2250 sectors for
every rung at 32× and above, but 1800 at 24×, 1200 at 16×, 600 at 8×, 300 at 4×.
So 48-vs-40 is length-clean (radius is the whole confound there, which is why the
correction above is complete), but **any rule spanning 48 down to 4 compares rungs
differing in radius *and* in timed span**, and the second effect is unmeasured.

### 5. Stock-ceiling table and the whole driver-free inference — REMOVED 2026-08-09 (0.8.0)

**Keith's second ruling, after reading our explanation of the A-vs-B item.**
Three corrections, and the middle one is a directive about the code rather than
about the backlog:

1. *"The drive is physically incapable of reading CD-RW Audio at > 32x. The
   governor enforces that, regardless of what is requested. The page2a value is
   the request, not the throughput."*
2. *"You should neither be querying nor returning a value for something that is
   unreachable without a driver. And you certainly shouldn't be inferring it.
   The existence, accessibility, and value of the SpeedRead setting is
   completely irrelevant. You request a speed, and the governor tells you what
   you can have. That is your authoritative data."*
3. The "we silently permit a read we believe corrupts Q" framing was **already
   false** — CD-DA cannot be read at 48x on this drive, the governor quantizes
   silently, and the throughput ladder confirms it.

**So the entire inference is gone, not just the open question about it.**
Removed: `stock_ceilings[]`, `adsc_uncap_classify`, and
`ACCUDISC_UNCAP_LIKELY_ON` (enum value **2, retired, never to be reused**).
`accudisc_speed_uncap_probe` still reports `max_x` — handed back verbatim as a
reported figure — but draws no verdict from it. With no driver and no set of our
own, the answer is `UNKNOWN`, which is the truth.

**Why this was wrong at the root, and it is worth keeping.** The comparison was
`max_x > stock_x`, and `max_x` is what the drive **accepts**, never what it
delivers. A raised advertised maximum was therefore not evidence about the
drive's behaviour at all — so no amount of care with the threshold, the table,
or that strict `>` could have made it sound. **A well-tested inference from the
wrong quantity still passes its tests:** the deleted table tests were good ones
(they correctly enforced "unknown model → UNKNOWN, never OFF", guarding a real
prior defect where a bare `max_x > 40` answered confidently about every drive in
the world), and not one of them could see that the quantity did not answer the
question. That is [[silent-narrowing]] one level up — a right value about the
wrong thing.

**And the A-vs-B question dissolved rather than being answered.** It only
existed because the comparison existed. No table, no boundary, no unvalidated
`>`, no CD-RW audio disc to acquire. Retracted with cdda2img (§2026-08-09b) so
they do not spend a disc on it either.

**What survives, unchanged:** sources 1 and 2 (we set it through this handle; an
attached driver answers), `speed_uncap_get/set/push/pop`, the `speed-uncap`
subcommand and `--uncap`. Those report or change a setting through the only
interface that actually knows it.

### 5c. The "--driver auto" advice contradicts itself when a driver WAS named — `[P2]`, DESK WORK

**Not read-speed session work** (classified 2026-08-09): it needs no drive and no
measurement, only the conditional. Do not let the session's abandonment rule take
it.

cdda2img §91.2, reproduced on an empty tray with `--driver plextor` passed
explicitly:

```
accudisc: driver plextor: selftest failed on PLEXTOR DVDR   PX-716A — staying on generic MMC
accudisc: using generic MMC
accudisc: read-speed uncap unsupported via generic MMC — a vendor driver is required (--driver auto)
```

Line 1 says the driver was found, attempted, and failed its selftest. Line 3 tells
the user to pass `--driver auto` — a flag they have already superseded by naming
the driver, and which cannot help, because the driver was never missing. The
advice is hard-coded and does not distinguish **"no driver permitted"** (where
`--driver auto` is the right fix) from **"driver attempted and failed"** (where it
is noise). Four identical sites: `cli/main.c:216, 841, 852, 1467`.

Fix: make the suffix conditional on whether a driver was permitted/attempted this
invocation. When one was attempted and failed, say so and name the reason instead
of suggesting a flag.

### 5d. `speed-uncap` needs a disc; `speed` does not — undocumented envelope split — `[P3]`, DESK WORK

**Not read-speed session work** (classified 2026-08-09): documentation only, and
the behaviour it documents is already measured. Do not let the session's
abandonment rule take it.

Also §91.2. The Plextor selftest issues a real vendor opcode, which requires a
medium, so **`speed-uncap` — report *or* set — is unavailable on an empty drive**.
`accudisc speed` reads mode page 2A directly, needs no vendor driver, and answered
the same question (`page2A max 40x`) with an empty tray. The two subcommands
report overlapping numbers with **different availability envelopes** and nothing
says so. Document in `accudisc.1` and the header; note that anything assuming it
can query uncap state before a disc is loaded cannot.

### 5b. `ACCUDISC_UNCAP_OFF` was authoritative-by-label, inferred-in-fact — MOOT 2026-08-09 (0.8.0)

The defect: `adsc_uncap_classify` returned `LIKELY_ON` **or** `OFF` from one
speed comparison, but only one branch carried a hedge — `accudisc.h` documented
`ACCUDISC_UNCAP_OFF` as *authoritative*, and sources 1–2 did produce it that
way. A consumer could not tell a driver-confirmed off from a speed-inferred one;
they collapsed to a single value and there was no `LIKELY_OFF`. Proposed fix was
a fourth enum value.

**Resolved by deletion instead, and the sequence is the point.** Checked once
after task 1 removed the SpeedRead enforcement: still live, because the
classifier survived the guard that consumed it. Checked again after 0.8.0
removed the inference itself: now moot, because the branch that could produce an
inferred `OFF` is gone. Every value the enum returns is authoritative or
`UNKNOWN`.

**Worth keeping:** two "is this moot now?" checks gave opposite answers a few
hours apart, and both were correct at the time. A mootness judgement is only
valid against the tree that existed when it was made — re-check rather than
inherit, and the cheap ones are worth re-running after any removal upstream.

### 6. Phase 4 — the Python binding — FIRST CUT LANDED 2026-07-27

`bindings/python/`: cffi **API mode** (`build_accudisc.py`), the wrapper in
`accudisc/__init__.py`, 33 device-free tests wired in as ctest
`test_python_binding` (skips on missing python3/cffi, never fails silently).

**API_PLAN §7.3's central premise was wrong and is superseded.** It justified
"streaming API over the sink" with the claim that the subprocess path
"structurally cannot hand the caller the PCM without copying it through a pipe".
No such pipe exists: `--pcm FILE` has the CLI write the file itself. cdda2img
caught this (§101.2) and they are right. The corrected shape:

- **`read_span()` returns `bytes`** — bounded, no sink, no temp file. This is
  the call that earns the binding, because the subprocess path forces a
  write-file/read-back/unlink round-trip *per recovery attempt*
  (`passes x rungs` per failed track).
- **Whole-disc reads stay file-based, and `read_to_file` IS the recommended
  path** — corrected 2026-07-29. This entry used to say it was not, on a
  device-free measurement of the copy (0.02 s cache-warm over 0.941 GB). That
  number was right and answered the wrong question; the on-drive A/B/A puts
  the binding **0.06 s** from the subprocess over a 112 s whole-disc rip,
  inside a 3.68 s noise floor. See the closed `read_to_file` item below.

Held as they were: `ERR_NOTFOUND` is absence (returns `None`), `accudisc_write`'s
caveat is a **positive** return (`_check` tests `rc < 0`), copy by default with
zero-copy opt-in.

**Two Python-specific hazards §7.2 did not cover, both measured:**

1. cffi's default callback returns **0** when the Python sink raises — and 0 is
   what the engine reads as *continue*. A sink bug would have produced a
   completed rip and a traceback on stderr. Fixed with `error=1` + `onerror=`,
   re-raising the cause out of `read()`.
2. "View released on return" does **not** reach a *slice* the sink took — a
   memoryview slice re-exports from the underlying buffer, so the parent's
   release misses it. A refcount detector for this was built and **withdrawn**:
   it read differently per calling frame and flagged correct code. Documented
   limit + `copy=True` default instead, with a test pinning that the hole
   exists so a future CPython closing it is noticed.

**Still to do on this entry:**

- ~~**`accudisc_probe_speed_ladder` is NOT bound**~~ — **BOUND 2026-07-29**
  (`ea11d56`). `Device.probe_speed_ladder()`, A/B'd against the CLI on Tracy at
  **both** `points=3` and `points=1`. The CLI-vs-CLI control fired and settled
  it: binding-vs-CLI and CLI-vs-CLI show the same max delta (0.41x) and mean
  (0.027x), while every structural field — 7 verdicts, 7 page-2A readings, the
  admitted ladder — is identical across all three runs. **The ABI window is
  spent:** `accudisc_speed_rung` is frozen at 14 bytes and pinned in
  `tests/test_binding.py`. The span defaults live in the binding rather than in
  a note asking the caller to reproduce them.
- ~~**`accudisc_write` is NOT bound**~~ — **BOUND 2026-07-29**.
  `Device.write()` returns `WriteResult`; OK/CAVEATS both mean the disc WAS
  written, `not_blank`/`error` arrive as exceptions (`Unsupported` vs other).
  **Both paths device-verified 2026-07-29:** refusal on the PX-716A with Tracy
  loaded, and success on a CDEmu virtual blank — `WriteResult.OK`, progress
  callback fired 6 times ending `(150, 150)`, and a full **round-trip through
  the binding** (write, then `read_span` of the burned track) came back
  byte-identical, 0 flagged, 0 hard. CDEmu state restored to the empty device
  it was found as.

  **`WriteResult.CAVEATS` is still device-free-only** — reaching it needs a
  CD-Text blob whose SIZE_INFO disagrees with the `.toc`, which the round-trip
  does not construct. The four-token mapping is tested; that one path is not.

<details><summary>the two entries as originally written</summary>

- **`accudisc_probe_speed_ladder` is NOT bound — but the reason it was held
  back is gone `[P2]`.** It gated on the `speeds` min/avg/max item, which
  landed 2026-07-28 and spent the ABI window: `accudisc_speed_rung` is now
  10 bytes (`requested_x`, `reported_x`, `measured_cx`, `min_cx`, `max_cx`)
  and still has no `size` field, deliberately. **Binding it is now the next
  step, and it is what makes any further growth of that struct a real
  version break.** So bind the struct as it is, or decide the size field is
  wanted, before writing the cdef — not after.

  Two things the binding must carry across, both of which are silent if
  missed: `points` is 1 or 3 and *nothing else* (2 is `ERR_INVAL`, not a
  round-down), and `min_cx`/`max_cx` of 0 mean "no gradient measured", not
  "measured zero" — a binding that surfaces them as plain ints hands the
  consumer a number that reads as a stalled rung.

  **cdda2img is waiting on this and will not infer it.** They keep their
  `speeds` regex until we send an explicit sentence saying it can go
  (§ca.4). Do not send that sentence when the code lands — send it when the
  probe is *bound* and they can read the numbers from the struct.
- **`accudisc_write` is NOT bound** — the one destructive path. Order agreed
  with cdda2img (§101.6): probes → `read_span` → status map → whole-disc (if at
  all) → `write` last, only after the read path is A/B'd on real media. It will
  need the `summary … result=` token and the positive-return rule.

</details>

- ~~**No hardware validation yet.**~~ **VALIDATED 2026-07-27 (cdda2img §102).**
  Their `tools/binding_ab.py` on the PX-716A with Tracy: **3/3 spans
  byte-identical** subprocess vs binding (inner LBA 300, middle 72415, outer
  162092 — sampled across radius deliberately, since the §9.3 phantom rung was a
  CAV radius term), and every compared TOC field agrees (11 track LSNs,
  `disc_last_lsn`, source, degrade, trusted, anomaly set, data_tracks,
  session_count). Reproduced after a harness refactor.

  **Read the scope precisely — the pass is narrower than "it works".** Each span
  is read three times (subprocess, subprocess, binding) and scored only if the
  two subprocess reads agree. On the damaged region (LBA 112500 +700) that
  control **fired**: two consecutive reads of the same span by the same
  transport differed in **36 sectors**. Without the control that would have been
  reported as "the binding differs in 36 sectors" — well-formed, reproducible-
  looking, wrong referent.

  So: the binding agrees with the subprocess path **where the disc reads
  deterministically**, and has not been shown to agree on a damaged region
  because nothing agrees with itself there. **Consequence for us:** byte-level
  A/B is not available as a check on recovery behaviour, and any future claim
  about the binding's recovery path needs an absolute gate (AccurateRip/CTDB),
  never a differential one. That is the RECOVERY.md invariant arriving from a
  new direction.
- Packaging is a **source package requiring a built libaccudisc**, not a wheel;
  a wheel wants task 2 (install properly) first. The pin is
  `accudisc_version_string()`; `pyproject.toml`'s version is checked against the
  loaded library by a test that fails on drift, and `Device()` refuses on
  compiled-vs-loaded major/minor skew with `AbiMismatch`.
- **`pip install .` works but the result is NOT relocatable** — `[P2]`, and it
  is another argument for task 2. Verified into a clean venv: it installs,
  imports, and raises correctly. But linked against an *uninstalled* library the
  extension carries an absolute `RUNPATH` to this build tree
  (`readelf -d …/_accudisc*.so` → `/home/kgr/Git/accudisc/build/src`). Correct
  here, meaningless elsewhere, and on a machine with some *other* libaccudisc it
  could resolve to the wrong one. Build-from-source-per-environment until
  discovery goes through `pkg-config accudisc`. cdda2img told (§bt.1) before
  they wrote CI against the looser claim in §bs.4.
- **The `toc` line has no token for `sessions_total` or the mapped-session
  count** — `[P3]`, raised by cdda2img §102.5. Confirmed there that
  `cli/format.c:47` prints `info->session_count`, i.e. the READ DISC INFORMATION
  count, so their `session_safe` gate was reading the right measurement all
  along — no bug. But the state we called the dangerous one,
  `sessions_total > mapped_session_count` (seams known to exist, not locatable),
  is **invisible to a subprocess consumer**: there is no token for either
  number, so no CLI caller can refuse on it. The binding can express it. Two
  more tokens on the `toc` acquisition line would close it for everyone.
  cdda2img is migrating and explicitly did not ask; recorded because the gap is
  real rather than cosmetic.
- **`accudisc_toc` is not pinned in `tests/test_abi.c`** — `[P3]`. It has no
  `size` field, so a field added in C arrives in a binding as zero with nothing
  complaining, and the pure guards (`check_audio_range` and friends) would then
  answer about a TOC that is not the disc's. The Python suite now pins
  `sizeof(accudisc_toc)`; the C side arguably should too, since API_PLAN §7.1
  claims size-less structs are pinned there and this one is not.

### 7. cdda2img §88 — CLOSED 2026-08-09. Nothing is owed either way.

Premise conceded (we quoted the manual's three ceilings in our own §au.1 and
then built a one-row table), the question sharpened into the A/B discriminator
and the false-positive direction, and the three-tray-state measurement requested.

**The "waiting on them" this entry recorded is retired**: task 5 is dropped, so
we are no longer waiting for a CD-RW audio reading and they should not spend a
disc producing one. The three-tray-state measurement they were asked for was in
service of a question nobody is now asking. Nothing to chase, nothing owed.

### 8. Their binary depends on our `build/` existing — `[P3]`, note only

The CLI **dynamically** links `libaccudisc.so.0` (`cli/CMakeLists.txt:3` links
the SHARED target) with `RUNPATH` pointing into `build/src`. Since the relink,
`rm -rf build` on our side leaves cdda2img's `accudisc` unable to start, and a
rebuild has a window where the `.so` is being replaced. Their end-of-run engine
re-hash catches the mixed-build case; nothing catches the missing-`.so` case.
Installing properly (task 2) would fix both.

**STILL LIVE 2026-08-09, but the blocker is gone and the remedy is now one
symlink.** Verified rather than assumed:
`/home/kgr/Git/cdda2img/tools/accudisc/accudisc` still resolves to
`/home/kgr/Git/accudisc/build/cli/accudisc` (symlink dated 2026-07-26), so the
dependency is real today. But an installed copy now exists —
`/usr/local/bin/accudisc` with `/usr/local/lib/libaccudisc.so.0` beside it — so
repointing that symlink fixes it outright, with no build-tree coupling left.

**And there is a second, larger reason to repoint it, found while checking the
first.** `getcap` on both binaries: `/usr/local/bin/accudisc` carries
`cap_sys_rawio=ep`; **`build/cli/accudisc` carries nothing.** So cdda2img's
symlink currently resolves to an *unprivileged* binary, and every vendor-opcode
path it tries — the Plextor selftest, therefore the whole uncap surface — fails
for want of a capability that is sitting on the installed copy it is not using.
This is not new damage from any one rebuild; it is the steady state, because
`ACCUDISC_SETCAP_AFTER_BUILD` is OFF in this build tree while
`ACCUDISC_SETCAP_ON_INSTALL` is ON. The build tree is the *unarmed* artefact by
configuration, and their symlink points at it.

Worth telling them explicitly rather than filing: a caller seeing generic-MMC
fallback on a drive that has a working driver would reasonably suspect our
driver gating, and the actual cause is which of two binaries their symlink
names.

**Two reasons this is note-only and stays `[P3]`.** It is *their* symlink, so
the change is theirs to make, not ours to make for them. And it is
self-resolving: item 9's ruling deprecates cdda2img's use of the CLI in favour
of the Python binding, and the binding loads the installed library, so the
dependency disappears when that migration completes. Repointing the symlink is
worth suggesting as an interim, not worth engineering around.

### 9. cdda2img §106 — the CLI is **not** retired; cdda2img's *use* of it is deprecated — **CLOSED on our side**

**Reconciled 2026-08-09: nothing outstanding here is ours.** The ruling below was
relayed, the `speeds` span was answered from source, and the two pieces of work
it created have both landed — `ACCUDISC_ERR_NOT_BLANK` (0.4.0, 2026-07-29) and
the last unbound calls (`probe_speed_ladder` and `write`, both bound 2026-07-29,
device-verified). What remains is cdda2img completing their migration, which is
theirs. Kept in full rather than summarised because the ruling itself is the
durable part and was mis-relayed once already.

> **SETTLED BY KEITH 2026-07-27, and it is not what §106.2 relayed.** cdda2img
> reported "the CLI is retired", subprocess and fallback gone. The actual
> decision is narrower and points the other way: **the CLI stays. What is
> deprecated is cdda2img's use of it.** Verbatim: *"The whole purpose of the API
> is that all consumers use it exclusively. The CLI is our consumer of the API.
> Everyone else creates their own. There's little point in having an API if
> nobody uses it."*
>
> So the CLI is the **reference consumer** — our own proof that the public
> header is sufficient — not a supported integration surface for other tools.
> This *strengthens* `CLAUDE.md`'s existing line (the CLI uses the public header
> only, and its exit codes / `--progress-fd` / stderr conventions stay there by
> design, API_PLAN §3) rather than overturning it.
>
> **What actually changes:** binding the last three calls is required work —
> cdda2img can no longer shell out. **What does not:** the CLI is not going
> away, so their §106.5 fear is unfounded. `binding_ab.py`'s A side survives as
> a *test* instrument even once production use is deprecated; byte-level
> cross-transport comparison is theirs to keep or spend deliberately, not
> something removal takes from them.

Answered from source as §by; all three folded into their plan (§107.2).

- **The `speeds` span** they must reproduce is `cli/main.c:659-664` — `lba =
  leadout_lba / 4`, `count` clamped to `leadout_lba / 2` (middle half,
  representative CAV radius), clamp skipped when `--start` is explicit; rungs
  `{52,48,40,32,24,16,8,4}` filtered to the page-2A max. It is **derived per
  disc**; they had half-decided to record a constant from one Tracy run, which
  would silently mean a different radius on every disc of another length.

- ~~**`ACCUDISC_ERR_NOT_BLANK = -13`**~~ — **LANDED 2026-07-29, 0.4.0**, approved
  by Keith after cdda2img (§120.1) showed the deferral's precondition was about
  to be false: `result=not_blank` was a CLI token, and it was the insurance
  against the census-vs-construction gap, so retiring the CLI cashed it in.
  Three places as costed, plus `strerror`, the binding's `NotBlank` class and
  the cdef. `NotBlank` is a **sibling** of `Unsupported`, not a subclass —
  subclassing would keep `except Unsupported` catching a not-blank disc, i.e.
  backward compatibility bought by preserving the bug.

  Bumped to **0.4.0 with no struct change**, deliberately: a consumer that maps
  error codes to user actions is as broken by a silently changed *meaning* as by
  a moved field, and the version is the only signal it gets.

  The original entry, kept because the reasoning is the reusable part:

  Today
  `ACCUDISC_ERR_UNSUPPORTED` doubles as "disc is not blank". Verified this is
  *exact*: `src/write/burn.c:155` is the only `ERR_UNSUPPORTED` reachable from
  `accudisc_write`, and `src/write/` calls nothing from the driver/plan/session
  layers where the others live. But it is exact **by census, not by
  construction** — any future `ERR_UNSUPPORTED` under `adsc_write_run` silently
  joins "not blank", and the failure is well-formed on both sides: we would tell
  a user to insert a blank disc they are already holding, and neither suite would
  catch it. `-12` is `ERR_ABI`, so `-13` is free. Touches three places:
  `burn.c:155`, the CLI branch at `cli/main.c:946`, the contract at
  `accudisc.h:194`. Their `write_disc` is last in their own ordering, so this is
  not on the critical path.

- ~~**`read_to_file`'s docstring gives advice its own audience may not take**~~
  — **CLOSED 2026-07-29 by measurement (cdda2img §115).** The docstring is
  rewritten as a cost statement, and **the whole-disc entry point is decided:
  do not build one.**

  A/B/A, Tracy, whole disc (162892 sectors), req=40, pcm + c2 + raw sub on
  both arms, `accudisc 0.3.0` at both ends:

  | arm | wall clock | rate |
  |---|---|---|
  | A1 subprocess | 116.43 s | 18.65x |
  | B **binding** | 112.69 s | 19.27x |
  | A2 subprocess | 112.75 s | 19.26x |

  **0.06 s over 112 s against a 3.68 s subprocess-vs-subprocess noise floor.**
  It lands on the faster side, which is a rounding error with a sign rather
  than a result. Page 2A read identically before every arm, so all three ran
  on the same rung. PCM and C2 byte-identical between the binding and the
  *adjacent* subprocess arm over 383 MB and 48 MB; A1, the cold first pass,
  differs from both — which is "the disc warmed up", not "the transport is
  broken". Only `A1 == A2 != B` would have indicted the carrier.

  **Both of the old docstring's claims were wrong, and differently so.** It
  said the CLI "writes the file inside the library's address space" —
  `read_sink()` (`cli/main.c:1000`) also loops per sector and `fwrite`s, so
  the library never writes the file either way. And it called the cost "small",
  which was a guess nobody had numbered. Note the §110 pre-screen and this run
  answered *different* questions — 273x headroom said the sink is not
  intrinsically too slow; this says the nonlinear failure that harness could
  not see (callback falls behind, cache drains, read collapses to a
  seek-per-chunk crawl) does not occur here. Only the second was worth acting
  on.

  Scope, stated because the table reads as more general than it is: **one
  drive, one disc, one speed.** Not a claim about a 48x drive on a clean
  pressing.

- **The single-spin sequence test — `[P2]`, THEY ASKED FOR IT (§107.2), NOT
  BUILT.** Q3 dissolved: there is no cross-call lead-in cache *and no inline
  mechanism to lose*. `--fulltoc`'s "single-spin capture" is two ordinary public
  calls (`accudisc_read_full_toc`, `accudisc_read_cdtext`) made before
  `accudisc_read` on the same open handle (`cli/main.c:1354-1368`); the "one
  spin-up instead of three" is a property of **one process holding the handle
  open**, not of anything the library combines. `Device.__init__` calls
  `accudisc_open` and nothing else (verified), so a binding consumer holding one
  `Device` across the same three calls in the same order issues an identical
  command sequence. What the test must pin is the property that **has no other
  way to fail**: a consumer that opens/closes between the calls gets every byte
  correct and three spin-ups, and nothing on either side goes red. Assert the
  command sequence on one handle, lead-in before audio.

  **§109.4 confirms they want it and are pinning the other half.** They will
  pin the one-`Device` discipline on their side; we pin the command sequence
  on ours. Their framing is worth keeping: the restructuring of
  `read_disc_c2` from one CLI call into three binding calls *is* the
  migration, and it is the one part where getting it wrong "produces a
  correct image, three spin-ups and nothing red".

### 10. cdda2img §109 — `accudisc_write_opts` has the same defect, older and on the destructive path — **DONE 2026-07-29**

> **Landed.** `uint32_t size` leads the struct, `ACCUDISC_WRITE_OPTS_INIT`
> sets it, `accudisc_write` runs `adsc_abi_import` **before the device
> check** — mirroring `accudisc_read_cdda` (`engine.c:355`) for the two
> reasons that code states: a stale binding is diagnosed as ERR_ABI
> ("rebuild") rather than ERR_INVAL ("fix your arguments"), and the guard
> stays reachable without a drive.
>
> **The field landed in padding, so `sizeof` did not move (24 bytes before
> and after) — and that is a hazard, not a saving.** It means an
> already-compiled caller still type-checks and still passes 24 bytes. What
> saves it is that those bytes now start with `simulate`, which is 0 or 1,
> and both are below `sizeof(uint32_t)` and refused. Old callers fail loudly
> on the first call. `tests/test_abi.c` drives *both* values rather than
> leaving that to be inferred from the layout, and pins
> `offsetof(cdtext_path) == 16` so a later field that does move things is
> noticed by the assertion instead of by a burn.
>
> **The guard was made to fail before it was trusted:** replacing the
> negotiation with a plain `memcpy` failed exactly the four refusal
> assertions and left the acceptance ones passing — the correct signature,
> since a bypass accepts everything. Restored, 33/33.
>
> API_PLAN §8 row 7. Man page `accudisc.8` updated (the struct, the macro,
> and `ACCUDISC_ERR_ABI` in the return list).

<details><summary>original entry</summary>

Raised by cdda2img while reading our tree, and it is a fair catch. We held
`accudisc_probe_speed_ladder` unbound to keep the `accudisc_speed_rung` ABI
window open (task 6, spent 2026-07-28) — and while in the header they
checked which structs carry the §7.1 `size` guard:

| struct | `size` field | direction |
|---|---|---|
| `accudisc_read_req` | yes | caller → library |
| `accudisc_read_stats` | yes | library → caller |
| `accudisc_speed_rung` | **no** | library → caller |
| `accudisc_write_opts` | **no** | caller → library |

**The two unguarded ones fail differently, and `write_opts` is the worse.**
`speed_rung` is an OUT array — a stride mismatch corrupts everything past
element 0, loudly and probably immediately. `write_opts` is an IN struct:
growing it means the library reads *past the end* of a short struct the
caller passed. Quiet, and on the burn path.

Their case, checked against our header and correct on both points:
`accudisc_write_opts` is labelled *"Provisional API — the write engine is
young; fields may grow"*, and **it has already grown once** — `cdtext_path`
was appended, its safety resting on "zero-init callers get NULL and the
prior behaviour unchanged". That reassurance holds only while every consumer
recompiles against the current header, and a Python binding shipping a built
`.so` is precisely the consumer for whom it stops holding. It is also last
in the binding queue, so on current sequencing the unguarded struct sits
exposed longest while the one guarded by omission got fixed first.

**Their ask: add `uint32_t size` to `accudisc_write_opts` before
`accudisc_write` is bound.** Same one-line fix already applied twice
(`src/abi.c`, IN rule: short zero-extends, long accepted only if every byte
past our end is zero, zero always refused). Measured today at 24 bytes;
check whether the field lands in existing padding as it did for `read_req`.

Not done here because it is a separate change from the `speeds` work and
deserves its own commit and its own device-free accept/refuse test — but it
is genuinely inside the same window, and the window is closing.

- **Also from §109, no action needed, worth keeping:** their general rule for
  the rest of the migration — *"where the CLI publishes a scalar, ask what
  struct it was computed from, because the binding consumer gets the
  struct."* Derived from finding that `read`'s exit 3 is not carried by the
  library at all but computed by the CLI from `accudisc_read_stats`
  (`hard_errors || sectors_suspect || sectors_flagged`), so there is nothing
  for us to surface. Same shape as §by.1's `speeds` span being a function of
  `leadout_lba` rather than a constant — twice in one migration.
- **§109.3 corrects our `read_to_file` docstring on the facts, not just the
  advice** — folded into the `[P3]` item in task 9. The docstring says the
  CLI "writes the file inside the library's address space"; `read_sink()`
  (`cli/main.c:1000`) also loops per sector and `fwrite`s, so the library
  never writes the file either way. The real delta is a C callback versus a
  Python callback per chunk — bounded, not architectural. Verify before
  rewriting.
- **The wall-clock number will arrive as A/B/A on one disc**, not one rip
  each, with engine-reported throughput alongside wall clock. Their
  reasoning: two ~95 s whole-disc rips vary run-to-run by more than the
  quantity being measured. Still the gate on any library-side whole-disc
  entry point — build nothing before it lands.

</details>

## Deferred (explicitly, by user decision)

- Python / Rust bindings (generated against `include/accudisc/*.h` only).
  **Reopened 2026-07-25** — see "Library API completion" above for the
  prerequisites. **No longer deferred as of 2026-07-27: the Python binding is
  built** (task 6 above). Rust is still deferred and inherits the Python
  answers, per §7.3's "Python first, then Rust" — including the two hazards
  §7.2 missed, of which one (`catch_unwind` across `extern "C"`) it did name.
- Man page (must mirror `docs/reference/ATTRIBUTION.md`).
  **No longer deferred — first cut written 2026-07-26:** `docs/man/accudisc.1`
  (CLI) and `docs/man/accudisc.8` (library/API).

  **Correction 2026-08-09 — the next two clauses used to say "both are untracked
  and neither is installed by CMake yet", and both halves are false.** They are
  tracked (`git ls-files docs/man/` returns `accudisc.1`, `accudisc.8` and
  `.gitkeep`) and they are installed (`CMakeLists.txt`, `install(FILES
  docs/man/accudisc.1 …man1)` and the `.8` into `man8`). The line was written
  when the pages were new and was never revisited; nothing was wrong with the
  work, only with this record of it.

  **Genuinely outstanding against this entry, and it is the whole of it:** the
  pages do not mirror `ATTRIBUTION.md`. They credit nothing beyond the MIT
  statement, so the reference-source credits — QPxTool, cdrtools, REDUMP,
  PlexTools-derived ATIP codes — still need adding. That obligation is the
  reason this entry named the man page in the first place.
- Write / burn (DAO) path — paused; do not start without direction.
  **This line is stale as written.** The DAO path shipped and is
  hardware-verified (`src/write/`, `RECORDING_PLAN.md` §9 phases 1–2 and §11.7/
  §11.8). Left in place rather than deleted because the *decision* it records —
  do not extend the write path without direction — still stands, and RECORDING_
  PLAN §9 phases 3–5 (`--sub` passthrough, vendor write features) are genuinely
  not started.

## `accudisc speed N` sets the WRITE speed too, for every N — not just 0 — **FIXED 0.31.0**

**Fixed 2026-08-28.** `adsc_cdb_set_streaming_desc` takes `write_kbps` and puts
it in Write Size; `accudisc_set_speed`/`_set_speed_range` read the drive's
current write speed from page 2A immediately before building the descriptor.
Public header unchanged. Verified on the PX-716A from READ 24x / WRITE 8x:
`speed 32` -> READ 32x WRITE **8x**, and `speed 48` with SpeedRead OFF ->
READ 40x (clamped) WRITE **8x** — the case that used to escape the ceiling.

**A test gap stated rather than implied:** deleting the current-write-speed read
in `accudisc_set_speed` leaves all 47 tests green, because no device-free test
can observe a page-2A round trip. The descriptor is pinned; the wiring rests on
the hardware runs. Noted in `tests/test_cdb.c` beside the assertions.

The analysis below is kept as the record of how it was found.


Found 2026-08-28 restoring the drive after the 0.29.0 ladder; **cause corrected
the same afternoon** by the speed-persistence matrix
(`private/research/incoming/2026-08-28-speed-persistence.md`).

THE MECHANISM IS OURS. `adsc_cdb_set_streaming_desc` (`src/mmc/cdb.c`) writes
`Write Size = read_size` into the SET STREAMING performance descriptor, with the
comment "(mirror; unused for read)". Measured: the drive USES it.

    SET STREAMING 8x       -> READ 8x  AND WRITE 8x
    SET CD SPEED r=8 w=4   -> READ 8x      WRITE 4x   (only write moves)

So SET STREAMING overrides SET CD SPEED, not the reverse, and `accudisc speed N`
— a READ-speed command in the help, the header and the man page — silently
retunes the write speed at every N. The `speed 0` case below is one instance of
it, not a separate bug.

**Consequences, all measured:**

- A restore must be **SET STREAMING first, then SET CD SPEED**. The other order
  loses the write speed, because streaming overwrites both fields.
- A restore must replay the **kB/s the drive reported**, not the Nx requested.
  The ladders differ — READ admits {4,8,24,32,40}, WRITE {4,8,16,32,48} — so
  `accudisc_set_speed`, which takes Nx, cannot round-trip an off-ladder value.
  (Those are ACCEPTANCE ladders from page 2A. Per §4a above, acceptance is not
  delivery — a 48x write rung means the field took the value, and no burn in
  that matrix ran at 48x to test it.)
- A burn does NOT disturb the read speed (0.29.0 reads it back and passes it
  through), so the leak is one-directional: reading disturbs writing.

**FIX CHOSEN ON EVIDENCE, 2026-08-28 15:50 — it is (b), and (a) is WORSE than
the bug.** Measured by sending the descriptor directly, start READ 24x /
WRITE 8x, then `SET STREAMING 32x` three ways
(`private/research/incoming/2026-08-28-speed-leak-and-48x.md` §1):

    Write Size = read_size (SHIPPING)  -> READ 32x  WRITE 32x   leaked
    Write Size = 0         (fix a)     -> READ 32x  WRITE 48x   WORSE
    Write Size = cur_write (fix b)     -> READ 32x  WRITE  8x   EXACT

There is NO "leave alone" encoding for this field: zero reads as **maximum**,
exactly as 0xFFFF does in SET CD SPEED. So (a) — which I listed first as the
obvious one-line fix — silently jumps the write speed to the drive's top rung,
replacing a visible leak with an invisible one.

TO DO: mirror the CURRENT write speed instead. `adsc_write_cur_write_kbps()`
already exists (0.29.0). Note it must be read BEFORE the streaming call and the
descriptor takes kB/s, so no Nx round-trip is involved — which also sidesteps
the differing-ladders problem below.

**And the leak escapes the uncap, which is the sharpest form of it.** With
SpeedRead OFF (`max_x 40`), `SET STREAMING 48x` gives READ 40x — clamped
correctly — and WRITE 48x, not clamped. The leak does not merely move a setting
the caller did not ask about; it moves it PAST a limit the caller deliberately
left in place.

### The original filing, kept because its second half still stands

Found 2026-08-28 while restoring the drive after the 0.29.0 write-speed ladder.

Measured on the PX-716A:

    before   WRITE 2822 kB/s (16.0x)   READ 7056 kB/s (40.0x)
    after    WRITE 8467 kB/s (48.0x)   READ 7056 kB/s (40.0x)

Two separate things, both worth closing:

1. **SET STREAMING's RDD bit is refused by this drive.** (Still true and still
   worth its own line.) `accudisc_set_speed(dev,
   0)` sets desc[0] = 0x04 ("restore drive defaults") and the drive rejects it;
   `src/device.c` latches streaming off and falls back to CDROM_SELECT_SPEED. The
   only mechanism we have that is *named* restore-defaults does not work on the
   one drive we test on. Do not assume it works elsewhere either — it is
   currently evidenced nowhere.

2. ~~**The fallback resets the write speed as a side effect.**~~ **SUPERSEDED
   — the guess here was wrong.** I attributed the write-speed movement to the
   kernel's CDROM_SELECT_SPEED fallback issuing 0xFFFF in both fields. That
   explains only the `speed 0` case, where the RDD bit is refused and the
   fallback runs at all. The general cause is our own descriptor mirroring the
   rate into Write Size, above — which moves the write speed at EVERY N, on the
   SET STREAMING path, with no fallback involved.

   Recorded rather than deleted because the error is instructive: a candidate
   that fits the one case in front of you is not thereby the cause, and I filed
   it without testing any other N.

Now that 0.29.0 sets a write speed deliberately, this matters more than it did:
a burn leaves the drive at the burn's speed (consistent with `speed`'s documented
"a standalone set persists (not restored)", and with cdrecord), and the obvious
way to undo that quietly moves a second setting.

There is also no CLI command that REPORTS the current write speed — `speed` shows
page 2A's read fields only. Diagnosing the above needed a throwaway probe.
`adsc_write_cur_write_kbps()` exists internally as of 0.29.0.

## The write-speed ladder saturates at 32x, and 48x CD-DA is accepted (2026-08-28)

Measured, simulate only, 2400 sectors, engine-reported lead-in settle
subtracted, best of 2 (full table and cautions in
`private/research/incoming/2026-08-28-speed-leak-and-48x.md` §3):

    ask   payload ms      ask   payload ms
     4x     38958         24x     10021   <- same as 16x
     8x     19659         32x      7690
    16x     10039         40x      7711   <- same as 32x
                          48x      7683   <- same as 32x

Repeatability 0.04% over three runs each at 32x and 48x, so the collapse is far
outside the noise. **A 48x CD-DA write is ACCEPTED, not refused**, and page 2A
echoes 48.0x — the phantom-48x trap of §4a, now confirmed in the WRITE field as
well as the read one.

Two cautions that must travel with those numbers:

- They are NOT medium write rates. `--simulate` runs the path with the laser
  off; the figures are ~5x below the request and the SETTLE also scales with
  requested speed (32.3 s at 4x, 8.1 s at 32x), which is what pacing the whole
  operation would look like. Comparator between rungs only.
- Short burn, fixed radius. A CAV drive writes outer tracks faster, so a
  full-disc burn may separate rungs this cannot — §4b's confound.

The SpeedRead uncap raises the advertised ceiling (`max_x` 40 -> 48) and changes
the write payload times not at all (7690/7711/7683 off vs 7860/7685/7681 on).
It is a read uncap and behaves as one.

**Not testable here:** a Mode 1 data write at any speed. `src/write/tocparse.c:234`
refuses a non-`TRACK AUDIO` track ("this is an audio-only writer"), which is
CLAUDE.md scope rather than a defect. Verified against a real CD_ROM/MODE1 toc
over an ISO: refused, exit 2.

## cdrecord confirms phantom-48x on the WRITE path, and shows CAV doing it (2026-08-28)

Independent check on kgr's instruction, using cdrecord in dummy mode over a
93 MB ISO because the subject is the drive rather than our software.
`cdrecord dev=/dev/sr0 -dummy -v speed=48`, CD-R blank:

    Power-Rec write speed:     48x (recommended)     [Forcespeed OFF, SpeedRead OFF]
    running rate:              20.8x -> 26.7x, climbing monotonically
    Average write speed        15.8x
    Last selected write speed: 48x
    Max media write speed:     48x
    Last actual write speed:   25x

**Accepted at 48x, reported 48x everywhere, delivered 25x** — §4a's pattern
stated by the reference tool, which prints `Last actual` as a separate field
precisely because `Last selected` cannot be trusted. Agrees with our own
saturation finding (32/40/48 indistinguishable) from a different direction.

Two things cdrecord shows that our harness cannot:

- the rate **climbs monotonically across the disc** (20.8x -> 26.7x) — CAV,
  outer tracks faster, so a single "delivered rate" is a fiction for this drive.
  §4b's radius confound, visible directly rather than argued.
- **POWEREC was active and recommended the full 48x.** So the Plextor
  power-calibration override I flagged this morning as a candidate reason a
  requested speed might not hold is NOT what capped this run.

One cdrecord line to distrust: it printed `Profile: DVD-RW restricted overwrite`
for a CD-R. Ours reads `profile=0x0009 kind=BLANK` with ATIP `type=CD-R
manufacturer=Taiyo Yuden`, which is correct. Do not use that line as a
cross-check of media identity on this drive.
