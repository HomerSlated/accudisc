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

/* FROZEN for a sharper reason: accudisc_speed_rung is an OUT ARRAY with no
 * size field, so its size is the STRIDE the library writes at. A caller built
 * against a smaller version does not get truncated rows, it gets its buffer
 * walked past the end — the one growth in this header where the failure is
 * memory corruption rather than a wrong number. It went 6 -> 10 -> 14 -> 20;
 * every one of those was a hard break requiring both sides rebuilt, and this
 * assertion is here so the next one cannot happen without saying so. */
_Static_assert(sizeof(accudisc_speed_rung) == 20,
               "accudisc_speed_rung grew: hard ABI break, rebuild every "
               "consumer and say so in API_PLAN §8");

/* NOT frozen — these two are allowed to grow; that is what the size field is
 * for. Pinned only so that growing them is *noticed*: a new field here means
 * updating the number below, bumping ACCUDISC_VERSION_MINOR so the .so version
 * moves with the layout, and adding a row to API_PLAN §8. Tripping these is a
 * reminder, not a defect. */
_Static_assert(sizeof(accudisc_read_req) == 72, "read_req grew — see above");
_Static_assert(sizeof(accudisc_read_stats) == 160, "read_stats grew — see above");

/* 0.22.0 moved these: read_req 64 -> 72 (buffer_bytes, plus the padding it
 * pulled in) and read_stats 144 -> 160 (buffer_peak_chunks, buffer_stalls).
 * Both APPENDED, so nothing above them moved — subq_misposition in particular
 * keeps the offset 0.21.0 shipped it at, which is the whole reason they went
 * to the tail rather than beside the counters they belong with. */
_Static_assert(offsetof(accudisc_read_stats, subq_misposition) == 140,
               "subq_misposition moved: 0.21.0 consumers read the wrong field");
_Static_assert(sizeof(accudisc_write_opts) == 32, "write_opts grew — see above");
/* 24 -> 32 in 0.26.0: `burnproof` appended at offset 24. Guarded by the `size`
 * field, so a 0.25.0 caller passing 24 is honoured and simply gets AUTO. */
_Static_assert(offsetof(accudisc_write_opts, burnproof) == 24,
               "write_opts: burnproof was supposed to APPEND");

/* accudisc_features has NO size field, so the version bump is the only signal a
 * consumer gets that it grew — 11 -> 16 bytes in 0.26.0, five write-capability
 * flags appended. Pinned because "always bump the version" is a load-bearing
 * invariant for size-less structs rather than a habit, and cdda2img found the
 * case where it was not kept (their §113.2). */
_Static_assert(sizeof(accudisc_features) == 16, "features grew — bump the version");
_Static_assert(offsetof(accudisc_features, c2_verdict) == 10,
               "features: c2_verdict moved, breaking every 0.25.0 consumer");
_Static_assert(offsetof(accudisc_features, buf_claimed) == 13,
               "features: the write flags were supposed to APPEND");

/* The size field landed in padding, so adding it did NOT move sizeof. Pinned
 * because that fact is load-bearing in the header's own explanation of why an
 * old caller fails loudly, and a future field that changes it would leave that
 * paragraph describing a layout that no longer exists. */
_Static_assert(offsetof(accudisc_write_opts, size) == 0,
               "write_opts: size must lead — adsc_abi_import writes offset 0");
_Static_assert(offsetof(accudisc_write_opts, cdtext_path) == 16,
               "write_opts: the guard was supposed to be free; it moved a field");

/* accudisc_offset_info, pinned from 0.23.0 — and it had NO pin before that,
 * which is precisely how the near-miss below stayed invisible.
 *
 * 0.23.0 appended ar_acc_ok/ar_acc_bad, 36 -> 44 bytes. The lookup used to
 * decide whether it could write values[] by asking `out->size >= sizeof(*out)`,
 * which was right only while value_sources[] was the last field. Appending
 * after it would have made every 0.22.0 caller — reporting the correct size of
 * 36 — fail that test and silently stop receiving values[] on an ambiguous
 * key: no error, no crash, an empty array where four offsets belonged.
 * offsets.c now gates each field on reaching its OWN end. These pins are what
 * make the next append notice the same problem. */
_Static_assert(sizeof(accudisc_offset_info) == 44,
               "offset_info grew — see above; and re-check the per-field size "
               "gates in accudisc_offset_for_inquiry, which are what let an "
               "older caller keep the fields it was compiled to know about");
_Static_assert(offsetof(accudisc_offset_info, values) == 16,
               "offset_info: values[] moved, breaking 0.22.0 consumers");
_Static_assert(offsetof(accudisc_offset_info, value_sources) == 32,
               "offset_info: value_sources[] moved, breaking 0.22.0 consumers");
/* 36 is the OLD sizeof. A 0.22.0 caller passes exactly that, so this is the
 * boundary the values[] gate has to sit at or below, not above. */
_Static_assert(offsetof(accudisc_offset_info, ar_acc_ok) == 36,
               "offset_info: the accuracy counts were supposed to APPEND");
