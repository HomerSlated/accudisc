/* Phase 3 Step B1: the write model carries an optional CD-Text pass-through
 * blob, and adsc_write_load_model slurps the .toc (+ optional blob) from disk
 * into that model without a device. This exercises the intake path: the blob
 * is attached byte-for-byte as a borrowed pointer, absence leaves it NULL/0,
 * and a missing file fails cleanly with nothing left allocated. */

#define _XOPEN_SOURCE 700
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <accudisc/accudisc.h>

#include "cdda/crc16.h"
#include "write/write.h"

/* Write a byte buffer to a fresh temp file; returns its malloc'd path. */
static char *write_temp(const void *data, size_t len)
{
    char tmpl[] = "/tmp/adsc_intakeXXXXXX";
    int fd = mkstemp(tmpl);
    assert(fd >= 0);
    if (len)
        assert(write(fd, data, len) == (ssize_t)len);
    close(fd);
    return strdup(tmpl);
}

int main(void)
{
    static const char TOC[] =
        "CD_DA\n"
        "TRACK AUDIO\n"
        "FILE \"x.bin\" 00:00:00 00:10:00\n";

    /* A VALID single-pack format-05 blob: the loader now validates the blob at
     * intake (B2), so a well-formed one is required for the carry test. 4-byte
     * header (data length = 20) + one title pack with a correct complemented
     * CRC, so validation makes no changes and the byte-exact carry holds. */
    uint8_t blob[22] = { 0x00, 0x14, 0x00, 0x00,   /* hdr: len-2 = 20 */
                         0x80, 0x01, 0x00, 0x00,   /* title, track 1, seq 0 */
                         'T', 'E', 'S', 'T', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    {
        uint16_t stored = (uint16_t)~adsc_crc16(blob + 4, 16);
        blob[20] = (uint8_t)(stored >> 8);
        blob[21] = (uint8_t)stored;
    }

    char *tocp = write_temp(TOC, strlen(TOC));
    char *blobp = write_temp(blob, sizeof blob);

    struct adsc_write_toc m;
    uint8_t *buf;

    /* --- with a CD-Text blob: parsed, attached byte-for-byte, buf owns it --- */
    buf = NULL;
    assert(adsc_write_load_model(tocp, blobp, &m, &buf) == ACCUDISC_OK);
    assert(m.ntracks == 1);
    assert(buf != NULL);
    assert(m.cdtext == buf);                    /* model borrows the owned buf */
    assert(m.cdtext_len == sizeof blob);
    assert(memcmp(m.cdtext, blob, sizeof blob) == 0);
    free(buf);

    /* --- no CD-Text: cdtext stays NULL/0 and buf is reset to NULL --------- */
    buf = (uint8_t *)0xdeadbeef;                 /* must be overwritten */
    assert(adsc_write_load_model(tocp, NULL, &m, &buf) == ACCUDISC_OK);
    assert(m.ntracks == 1);
    assert(m.cdtext == NULL);
    assert(m.cdtext_len == 0);
    assert(buf == NULL);

    /* --- missing .toc: ERR_OPEN, nothing attached ------------------------- */
    buf = NULL;
    assert(adsc_write_load_model("/nonexistent/x.toc", NULL, &m, &buf) ==
           ACCUDISC_ERR_OPEN);
    assert(buf == NULL);

    /* --- valid .toc but missing blob: ERR_OPEN, buf stays NULL ------------ */
    buf = NULL;
    assert(adsc_write_load_model(tocp, "/nonexistent/x.cdtext", &m, &buf) ==
           ACCUDISC_ERR_OPEN);
    assert(buf == NULL);

    /* --- NULL arguments rejected ------------------------------------------ */
    assert(adsc_write_load_model(NULL, NULL, &m, &buf) == ACCUDISC_ERR_INVAL);
    assert(adsc_write_load_model(tocp, NULL, NULL, &buf) == ACCUDISC_ERR_INVAL);
    assert(adsc_write_load_model(tocp, NULL, &m, NULL) == ACCUDISC_ERR_INVAL);

    unlink(tocp);
    unlink(blobp);
    free(tocp);
    free(blobp);
    return 0;
}
