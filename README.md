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

## Status

Early skeleton. Core reading engine is being imported from the `c2read`
prototype.
