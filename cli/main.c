#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <accudisc/accudisc.h>

static void usage(FILE *to)
{
    fprintf(to,
        "usage: accudisc [--device DEV] <command> [options]\n"
        "\n"
        "commands:\n"
        "  info           identify the drive (INQUIRY)\n"
        "  toc            list tracks (READ TOC, LBA)\n"
        "  fulltoc [FILE] parsed session structure, or raw dump to FILE\n"
        "  cdtext FILE    dump raw CD-Text packs to FILE (no file if absent)\n"
        "  text           decode and print CD-Text (block 0)\n"
        "  scan           MCN and per-track ISRCs from the Q subchannel\n"
        "  features       probe C2/subchannel capability (claim + smoke);\n"
        "                 exits 0 iff C2 is clearly usable\n"
        "  speed-report   print max/current read speed (mode page 2A)\n"
        "  stop           spin the spindle down (no eject)\n"
        "  read           read CD-DA sectors (see read options)\n"
        "  cxscan         hardware C1/C2/CU error census (needs --driver)\n"
        "  version        print the library version\n"
        "\n"
        "driver options (vendor features are OFF unless requested):\n"
        "  --driver auto|NAME  permit loading a vendor driver: auto-match\n"
        "                      the drive, or request NAME (warn if missing)\n"
        "  --drivers-dir DIR   driver location (default: $ACCUDISC_DRIVER_DIR\n"
        "                      or the installed directory)\n"
        "\n"
        "read options:\n"
        "  --start LBA    first sector (default 0)\n"
        "  --count N      sectors to read (default: through lead-out)\n"
        "  --no-c2        do not request C2 pointers (default: requested)\n"
        "  --c2beb        request C2 + block-error bits (296 B) variant\n"
        "  --sub raw|q    also capture subchannel (raw P-W or formatted Q)\n"
        "  --any          expected sector type ANY (default CD-DA)\n"
        "  --pcm FILE     write raw s16le PCM here\n"
        "  --c2f FILE     write the C2 bitmap stream here\n"
        "  --subf FILE    write the subchannel stream here (needs --sub)\n"
        "  --chunk N      sectors per READ CD (default: max under 64 KiB)\n"
        "  --retries K    per-sector attempts on failed chunks (default 2)\n"
        "  --c2-retries N hunt a C2-clean copy of each flagged sector with\n"
        "                 up to N cache-defeated rereads (default 0 = off)\n"
        "  --verify P     read everything P times (cache-defeated); sectors\n"
        "                 whose reads disagree are resolved by consensus or\n"
        "                 marked suspect (default 1 = off)\n"
        "  --overlap K    boundary overlap check: extend each chunk by K\n"
        "                 sectors and verify the seam against the next chunk\n"
        "                 (default 0 = off, max 8)\n"
        "  --ladder LIST  speed ladder for problem-sector rereads, e.g.\n"
        "                 32,16,8,4 — attempt n runs at the n-th speed;\n"
        "                 pass speed is restored for streaming\n"
        "  --speed X      set drive read speed to Xx first (best-effort)\n"
        "  --map          render a per-sector disc map when done\n"
        "  --map-file F   live status map: F is exactly COUNT bytes, one\n"
        "                 status byte per sector (ACCUDISC_MAP_* encoding,\n"
        "                 see accudisc.h), updated in place during the read;\n"
        "                 mmap it MAP_SHARED to watch progress/state live\n"
        "  --progress-fd N  machine progress on fd N, newline-delimited:\n"
        "                 'progress <done> <total>' lines, then a final\n"
        "                 'summary hard= c2= recovered= suspect= rereads=\n"
        "                 slips=' line (stable format; -q does not mute it)\n"
        "  -q             quiet: no human progress line\n"
        "\n"
        "exit codes (all commands):\n"
        "  0  completed, no caveats\n"
        "  1  usage / argument / local file error\n"
        "  2  fatal: device, transport, or command could not complete\n"
        "  3  completed with caveats: data absent (cdtext/fulltoc/scan),\n"
        "     or read finished with hard/suspect/C2-flagged sectors\n"
        "  (exception: 'features' keeps its frozen 0-iff-C2-usable contract)\n");
}

