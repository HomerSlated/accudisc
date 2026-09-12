/* Linux SG_IO backend.
 *
 * Read-class commands pass the kernel's unprivileged SG_IO filter on a
 * read-only fd (no root needed for users in the cdrom group); vendor opcodes
 * and MODE SELECT require an O_RDWR open — callers opt in via rw.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/cdrom.h>
#include <scsi/sg.h>
#include <stdio.h>
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

    cmd->io_errno = 0;
    cmd->host_status = cmd->driver_status = 0;
    cmd->scsi_status = 0;

    if (ioctl(t->fd, SG_IO, &io) < 0) {
        cmd->io_errno = errno; /* ENOMEDIUM, EPERM (filter), EINVAL, ... */
        return ACCUDISC_ERR_IO;
    }

    /* Record the short-transfer residual: SG_IO can complete with GOOD status
     * yet move fewer bytes than requested (partial DMA / drive under-run). The
     * generic transport only reports it; the fixed-length caller (READ CD)
     * decides whether a residual is an error — allocation-length commands do
     * not. */
    cmd->resid = adsc_resid_clamp(io.resid, cmd->buf_len);

    /* All three status fields, on EVERY completed ioctl. Until 0.36.0 they were
     * filled only on the sense-less failure branch, so a CHECK CONDITION left
     * scsi_status at 0 — and anything printing it (the trace) would have shown
     * "status=0x00" for a command the drive refused. That is the failure that
     * produced thirteen false "implemented" rows in the selector sweep
     * (re-tools/selsweep.c, 2026-09-09): one field checked, the drive believed. */
    cmd->host_status = io.host_status;
    cmd->driver_status = io.driver_status;
    cmd->scsi_status = io.status;

    if ((io.info & SG_INFO_OK_MASK) != SG_INFO_OK) {
        /* sb_len_wr > 0: the drive returned sense — a CHECK CONDITION the
         * caller can decode; anything else (host/driver/transport) is a
         * hard failure with nothing to decode. */
        if (io.sb_len_wr > 0) {
            cmd->sense_len = io.sb_len_wr;
            return ACCUDISC_ERR_SENSE;
        }
        /* Keep WHY. A bare ERR_IO here is otherwise unattributable after the
         * fact: DRIVER_TIMEOUT, DID_ERROR from the adapter and a status-only
         * failure all look identical to the caller. */
        return ACCUDISC_ERR_IO;
    }
    return ACCUDISC_OK;
}

void adsc_io_detail(const adsc_cmd *cmd, char *out, size_t cap)
{
    if (!out || cap == 0)
        return;
    if (!cmd) {
        snprintf(out, cap, "unknown");
        return;
    }
    if (cmd->io_errno) {
        snprintf(out, cap, "ioctl: %s", strerror(cmd->io_errno));
        return;
    }
    /* DRIVER_TIMEOUT (0x06) is the one worth naming outright: it is the most
     * common cause of a lead-in read failing on a slow or marginal disc, and
     * reads very differently from an adapter fault. */
    if ((cmd->driver_status & 0x0f) == 0x06) {
        snprintf(out, cap, "timeout (driver=0x%02x host=0x%02x)",
                 cmd->driver_status, cmd->host_status);
        return;
    }
    if (cmd->host_status || cmd->driver_status || cmd->scsi_status) {
        snprintf(out, cap, "host=0x%02x driver=0x%02x status=0x%02x",
                 cmd->host_status, cmd->driver_status, cmd->scsi_status);
        return;
    }
    snprintf(out, cap, "no detail reported");
}

int adsc_transport_select_speed(adsc_transport *t, unsigned speed_x)
{
    if (ioctl(t->fd, CDROM_SELECT_SPEED, speed_x) < 0)
        return ACCUDISC_ERR_IO;
    return ACCUDISC_OK;
}

/* How long to wait for the tray to actually move before calling the eject a
 * failure, and how often to look. Measured, not chosen: a clean eject on the
 * PX-716A (2026-09-12) had the tray reported open 1.80 s after the ioctl
 * returned, including the cost of a separate process opening the device to
 * ask. 5 s is that with headroom for a slower mechanism; the poll exits the
 * instant the tray moves, so a healthy drive never pays the cap. */
#define ADSC_EJECT_VERIFY_MS 5000
#define ADSC_EJECT_POLL_MS 100

/* Is a disc still sitting in the drive? 1 = yes (definitely), 0 = no or the
 * drive would not say. Only a POSITIVE answer is load-bearing — a drive that
 * does not implement CDROM_DRIVE_STATUS, or that answers CDS_NO_INFO, must
 * never be reported as having failed to eject on the strength of a shrug. */
static int disc_still_present(int fd)
{
    int st = ioctl(fd, CDROM_DRIVE_STATUS, CDSL_CURRENT);

    return st == CDS_DISC_OK;
}

int adsc_transport_eject(adsc_transport *t, char *why, size_t why_cap)
{
    int lock_errno = 0;
    unsigned waited = 0;

    /* Holding the device open auto-locks the drive door (CDO_LOCK), and
     * CDROMEJECT silently no-ops against a locked door — so unlock first,
     * exactly as eject(1)/util-linux do.
     *
     * The unlock is best-effort but NOT uninteresting: the kernel refuses it
     * with EBUSY when use_count != 1 (cdrom_ioctl_lock_door), i.e. when a
     * mounted filesystem or another process also holds the device. That is
     * the single most useful thing we can tell the user when the tray then
     * does not move, so keep the errno rather than discarding it. */
    if (ioctl(t->fd, CDROM_LOCKDOOR, 0) < 0)
        lock_errno = errno;

    if (ioctl(t->fd, CDROMEJECT) < 0) {
        if (why && why_cap)
            snprintf(why, why_cap, "CDROMEJECT: %s", strerror(errno));
        return ACCUDISC_ERR_IO;
    }

    /* Do not believe the return value. Measured on the PX-716A 2026-09-12
     * with a second process holding /dev/sr0 open (the same use_count
     * condition a mount creates): CDROMEJECT returns 0, no error is reported
     * anywhere, and the tray stays shut — verified by polling the drive once
     * a second for twelve seconds. A mount never goes away on its own, so
     * this is a permanently wrong success, not a slow one. */
    while (waited < ADSC_EJECT_VERIFY_MS) {
        if (!disc_still_present(t->fd))
            return ACCUDISC_OK;
        usleep(ADSC_EJECT_POLL_MS * 1000);
        waited += ADSC_EJECT_POLL_MS;
    }
    if (!disc_still_present(t->fd))
        return ACCUDISC_OK;

    if (why && why_cap) {
        if (lock_errno == EBUSY)
            snprintf(why, why_cap,
                     "the drive accepted the eject but the disc is still "
                     "loaded after %u.%us — the door is locked because "
                     "something else has the device open; unmount it (or "
                     "close whatever is using it) and try again",
                     ADSC_EJECT_VERIFY_MS / 1000,
                     (ADSC_EJECT_VERIFY_MS % 1000) / 100);
        else
            snprintf(why, why_cap,
                     "the drive accepted the eject but the disc is still "
                     "loaded after %u.%us (lock_door: %s)",
                     ADSC_EJECT_VERIFY_MS / 1000,
                     (ADSC_EJECT_VERIFY_MS % 1000) / 100,
                     lock_errno ? strerror(lock_errno) : "ok");
    }
    return ACCUDISC_ERR_IO;
}

int adsc_transport_load(adsc_transport *t)
{
    if (ioctl(t->fd, CDROMCLOSETRAY) < 0)
        return ACCUDISC_ERR_IO;
    return ACCUDISC_OK;
}
