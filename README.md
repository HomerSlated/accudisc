# AccuDisc

Precision Red Book CD-DA reader/writer — a C library, a CLI, and a vendor-driver
architecture for the drive features no other tool exposes.

AccuDisc reads audio CDs (and the audio portion of Mixed Mode discs) with the
full range of data and metadata the format carries — TOC and session structure,
subchannel P–W, C2 error pointers, CD-Text, ISRC, MCN, pregaps and indices — and
burns Red Book CD-R/RW Disc-At-Once. It talks to drives via MMC over SG_IO plus
proprietary vendor opcodes, with per-drive knowledge of offsets, caching and
quirks.

## Who this is for

**AccuDisc's intended consumer is a front-end application, not an end user.** It
is a library with a CLI, not an app. `cdda2img` is the reference consumer, and
the machine interface is designed around that relationship: token-delimited
stdout, a stable exit-code contract, a progress channel on a caller-supplied fd,
and a shared-memory status map a GUI can mmap and render live.

You can absolutely drive it directly, and everything is there to do so — but the
learning curve is steep and the flag surface assumes you already know what a
subchannel is, what a C2 pointer means, and why an index boundary is not a track
boundary. Nothing is hidden behind a friendly default that guesses for you.

**AccuDisc moves bits and reports what it saw.** No post-processing, no lookups
(no CDDB, no MusicBrainz), no tagging, no encoding, no analysis, no "repair" of
audio it could not read. Absolute verification — AccurateRip, CTDB — lives in the
calling application, because every signal AccuDisc produces is a *relative* one:
"stable across the reads of this run", never "correct against the pressing". That
is a deliberate scope boundary, not a gap. It is also what keeps the engine
auditable: there is exactly one place bits can change, and it is the drive.

**On media: CD-DA is the whole of what AccuDisc does today**, and there is no
ISO9660/CD-ROM data processing. Two different reasons sit behind what is missing,
and they should not be confused. **SA-CD is impossible, not deferred** — the DSD
layer is DVD-density, read at 650 nm, and encrypted, so a CD/DVD drive cannot
address it at all; a hybrid SACD's CD layer is ordinary Red Book and already
reads as CD-DA, which is the whole of the SACD story. **DVD and BD are simply not
scoped yet** — they are ordinary media this class of drive already addresses, so
that boundary is a current decision rather than a permanent one. Nothing outside
CD-DA is implemented, and nothing here should be read as a commitment to add it.

## What makes it different

**A complete capture, in one pass.** Audio, C2 pointers and subchannel for a
given sector always come from the *same* READ CD transfer, so they stay aligned
sector-for-sector — the property C2-guided recovery depends on. A single
invocation can emit the PCM, the C2 bitmap stream, the raw P–W subchannel, the
de-interleaved and Reed-Solomon-corrected CD+G pack stream, the raw full TOC and
the raw CD-Text blob, off one spin-up.

**A frame-accurate status surface.** `--map-file F` gives you one status byte per
sector — pending / ok / C2 / hard / recovered / suspect, with a severity nibble —
updated in place through a `MAP_SHARED` mapping. Another process mmaps the same
bytes and watches the rip progress live, with no polling and no IPC; this is what
drives an EAC-style colour-coded disc map. The file persists after exit for
post-mortem analysis. `--progress-fd N` carries the machine-readable progress and
summary tokens alongside it, unaffected by `-q`.

**Composable recovery, not a monolithic "secure mode".** Each lever is
independent and each is off by default:

- `--verify P` — read everything P times with cache defeat; disagreements are
  resolved by consensus or delivered best-effort and marked SUSPECT.
- `--c2-retries N` — hunt a C2-clean copy of each flagged sector; the winning
  read replaces the whole sector, so alignment survives.
- `--overlap K` — extend each chunk by K sectors and check the seam against the
  next chunk, catching drive slips that back-to-back reads cannot see.
- `--ladder 32,16,8,4` — descend a speed ladder across rescue attempts, because
  a drive can misread the same way at the same speed every time and consensus
  votes must be speed-diverse.

