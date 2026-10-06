/* Write-parameters round trip: does the drive HOLD the page 05 a burn sends?
 *
 * Purpose. Three burns on a PX-716A (2026-09-28 x2, 2026-10-02) completed with
 * every command GOOD and read back as a virgin blank after eject + load. One
 * explanation is that the drive never left Test Write: until 0.48.0 the burn
 * MODE SELECTed page 05 and never read it back, so a select that returned GOOD and
 * changed nothing -- ignored by the firmware, or mangled in the data-out phase
 * by the USB-IDE bridge -- is invisible to it. This probe sends the burn's own
 * page (it calls the same function the burn does) and reads it straight back.
 *
 * Two round trips, because one cannot be trusted. If the drive's page already
 * holds live DAO, a select that was applied and a select that was dropped read
 * back the same. So the first round trip asks for Test Write ON and the second
 * for Test Write OFF: the bit has to be seen to move both ways before "it
 * reads back as sent" means anything.
 *
 * What it changes. The drive's write-parameter REGISTERS, twice, and then it
 * puts back the page it found. No disc is needed and none is written: nothing
 * here fires the laser, calibrates power or sends a cue sheet. The result is
 * most relevant with a blank loaded, since that is the state a burn sees, but
 * it runs with an empty tray too and says which it had.
 *
 * What it does NOT cover. The burn sends SET CD SPEED and SEND OPC between its
 * MODE SELECT and the first WRITE; a drive that reset page 05 on either would
 * pass here. Since 0.48.0 the burn reads the page back itself after SET CD
 * SPEED and refuses if it is not held (ACCUDISC_ERR_WRITE_PARAMS), so that
 * half is closed there, and since 0.48.1 it reads the page again after SEND
 * OPC, which closes the other.
 *
 *   cmake --build build
 *   gcc -O2 -o build/wparamsprobe tools/wparamsprobe.c -I include -I src \
 *       build/src/libaccudisc.a -ldl
 *   flock /var/tmp/sr0.lock ./build/wparamsprobe [--cdtext] [--debug] /dev/sr0
 *
 * Exit: 0 = both round trips read back as sent; 1 = the drive did not hold
 * what was sent (the finding); 2 = a command failed, so no verdict.
 */

#include <stdio.h>
#include <string.h>

#include <accudisc/accudisc.h>

#include "internal.h"
#include "write/write.h"

static void hex(const char *tag, const uint8_t *p, uint32_t n)
{
    printf("  %-7s", tag);
    for (uint32_t i = 0; i < n; i++)
        printf(" %02x", p[i]);
    printf("\n");
}

static void decode(const char *tag, const uint8_t *p, uint32_t n)
{
    if (n < 9) {
        printf("  %-7s (short page: %u bytes)\n", tag, n);
        return;
    }
    printf("  %-7s write type %u%s, Test Write %s, BURN-Proof %s, "
           "multisession %u, data block type %u, session format 0x%02x\n",
           tag, p[2] & 0x0f, (p[2] & 0x0f) == 2 ? " (DAO)" : "",
           (p[2] & 0x10) ? "ON" : "off", (p[2] & 0x40) ? "on" : "off",
           p[3] >> 6, p[4] & 0x0f, p[8]);
}

/* 0 = read back as sent AND the request could prove it; 1 = it did not hold;
 * 2 = no verdict (a command failed, or the request changed nothing). */
static int run(accudisc_device *dev, const char *name,
               const struct adsc_write_params *wp,
               struct adsc_wparams_roundtrip *rt)
{
    int rc = adsc_write_params_roundtrip(dev, wp, rt);