static int fail_dev(accudisc_device *dev, const char *what, int err)
{
    accudisc_sense s;

    accudisc_last_sense(dev, &s);
    fprintf(stderr, "accudisc: %s: %s", what, accudisc_strerror(err));
    if (s.valid)
        fprintf(stderr, " (key=0x%x asc=0x%02x ascq=0x%02x)", s.key, s.asc,
                s.ascq);
    fputc('\n', stderr);
    return 2;
}

static int cmd_info(accudisc_device *dev)
{
    accudisc_drive_id id;
    int32_t offset;
    int err = accudisc_drive_identify(dev, &id);

    if (err != ACCUDISC_OK)
        return fail_dev(dev, "identify", err);
    printf("vendor %s\nproduct %s\nrevision %s\n", id.vendor, id.product,
           id.revision);
    if (accudisc_read_offset(dev, &offset) == ACCUDISC_OK)
        printf("read_offset %+d samples\n", offset);
    else
        printf("read_offset unknown\n");
    printf("access %s\n", accudisc_access_method(dev));
    return 0;
}

/* Plextor Q-Check style census: drive reads while the vendor counters are
 * armed; we sample them every 75 sectors (one second of audio). */
static int cmd_cxscan(accudisc_device *dev, int argc, char **argv)
{
    long start = 0;
    unsigned speed = 0;

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--start") && i + 1 < argc)
            start = strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--speed") && i + 1 < argc)
            speed = (unsigned)strtol(argv[++i], NULL, 0);
        else {
            usage(stderr);
            return 1;
        }
    }

    int err = accudisc_counter_scan_begin(dev);
    if (err == ACCUDISC_ERR_UNSUPPORTED) {
        fprintf(stderr, "accudisc: counter scan unsupported via %s — a "
                        "vendor driver is required (--driver auto)\n",
                accudisc_access_method(dev));
        return 2;
    }
    if (err != ACCUDISC_OK)
        return fail_dev(dev, "counter scan begin", err);

    accudisc_toc toc;
    err = accudisc_read_toc(dev, &toc);
    if (err != ACCUDISC_OK) {
        accudisc_counter_scan_end(dev);
        return fail_dev(dev, "read toc", err);
    }
    if (speed)
        accudisc_set_speed(dev, speed);

    uint64_t tc1 = 0, tc2 = 0, tcu = 0;
    uint32_t peak_c1 = 0, peak_c2 = 0, peak_cu = 0;
    int ret = 0;

    for (long lba = start; lba < (long)toc.leadout_lba; lba += 75) {
        accudisc_read_req req = {0};
        accudisc_counters c;

        req.lba = (uint32_t)lba;
        req.count = (uint32_t)(toc.leadout_lba - lba < 75
                                   ? toc.leadout_lba - lba : 75);
        req.retries = 1;
        err = accudisc_read_cdda(dev, &req, NULL, NULL, NULL);
        if (err != ACCUDISC_OK)
            fprintf(stderr, "accudisc: read at %ld: %s (continuing)\n", lba,
                    accudisc_strerror(err));
        err = accudisc_counter_scan_read(dev, &c);
        if (err != ACCUDISC_OK) {
            ret = fail_dev(dev, "counter read", err);
            break;
        }
        printf("cx %ld %u %u %u\n", lba, c.c1, c.c2, c.cu);
        tc1 += c.c1; tc2 += c.c2; tcu += c.cu;
        if (c.c1 > peak_c1) peak_c1 = c.c1;
        if (c.c2 > peak_c2) peak_c2 = c.c2;
        if (c.cu > peak_cu) peak_cu = c.cu;
    }
    accudisc_counter_scan_end(dev);
    fprintf(stderr, "cx summary: C1 %llu (peak %u/s)  C2 %llu (peak %u/s)  "
                    "CU %llu (peak %u/s)\n",
            (unsigned long long)tc1, peak_c1, (unsigned long long)tc2,
            peak_c2, (unsigned long long)tcu, peak_cu);
    return ret;
}