**Vendor isolation as an architecture, not a footnote.** The core library is pure
MMC/SG — no proprietary opcode is compiled into it. Every hardware-specific
feature lives in a separately `dlopen`'d module under `drivers/`, and the gate
order is enforced: identify the drive → locate a driver → the application's
attach call *is* the permission grant → a selftest that reads, sets and re-reads
real device state → use. Any failure leaves the device on generic MMC/SG, fully
usable. Vendor features are off unless explicitly requested with `--driver`.

## Proprietary drive features — work in progress

This is the headline differentiator, and it is honest about its state.

Consumer optical drives shipped firmware features that no ripping tool exposes on
any platform. On Windows you cannot activate SecuRec, GigaRec, VariRec or
SilentMode without the hardware vendor's own closed application; on Linux you
cannot activate them at all. AccuDisc is bringing them to a permissively
licensed, scriptable tool.

**Shipping today — exactly two**, both reachable from the CLI and the API:

| Feature | What it does | Opcode | Exposed as |
|---|---|---|---|
| SpeedRead | lifts the firmware's CD read-speed cap (PX-716A: 40× → 48×) | `0xE9` mode page `0xBB` | `speed-uncap` subcommand, `read --uncap` |
| Q-Check | the drive's own C1/C2/CU error-counter census | `0xEA` | `cxscan` subcommand |

Note that SpeedRead is an **audio-only** accelerator: it pins the drive's CAV RPM
to its outer-edge target across the whole disc, and the subchannel channel-clock
cannot track that on inner and mid tracks, so Q decodes to garbage while the
audio stays clean. Measured on a PX-716A over a whole-disc read: 99.2% Q-CRC-ok
with it off, 40.6% with it on, and 0.0% across the inner/mid band (10–60% of the
disc). Combining `--uncap` with `--sub` is therefore refused.

**Reverse-engineered and pinned, not yet wired.** The opcodes, pages and CDB
framing below are documented in `drivers/plextor/FEATURES.md` and
`drivers/plextor/PROTOCOL.md`:

| Feature | Opcode / page |
|---|---|
| SecuRec — drive-enforced disc password lock | `0xD5` (+ state on `0xE9` page `0xD5`) |
| GigaRec — CD-R recording density, 0.6–1.4× | `0xE9` page `0x04` |
| VariRec — manual laser power | `0xE9` page `0x02` |
| SilentMode — speed and noise caps | `0xE9` pages `0x06`/`0x07`/`0x08` |
| Book Type bitset (DVD±R) | `0xE9` page `0x22` |
| Single Session / Hide CD-R | `0xE9` page `0x01` |
| Test Write simulation | `0xE9` page `0x21` |
| PoweRec — optimal write power | `0xED` |
| AutoStrategy / write strategy | `0xE4` read, `0xE5` write |

> **Read the "Working" column in `FEATURES.md` precisely.** A tick there means
> *the command exchange was live-verified on a PX-716A through a raw SG_IO
> scratch tool* (`drivers/plextor/re-tools/sgsend.c`). It does **not** mean the
> feature is reachable from the driver, the CLI or the API. Only SpeedRead and
> Q-Check are wired into `drivers/plextor/plextor.c`. **You cannot invoke GigaRec
> today.**

The work so far is PX-716A-focused, since that is the drive it was verified on.
The driver ABI (`include/accudisc/driver.h`) is vendor-neutral and built to take
others.

## Using it

Examples are organised by objective. Every flag shown exists; `accudisc --help`
is the full list and `man 1 accudisc` is the reference.

**Find out what is in the drive before doing anything.** `disc` is a pre-flight
guard, designed to be the first thing a front end calls. It branches on a token,
not on prose, and exit 3 means "classified fine, but neither ripping nor burning
is legal for this disc":

```sh
accudisc --device /dev/sr0 disc
# disc kind=AUDIO profile=0x0008 disc_status=2 erasable=0 \
#      audio_tracks=19 data_tracks=0 reason=audio
```

**Capture a disc with everything it carries**, in a single spin-up:

```sh
accudisc read --sub raw \
    --pcm disc.pcm --c2f disc.c2 --subf disc.sub \
    --fulltoc disc.fulltoc --cdtext disc.cdt --map
```

**Test a recovery strategy against a damaged span.** Point it at the sectors that
actually failed — the whole disc is rarely the problem:

```sh
accudisc read --start 128000 --count 4000 \
    --verify 3 --c2-retries 8 --overlap 4 --ladder 16,8,4 \
    --pcm span.pcm
```

**Characterise an unknown drive.** What it claims, what it honours, and how its
C2 bitmap lines up with its audio:

```sh
accudisc features --all                  # C2 / Accurate Stream / rotation type
accudisc speeds --ladder 40,32,24,16,8,4 # requested vs page 2A vs measured
accudisc c2lag --start 100000 --count 20000 --speed 32   # needs a damaged span
```

**Watch a rip live from another process.** `status.map` is exactly `count` bytes,
one per sector, updated in place:

```sh
accudisc read --map-file status.map --progress-fd 3 -q --pcm disc.pcm 3>prog
```

```python
import mmap
with open("status.map", "rb") as f:
    m = mmap.mmap(f.fileno(), 0, prot=mmap.PROT_READ)
state, severity = m[i] & 0x0F, m[i] >> 4   # sector start_lba + i
```

**Take a hardware error census** using the drive's own C1/C2/CU counters,
sampled once per second of audio:

```sh
accudisc --driver auto cxscan --speed 8
```

**Burn an audio session**, simulating first:

```sh
accudisc write --toc disc.toc --bin disc.bin --simulate
accudisc write --toc disc.toc --bin disc.bin --cdtext disc.cdt
```

## Recovery profiles

`docs/reference/RECOVERY.md` is the design of record for recovery, and its
deliverable target is **a portfolio of per-disc recovery profiles** — ranked,
disc-specific recommended methods rather than one universal "secure mode".

The premise is that damage classes call for different levers, and that the levers
are not interchangeable. Targeted re-reads (`--verify`, `--c2-retries`,
`--ladder`) fix *localised audio* and only that; whole-disc speed is the only
lever that moves the Q subchannel, and only its transient part. Three disc
classes are named so far — **clean**, **disc-wide marginal**, and **localised
static** — and a drive that self-throttles its speed ceiling at disc init is
already telling you which one it thinks it has, before a sector is read.

**This is designed, not built.** No profile is produced today: the measurement
suite behind it exists, the methodology has known confounds documented in
RECOVERY.md §12.5–12.6, and the next iteration is scheduled at a
weeks-to-months pace. Treat the section above as a roadmap, not a feature.

(Unrelated, despite the word: the `profile=0x0008` token in `disc` and `media`
output is the MMC *disc* profile — the media-type code. It has nothing to do
with recovery profiles.)

## Components

| Component | Path | Description |
|---|---|---|
| `libaccudisc` | `src/`, `include/accudisc/` | Shared library + public C API |
| `accudisc` | `cli/` | Command-line interface |
| Vendor drivers | `drivers/` | `dlopen`'d modules; Plextor today |
| Python bindings | `bindings/python/` | Placeholder — not yet implemented |
| Rust bindings | `bindings/rust/` | Placeholder — not yet implemented |

## The C API

`include/accudisc/*.h` is the contract. The CLI and every binding are built
against it exclusively, never against `src/` internals. Device handles are
opaque, every identifier is prefixed `accudisc_` / `ACCUDISC_`, and libc types
are kept out of the ABI where avoidable. Current version **0.3.0**.

```c
#include <accudisc/accudisc.h>

int err = 0;
accudisc_device *dev = accudisc_open("/dev/sr0", 0, &err);
if (!dev)
        return fprintf(stderr, "%s\n", accudisc_strerror(err)), 2;

accudisc_toc toc;
if (accudisc_read_toc(dev, &toc) == ACCUDISC_OK)
        for (uint8_t i = 0; i < toc.track_count; i++)
                if (ACCUDISC_TRACK_IS_AUDIO(&toc.tracks[i]))
                        printf("track %u lba %u\n", toc.tracks[i].number,
                               toc.tracks[i].lba);
```

A read streams chunks to a sink callback. **The chunk buffer is library-owned and
valid only for the duration of the call** — retain the pointer and you are
reading freed memory:

