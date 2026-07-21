# AccuDisc

Precision Red Book CD-DA reader/writer.

AccuDisc reads audio CDs (and the audio portion of Mixed Mode discs) with the
full range of data and metadata the format carries — TOC, subchannel P–W, C2
error pointers, CD-Text, ISRC, MCN, pregaps and indices — and writes Red Book
CD-R/RW. It talks to drives via MMC (SG_IO/ioctl) and proprietary vendor
opcodes, with per-drive knowledge of offsets, caching, and quirks.

It deliberately does **not** process the audio further: tagging, lookup
(MusicBrainz etc.), and encoding belong to other tools.

## Components

| Component | Path | Description |
|---|---|---|
| `libaccudisc` | `src/`, `include/accudisc/` | Shared library + public C API |
| `accudisc` | `cli/` | Command-line interface |
| Python bindings | `bindings/python/` | |
| Rust bindings | `bindings/rust/` | |

## Building

```sh
cmake -B build
cmake --build build
```

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

**MIT** throughout (see `LICENSE`) — library, CLI, headers, bindings, and the
vendor drivers under `drivers/` — usable in any software, free or proprietary.
Drivers are standalone dlopen'd modules for **architectural** reasons (vendor
isolation: the core stays pure MMC/SG, hardware-specific opcodes live in
drivers), not licensing ones. The Plextor driver is MIT like the rest; its
vendor opcodes are functional hardware identifiers — facts, not copyrightable
expression — independently verified on hardware, with no third-party source
copied (see `drivers/plextor/LICENSE.md`). Data and technique sources are
credited in `docs/reference/ATTRIBUTION.md`, notably the drive read-offset
table from the REDUMP Disc Preservation Project (https://redump.org).

## Status

Read side functional: TOC/session/CD-Text/Q-subchannel decode, C2 +
subchannel capture with frame-accurate status mapping, vendor-driver
architecture with Plextor error-counter scan. Write path upcoming.