static int cmd_toc(accudisc_device *dev)
{
    accudisc_toc toc;
    int err = accudisc_read_toc(dev, &toc);

    if (err != ACCUDISC_OK)
        return fail_dev(dev, "read toc", err);
    for (uint8_t i = 0; i < toc.track_count; i++) {
        const accudisc_track *t = &toc.tracks[i];
        printf("track %u lba %u sectors %u %s\n", t->number, t->lba,
               t->sectors, ACCUDISC_TRACK_IS_AUDIO(t) ? "audio" : "data");
    }
    printf("leadout lba %u\n", toc.leadout_lba);
    return 0;
}

static int dump_blob(accudisc_device *dev, const char *path, const char *what,
                     int (*fn)(accudisc_device *, uint8_t **, uint32_t *))
{
    uint8_t *buf = NULL;
    uint32_t len = 0;
    int err = fn(dev, &buf, &len);

    if (err == ACCUDISC_ERR_NOTFOUND) {
        fprintf(stderr, "accudisc: %s: absent (no data on this disc)\n",
                what);
        return 3;
    }
    if (err != ACCUDISC_OK)
        return fail_dev(dev, what, err);

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        perror(path);
        accudisc_free(buf);
        return 1;
    }
    fwrite(buf, 1, len, fp);
    fclose(fp);
    accudisc_free(buf);
    fprintf(stderr, "accudisc: %s: %u bytes -> %s\n", what, len, path);
    return 0;
}

static int cmd_fulltoc_parsed(accudisc_device *dev)
{
    uint8_t *raw = NULL;
    uint32_t len = 0;
    int err = accudisc_read_full_toc(dev, &raw, &len);

    if (err == ACCUDISC_ERR_NOTFOUND) {
        fprintf(stderr, "accudisc: full TOC: absent (no data on this "
                        "disc)\n");
        return 3;
    }
    if (err != ACCUDISC_OK)
        return fail_dev(dev, "full TOC", err);

    accudisc_fulltoc ft;
    err = accudisc_fulltoc_parse(raw, len, &ft);
    accudisc_free(raw);
    if (err != ACCUDISC_OK) {
        fprintf(stderr, "accudisc: full TOC parse: %s\n",
                accudisc_strerror(err));
        return 2;
    }

    printf("sessions %u..%u\n", ft.first_session, ft.last_session);
    for (uint16_t i = 0; i < ft.entry_count; i++) {
        const accudisc_fulltoc_entry *e = &ft.entries[i];

        if (e->point == 0xA0)
            printf("session %u first_track %u disc_type 0x%02x\n",
                   e->session, e->pmin, e->psec);
        else if (e->point == 0xA1)
            printf("session %u last_track %u\n", e->session, e->pmin);
        else if (e->point == 0xA2)
            printf("session %u leadout lba %d\n", e->session,
                   accudisc_msf_to_lba(e->pmin, e->psec, e->pframe));
        else if (e->point <= 0x63)
            printf("session %u track %u lba %d ctrl 0x%x\n", e->session,
                   e->point,
                   accudisc_msf_to_lba(e->pmin, e->psec, e->pframe),
                   e->adr_ctrl & 0x0f);
        else
            printf("session %u point 0x%02x\n", e->session, e->point);
    }
    return 0;
}

static int cmd_text(accudisc_device *dev)
{
    uint8_t *raw = NULL;
    uint32_t len = 0;
    int err = accudisc_read_cdtext(dev, &raw, &len);

    if (err == ACCUDISC_ERR_NOTFOUND) {
        fprintf(stderr, "accudisc: CD-Text: absent (no data on this "
                        "disc)\n");
        return 3;
    }
    if (err != ACCUDISC_OK)
        return fail_dev(dev, "CD-Text", err);

    accudisc_cdtext *text = NULL;
    err = accudisc_cdtext_decode(raw, len, &text);
    accudisc_free(raw);
    if (err != ACCUDISC_OK) {
        fprintf(stderr, "accudisc: CD-Text decode: %s\n",
                accudisc_strerror(err));
        return 2;
    }

    if (text->album.title[0])
        printf("album title %s\n", text->album.title);
    if (text->album.performer[0])
        printf("album performer %s\n", text->album.performer);
    if (text->album.code[0])
        printf("album upc %s\n", text->album.code);
    for (unsigned t = 1; t <= 99; t++) {
        const accudisc_cdtext_strings *s = &text->track[t];

        if (s->title[0])
            printf("track %u title %s\n", t, s->title);
        if (s->performer[0])
            printf("track %u performer %s\n", t, s->performer);
        if (s->songwriter[0])
            printf("track %u songwriter %s\n", t, s->songwriter);
        if (s->code[0])
            printf("track %u isrc %s\n", t, s->code);
    }
    accudisc_free(text);
    return 0;
}

