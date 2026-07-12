/* Pure MMC/SPC CDB builders: fill a caller-provided CDB buffer, touch no
 * hardware. Byte layouts pinned from c2read.c (itself pinned from redumper
 * scsi/mmc.ixx), built with explicit shifts so they are compiler-independent
 * and unit-testable without a drive. */

#ifndef ADSC_CDB_H
#define ADSC_CDB_H

#include <stdint.h>

/* Opcodes */
#define ADSC_OP_INQUIRY       0x12
#define ADSC_OP_START_STOP    0x1B
#define ADSC_OP_READ_TOC      0x43
#define ADSC_OP_GET_CONFIG    0x46
#define ADSC_OP_MODE_SELECT10 0x55
#define ADSC_OP_MODE_SENSE10  0x5A
#define ADSC_OP_READ_CD       0xBE

/* READ TOC/PMA/ATIP formats */
#define ADSC_TOC_FMT_TOC     0x00 /* track descriptors */
#define ADSC_TOC_FMT_FULL    0x02 /* raw ("full") TOC incl. sessions */
#define ADSC_TOC_FMT_ATIP    0x04 /* ATIP: recordable disc pregroove info */
#define ADSC_TOC_FMT_CDTEXT  0x05 /* CD-Text packs from the lead-in */

/* READ CD expected sector type (CDB byte 1, bits 4-2) */
#define ADSC_SECTOR_ANY   0x0
#define ADSC_SECTOR_CDDA  0x1

/* READ CD C2 error field selection (CDB byte 9, bits 2-1) */
#define ADSC_C2_NONE 0x0
#define ADSC_C2_294  0x1 /* C2 pointer bits */
#define ADSC_C2_296  0x2 /* C2 + block-error bits */

/* READ CD sub-channel selection (CDB byte 10, bits 2-0) */
#define ADSC_SUB_NONE 0x0
#define ADSC_SUB_RAW  0x1 /* raw interleaved P-W, 96 B */
#define ADSC_SUB_Q    0x2 /* formatted Q, 16 B */

#define ADSC_LEADOUT_TRACK 0xAA

void adsc_cdb_inquiry(uint8_t cdb[6], uint8_t alloc);

/* start: 1 = spin up, 0 = spin down; loej: 1 = eject/load with start. */
void adsc_cdb_start_stop(uint8_t cdb[6], unsigned start, unsigned loej);

/* time_bit selects MSF addressing (format-dependent); track doubles as the
 * session number for ADSC_TOC_FMT_FULL. */
void adsc_cdb_read_toc(uint8_t cdb[10], unsigned format, unsigned time_bit,
                       unsigned track, uint16_t alloc);

/* rt: 10b = return only the named feature (the mode c2read uses). */
void adsc_cdb_get_configuration(uint8_t cdb[10], unsigned rt,
                                uint16_t feature, uint16_t alloc);

void adsc_cdb_mode_sense10(uint8_t cdb[10], unsigned page, uint16_t alloc);
void adsc_cdb_mode_select10(uint8_t cdb[10], uint16_t param_len);

/* Always sets include-user-data (byte 9 bit 4); c2 = ADSC_C2_*,
 * sub = ADSC_SUB_*, sector_type = ADSC_SECTOR_*. */
void adsc_cdb_read_cd(uint8_t cdb[12], uint32_t lba, uint32_t nsec,
                      unsigned sector_type, unsigned c2, unsigned sub);

/* Transfer length per sector for a given C2/sub-channel selection. */
uint32_t adsc_read_cd_sector_len(unsigned c2, unsigned sub);

#endif /* ADSC_CDB_H */