    printf("%s\n", name);
    if (rt->before_len) {
        hex("before", rt->before, rt->before_len);
        decode("", rt->before, rt->before_len);
    }
    if (rt->sent_len) {
        hex("sent", rt->sent, rt->sent_len);
        decode("", rt->sent, rt->sent_len);
    }
    if (rt->stage == ADSC_WPRT_STAGE_DONE) {
        hex("after", rt->after, rt->after_len);
        decode("", rt->after, rt->after_len);
    }
    if (rc != ACCUDISC_OK) {
        printf("  NO VERDICT: %s failed: %s", rt->stage == ADSC_WPRT_STAGE_SELECT
                   ? "MODE SENSE / MODE SELECT" : "the read-back MODE SENSE",
               accudisc_strerror(rc));
        if (rc == ACCUDISC_ERR_SENSE)
            printf(" (sense %x/%02x/%02x)", dev->last_sense.key,
                   dev->last_sense.asc, dev->last_sense.ascq);
        printf("\n");
        return 2;
    }
    if (rt->ignored) {
        printf("  DID NOT HOLD: MODE SELECT returned GOOD and the page is "
               "byte-for-byte what it was before\n");
        return 1;
    }
    if (!rt->fields_ok) {
        printf("  DID NOT HOLD: a field that decides the burn reads back "
               "different from what was sent\n");
        return 1;
    }
    if (!rt->page_ok) {
        printf("  DID NOT HOLD: the burn's fields match, but %u other byte(s) "
               "of the page differ from what was sent\n", rt->diff_bytes);
        return 1;
    }
    if (!rt->changed) {
        printf("  NO VERDICT: the page already held these values, so a "
               "dropped select would read back the same\n");
        return 2;
    }
    printf("  HELD: the page changed and reads back exactly as sent\n");
    return 0;
}

int main(int argc, char **argv)
{
    const char *path = "/dev/sr0";
    unsigned flags = ACCUDISC_OPEN_RDWR;
    struct adsc_write_params wp = {0};
    int err = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--cdtext"))
            wp.cdtext = 1;
        else if (!strcmp(argv[i], "--debug"))
            flags |= ACCUDISC_OPEN_TRACE | ACCUDISC_OPEN_TRACE_DATA;
        else if (argv[i][0] == '-') {
            fprintf(stderr, "usage: wparamsprobe [--cdtext] [--debug] "
                            "[device]\n");
            return 2;
        } else
            path = argv[i];
    }

    accudisc_device *dev = accudisc_open(path, flags, &err);
    if (!dev) {
        fprintf(stderr, "open %s: %s\n", path, accudisc_strerror(err));
        return 2;
    }

    accudisc_disc_probe dp;
    if (accudisc_probe_disc(dev, &dp) == ACCUDISC_OK)
        printf("medium: %s (%s, profile 0x%04x, tray %s)\n",
               accudisc_disc_kind_str(dp.kind),
               accudisc_disc_reason_str(dp.reason), dp.profile,
               accudisc_tray_state_str(dp.tray));
    else
        printf("medium: not determined\n");

    /* BURN-Proof exactly as the burn's default chooses it: on if claimed. */
    accudisc_features caps;
    memset(&caps, 0, sizeof caps);
    wp.burnproof = (adsc_probe_cd_mastering(dev, &caps) == 0) &&
                   caps.buf_claimed;
    printf("request: DAO, BURN-Proof %s (%s by the drive), CD-Text %s\n",
           wp.burnproof ? "on" : "off",
           wp.burnproof ? "claimed" : "not claimed", wp.cdtext ? "yes" : "no");

    struct adsc_wparams_roundtrip a, b;
    wp.simulate = 1;
    int va = run(dev, "round trip 1: Test Write ON (what --simulate sends)",
                 &wp, &a);
    wp.simulate = 0;
    int vb = run(dev, "round trip 2: Test Write OFF (what a live burn sends)",
                 &wp, &b);

    /* Pop what we pushed: the page as it was found, not a factory default. */
    if (a.before_len) {
        int rr = adsc_write_params_restore(dev, a.before, a.before_len);

        /* Said as what it is: after this probe, of all programs, a GOOD
         * from MODE SELECT is not printed as "restored". */
        printf("restore: MODE SELECT of the page found at open %s\n",
               rr == ACCUDISC_OK ? "returned GOOD (not re-read)"
                                 : "FAILED (a power cycle resets the page)");
    }
    accudisc_close(dev);

    int v = (va == 1 || vb == 1) ? 1 : (va == 2 || vb == 2) ? 2 : 0;

    printf("verdict: %s\n",
           v == 0 ? "the drive holds page 05 as the burn sends it, and Test "
                    "Write was seen to move both ways"
           : v == 1 ? "the drive did NOT hold the page it was sent"
                    : "incomplete -- see NO VERDICT above");
    return v;
}
