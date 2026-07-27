# accudisc — Python bindings

`cffi` bindings generated from `include/accudisc/accudisc.h`.

## Build

The C library must be built first; the binding links it.

```sh
cmake -B build && cmake --build build          # from the repo root
cd bindings/python && python3 build_accudisc.py
PYTHONPATH=. python3 tests/test_binding.py      # 33 tests, no drive needed
```

`ctest -R python_binding` does all of that as part of the normal test run, and
*skips* (rather than fails) when `python3` or `cffi` is missing.

Header and library discovery, in order: `ACCUDISC_INCLUDE_DIR` /
`ACCUDISC_LIB_DIR` → `pkg-config accudisc` → this checkout's `include/` and
`build/src/`. The third is the working default because the library is not
installed yet; an RPATH is set so `libaccudisc.so.0` is found without
`LD_LIBRARY_PATH`.

`pip install .` works and was verified into a clean venv — but note **what the
RPATH means for distribution**. Built against an uninstalled tree, the
extension carries an *absolute* `RUNPATH` to that build directory:

```
$ readelf -d …/site-packages/accudisc/_accudisc*.so | grep RUNPATH
  RUNPATH  [/home/kgr/Git/accudisc/build/src]
```

That is correct for this machine and meaningless anywhere else. So the install
is not relocatable until the library is installed properly and found via
`pkg-config` (repo TODO task 2). For CI, build the C library in the same job
and install the binding from source; do not copy the built package between
machines.

## Why API mode

`ffi.set_source` with a real `#include`, never `ffi.dlopen`. In ABI mode every
struct layout would be transcribed by hand out of a 1520-line header, and a
field declared in the wrong order or width produces plausible numbers with no
exception — this project's dominant failure class arriving through the FFI.
In API mode the C compiler resolves every offset, and a mismatched declaration
raises at import naming the field and both sizes:

```
ffi.error: struct accudisc_read_req: wrong size for field 'lba'
           (cdef says 8, but C compiler says 4)
```

That is measured, not assumed — the check was verified by deliberately
declaring a wrong type and confirming the failure.

## Usage

```python
import accudisc as ad

with ad.Device("/dev/sr0") as dev:
    print(dev.identify(), dev.access_method)

    toc, info = dev.read_toc_src()
    if not toc.trusted:                 # copy-protection / malformed lead-in
        raise SystemExit(f"untrusted geometry: {toc.anomalies!r}")
    if info.degraded:
        print(f"lead-in degraded: {info.degrade.token}")

    session = ad.Device.default_audio_session(toc)
    lba, count = ad.Device.session_audio_range(toc, session)

    # A bounded span, straight into memory — no sink, no temp file.
    data, result = dev.read_span(lba, 4000, c2=ad.C2.PTRS)
    print(len(data), result.stats.sectors_flagged)
```

Streaming, for spans too large to hold:

```python
def sink(chunk):
    out.write(chunk.data)               # valid ONLY during this call

result = dev.read(lba, count, sink=sink, c2=ad.C2.PTRS,
                  sub=ad.Sub.RAW, status_map=True)
print(result.state_counts())
```

## Three things worth knowing before you use it

**`chunk.data` dies when your sink returns.** By default it is `bytes` and
yours to keep. With `copy=False` it is a `memoryview` over library memory,
released on return, so a stored reference raises `ValueError` instead of
reading freed memory as plausible PCM. A *slice* you took of it escapes that
release — measured, and the reason `copy=True` is the default. Copy what you
need inside the call.

**A raising sink cancels the read.** cffi's default callback would print the
traceback and return 0, which the engine reads as *continue*; the trampoline
returns nonzero instead and re-raises your exception from `read()`, so the
traceback names the cause rather than "cancelled".

**Status-map severity is not comparable across states.** The high nibble means
~log2 of fired C2 bits for `C2`, ~log2 of differing bytes for `SUSPECT`, and a
raw reread count for `RECOVERED`. Ranking sectors by it across mixed states
compares three different quantities. And every state is a *relative* claim —
absolute verification (AccurateRip, CTDB) is the calling application's job and
always outranks anything in the map.

## What is deliberately absent

* **No subprocess, no `--progress-fd` parsing, no exit codes.** Those are
  process conventions that stay in the CLI by design (API_PLAN §3); the mapping
  is `docs/reference/cli-machine-interface.md`.
* **No whole-disc-to-file fast path.** `read_to_file` exists, but for a whole
  disc the CLI's `--pcm` writes the file inside the library's own address
  space, which this cannot beat — it routes every sector through Python first.
* **`accudisc_probe_speed_ladder` is not bound yet**, on purpose:
  `accudisc_speed_rung` has no `size` field and a pending change grows it. See
  `docs/reference/TODO.md`.
* **`accudisc_write` is not bound yet** — the one destructive path, and it
  should not ride along with a freshly written read path.
