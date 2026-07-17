# tools/

Dev scripts and hardware probes. Not part of the build or the install; they
exist to answer questions about drives and discs before that knowledge is
turned into library code.

## Hardware probes (C)

Not wired into CMake — they use library internals (`src/`), which the public
ABI deliberately hides, so they link the static lib directly:

```sh
cmake --build build
gcc -o /tmp/mediaprobe tools/mediaprobe.c -I include -I src build/src/libaccudisc.a -ldl
/tmp/mediaprobe /dev/sr0
```

- **`mediaprobe.c`** — read-only. GET CONFIGURATION current profile, Real-Time
  Streaming (0x0107) bits, mode page 2A max/current, READ DISC INFORMATION,
  TOC + logical type, and the GET PERFORMANCE (0xAC) nominal curve with a
  CLV/CAV verdict. Changes no drive state; safe on CD/DVD/BD.
  *Known wart (deliberate, mirrors what the real code must avoid): it runs the
  CD track-CTRL classifier unconditionally, so it calls a DVD "CD-ROM (data)".
  Logical type must be gated on a CD profile (0x08/09/0A).*

- **`speedprobe.c`** — SET STREAMING (0xB6) flag-bit harness: does GET
  PERFORMANCE reflect a set ceiling; does Exact (0x02) work; does real RDD
  (0x04) restore. **Needs `CAP_SYS_RAWIO`** (data-OUT does not pass the
  kernel's SG filter without it, regardless of open mode — measured):
  `doas setcap cap_sys_rawio+ep build/speedprobe`. Changes drive state.
  *Build onto the real filesystem (`build/`), NOT `/tmp` — that is tmpfs and
  won't hold the `security.capability` xattr, so the cap silently won't bind.*

- **`ss_variants.c`** — the probe that cracked the SET STREAMING mystery.
  Isolates the CDB Parameter List Length offset: len@8-9 (spec position we
  wrongly used) fails 4/1b; len@9-10 (schily "Sz not G5 alike") succeeds and
  drops page 2A to the commanded ceiling. Also shows this drive rejects RDD
  (0x04) with 5/26/00. Needs `CAP_SYS_RAWIO`; restores to full speed.

- **`speedcheck.c`** — end-to-end check of the *library's* SET STREAMING path
  (`adsc_mmc_set_streaming`) across a speed ladder, reading back page 2A. Needs
  `CAP_SYS_RAWIO`. Companion after the 9-10 offset fix.

## Offline Q analysis (Python)

Operate on a raw subchannel capture (`accudisc read --sub raw --subf FILE`),
96 bytes/sector:

- **`qdecode.py FILE.sub START_LBA [--only-bad] [--boundaries]`** — per-frame Q
  decode with CRC gating. ADR-aware: only ADR=1 frames carry position; ADR=2 is
  MCN, ADR=3 is ISRC. **This matters** — decoding the ~1-per-98 MCN frames as
  position manufactures phantom index-0 boundaries.

- **`pregap.py FILE.sub START_LBA TRACK INDEX1_LBA`** — per-boundary pregap
  census: extent, damage, and whether the recovery-critical anchors survived.

These were the oracle for `accudisc_index_map_decode` (`src/cdda/index_map.c`)
and `accudisc pregaps`, which supersede them for routine use. Kept because an
independent second implementation is what caught the C decoder's over-strict
UNKNOWN rule.

## Generators

- **`gen_media_db.py`** — ATIP media catalog -> `src/drive/media_atip_db.inc`
- **`gen_offsets.py`** — read-offset table

## Test targets

`/dev/sr1` on the dev box is **CDEmu** (virtual, backend in `reference/libmirage`).
It is a free negative control: it *advertises* the Real-Time Streaming feature
and then **rejects GET PERFORMANCE** (Illegal Request). Anything that trusts a
feature bit instead of smoke-testing it will assert nonsense there — a virtual
drive has no spindle, no radius, and no rotation.
