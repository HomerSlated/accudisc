# Plextor consumer features — opcode binding progress (PX-716A)

**Licensing: MIT** (same as the core; opcodes are hardware facts — see
`LICENSE.md`). Lives in the driver zone for architectural vendor-isolation, not
licensing. See `PROTOCOL.md` for the detailed protocol notes. Feature names are
taken from the PlexTools / PX-716 user manual.

Legend — **Identified**: opcode + CDB framing pinned. **Working**: exchange
live-verified on the PX-716A via raw SG_IO (`re-tools/sgsend.c`). ☑ = done,
☒ = not yet, ◐ = partially (see note). "GET-verified" means the read path was
confirmed returning coherent state; write-time *effects* (density, laser power,
book type) are not observable without a burn — the write/burn path is paused.

| # | Feature (manual) | Opcode / page | Identified | Working | Notes |
|---|------------------|---------------|:---------:|:-------:|-------|
| 1 | **SpeedRead** (uncap CD read speed) | `0xE9` MODE, page `0xBB` | ☑ | ☑ | **Fully verified**: SET ON flips mode-page-2A max read speed 40×→48× (7056→8467 kB/s); SET OFF restores 40×. Value at CDB[3], state echoed at resp[2]. **⚠ Corrupts the Q subchannel** — see below. |

### ⚠ SpeedRead destroys subchannel Q on inner/mid tracks (session 4, live)

Measured on the PX-716A reading ABBA *Gold* whole-disc (`read --start 0 --sub
raw`), SpeedRead ON vs OFF, same command, same ~24.2× average, back-to-back:

| SpeedRead | total Q-CRC ok | 0–10% | 10–60% (inner/mid) | 70–100% (outer) |
|-----------|----------------|-------|--------------------|-----------------|
| **ON**    | **40.6 %**     | 55 %  | **0.0 %** (dead)   | ~99 %           |
| **OFF**   | **99.2 %**     | 98 %  | ~99–100 %          | ~99 %           |

Cause: SpeedRead pins the drive's CAV RPM to its 48×-outer target across the
whole disc. On inner tracks the linear velocity is far below what that RPM
implies and the subchannel channel-clock cannot track it, so Q decodes to
garbage; the outer tracks (linear speed matches RPM) stay clean. The **audio
main channel is unaffected** (0 hard errors, 0 C2 both runs) — the damage is
Q-only, and silent. An *isolated* read of an inner LBA is clean even at
`--speed 40`, because the drive then spins only as fast as that radius needs;
the corruption requires the sustained high RPM of a full-disc SpeedRead pass.

**Rule for the read engine: never enable SpeedRead when `--sub` is requested.**
SpeedRead is an audio-only accelerator.

**Relation to the cdda2img 47 % Q loss (their §9) — NOT established.** Their
incident predates AccuDisc's SpeedRead support, so SpeedRead cannot have been
the cause *unless* another tool (e.g. PlexTools) had left the persistent bit
on. Against that: a whole-disc read here with SpeedRead **off** was 99.2 %
clean — i.e. this test does **not** reproduce their 47 % in their nominal
config. So their cause is still open. The one suggestive link is the *pattern*:
their missing pre-gaps (tracks 5/6/7/9) are inner/mid, matching the inner dead
zone SpeedRead produces — which is a reason to check whether SpeedRead (or any
high-inner-RPM condition) was in fact active, not proof that it was. Q-health
counters in the read summary are needed to settle it by re-running the rip with
SpeedRead verified off.
| 2 | **Write Strategy / AutoStrategy** | `0xE4` read / `0xE5` write | ☑ | ◐ | GET-verified: AutoStrategy currently ON (resp[2]&0x0F=1). Enable/disable = `0xE4` CDB[2]=`0x10\|state`. Strategy DB read `0xE4` CDB[1]=0x02 CDB[2]=0x03; custom strategy push = `0xE5`. Manual write-strategy needs AutoStrategy OFF. Effects need a burn. |
| 3 | **SecuRec** (disc password lock) | state `0xE9` page `0xD5`; set `0xD5` SEND_AUTH | ☑ | ◐ | State GET-verified: not protected (resp[3]=0). Password load = opcode `0xD5`, 16-byte WRITE `[00][len][14×passwd]`, CDB[2]=01 CDB[3]=01 CDB[4]=02 CDB[10]=0x10. OFF = `0xD5` with no data. Drive-enforced read-lock (auth handshake `0xD4`/`0xD5`), **not** container encryption. Not burn-tested. |
| 4 | **GigaRec** (CD-R density 0.6–1.4×) | `0xE9` MODE, page `0x04` | ☑ | ◐ | GET-verified (off / 1.0×). **Corrects session-2: page is 0x04, not 0x06.** Rate at resp[3], disc-rate resp[4]. Rate table validated (see PROTOCOL.md). SET is write-time; effect needs a burn. |
| 5 | **VariRec** (manual laser power) | `0xE9` MODE, page `0x02` | ☑ | ◐ | GET-verified (off). CD: CDB[3]=`0x02\|disc_type`; resp[2]=state, resp[3]=power, resp[5]=strategy. DVD variant same page, disc_type bit. Effect needs a burn. |
| 6 | **SilentMode** (speed/noise caps) | `0xE9` MODE, pages `0x06`/`0x07`/`0x08` | ☑ | ☑ | GET-verified: main page 0x08 returns full settings block (`08 06 00 04 08 00 19 0d`). Disc=0x06, Tray=0x07, Main=0x08. Read/write toggles. |
| 7 | **Single Session / Hide CD-R** | `0xE9` MODE, page `0x01` | ☑ | ☑ | GET-verified (off). resp[2] bit0=single-session, bit1=hide-CD-R. SET value = `2*hide + ss` at CDB[3]. |
| 8 | **Book Type / bitset** (DVD±R) | `0xE9` MODE, page `0x22` | ☑ | ☑ | GET-verified (resp[2]=1). Per-disc-type book-type override for DVD compatibility. |
| 9 | **Test Write / simulation** (DVD+) | `0xE9` MODE, page `0x21` | ☑ | ☑ | GET-verified (off). |
| 10 | **PoweRec** (optimal write power) | `0xED` (MODE2) | ☑ | ☑ | GET-verified: ON, recommended-speed field = `ntoh16(resp[4..5])`. CDB[1]=00 GET, CDB[2]=00, len at CDB[9]=0x08. |
| 11 | **Q-Check** (C1/C2/PI-PO/jitter/beta) | `0xEA` | ☑ | ☑ | Already implemented in `plextor.c` (subcmds 0x15/0x16/0x17). The one shipping feature. |

