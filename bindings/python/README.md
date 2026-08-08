# accudisc — Python bindings

`cffi` bindings generated from `include/accudisc/accudisc.h`.

## Build

The C library must be built first; the binding links it.

```sh
cmake -B build && cmake --build build          # from the repo root
cd bindings/python && python3 build_accudisc.py
PYTHONPATH=. python3 tests/test_binding.py      # 55 tests, no drive needed
```

`ctest -R python_binding` does all of that as part of the normal test run, and
*skips* (rather than fails) when `python3` or `cffi` is missing.

## Discovery — which library you actually linked

In order, first match wins:

| # | source | notes |
|---|---|---|
| 1 | `ACCUDISC_INCLUDE_DIR` + `ACCUDISC_LIB_DIR` | explicit; silent |
| 2 | `pkg-config accudisc` | **the supported route for a consumer** |
| 3 | this checkout's `include/` + `build/src/` | development only; **warns** |

**`pkg-config` beats the build tree whenever it answers.** The hazard is when it
*doesn't*: on a developer box that has never run `make install` there is no
`accudisc.pc` anywhere on the search path, so a consumer's install falls through
to (3) and silently produces a working extension bound to a build tree. It
behaves identically until that tree moves.

Two things guard it now:

* Branch 3 **warns on stderr, every time**, naming the tree it chose.
* `ACCUDISC_REQUIRE_INSTALLED=1` makes branch 3 a **hard error**. Installers
  should set it — then "no installed library" fails at build time with a
  message, instead of at import time with a missing `libaccudisc.so.0` that
  names nothing leading back to the choice.

So the answer to "what should an installer do": set
`ACCUDISC_REQUIRE_INSTALLED=1` and put the library's `.pc` on
`PKG_CONFIG_PATH` if the prefix is not a default one. There is no intended
fallback beyond that — failing loudly is the design.

## Install

`make install` from the repo root installs the binding along with everything
else — see the top-level README. It builds a **separate** extension whose
`RUNPATH` points at the *installed* libdir, staged in the build tree rather
than written over `bindings/python/accudisc/`: the development extension there
must keep pointing at `build/src` for the test suite, and the two files differ
in exactly one respect that nothing at import time can check.

**Runtime path is separately settable, and that is the whole of the
relocatability fix.** `ACCUDISC_RUNTIME_LIB_DIR` controls what gets recorded:

| value | effect |
|---|---|
| unset | follows the link directory (development default) |
| a path | record exactly that `RUNPATH` — the installed libdir |
| set and **empty** | record **no** `RUNPATH`; let `ld.so` resolve it |

The empty case is an instruction, not an absence, which is why the code tests
for membership rather than truthiness. It is what a distro packager targeting
`/usr` wants — **and only works where the loader already searches the install
libdir.** Measured: empty `RUNPATH` plus a prefix outside `ld.so.conf` gives
`ImportError: libaccudisc.so.0: cannot open shared object file`, the same
failure as the non-relocatable build it replaced. Run `ldconfig`, or leave the
default. Under CMake this is driven by `ACCUDISC_INSTALL_RPATH`.

### pip / pipx

Once the C library is installed and visible to `pkg-config`, a plain
`pip install bindings/python` produces a **relocatable** extension — discovery
takes the `pkg-config` branch, so the `RUNPATH` names the installed libdir and
nothing points into a build tree. Verified end-to-end: a fresh venv, the build
tree moved aside, `python3 -c 'import accudisc'` from `/`.

```sh
PKG_CONFIG_PATH=<prefix>/lib64/pkgconfig pip install ./bindings/python
```

**Delete `bindings/python/build/` before a `pip install` whose RUNPATH differs
from the last one.** `setup.py` goes through `cffi_modules` → setuptools
`build_ext`, which rebuilds on file timestamps and never runs this package's
own stale-extension cleanup; a leftover `build/lib*/` is packaged as-is. That
is guarded — the RUNPATH is stamped into the generated C so a change to it
changes the source and forces a relink — but the guard costs nothing to
double-check and the failure it prevents is silent:

```
with the stamp:     /opt/aaa, then /opt/bbb   (correct)
without it:         /opt/aaa, then /opt/aaa   (silently stale)
```

**Do not both `make install` and `pip install` the binding.** Two copies of
`accudisc/` on one interpreter is the §113 stale-extension bug at package
granularity — whichever wins the path is the one you get, and neither is
labelled. Pick one.