static int cmd_scan(accudisc_device *dev)
{
    accudisc_toc toc;
    int err = accudisc_read_toc(dev, &toc);

    if (err != ACCUDISC_OK)
        return fail_dev(dev, "read toc", err);

    int found = 0;
    char mcn[14];
    err = accudisc_scan_mcn(dev, toc.tracks[0].lba, mcn);
    if (err == ACCUDISC_OK) {
        printf("mcn %s\n", mcn);
        found = 1;
    } else if (err == ACCUDISC_ERR_NOTFOUND)
        printf("mcn absent\n");
    else
        return fail_dev(dev, "mcn scan", err);

    for (uint8_t i = 0; i < toc.track_count; i++) {
        const accudisc_track *t = &toc.tracks[i];
        char isrc[13];

        if (!ACCUDISC_TRACK_IS_AUDIO(t))
            continue;
        err = accudisc_scan_isrc(dev, t->lba, isrc);
        if (err == ACCUDISC_OK) {
            printf("track %u isrc %s\n", t->number, isrc);
            found = 1;
        } else if (err == ACCUDISC_ERR_NOTFOUND)
            printf("track %u isrc absent\n", t->number);
        else
            return fail_dev(dev, "isrc scan", err);
    }
    return found ? 0 : 3; /* nothing at all on this disc: a caveat */
}

static int cmd_features(accudisc_device *dev)
{
    accudisc_features f;
    int err = accudisc_probe_features(dev, &f);

    if (err != ACCUDISC_OK)
        return fail_dev(dev, "probe", err);
    if (f.feature_present)
        printf("cd_read_feature present current=%u dap=%u c2_flags=%u "
               "cd_text=%u\n", f.current, f.dap, f.c2_claimed,
               f.cdtext_claimed);
    else
        printf("cd_read_feature absent\n");
    printf("combo c2 %s\n", f.ok_c2 ? "ok" : "failed");
    printf("combo sub_raw %s\n", f.ok_sub_raw ? "ok" : "failed");
    printf("combo sub_q %s\n", f.ok_sub_q ? "ok" : "failed");
    printf("combo c2+sub_raw %s\n", f.ok_c2_sub_raw ? "ok" : "failed");
    printf("combo c2+sub_q %s\n", f.ok_c2_sub_q ? "ok" : "failed");
    printf("verdict %s\n",
           f.c2_verdict == ACCUDISC_C2_SUPPORTED     ? "C2_SUPPORTED"
           : f.c2_verdict == ACCUDISC_C2_UNVERIFIED  ? "C2_UNVERIFIED"
                                                     : "C2_UNSUPPORTED");

    /* Positional determinism, probed mid-disc where damage is least likely
     * to masquerade as jitter. */
    accudisc_toc toc;
    uint8_t accurate = 0;
    uint32_t probe_lba = accudisc_read_toc(dev, &toc) == ACCUDISC_OK
                             ? toc.leadout_lba / 2 : 3000;
    if (accudisc_probe_accurate_stream(dev, probe_lba, &accurate)
        == ACCUDISC_OK)
        printf("accurate_stream %s\n", accurate ? "yes" : "no");
    else
        printf("accurate_stream unknown\n");

    return f.c2_verdict == ACCUDISC_C2_SUPPORTED ? 0 : 1;
}

