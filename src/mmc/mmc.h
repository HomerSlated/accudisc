/* MMC command execution over the transport: CDB builders from cdb.h wired to
 * adsc_dev_exec(). Ports of the command patterns proven in c2read.c. */

#ifndef ADSC_MMC_H
#define ADSC_MMC_H

#include <stdint.h>

#include "../internal.h"
#include "cdb.h"

int adsc_mmc_inquiry(struct accudisc_device *dev, accudisc_drive_id *out);

/* READ TOC/PMA/ATIP with two-step allocation (4-byte header for the data
 * length, then the full response). *out is malloc'd (caller frees), *out_len
 * is the full response including the 2-byte length field. Returns
 * ACCUDISC_ERR_SHORT for an empty response (e.g. no CD-Text on disc). */
int adsc_mmc_read_toc_raw(struct accudisc_device *dev, unsigned format,
                          unsigned time_bit, unsigned track,
                          uint8_t **out, uint32_t *out_len);

/* One READ CD for nsec sectors into buf (nsec * sector_len bytes, where
 * sector_len must equal adsc_read_cd_sector_len(c2, sub)). Per-sector field
 * order is AUDIO, C2, SUB (probed on PX-716A; matches redumper
 * SectorOrder::DATA_C2_SUB — reprobe per drive before trusting). Returns
 * ACCUDISC_ERR_SHORT if the drive completes with GOOD status but transfers
 * fewer than nsec*sector_len bytes — buf is then partly stale and untrusted. */
int adsc_mmc_read_cd(struct accudisc_device *dev, uint32_t lba, uint32_t nsec,
                     unsigned sector_type, unsigned c2, unsigned sub,
                     void *buf, uint32_t sector_len);

/* MODE SENSE(10) for one page, two-step allocation. On success *len is the
 * total bytes read (mode header + block descriptors + page) and *page_off
 * the offset of the page itself (8 + block-descriptor length). buf must
 * hold cap bytes; cap >= 16 required. */
int adsc_mmc_mode_sense10(struct accudisc_device *dev, unsigned page,
                          uint8_t *buf, uint32_t cap,
                          uint32_t *len, uint32_t *page_off);

/* MODE SELECT(10) sending back a (modified) MODE SENSE(10) response; zeroes
 * the mode data length and clears the page's PS bit per SPC before sending.
 * Requires an ACCUDISC_OPEN_RDWR device. */
int adsc_mmc_mode_select10(struct accudisc_device *dev, uint8_t *buf,
                           uint32_t len, uint32_t page_off);

/* GET CONFIGURATION for a single feature (RT=10b). *out receives the raw
 * response (8-byte header + first descriptor), cap bytes available. */
int adsc_mmc_get_configuration(struct accudisc_device *dev, uint16_t feature,
                               uint8_t *out, uint32_t cap);

/* START STOP UNIT; start=0, loej=0 spins the spindle down without ejecting. */
int adsc_mmc_start_stop(struct accudisc_device *dev, unsigned start,
                        unsigned loej);

/* READ DISC INFORMATION (standard). Fills up to cap bytes of the response
 * into buf; *len is the byte count read. */
int adsc_mmc_read_disc_info(struct accudisc_device *dev, uint8_t *buf,
                            uint32_t cap, uint32_t *len);

/* WRITE(10): nblocks of block_bytes each from buf, to absolute lba (signed;
 * the DAO lead-in gap starts at -150). */
int adsc_mmc_write10(struct accudisc_device *dev, int32_t lba,
                     uint32_t nblocks, const void *buf, uint32_t block_bytes);

/* SYNCHRONIZE CACHE — flush the write buffer (finish DAO). */
int adsc_mmc_sync_cache(struct accudisc_device *dev);

/* SEND CUE SHEET — the DAO layout descriptor (8 bytes/entry). */
int adsc_mmc_send_cue_sheet(struct accudisc_device *dev, const uint8_t *cue,
                            uint32_t len);

/* SEND OPC INFORMATION (DoOPC) — power calibration before a real burn. */
int adsc_mmc_send_opc(struct accudisc_device *dev);

/* SET STREAMING (0xB6): command the drive's read-speed *ceiling* over an LBA
 * range via a performance descriptor {Start, End, Read Size, Read Time}. This
 * is the path the PX-716A honours (SET CD SPEED / CDROM_SELECT_SPEED is
 * advisory on it). speed_x is Nx (1x = 176.4 kB/s); with the Exact bit clear
 * the drive runs CAV under this ceiling. speed_x == 0 restores drive defaults
 * (RDD). start/end are absolute LBAs; end == 0xFFFFFFFF means "to end of disc".
 * exact != 0 sets the Exact bit (pin the rate = CLV). Data-OUT; may require
 * CAP_SYS_RAWIO (see the setcap build target). */
int adsc_mmc_set_streaming(struct accudisc_device *dev, unsigned speed_x,
                           uint32_t start_lba, uint32_t end_lba, unsigned exact);

/* GET PERFORMANCE (0xAC), nominal-performance curve. Fills buf with the raw
 * response (8-byte header + N x 16-byte descriptors), sets *len to bytes
 * returned. max_desc bounds the request; caller sizes buf as 8 + max_desc*16. */
int adsc_mmc_get_performance(struct accudisc_device *dev, uint32_t start_lba,
                             uint16_t max_desc, uint8_t *buf, uint32_t cap,
                             uint32_t *len);

#endif /* ADSC_MMC_H */