### Wheel

```sh
cmake --build build --target wheel     # -> build/bindings/python/wheel/*.whl
```

Produces `accudisc-<version>-cp310-abi3-linux_x86_64.whl`. One wheel serves
CPython 3.10 through 3.14 — verified by building on 3.14 and installing on
3.10.20 with the build tree moved away.

**The `cp310-abi3` tag comes from `setup.cfg`, and it is not optional.** The
extension has always been limited-API, but without `[bdist_wheel] py_limited_api
= cp310` the *wheel* is tagged for the building interpreter — the first one
built here came out `cp314-cp314`, which pip refuses to install on 3.10 even
though the `.so` inside it works there. An abi3 wheel declares the **oldest**
CPython it supports, so the floor belongs in that tag, not the builder's
version.

**The wheel is prefix-specific.** Its `RUNPATH` is `ACCUDISC_INSTALL_RPATH` —
the prefix the build tree was configured for — so a wheel travels with the
`make install` that matches it. Installing one against a different prefix
reproduces the exact failure the staging split exists to prevent. It does not
require the library to be installed to *build*: it links the build tree and
records the install RUNPATH.

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
                  sub=ad.Sub.RAW, status_map=True, subq_map=True)
print(result.state_counts())        # audio health, MapState
print(result.subq_state_counts())   # Q health, SubQState
```

Those two maps are readable **after** the call. To draw a disc map *while the
read runs*, pass your own buffer instead of `True` — you hold the reference
before the call, so another thread can poll the same bytes the engine is
writing:

```python
buf = bytearray(count)
t = threading.Thread(target=dev.read, kwargs=dict(
        lba=lba, count=count, sub=ad.Sub.RAW, status_map=buf))
t.start()
while t.is_alive():
    render(buf)          # measured filling 26 -> 1950 of 2000 during one read
    time.sleep(0.15)
```

Any writable buffer works — `bytearray`, `mmap`, or a `memoryview` of one — and
an `mmap` of a file gives **cross-process** watching, the same mechanism as the
CLI's `--map-file`. The length must be exact; for one whole-disc buffer pass
`memoryview(buf)[lba:lba + count]`. Note `mmap` slicing needs the memoryview
form: `m[a:b]` returns immutable `bytes` and is refused.

`subq_map=True` needs `sub=Sub.RAW` and raises `InvalidArgument` otherwise. The
two lanes are independent — a sector can be audio-clean with a dead Q — and
their state numbering is parallel but **not interchangeable**: decode the Q lane
with `subq_state()`, never `map_state()`, which would silently report
`NO_AUDIO` as `RECOVERED`. Note also that `SubQState.NO_POSITION` is *healthy*:
it is an interleaved MCN or ISRC frame, around 1% of sectors on a disc that
carries them, so a health figure is `OK + NO_POSITION`.

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

## Integrating behind a transport switch

`AbiMismatch` is its own exception type so a consumer can catch exactly the
"stale extension against a newer `.so`" case and fall back, rather than failing
a rip. There are **three** ways to not have a usable binding, not one, and the
middle one does not raise anything at all:

```python
try:
    import accudisc
except ImportError:
    transport = Subprocess()            # 1. genuinely absent
else:
    if not all(hasattr(accudisc, n) for n in ("Device", "AbiMismatch")):
        transport = Subprocess()        # 2. imported, but not us — see below
    else:
        try:
            transport = Binding()       # constructs a Device
        except accudisc.AbiMismatch:
            transport = Subprocess()    # 3. stale extension vs newer .so