static int cmd_speed_report(accudisc_device *dev)
{
    unsigned max_kbps = 0, cur_kbps = 0;
    int err = accudisc_get_speed(dev, &max_kbps, &cur_kbps);

    if (err != ACCUDISC_OK)
        return fail_dev(dev, "mode page 2A", err);
    printf("speed max_kbps %u current_kbps %u max_x %u current_x %u\n",
           max_kbps, cur_kbps, max_kbps / 176, cur_kbps / 176);
    return 0;
}

/* ---- read ------------------------------------------------------------- */

struct read_ctx {
    FILE *pcm, *c2f, *subf;
    uint32_t total, done;
    int quiet;
    int prog_fd; /* -1 = off; machine 'progress <done> <total>' lines */
    double last_prog;
    const uint8_t *map;
};

static double mono_now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int read_sink(void *user, const accudisc_chunk *c)
{
    struct read_ctx *ctx = user;

    for (uint32_t s = 0; s < c->nsec; s++) {
        const uint8_t *sec = c->data + (size_t)s * c->sector_len;

        if (ctx->pcm)
            fwrite(sec, 1, c->audio_len, ctx->pcm);
        if (ctx->c2f && c->c2_len)
            fwrite(sec + c->audio_len, 1, c->c2_len, ctx->c2f);
        if (ctx->subf && c->sub_len)
            fwrite(sec + c->audio_len + c->c2_len, 1, c->sub_len, ctx->subf);
    }
    ctx->done += c->nsec;

    double now = mono_now();
    if (now - ctx->last_prog >= 0.25 || ctx->done == ctx->total) {
        if (!ctx->quiet)
            fprintf(stderr, "\r  %u / %u sectors (%.1f%%) ", ctx->done,
                    ctx->total, 100.0 * ctx->done / ctx->total);
        if (ctx->prog_fd >= 0)
            dprintf(ctx->prog_fd, "progress %u %u\n", ctx->done,
                    ctx->total);
        ctx->last_prog = now;
    }
    return 0;
}

/* Condensed terminal disc map from the status map: each cell is the WORST
 * state in its bucket. */
static void render_map(const uint8_t *map, uint32_t count)
{
    enum { WIDTH = 64 };
    static const struct { uint8_t state; int rank; char ch; } marks[] = {
        { ACCUDISC_MAP_HARD,      5, 'X' },
        { ACCUDISC_MAP_SUSPECT,   4, '?' },
        { ACCUDISC_MAP_C2,        3, '!' },
        { ACCUDISC_MAP_PENDING,   2, ' ' },
        { ACCUDISC_MAP_RECOVERED, 1, 'r' },
        { ACCUDISC_MAP_OK,        0, '.' },
    };
    uint32_t bucket = (count + WIDTH - 1) / WIDTH;

    fprintf(stderr, "  disc map (%u sectors/cell): '.' ok, 'r' recovered, "
                    "'!' C2, '?' suspect, 'X' hard, ' ' pending\n  [",
            bucket);
    for (uint32_t cell = 0; cell * bucket < count; cell++) {
        uint32_t from = cell * bucket;
        uint32_t to = from + bucket < count ? from + bucket : count;
        char ch = '.';
        int worst = -1;

        for (uint32_t i = from; i < to; i++) {
            uint8_t st = ACCUDISC_MAP_STATE(map[i]);

            for (size_t m = 0; m < sizeof(marks) / sizeof(marks[0]); m++) {
                if (marks[m].state == st) {
                    if (marks[m].rank > worst) {
                        worst = marks[m].rank;
                        ch = marks[m].ch;
                    }
                    break;
                }
            }
        }
        fputc(ch, stderr);
    }
    fprintf(stderr, "]\n");
}

