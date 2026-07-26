/* Caller-declared struct size negotiation — API_PLAN §7.1.
 *
 * The point of a size field is the case that does not exist yet: a caller
 * compiled against a different header. An implementation whose short-struct and
 * long-struct paths are never exercised is indistinguishable from no size field
 * at all, so every branch is driven here with a synthetic caller layout.
 *
 * adsc_abi_import/adsc_abi_export are internal and pure, which is what makes
 * this possible without a drive. The end-to-end cases at the bottom go through
 * the real accudisc_read_cdda with a NULL device: an ERR_INVAL there is the
 * positive evidence that the ABI layer ACCEPTED the struct and execution
 * reached the argument checks behind it. ERR_ABI and ERR_INVAL being distinct
 * codes is what makes that distinguishable.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <accudisc/accudisc.h>

#include "internal.h"

static int fails;

static void ck(int cond, const char *what)
{
    printf("  %-4s %s\n", cond ? "ok" : "FAIL", what);
    if (!cond)
        fails++;
}

/* LP64 only, for both groups below — the pointer members make these numbers
 * model-dependent, and a wrong-for-this-model assertion is worse than none. */
#if defined(__LP64__) || defined(_LP64)

/* FROZEN. accudisc_chunk carries no size field and cannot negotiate, because
 * the library allocates it and the caller's sink reads it — the hazard runs the
 * opposite way from read_req, and a size field would not help. Growing it is an
 * soname bump, so this assertion exists to make that a deliberate act rather
 * than something a new field does quietly. */
_Static_assert(sizeof(accudisc_chunk) == 32, "accudisc_chunk grew: soname bump");

/* NOT frozen — these two are allowed to grow; that is what the size field is
 * for. Pinned only so that growing them is *noticed*: a new field here means
 * updating the number below, bumping ACCUDISC_VERSION_MINOR so the .so version
 * moves with the layout, and adding a row to API_PLAN §8. Tripping these is a
 * reminder, not a defect. */
_Static_assert(sizeof(accudisc_read_req) == 56, "read_req grew — see above");
_Static_assert(sizeof(accudisc_read_stats) == 136, "read_stats grew — see above");

#endif