## Not consumer features (present in the opcode inventory; noted for safety)

| Opcode | Meaning | Caution |
|--------|---------|---------|
| `0xEE` | **Drive reset / reboot** (no data) | Do **not** send casually — resets the drive. |
| `0xD4` / `0xD5` | GET_AUTH / SEND_AUTH | SecuRec + PX-755/760 auth handshake. |
| `0xE3` | PlexEraser | Destructive media erase. Never probe live. |
| `0xEB` | PoweRec transfer-rate / recommended speed readout | Read-only status. |
| `0xF1` | EEPROM read (TLA etc.) | PX-716 reportedly rejects the TLA form. |
| `0xF3` / `0xF5` | FE/TE (focus/tracking error) scan + readout | Diagnostic. |
| `0xD8` | READ CD-DA (classic raw audio) | Not needed; all five `0xBE` combos work in the core. |
| `0xDE` `0xDF` `0xE1` `0xE2` | unmapped | Not exposed by QPxTool or the manual; internal/DVD/HDD (0xDF is model-gated to PX-PH2 external HDD). Not CD-DA consumer features — left unmapped. |

## Cross-validation

The whole table was confirmed three independent ways: (1) PlexTools static RE
(sessions 1–2, opcode inventory + helper structure); (2) QPxTool source
(pages + CDB framing); (3) the live PX-716A — both raw SG_IO (`sgsend`) and
QPxTool's own `cdvdcontrol -c`, whose reported states match the raw reads
exactly (SpeedRead OFF, PoweRec ON, GigaRec OFF, SecuRec OFF, AutoStrategy
AUTO[1], TestWrite OFF, …).

## The 0xE9 MODE command (verified model)

```
CDB:  E9  DIR  PAGE  VAL  ..  ..  ..  ..  ..  L9  L10  ..
       0   1    2     3                      9  10
```

- **DIR** (CDB[1]): `0x00` = GET (read current), `0x10` = SET.
- **PAGE** (CDB[2]): feature page (table above).
- **VAL** (CDB[3..]): value(s) to set (GET leaves 0).
- **Length**: an 8-byte page; drive returns a fixed 8-byte block. Framing puts
  `0x08` at CDB[10] for most pages, CDB[9] for SS/Hide and PoweRec — the drive
  is lenient about which. Always an 8-byte **data-in**, even for SET (the drive
  echoes the resulting state).
- **Response**: `resp[0]` = page echo, **`resp[1]` = `0x06` constant header**
  (this is the byte session-2 misread as "page 6"), `resp[2..]` = state/values.

Provenance: opcode/page constants and CDB framing cross-referenced against
QPxTool (GPL) — see `../../docs/ATTRIBUTION.md` — and independently
live-verified on the user's own PX-716A. See `PROTOCOL.md` for the full trace.
