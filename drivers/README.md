# AccuDisc vendor drivers

External modules carrying everything hardware-specific; the core library is
pure MMC/SG and never contains a vendor opcode. Contract and rationale:
`include/accudisc/driver.h`.

Each driver is one directory building one `accudisc-drv-<name>.so` via
`accudisc_add_driver(<name> <sources...>)` (see CMakeLists.txt). Drivers
include only the installed headers — never library internals — and export a
single symbol:

```c
const accudisc_driver *accudisc_driver_entry(void);
```

Lifecycle enforced by the library: `match()` against the INQUIRY identity,
then `selftest()` must prove the vendor path really works on this drive
(read/set/re-read actual device state) before any capability is used. Fail
anything → the device silently stays on generic MMC.

During development point the loader at the build output:

```sh
ACCUDISC_DRIVER_DIR=$PWD/build/drivers accudisc --driver auto info
```

## Licensing model

The AccuDisc core is MIT and must stay cleanly separable from every driver:

- a driver directory without its own license file is MIT like the core;
- a driver may instead carry its own `LICENSE.md` — including non-free
  status — without affecting the core or any other driver;
- drivers are never linked into libaccudisc (dlopen only), so a "free"
  distribution can simply omit individual driver files.

## Drivers

- `plextor/` — Plextor 0xEA extensions: C1/C2/CU error-counter scan
  (Q-Check). Validated on PX-716A fw 1.11. **MIT**, like the core — the vendor
  opcodes are functional hardware identifiers (facts, not copyrightable
  expression), independently verified on hardware with no third-party source
  copied; it is a separate module for vendor isolation, not licensing. See
  `plextor/LICENSE.md`. Intended to reach full parity with PlexTools-class
  access methods.