int main(void)
{
    accudisc_read_req dst;
    unsigned char src[sizeof(accudisc_read_req) + 16];
    size_t bytes;
    int rc;

    /* ---- import: the size a caller that forgot the macro produces ---- */
    memset(src, 0, sizeof src);
    rc = adsc_abi_import(&dst, sizeof dst, src, 0);
    ck(rc == ACCUDISC_ERR_ABI, "import: size 0 is refused, not treated as empty");

    rc = adsc_abi_import(&dst, sizeof dst, src, 3);
    ck(rc == ACCUDISC_ERR_ABI, "import: size below the size field itself refused");

    /* ---- import: exact match ---- */
    {
        accudisc_read_req in = ACCUDISC_READ_REQ_INIT;
        in.lba = 12345;
        in.count = 678;
        in.sub = ACCUDISC_SUB_RAW;
        rc = adsc_abi_import(&dst, sizeof dst, &in, in.size);
        ck(rc == ACCUDISC_OK && dst.lba == 12345 && dst.count == 678 &&
               dst.sub == ACCUDISC_SUB_RAW,
           "import: exact size copies every field");
        ck(dst.size == sizeof(accudisc_read_req),
           "import: size is normalised to this build's");
    }

    /* ---- import: OLD caller, NEW library — the tail must be invented, not
     * read. The caller's memory past their declared end is poisoned; if the
     * library reads it, `cancel` becomes a garbage pointer and `allow_unsafe`
     * a garbage flag. Both are silent failures in the worst direction. ---- */
    {
        /* Cut before the accuracy-strategy block — the layer that grew
         * read_req from 40 to 56 bytes — so everything added since is on the
         * far side of the caller's declared end. */
        const uint32_t shortlen = (uint32_t)offsetof(accudisc_read_req,
                                                     speed_ladder);
        memset(src, 0xAA, sizeof src);
        memcpy(src, &shortlen, sizeof shortlen);
        /* lba/count sit inside the short region, so they must survive. */
        uint32_t lba = 999, count = 5;
        memcpy(src + offsetof(accudisc_read_req, lba), &lba, sizeof lba);
        memcpy(src + offsetof(accudisc_read_req, count), &count, sizeof count);

        memset(&dst, 0xBB, sizeof dst);
        rc = adsc_abi_import(&dst, sizeof dst, src, shortlen);
        ck(rc == ACCUDISC_OK, "import: short struct accepted");
        ck(dst.lba == 999 && dst.count == 5,
           "import: short struct keeps the fields it does have");
        ck(dst.cancel == NULL,
           "import: short struct zero-extends `cancel` (0xAA not read past end)");
        ck(dst.status_map == NULL,
           "import: short struct zero-extends `status_map`");
        ck(dst.allow_unsafe == 0,
           "import: short struct zero-extends `allow_unsafe` — the guard stays on");
        ck(dst.speed_ladder == NULL && dst.ladder_len == 0,
           "import: short struct zero-extends the speed ladder");
        ck(dst.size == sizeof(accudisc_read_req),
           "import: short struct still normalises size");
        /* The complement, so the two together locate the boundary rather than
         * merely observing zeros: a field INSIDE the declared region is copied
         * verbatim, poison and all. Without this, an implementation that
         * zeroed everything would pass every assertion above. */
        ck(dst.retries == 0xAA,
           "import: a field inside the declared region is copied, not zeroed");
    }

    /* ---- import: NEW caller, OLD library ---- */
    {
        const uint32_t longlen = (uint32_t)sizeof(accudisc_read_req) + 8;
        memset(src, 0, sizeof src);
        memcpy(src, &longlen, sizeof longlen);
        rc = adsc_abi_import(&dst, sizeof dst, src, longlen);
        ck(rc == ACCUDISC_OK,
           "import: long struct with an all-zero tail is accepted");

        src[sizeof(accudisc_read_req) + 4] = 1; /* a field we cannot honour */
        rc = adsc_abi_import(&dst, sizeof dst, src, longlen);
        ck(rc == ACCUDISC_ERR_ABI,
           "import: long struct with a SET tail field is refused, not dropped");
    }

    /* ---- import: an ABSURD size must be refused BEFORE the tail is scanned.
     * This is the case an uninitialised `accudisc_read_req req;` produces —
     * stack garbage in the size field, routinely a large number. Without a
     * bound, the loop that exists to stop the library reading past the end of
     * a caller's struct reads a megabyte past the end of a 56-byte one. Run
     * this under ASan: unbounded it faults, bounded it returns ERR_ABI having
     * dereferenced nothing beyond the struct. ---- */
    {
        static const uint32_t absurd[] = {
            (uint32_t)sizeof(accudisc_read_req) + ADSC_ABI_GROWTH_MAX + 1,
            0x100000u,     /* plausible-looking stack garbage */
            0xFFFFFFFFu,   /* the whole address space */
        };
        int all = 1;
        for (size_t i = 0; i < sizeof absurd / sizeof absurd[0]; i++) {
            memset(src, 0, sizeof src);
            memcpy(src, &absurd[i], sizeof absurd[i]);
            if (adsc_abi_import(&dst, sizeof dst, src, absurd[i]) !=
                ACCUDISC_ERR_ABI)
                all = 0;
        }
        ck(all, "import: an absurd size is refused before anything is read");

        accudisc_read_req garbage = ACCUDISC_READ_REQ_INIT;
        garbage.count = 10;
        garbage.size = 0x100000u; /* as if never initialised */
        ck(accudisc_read_cdda(NULL, &garbage, NULL, NULL, NULL) ==
               ACCUDISC_ERR_ABI,
           "read_cdda: uninitialised-looking size is ERR_ABI, not a fault");
    }

    /* ---- export ---- */
    bytes = 12345;
    rc = adsc_abi_export(0, sizeof(accudisc_read_stats), &bytes);
    ck(rc == ACCUDISC_ERR_ABI && bytes == 12345,
       "export: size 0 refused and n_out untouched");

    rc = adsc_abi_export((uint32_t)sizeof(accudisc_read_stats),
                         sizeof(accudisc_read_stats), &bytes);
    ck(rc == ACCUDISC_OK && bytes == sizeof(accudisc_read_stats),
       "export: exact size yields the whole struct");

    rc = adsc_abi_export((uint32_t)sizeof(accudisc_read_stats) - 24,
                         sizeof(accudisc_read_stats), &bytes);
    ck(rc == ACCUDISC_OK && bytes == sizeof(accudisc_read_stats) - 24,
       "export: short struct is honoured, we write only what they own");

    rc = adsc_abi_export((uint32_t)sizeof(accudisc_read_stats) + 8,
                         sizeof(accudisc_read_stats), &bytes);
    ck(rc == ACCUDISC_ERR_ABI,
       "export: long struct refused — an unfilled counter reads as a real 0");

    /* ---- end to end, no device ---- */
    {
        accudisc_read_req req = ACCUDISC_READ_REQ_INIT;
        accudisc_read_stats st = ACCUDISC_READ_STATS_INIT;
        req.lba = 0;
        req.count = 10;

        ck(accudisc_read_cdda(NULL, NULL, NULL, NULL, NULL) ==
               ACCUDISC_ERR_INVAL,
           "read_cdda: NULL req is ERR_INVAL, checked before any deref");

        accudisc_read_req bad = req;
        bad.size = 0;
        ck(accudisc_read_cdda(NULL, &bad, NULL, NULL, NULL) == ACCUDISC_ERR_ABI,
           "read_cdda: an uninitialised size is ERR_ABI, not ERR_INVAL");

        ck(accudisc_read_cdda(NULL, &req, NULL, NULL, NULL) ==
               ACCUDISC_ERR_INVAL,
           "read_cdda: a good size reaches the device check (ERR_INVAL)");

        accudisc_read_stats bad_st = st;
        bad_st.size = (uint32_t)sizeof(accudisc_read_stats) + 8;
        ck(accudisc_read_cdda(NULL, &req, NULL, NULL, &bad_st) ==
               ACCUDISC_ERR_ABI,
           "read_cdda: an oversized stats struct is ERR_ABI");

        ck(strcmp(accudisc_strerror(ACCUDISC_ERR_ABI), "unknown error") != 0,
           "strerror: ERR_ABI has its own message");
    }

    if (fails) {
        printf("test_abi: %d failure(s)\n", fails);
        return 1;
    }
    printf("test_abi: all pass\n");
    return 0;
}
