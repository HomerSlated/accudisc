#ifndef ADSC_LAYOUT_H
#define ADSC_LAYOUT_H

#include <stdint.h>

/* READ CD record layout for a combined C2 + subchannel read, raw P-W or
 * formatted Q — which of the two trailing fields the drive delivers first, and
 * whether the records sit at the stride the CDB asked for. Pure functions over a buffer; no drive access, so
 * every branch is reachable from a test.
 *
 * WHY THIS EXISTS. MMC-4/5/6 make the order normative ("shall transfer ... C2
 * Error flags, Sub-channel"), and the whole library — engine, binding, every
 * consumer's slicer — was written against it. A LITE-ON LH-20A1S (9L08)
 * delivers AUDIO | SUB | C2 instead, measured 2026-09-14, and 6 of the 66
 * combined-read rows in redumper's drive database do the same (counted
 * 2026-09-19 against snapshot 856faf2; an earlier "8 of 55" here was a naive
 * grep that also counted the enum declaration and the string-map entry).
 *
 * THE TRAIT TRACKS FIRMWARE, NOT VENDOR OR CHIPSET, so no drive table can hold
 * it — and redumper's own data is the proof. SH-D163B and SH-D162C are adjacent
 * TSSTcorp models carrying the same KREON firmware from the same author, and
 * they DISAGREE on the order. All 27 MediaTek-tagged rows there use the
 * standard order, yet this LH-20A1S is independently confirmed MediaTek
 * (MT1899E, CdrInfo/Gough teardowns) and swaps. redumper itself has no runtime
 * detection in its rip path — a database lookup plus a manual
 * --drive-sector-order override — and this drive is absent from that database
 * altogether, so it would be misparsed there. Only the content
 * can decide: a Q frame carries a CRC-16, and a CRC that verifies at one
 * candidate position and not the other says where the subchannel is.
 *
 * FORMATTED Q (16 B) TOO, since 0.40.0. The same LITE-ON sends AUDIO | Q | C2
 * for C2 + formatted Q (measured 2026-09-16, 16/16 sectors). MMC-5 Table 368
 * makes the CRC in bytes 10-11 OPTIONAL — present or 00h — rather than absent,
 * and that drive fills it: its formatted Q bytes 0-11 were identical to the Q
 * de-interleaved from raw P-W, including a frame that failed CRC. So the check
 * is the same one, on bytes 0-11 taken directly. Bytes 12-15 are never looked
 * at; MMC calls 12-14 pad, and that drive puts non-zero bytes there. A drive
 * that omits the CRC gives no evidence at either position, and its buffer is
 * UNKNOWN like any other buffer without evidence. */

enum {
    ADSC_LAYOUT_UNKNOWN = 0,
    ADSC_LAYOUT_MMC = 1,       /* AUDIO | C2 | SUB — the standard's order */
    ADSC_LAYOUT_SUB_FIRST = 2  /* AUDIO | SUB | C2 */
};

/* Number of records in buf (nsec at stride) whose subchannel, taken at sub_off
 * within the record, carries a CRC-valid Q frame. sub_len is 96 (raw P-W: Q is
 * de-interleaved first) or 16 (formatted Q: bytes 0-11 are the frame); any
 * other value counts nothing. Records whose window would run past buf_len are
 * not counted. */
uint32_t adsc_layout_q_hits(const uint8_t *buf, uint32_t buf_len,
                            uint32_t nsec, uint32_t stride, uint32_t sub_off,
                            uint32_t sub_len);

typedef struct adsc_layout_verdict {
    int layout;            /* ADSC_LAYOUT_*: what THIS buffer shows */
    uint32_t hits_mmc;     /* CRC-valid Q at audio + c2_len */
    uint32_t hits_sub;     /* CRC-valid Q at audio */
    uint32_t misframed;    /* 0, or the stride the records actually sit at */
} adsc_layout_verdict;

/* Read what a buffer of nsec records at sector_len (2352 + c2_len + sub_len,
 * sub_len 96 or 16) shows. Any other shape is UNKNOWN and not misframed.
 *
 * `layout` is decided by EVIDENCE in this buffer only: hits at one position and
 * none (or a clear minority) at the other. A buffer with no CRC-valid Q at
 * either position — damaged subchannel, a drive that zero-fills P-W — is
 * UNKNOWN, never a default. Assuming the standard's order on no evidence is
 * the zero-sector failure DiscImageCreator ships.
 *
 * It keys on a POSITIVE CRC, never on a field being zero, so a silent sector
 * (LBA 0 of most discs, where combo probes read) is not a trap: the audio is
 * silent but its Q frame is not, and still verifies. Do not "fix" the probe
 * location on this detector's account.
 *
 * `misframed` is set when at least 4 records were read, fewer than half carry
 * valid Q at the expected stride under either order, and some stride in
 * (sector_len, sector_len + 16] explains at least half. That is the signature
 * of a transfer that padded each record (libata PIO on the LH-20A1S: 2742 ->
 * 2752) — audio still GOOD, every record after the first displaced. */
void adsc_layout_inspect(const uint8_t *buf, uint32_t nsec,
                         uint32_t sector_len, uint32_t c2_len,
                         adsc_layout_verdict *v);

/* Rewrite every record of a SUB_FIRST buffer into the standard's order, in
 * place, so every consumer slicing AUDIO | C2 | SUB stays correct. A record
 * shape other than 2352 + c2_len + (96 or 16) is left untouched. */
void adsc_layout_to_mmc(uint8_t *buf, uint32_t nsec, uint32_t sector_len,
                        uint32_t c2_len);

#endif /* ADSC_LAYOUT_H */