```c
static int sink(void *user, const accudisc_chunk *c)
{
        for (uint32_t s = 0; s < c->nsec; s++)
                fwrite(c->data + (size_t)s * c->sector_len, 1, c->audio_len,
                       user);
        return 0;               /* nonzero cancels the read */
}

accudisc_read_req   req = ACCUDISC_READ_REQ_INIT;
accudisc_read_stats st  = ACCUDISC_READ_STATS_INIT;

req.lba = 0;
req.count = 4000;
req.c2 = ACCUDISC_C2_PTRS;
req.c2_retries = 8;
req.status_map = map;           /* req.count bytes, or NULL */

int rc = accudisc_read_cdda(dev, &req, sink, pcm, &st);
```

Those two `_INIT` macros are not optional. `accudisc_read_req` and
`accudisc_read_stats` are caller-allocated, cross FFI boundaries, and have both
grown in place across releases (32 → 40 → 56 bytes and 80 → 104 → 128
respectively). A binding compiled against one header and loaded against a
different `.so` would have run off the end of a struct — in the worst case
reading garbage into a safety flag, which is well-formed data with the wrong
referent and nothing downstream able to detect it. So each struct carries a
leading `uint32_t size` and the caller sets it, and the library negotiates:
an older caller's smaller struct gets older behaviour, a newer caller asking for
fields this build does not have gets `ACCUDISC_ERR_ABI`, and a zero `size` — what
forgetting the macro produces — always fails loudly on the first call.

One more asymmetry worth knowing before writing a wrapper:
`ACCUDISC_WROTE_WITH_CAVEATS` is a **positive** return from `accudisc_write`, not
an error. The burn completed. Test `rc > 0`, never `rc != ACCUDISC_OK`.

**Python and Rust bindings are WIP.** `bindings/python/` and `bindings/rust/`
are placeholders. The design is settled — a streaming API layered over the sink
callback, generated from the public header — and explicitly *not* a subprocess
wrapper around the CLI. `docs/reference/cli-machine-interface.md` tabulates what
each CLI exit code means in library terms, which is the mapping a binding must
reproduce.

## Building

```sh
cmake -B build
cmake --build build
```

C11, `-Wall -Wextra`.

## Installing

```sh
cmake -B build -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build
sudo cmake --install build          # or: sudo make -C build install
```

Installs the CLI, the shared and static library, the public headers, the vendor
drivers (`plextor` today), the pkg-config file, both man pages, and the Python
binding. The prefix is honoured throughout — `/usr`, `/usr/local`, `~/.local`,
a staging tree — and `DESTDIR` works for packaging.

Two things happen at install time that a plain file copy would not do:

* **`strip`**, on the CLI, the shared library, the drivers and the Python
  extension. `libaccudisc.a` is deliberately exempt: a static archive's symbol
  table *is* its linking interface, and `--strip-unneeded` there breaks every
  static link against it. That exemption is why the files are named one at a
  time instead of using `cmake --install --strip`, which would strip the
  archive too.
* **`setcap cap_sys_rawio=ep`** on the installed CLI. Vendor opcodes and the
  burn path need it; the base read/rip path does not. This is the *durable*
  arming — a file capability binds to the **inode**, and the build tree gets a
  fresh one on every relink, so only the installed binary stays armed.

**Order matters and is enforced: strip first, setcap second.** `strip`
preserves the inode, but the kernel clears the `security.capability` xattr on
any write to the file, so the reverse order leaves the binary silently unarmed
— and unarmed fails quietly, falling back to generic MMC with no error. Check
with `getcap`, never by comparing inodes.

Under `DESTDIR` the setcap step is **skipped** with a message naming the
command a package's post-install script must run, because capabilities do not
survive tar/cpio.

Useful knobs:

| option | default | why you would change it |
|---|---|---|
| `ACCUDISC_INSTALL_RPATH` | the installed libdir | set **empty** for distro packaging targeting `/usr` — but read the warning below first |
| `ACCUDISC_SETCAP_ON_INSTALL` | `ON` | `OFF` if the target filesystem carries no capabilities, or you intend to run the tool as root |
| `ACCUDISC_INSTALL_PYTHON` | `ON` | `OFF` to skip the binding (it needs `python3` + `cffi` at build time) |
| `ACCUDISC_PYTHON_SITEDIR` | `<libdir>/pythonX.Y/site-packages` | a different layout, or a different interpreter's directory |

