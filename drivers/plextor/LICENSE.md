# Plextor driver — licensing status: unlicensed (non-free)

This driver is NOT covered by AccuDisc's MIT license.

It implements Plextor's proprietary vendor-opcode interfaces (the access
methods used by PlexTools and other Plextor-licensed applications such as
Nero). The knowledge embodied here comes from:

- the Plextor opcodes long documented in cdrtools (readcd) and cdrdao;
- first-party reverse engineering of Plextor hardware and firmware —
  probed hardware opcodes only, with no redistribution of vendor binaries
  or sources.

Plextor no longer exists as an optical-drive manufacturer and all vendor
licenses have expired; the protocol itself is abandonware. The clean-room
provenance above infringes no copyright, but the formal legal standing of
the protocol is **unlicensed — redistribution rights unknown** (comparable
to components distributed in "non-free"/"ugly" package categories).

Consequences, by design:

- this driver is built and shipped as a standalone module
  (`accudisc-drv-plextor.so`), never linked into the MIT core;
- distributions wanting a purely free package set can ship AccuDisc
  without this file and lose nothing but Plextor-specific extras;
- any legal challenge to this driver affects neither the core nor any
  other driver.