_Static_assert(offsetof(accudisc_offset_info, ar_acc_bad) == 40,
               "offset_info: the accuracy counts were supposed to APPEND");

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
     * library reads it, `cancel` becomes a garbage pointer and `status_map` a
     * garbage destination the engine writes one byte per sector through. Both
     * are silent failures in the worst direction. ---- */
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
        /* The claim that made subq_map additive rather than an soname bump: a
         * caller compiled against 0.4 declares a struct that ends before this
         * field exists, and must come out with the lane switched off. If this
         * ever read the caller's memory instead, the engine would store a Q
         * health byte per sector through a garbage pointer. */
        ck(dst.subq_map == NULL,
           "import: short struct zero-extends `subq_map` — the 0.4 caller's "
           "lane stays off");
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

    /* ---- accudisc_write: the same guard on the destructive path ------------
     * The stakes differ from read's. There, a struct read past its end returns
     * a wrong answer that a later check might catch; here it burns a disc. So
     * these go through the real accudisc_write with a NULL device, and the
     * ERR_INVAL cases are the positive evidence: reaching the device check
     * means the ABI layer ACCEPTED the struct, which is the half that a guard
     * refusing everything would also pass. ---- */
    {
        accudisc_write_opts o = ACCUDISC_WRITE_OPTS_INIT;

        ck(o.size == sizeof(accudisc_write_opts),
           "write_opts: the INIT macro sets size to this build's");

        ck(accudisc_write(NULL, NULL, NULL, NULL, NULL, NULL) ==
               ACCUDISC_ERR_INVAL,
           "write: NULL opts is ERR_INVAL, checked before any deref");

        ck(accudisc_write(NULL, "t.toc", "a.bin", &o, NULL, NULL) ==
               ACCUDISC_ERR_INVAL,
           "write: a good size reaches the device check (ERR_INVAL)");

        /* This is the case cdda2img asked for: a caller built against the
         * PREVIOUS header passes 24 bytes whose first four are `simulate`.
         * Both its values are below sizeof(uint32_t), so both are refused —
         * which is the whole reason sizeof() not changing is safe. */
        accudisc_write_opts old_layout = o;
        old_layout.size = 0;
        ck(accudisc_write(NULL, "t.toc", "a.bin", &old_layout, NULL, NULL) ==
               ACCUDISC_ERR_ABI,
           "write: an old caller's simulate=0 read as size is ERR_ABI");
        old_layout.size = 1;
        ck(accudisc_write(NULL, "t.toc", "a.bin", &old_layout, NULL, NULL) ==
               ACCUDISC_ERR_ABI,
           "write: an old caller's simulate=1 read as size is ERR_ABI");

        accudisc_write_opts absurd_o = o;
        absurd_o.size = 0x100000u; /* as if never initialised */
        ck(accudisc_write(NULL, "t.toc", "a.bin", &absurd_o, NULL, NULL) ==
               ACCUDISC_ERR_ABI,
           "write: uninitialised-looking size is ERR_ABI, not a fault");

        /* A NEWER caller. Zero tail = "declared, not used" and is accepted;
         * a set byte past our end is a feature we would silently drop, and is
         * refused. Without the second half the first proves nothing — a guard
         * that accepts every long struct passes it. */
        {
            unsigned char big[sizeof(accudisc_write_opts) + 8];
            const uint32_t longlen = (uint32_t)sizeof big;
            memset(big, 0, sizeof big);
            memcpy(big, &o, sizeof o);
            memcpy(big, &longlen, sizeof longlen);
            ck(accudisc_write(NULL, "t.toc", "a.bin",
                              (const accudisc_write_opts *)big, NULL, NULL) ==
                   ACCUDISC_ERR_INVAL,
               "write: long struct with an all-zero tail is accepted");

            big[sizeof(accudisc_write_opts) + 4] = 1;
            ck(accudisc_write(NULL, "t.toc", "a.bin",
                              (const accudisc_write_opts *)big, NULL, NULL) ==
                   ACCUDISC_ERR_ABI,
               "write: long struct with a SET tail field is refused, not dropped");
        }

        /* A SHORTER caller: cut before cdtext_path, the field that was already
         * appended once. The poisoned tail must be invented as NULL rather than
         * read — a garbage cdtext_path is a burn with the wrong lead-in. */
        {
            unsigned char small[sizeof(accudisc_write_opts) + 8];
            const uint32_t shortlen =
                (uint32_t)offsetof(accudisc_write_opts, cdtext_path);
            memset(small, 0xAA, sizeof small);
            memcpy(small, &shortlen, sizeof shortlen);
            accudisc_write_opts dst_o;
            memset(&dst_o, 0xBB, sizeof dst_o);
            rc = adsc_abi_import(&dst_o, sizeof dst_o, small, shortlen);
            ck(rc == ACCUDISC_OK, "write_opts: short struct accepted");
            ck(dst_o.cdtext_path == NULL,
               "write_opts: short struct zero-extends cdtext_path (0xAA not read)");
            ck(dst_o.simulate == (int)0xAAAAAAAA,
               "write_opts: a field inside the declared region is copied, not zeroed");
        }
    }

    if (fails) {
        printf("test_abi: %d failure(s)\n", fails);
        return 1;
    }
    printf("test_abi: all pass\n");
    return 0;
}
