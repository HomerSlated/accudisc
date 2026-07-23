/* SCSI pass-through transport. Linux SG_IO today; the interface is the seam
 * for other backends (and for the vendor-opcode drivers) later. */

#ifndef ADSC_TRANSPORT_H
#define ADSC_TRANSPORT_H

#include <stdint.h>

#include <accudisc/accudisc.h>

#define ADSC_SENSE_LEN 32
#define ADSC_CDB_MAX 16

/* Generous read timeout: a disc defect can trigger long in-drive retry
 * cycles before the command completes or fails. */
#define ADSC_TIMEOUT_READ_MS 60000
#define ADSC_TIMEOUT_CTRL_MS 30000
#define ADSC_TIMEOUT_WRITE_MS 120000

typedef enum adsc_xfer_dir {
    ADSC_XFER_NONE,
    ADSC_XFER_IN,  /* device -> host */
    ADSC_XFER_OUT  /* host -> device */
} adsc_xfer_dir;

typedef struct adsc_transport {
    int fd;
    int rw; /* opened for write (vendor opcodes / MODE SELECT allowed) */
} adsc_transport;

/* One SCSI command. Fill cdb/cdb_len/dir/buf/buf_len/timeout_ms; on return
 * sense_len > 0 means the drive supplied sense data (raw, undecoded). */
typedef struct adsc_cmd {
    uint8_t cdb[ADSC_CDB_MAX];
    uint8_t cdb_len;
    adsc_xfer_dir dir;
    void *buf;
    uint32_t buf_len;
    uint32_t timeout_ms;
    /* out */
    uint8_t sense[ADSC_SENSE_LEN];
    uint8_t sense_len;
    uint32_t resid; /* bytes NOT transferred (buf_len - actual); 0 = full */
} adsc_cmd;

/* Clamp a raw SG_IO residual (dxfer_len - bytes actually transferred) to
 * [0, buf_len]. A negative residual (a nonsensical over-transfer) and one
 * larger than the buffer both clamp, so a caller always gets a trustworthy
 * count of the bytes that did NOT arrive. */
static inline uint32_t adsc_resid_clamp(int raw, uint32_t buf_len)
{
    if (raw <= 0)
        return 0;
    return (uint32_t)raw > buf_len ? buf_len : (uint32_t)raw;
}

/* Post-process an exec result for a FIXED-length data-IN command (READ CD:
 * exactly buf_len bytes must arrive). A GOOD-status transfer that fell short
 * (resid != 0) left stale bytes in the buffer tail, so it is promoted to
 * ACCUDISC_ERR_SHORT rather than trusted; any non-OK rc passes through. NOT
 * for allocation-length commands (MODE SENSE, READ TOC, GET PERFORMANCE),
 * where a drive legitimately returns fewer bytes than requested. */
static inline int adsc_exec_check_short(int rc, uint32_t resid)
{
    if (rc == ACCUDISC_OK && resid != 0)
        return ACCUDISC_ERR_SHORT;
    return rc;
}

/* Returns ACCUDISC_OK or ACCUDISC_ERR_OPEN (errno preserved). */
int adsc_transport_open(adsc_transport *t, const char *path, int rw);
void adsc_transport_close(adsc_transport *t);

/* Execute cmd. ACCUDISC_OK on GOOD status; ACCUDISC_ERR_SENSE when the drive
 * returned CHECK CONDITION (cmd->sense populated); ACCUDISC_ERR_IO for
 * ioctl/host/driver/transport failures (no usable sense). On any completed
 * ioctl (OK or CHECK CONDITION) cmd->resid is set to the untransferred byte
 * count — fixed-length callers must check it (see adsc_exec_check_short). */
int adsc_transport_exec(adsc_transport *t, adsc_cmd *cmd);

/* CDROM_SELECT_SPEED: unprivileged Nx read-speed set via the block layer. */
int adsc_transport_select_speed(adsc_transport *t, unsigned speed_x);

/* Tray control via the block-layer CDROM ioctls (CDROMEJECT / CDROMCLOSETRAY):
 * unprivileged for cdrom-group members, no CAP_SYS_RAWIO needed. */
int adsc_transport_eject(adsc_transport *t);
int adsc_transport_load(adsc_transport *t);

/* Decode raw sense (fixed 0x70/0x71 and descriptor 0x72/0x73 formats) into
 * the public struct; out->valid = 0 if unrecognizable. */
void adsc_sense_decode(const uint8_t *sense, unsigned len, accudisc_sense *out);

#endif /* ADSC_TRANSPORT_H */