static int cmd_read(accudisc_device *dev, int argc, char **argv)
{
    accudisc_read_req req = {0};
    struct read_ctx ctx = {0};
    const char *pcm_path = NULL, *c2_path = NULL, *sub_path = NULL;
    const char *map_path = NULL;
    long start = 0, count = -1;
    int want_map = 0;
    uint16_t ladder[8];

    ctx.prog_fd = -1;
    req.c2 = ACCUDISC_C2_PTRS;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];

        if (!strcmp(a, "--start") && i + 1 < argc)
            start = strtol(argv[++i], NULL, 0);
        else if (!strcmp(a, "--count") && i + 1 < argc)
            count = strtol(argv[++i], NULL, 0);
        else if (!strcmp(a, "--no-c2"))
            req.c2 = ACCUDISC_C2_NONE;
        else if (!strcmp(a, "--c2beb"))
            req.c2 = ACCUDISC_C2_PTRS_BEB;
        else if (!strcmp(a, "--sub") && i + 1 < argc) {
            const char *m = argv[++i];
            if (!strcmp(m, "raw"))
                req.sub = ACCUDISC_SUB_RAW;
            else if (!strcmp(m, "q"))
                req.sub = ACCUDISC_SUB_Q;
            else {
                usage(stderr);
                return 1;
            }
        } else if (!strcmp(a, "--any"))
            req.any_type = 1;
        else if (!strcmp(a, "--pcm") && i + 1 < argc)
            pcm_path = argv[++i];
        else if (!strcmp(a, "--c2f") && i + 1 < argc)
            c2_path = argv[++i];
        else if (!strcmp(a, "--subf") && i + 1 < argc)
            sub_path = argv[++i];
        else if (!strcmp(a, "--chunk") && i + 1 < argc)
            req.chunk_sectors = (uint16_t)strtol(argv[++i], NULL, 0);
        else if (!strcmp(a, "--retries") && i + 1 < argc)
            req.retries = (uint8_t)strtol(argv[++i], NULL, 0);
        else if (!strcmp(a, "--c2-retries") && i + 1 < argc)
            req.c2_retries = (uint8_t)strtol(argv[++i], NULL, 0);
        else if (!strcmp(a, "--verify") && i + 1 < argc)
            req.verify_passes = (uint8_t)strtol(argv[++i], NULL, 0);
        else if (!strcmp(a, "--overlap") && i + 1 < argc)
            req.overlap_sectors = (uint8_t)strtol(argv[++i], NULL, 0);
        else if (!strcmp(a, "--ladder") && i + 1 < argc) {
            char *p = argv[++i];
            while (*p && req.ladder_len < 8) {
                ladder[req.ladder_len++] = (uint16_t)strtol(p, &p, 10);
                if (*p == ',')
                    p++;
                else
                    break;
            }
            req.speed_ladder = ladder;
        }
        else if (!strcmp(a, "--speed") && i + 1 < argc)
            req.speed_x = (uint16_t)strtol(argv[++i], NULL, 0);
        else if (!strcmp(a, "--map"))
            want_map = 1;
        else if (!strcmp(a, "--map-file") && i + 1 < argc)
            map_path = argv[++i];
        else if (!strcmp(a, "--progress-fd") && i + 1 < argc)
            ctx.prog_fd = (int)strtol(argv[++i], NULL, 0);
        else if (!strcmp(a, "-q"))
            ctx.quiet = 1;
        else {
            usage(stderr);
            return 1;
        }
    }
    if (sub_path && req.sub == ACCUDISC_SUB_NONE) {
        fprintf(stderr, "accudisc: --subf requires --sub raw|q\n");
        return 1;
    }
    if (c2_path && req.c2 == ACCUDISC_C2_NONE) {
        fprintf(stderr, "accudisc: --c2f requires C2 (drop --no-c2)\n");
        return 1;
    }

    if (count < 0) { /* default: through lead-out */
        accudisc_toc toc;
        int err = accudisc_read_toc(dev, &toc);
        if (err != ACCUDISC_OK)
            return fail_dev(dev, "read toc", err);
        if ((uint32_t)start >= toc.leadout_lba) {
            fprintf(stderr, "accudisc: start %ld >= lead-out %u\n", start,
                    toc.leadout_lba);
            return 1;
        }
        count = (long)toc.leadout_lba - start;
    }

    req.lba = (uint32_t)start;
    req.count = (uint32_t)count;
    ctx.total = req.count;

    /* The status map is the library's caller-owned buffer; --map-file just
     * makes that buffer a MAP_SHARED file so any other process can watch
     * the same bytes live. ftruncate zero-fills, and 0 = PENDING. */
    uint8_t *map;
    if (map_path) {
        int mfd = open(map_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (mfd < 0 || ftruncate(mfd, (off_t)req.count) != 0) {
            perror(map_path);
            if (mfd >= 0)
                close(mfd);
            return 1;
        }
        map = mmap(NULL, req.count, PROT_READ | PROT_WRITE, MAP_SHARED,
                   mfd, 0);
        close(mfd);
        if (map == MAP_FAILED) {
            perror(map_path);
            return 1;
        }
    } else {
        map = calloc(req.count, 1);
        if (!map) {
            fprintf(stderr, "accudisc: out of memory\n");
            return 2;
        }
    }
    req.status_map = map;

    int ret = 1;
    if (pcm_path && !(ctx.pcm = fopen(pcm_path, "wb"))) {
        perror(pcm_path);
        goto out;
    }
    if (c2_path && !(ctx.c2f = fopen(c2_path, "wb"))) {
        perror(c2_path);
        goto out;
    }
    if (sub_path && !(ctx.subf = fopen(sub_path, "wb"))) {
        perror(sub_path);
        goto out;
    }

    accudisc_read_stats st;
    double t0 = mono_now();
    int err = accudisc_read_cdda(dev, &req, read_sink, &ctx, &st);
    double secs = mono_now() - t0;

    if (!ctx.quiet)
        fputc('\n', stderr);
    if (err != ACCUDISC_OK) {
        ret = fail_dev(dev, "read", err);
        goto out;
    }

    fprintf(stderr, "accudisc read summary\n");
    fprintf(stderr, "  sectors read     : %llu (%.1f s, %.1f sectors/s)\n",
            (unsigned long long)st.sectors_read, secs,
            (double)st.sectors_read / (secs > 0 ? secs : 1));
    fprintf(stderr, "  hard read errors : %llu sectors (zero-filled)\n",
            (unsigned long long)st.hard_errors);
    if (st.hard_errors)
        fprintf(stderr, "  hard error sense : medium %llu, hardware %llu, "
                        "other %llu\n",
                (unsigned long long)st.sense_medium,
                (unsigned long long)st.sense_hardware,
                (unsigned long long)st.sense_other);
    fprintf(stderr, "  C2-flagged       : %llu sectors, %llu bits, "
                    "max %u bits/sector\n",
            (unsigned long long)st.sectors_flagged,
            (unsigned long long)st.c2_bits, st.max_bits_sector);
    if (st.sectors_flagged)
        fprintf(stderr, "  flagged span     : LBA %lld .. %lld\n",
                (long long)st.first_flagged_lba,
                (long long)st.last_flagged_lba);
    if (req.c2_retries || req.verify_passes >= 2 || req.overlap_sectors)
        fprintf(stderr, "  accuracy         : %llu recovered, %llu suspect, "
                        "%llu extra reads, %llu slips\n",
                (unsigned long long)st.sectors_recovered,
                (unsigned long long)st.sectors_suspect,
                (unsigned long long)st.rereads,
                (unsigned long long)st.slips);
    if (want_map)
        render_map(map, req.count);
    /* Machine mirror of the summary block — the stable interface for
     * subprocess consumers (the human block above is not). */
    if (ctx.prog_fd >= 0)
        dprintf(ctx.prog_fd,
                "summary hard=%llu c2=%llu recovered=%llu suspect=%llu "
                "rereads=%llu slips=%llu\n",
                (unsigned long long)st.hard_errors,
                (unsigned long long)st.sectors_flagged,
                (unsigned long long)st.sectors_recovered,
                (unsigned long long)st.sectors_suspect,
                (unsigned long long)st.rereads,
                (unsigned long long)st.slips);
    /* Exit 3 = delivered but degraded: the caller should gate before
     * trusting the image (relative signals only — see the header). */
    ret = (st.hard_errors || st.sectors_suspect || st.sectors_flagged) ? 3
                                                                       : 0;

out:
    if (ctx.pcm)
        fclose(ctx.pcm);
    if (ctx.c2f)
        fclose(ctx.c2f);
    if (ctx.subf)
        fclose(ctx.subf);
    if (map_path)
        munmap(map, req.count); /* the file stays for post-mortem reads */
    else
        free(map);
    return ret;
}