```

Every branch assigns `transport`, and `accudisc` is only read on the path where
the import actually succeeded. Both matter:

**Do not collapse it into `except (ImportError, accudisc.AbiMismatch)`.** If the
*import* is what failed, the name `accudisc` is unbound when the exception tuple
is evaluated, so you get a `NameError` naming the wrong problem entirely — in
exactly the situation the handler was written for.

**`import accudisc` can succeed and give you something that is not the
binding.** A *directory* named `accudisc` anywhere on `sys.path` is recorded as
a PEP 420 namespace portion: the import completes, `__file__` is `None`, and
there is no `Device`. Nothing raises, so no `except` clause can see it, and the
failure surfaces at the first attribute access arbitrarily far from the import.
The case that bites is **this repository's own root**, from its parent
directory:

```
$ cd ~/Git && python3 -c "import accudisc; print(accudisc)"
<module 'accudisc' (namespace) from ['/home/kgr/Git/accudisc']>
```

A real package later on `sys.path` does win the scan, so a correct environment
never sees this — which is precisely why the hazard stays invisible until an
interpreter without the binding installed hits it. Hence step 2: an *identity*
check, not a version check. Version skew is what `AbiMismatch` is for, and it
cannot fire on an object that has no `Device` to call. (Reported by cdda2img,
whose own harness had reasoned the phantom harmless; pinned here by
`test_imported_package_is_the_binding_not_a_namespace_phantom`.)

**One extension serves every CPython >= 3.2.** `set_source` passes
`py_limited_api=True`, so the build produces a single `_accudisc.abi3.so` rather
than one file per interpreter, and `build_accudisc.py` deletes stale siblings so
exactly one exists and it is always current. Verified rather than assumed: an
extension built on 3.14.6 imports and passes all 55 tests on 3.10.20.

Both halves are needed. "Exactly one extension, always current" is true but
*insufficient* on its own, because a per-interpreter extension makes the last
interpreter to build the only one that can import — which is how cdda2img's 3.10
venv ended up unable to load a binding built here on 3.14.

**Running under `uv`:** pass `--no-project`.

```sh
ACCUDISC_INCLUDE_DIR=…/accudisc/include ACCUDISC_LIB_DIR=…/accudisc/build/src \
uv run --no-project --python 3.14 --with cffi --with …/accudisc/bindings/python \
    python your_script.py
```

Without `--no-project`, `uv run --python 3.14` **deletes and recreates the
surrounding project's `.venv`** at that version, and the next `uv run` silently
uses a different interpreter than the one you thought you pinned. (Found the
hard way by cdda2img, not by us.)

## What is deliberately absent

* **No subprocess, no `--progress-fd` parsing, no exit codes.** Those are
  process conventions that stay in the CLI by design (API_PLAN §3); the mapping
  is `docs/reference/cli-machine-interface.md`.
* **No whole-disc-to-file fast path, and none is needed.** `read_to_file`
  routes every sector through Python. Measured A/B/A over a whole disc
  (cdda2img, 2026-07-29): binding 112.69 s vs subprocess 112.75 s adjacent,
  against a 3.68 s subprocess-vs-subprocess noise floor — a **0.06 s**
  difference over 112 seconds, with PCM and C2 byte-identical. The drive is
  the bottleneck at 40x, not the sink, so there is deliberately no library-side
  entry point. One drive, one disc, one speed.

Both previously-absent calls are now bound (2026-07-29):

* **`accudisc_probe_speed_ladder`** — `Device.probe_speed_ladder()`. Binding it
  spent the free ABI window on `accudisc_speed_rung`; that struct is frozen at
  14 bytes and growing it now costs a version break.
* **`accudisc_write`** — `Device.write()`. Read `WriteResult` before using it:
  a **return** means the disc was written, an **exception** means it was not,
  and `WriteResult.CAVEATS` is a completed burn that needs surfacing rather
  than a failure.

### What the write binding has been run against

Both paths, on a device:

* **Refusal**, on the PX-716A with a non-blank disc: the guarded
  `accudisc_write_opts` is accepted, the call reaches the library's blank check,
  and `Unsupported` is raised — the binding equivalent of `result=not_blank`.
* **Success**, on a CDEmu virtual blank: `WriteResult.OK` returned, the progress
  callback fired 6 times ending at `(150, 150)`, and a **full round-trip**
  through the binding — write, then `read_span` of the burned track — came back
  **byte-identical** (blake2b `0b2d87d2…`, 0 flagged, 0 hard errors).

To reproduce the blank:

```sh
cdemu create-blank --writer-id=WRITER-TOC --medium-type=cdr74 0 /var/tmp/cdr
# ... burn to /dev/sr1 ... then to read it back:
cdemu unload 0 && cdemu load 0 /var/tmp/cdr.toc
```

**`WriteResult.CAVEATS` remains device-free-only.** Reaching it needs a CD-Text
blob whose SIZE_INFO disagrees with the `.toc`, which the round-trip above does
not construct. The mapping is tested; the path is not.
