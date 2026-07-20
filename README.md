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

## License

The core (library, CLI, headers, bindings) is **MIT** — usable in any
software, free or proprietary. Vendor drivers under `drivers/` are
standalone dlopen'd modules licensed individually; the Plextor driver is
non-free (see `drivers/plextor/LICENSE.md`). Data and technique sources are
credited in `docs/reference/ATTRIBUTION.md`, notably the drive read-offset table from
the REDUMP Disc Preservation Project (https://redump.org).

## Status

Read side functional: TOC/session/CD-Text/Q-subchannel decode, C2 +
subchannel capture with frame-accurate status mapping, vendor-driver
architecture with Plextor error-counter scan. Write path upcoming.