/* ---- entry -------------------------------------------------------------- */

static void log_to_stderr(void *user, const char *msg)
{
    (void)user;
    fprintf(stderr, "accudisc: %s\n", msg);
}

int main(int argc, char **argv)
{
    const char *device = "/dev/sr0";
    const char *driver = NULL;    /* --driver: NULL = vendor features off */
    const char *drivers_dir = NULL;
    const char *command = NULL;
    char *rest[64];
    int nrest = 0;

    /* Global flags are accepted anywhere on the line; the first bare word
     * is the command, everything else is handed to it. */
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (!strcmp(a, "--device") && i + 1 < argc)
            device = argv[++i];
        else if (!strcmp(a, "--driver") && i + 1 < argc)
            driver = argv[++i];
        else if (!strcmp(a, "--drivers-dir") && i + 1 < argc)
            drivers_dir = argv[++i];
        else if (!strcmp(a, "--version") || !strcmp(a, "-V")) {
            printf("accudisc %s\n", accudisc_version_string());
            return 0;
        } else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage(stdout);
            return 0;
        } else if (!command)
            command = a;
        else if (nrest < (int)(sizeof(rest) / sizeof(rest[0])))
            rest[nrest++] = argv[i];
    }
    if (!command) {
        usage(stderr);
        return 1;
    }

    if (!strcmp(command, "version")) {
        printf("accudisc %s\n", accudisc_version_string());
        return 0;
    }

    /* Vendor opcodes are blocked by the kernel's SG_IO filter on read-only
     * fds, so a permitted driver implies a read-write open. */
    int err = 0;
    accudisc_device *dev =
        accudisc_open(device, driver ? ACCUDISC_OPEN_RDWR : 0, &err);
    if (!dev) {
        fprintf(stderr, "accudisc: open %s: %s\n", device,
                accudisc_strerror(err));
        return 2;
    }
    accudisc_set_log(dev, log_to_stderr, NULL);

    if (driver) {
        const char *name = strcmp(driver, "auto") == 0 ? NULL : driver;
        accudisc_driver_attach(dev, name, drivers_dir);
        /* Failure detail arrives via the log sink; the device stays usable
         * on generic MMC either way. */
        fprintf(stderr, "accudisc: using %s\n", accudisc_access_method(dev));
    }

    int rc;
    if (!strcmp(command, "info"))
        rc = cmd_info(dev);
    else if (!strcmp(command, "toc"))
        rc = cmd_toc(dev);
    else if (!strcmp(command, "fulltoc"))
        rc = nrest > 0 ? dump_blob(dev, rest[0], "full TOC",
                                   accudisc_read_full_toc)
                       : cmd_fulltoc_parsed(dev);
    else if (!strcmp(command, "cdtext") && nrest > 0)
        rc = dump_blob(dev, rest[0], "CD-Text", accudisc_read_cdtext);
    else if (!strcmp(command, "text"))
        rc = cmd_text(dev);
    else if (!strcmp(command, "scan"))
        rc = cmd_scan(dev);
    else if (!strcmp(command, "features"))
        rc = cmd_features(dev);
    else if (!strcmp(command, "speed-report"))
        rc = cmd_speed_report(dev);
    else if (!strcmp(command, "stop")) {
        rc = accudisc_spindle_stop(dev);
        if (rc != ACCUDISC_OK)
            rc = fail_dev(dev, "stop", rc);
    } else if (!strcmp(command, "read"))
        rc = cmd_read(dev, nrest, rest);
    else if (!strcmp(command, "cxscan"))
        rc = cmd_cxscan(dev, nrest, rest);
    else {
        fprintf(stderr, "accudisc: unknown command '%s'\n", command);
        usage(stderr);
        rc = 1;
    }

    accudisc_close(dev);
    return rc;
}
