# Plextor driver — licensing: MIT (same as the core)

This driver is covered by AccuDisc's **MIT** license, like the rest of the
package. It is a separate dlopen module for **architectural** reasons (vendor
isolation — the core stays pure MMC/SG, hardware-specific opcodes live in
drivers), not for licensing reasons.

## Why MIT is correct here

What this driver embodies is a set of **functional hardware identifiers** —
SCSI vendor opcode numbers and CDB field layouts for Plextor drives. These are
facts about how the hardware behaves, not copyrightable expression. They were
obfuscated and hard to obtain, but that does not make them ownable.

No third-party source code is copied. The command set is documented in several
places we used as *references* — **QPxTool** (GPL-2.0), cdrtools, and cdrdao —
and every command here was independently verified by raw SG_IO against the
owner's own PX-716A. Citing those references is courtesy (see
`../../docs/reference/ATTRIBUTION.md`); it is not a derivation, so no copyleft attaches.

The accompanying `PROTOCOL.md` / `FEATURES.md` and the RE notes record how the
command set was confirmed on real hardware.
