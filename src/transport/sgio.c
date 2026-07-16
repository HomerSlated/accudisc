/* Linux SG_IO backend.
 *
 * Read-class commands pass the kernel's unprivileged SG_IO filter on a
 * read-only fd (no root needed for users in the cdrom group); vendor opcodes
 * and MODE SELECT require an O_RDWR open — callers opt in via rw.
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/cdrom.h>
#include <scsi/sg.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "transport.h"

int adsc_transport_open(adsc_transport *t, const char *path, int rw)
{
    t->rw = rw;
    /* O_NONBLOCK: open must not hang on a spun-down or empty drive. */
    t->fd = open(path, (rw ? O_RDWR : O_RDONLY) | O_NONBLOCK);
    if (t->fd < 0)
        return ACCUDISC_ERR_OPEN;
    return ACCUDISC_OK;
}

void adsc_transport_close(adsc_transport *t)
{
    if (t->fd >= 0)
        close(t->fd);
    t->fd = -1;
}

int adsc_transport_exec(adsc_transport *t, adsc_cmd *cmd)
{
    sg_io_hdr_t io;

    memset(&io, 0, sizeof(io));
    memset(cmd->sense, 0, sizeof(cmd->sense));
    cmd->sense_len = 0;

    io.interface_id = 'S';
    switch (cmd->dir) {
    case ADSC_XFER_IN:  io.dxfer_direction = SG_DXFER_FROM_DEV; break;
    case ADSC_XFER_OUT: io.dxfer_direction = SG_DXFER_TO_DEV;   break;
    default:            io.dxfer_direction = SG_DXFER_NONE;     break;
    }
    io.cmd_len = cmd->cdb_len;
    io.mx_sb_len = sizeof(cmd->sense);
    io.dxfer_len = cmd->buf_len;
    io.dxferp = cmd->buf;
    io.cmdp = cmd->cdb;
    io.sbp = cmd->sense;
    io.timeout = cmd->timeout_ms ? cmd->timeout_ms : ADSC_TIMEOUT_CTRL_MS;

    if (ioctl(t->fd, SG_IO, &io) < 0)
        return ACCUDISC_ERR_IO;

    if ((io.info & SG_INFO_OK_MASK) != SG_INFO_OK) {
        /* sb_len_wr > 0: the drive returned sense — a CHECK CONDITION the
         * caller can decode; anything else (host/driver/transport) is a
         * hard failure with nothing to decode. */
        if (io.sb_len_wr > 0) {
            cmd->sense_len = io.sb_len_wr;
            return ACCUDISC_ERR_SENSE;
        }
        return ACCUDISC_ERR_IO;
    }
    return ACCUDISC_OK;
}

int adsc_transport_select_speed(adsc_transport *t, unsigned speed_x)
{
    if (ioctl(t->fd, CDROM_SELECT_SPEED, speed_x) < 0)
        return ACCUDISC_ERR_IO;
    return ACCUDISC_OK;
}

int adsc_transport_eject(adsc_transport *t)
{
    if (ioctl(t->fd, CDROMEJECT) < 0)
        return ACCUDISC_ERR_IO;
    return ACCUDISC_OK;
}

int adsc_transport_load(adsc_transport *t)
{
    if (ioctl(t->fd, CDROMCLOSETRAY) < 0)
        return ACCUDISC_ERR_IO;
    return ACCUDISC_OK;
}