**An empty `ACCUDISC_INSTALL_RPATH` is only safe where the loader already
searches the install libdir.** It is the right setting for `/usr`, and it is
*wrong* anywhere `ld.so` will not find `libaccudisc.so.0` by itself. Measured:
an empty-RPATH install to a prefix outside `ld.so.conf` produces a CLI that
dies with

```
error while loading shared libraries: libaccudisc.so.0: cannot open shared
object file: No such file or directory
```

The default value exists precisely to avoid that, so change it only
deliberately. If you do — or if you install to `/usr/local` on a system whose
`ld.so.conf` does not cover it — run `ldconfig` afterwards, or add the
directory to `/etc/ld.so.conf.d/`. The install does **not** run `ldconfig`
itself: under `DESTDIR` it would be wrong, and outside it the cache is the
distribution's to manage.

**Changing the prefix requires a rebuild, not just a re-install.** The driver
search directory is compiled into the library
(`ACCUDISC_DRIVER_DIR_DEFAULT`), so a re-install to a new prefix would leave
the library looking for drivers under the old one.

### A wheel, for consumers that resolve dependencies

```sh
cmake --build build --target wheel      # -> build/bindings/python/wheel/*.whl
```

`cp310-abi3`, so one wheel serves CPython 3.10–3.14. Use it where a consumer's
installer (`pip`, `pipx`) has to *find* the package rather than compile it from
this checkout. It is **prefix-specific** — its `RUNPATH` is the prefix the build
tree was configured for — so it travels with the matching `make install`. See
`bindings/python/README.md` for the discovery rules and
`ACCUDISC_REQUIRE_INSTALLED`, which an installer should set so that "no
installed library found" fails loudly instead of silently linking a build tree.

## Signatures

Source files under `include/`, `src/`, `cli/` and `drivers/` may carry a
detached OpenPGP signature alongside them (`<file>.sig`), produced by this
project's automated security-audit agent. The public key ships in the repo, so
anyone can check one:

```sh
gpg --import docs/guardian_public.asc
gpg --verify src/mmc/mmc.c.sig src/mmc/mmc.c
```

Signing key `Guardian Security Agent <guardian@accudisc.local>`, fingerprint
`0041 E2FB 4258 7932 1C84 D24A 60A3 2C23 82E5 46AC`.

**What a signature asserts:** that this exact file content passed an automated
security audit on the signature date with no findings rated CRITICAL or HIGH.
That is all. **It is not a guarantee that the code is secure**, not a
third-party certification, and not a statement of authorship. An unsigned file
has simply not been audited at its current content — usually because it changed
since the last audit, which deletes the stale signature by design.

## License

**MIT** throughout (see `LICENSE`) — library, CLI, headers, bindings and the
vendor drivers under `drivers/`. Usable in any software, free or proprietary.

Vendor opcodes are functional hardware identifiers — facts, not copyrightable
expression — independently verified on hardware, with no third-party source
copied.

Data and technique sources are credited in `docs/reference/ATTRIBUTION.md`,
notably the drive read-offset table from the REDUMP Disc Preservation Project
(<https://redump.org>).

## Status

**Read path: functional.** TOC/session/CD-Text/Q-subchannel decode, C2 and
subchannel capture with frame-accurate status mapping, the composable recovery
levers, CD+G extraction, and the drive/media probes (`features`, `speeds`,
`c2lag`, `media`, `disc`).

**Write path: implemented and hardware-verified.** DAO burning of a full audio
session from a cdrdao `.toc` plus raw BIN, including the lead-in with MCN, ISRCs
and pregaps. Verified on a Plextor PX-716A against a Taiyo Yuden CD-R: a 19-track
/ 347,208-sector burn read back bit-exact, with the CD-Text blob byte-identical
in and out (760 bytes, 42 packs) and zero non-clean SG_IO status across the run.
CD-Text on write is currently pass-through of a raw `READ TOC` format-0x05 blob —
authoring CD-Text from strings or from `.toc` `CD_TEXT` blocks is designed but
not implemented, as is caller-supplied raw P–W subchannel and the vendor write
features (GigaRec, VariRec, PoweRec).

**Vendor drivers: two features shipping**, several more reverse-engineered and
pinned — see the section above, and note carefully what "verified" does and does
not mean there.

**Bindings: not started.** See `docs/reference/TODO.md` for the outstanding work.
