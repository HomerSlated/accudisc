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

## Drivers

- `plextor/` — Plextor 0xEA extensions: C1/C2/CU error-counter scan
  (Q-Check). Validated on PX-716A fw 1.11.
