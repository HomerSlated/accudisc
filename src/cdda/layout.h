#ifndef ADSC_LAYOUT_H
#define ADSC_LAYOUT_H

#include <stdint.h>

/* READ CD record layout for a combined C2 + raw P-W read — which of the two
 * trailing fields the drive delivers first, and whether the records sit at the
 * stride the CDB asked for. Pure functions over a buffer; no drive access, so
 * every branch is reachable from a test.
 *
 * WHY THIS EXISTS. MMC-4/5/6 make the order normative ("shall transfer ... C2
 * Error flags, Sub-channel"), and the whole library — engine, binding, every
 * consumer's slicer — was written against it. A LITE-ON LH-20A1S (9L08)
 * delivers AUDIO | SUB | C2 instead, measured 2026-09-14, and 8 of redumper's
 * 55 combined-read drives do the same. The order also varies with firmware and
 * command on the same model, so no drive table can hold it. Only the content
 * can: raw P-W carries a CRC-16 over each Q frame, and a CRC that verifies at
 * one candidate position and not the other says where the subchannel is. */

enum {
    ADSC_LAYOUT_UNKNOWN = 0,
    ADSC_LAYOUT_MMC = 1,       /* AUDIO | C2 | SUB — the standard's order */
    ADSC_LAYOUT_SUB_FIRST = 2  /* AUDIO | SUB | C2 */
};

/* Number of records in buf (nsec at stride) whose raw P-W, taken at sub_off
 * within the record, carries a CRC-valid Q frame. Records whose window would
 * run past buf_len are not counted. */
uint32_t adsc_layout_q_hits(const uint8_t *buf, uint32_t buf_len,
                            uint32_t nsec, uint32_t stride, uint32_t sub_off);

typedef struct adsc_layout_verdict {
    int layout;            /* ADSC_LAYOUT_*: what THIS buffer shows */
    uint32_t hits_mmc;     /* CRC-valid Q at audio + c2_len */
    uint32_t hits_sub;     /* CRC-valid Q at audio */
    uint32_t misframed;    /* 0, or the stride the records actually sit at */
} adsc_layout_verdict;

/* Read what a buffer of nsec records at sector_len (2352 + c2_len + 96) shows.
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
 * place, so every consumer slicing AUDIO | C2 | SUB stays correct. */
void adsc_layout_to_mmc(uint8_t *buf, uint32_t nsec, uint32_t sector_len,
                        uint32_t c2_len);

#endif /* ADSC_LAYOUT_H */
