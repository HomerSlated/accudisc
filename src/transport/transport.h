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
} adsc_cmd;

/* Returns ACCUDISC_OK or ACCUDISC_ERR_OPEN (errno preserved). */
int adsc_transport_open(adsc_transport *t, const char *path, int rw);
void adsc_transport_close(adsc_transport *t);

/* Execute cmd. ACCUDISC_OK on GOOD status; ACCUDISC_ERR_SENSE when the drive
 * returned CHECK CONDITION (cmd->sense populated); ACCUDISC_ERR_IO for
 * ioctl/host/driver/transport failures (no usable sense). */
int adsc_transport_exec(adsc_transport *t, adsc_cmd *cmd);

/* CDROM_SELECT_SPEED: unprivileged Nx read-speed set via the block layer. */
int adsc_transport_select_speed(adsc_transport *t, unsigned speed_x);

/* Decode raw sense (fixed 0x70/0x71 and descriptor 0x72/0x73 formats) into
 * the public struct; out->valid = 0 if unrecognizable. */
void adsc_sense_decode(const uint8_t *sense, unsigned len, accudisc_sense *out);

#endif /* ADSC_TRANSPORT_H */
