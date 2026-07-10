#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
        "  version        print the library version\n"
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
        "  --speed X      set drive read speed to Xx first (best-effort)\n"
        "  --map          render a per-sector disc map when done\n"
        "  -q             quiet: no progress line\n");
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
    return 1;
}

static int cmd_info(accudisc_device *dev)
{
    accudisc_drive_id id;
    int err = accudisc_drive_identify(dev, &id);

    if (err != ACCUDISC_OK)
        return fail_dev(dev, "identify", err);
    printf("vendor %s\nproduct %s\nrevision %s\n", id.vendor, id.product,
           id.revision);
    return 0;
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

    if (err != ACCUDISC_OK)
        return fail_dev(dev, "full TOC", err);

    accudisc_fulltoc ft;
    err = accudisc_fulltoc_parse(raw, len, &ft);
    accudisc_free(raw);
    if (err != ACCUDISC_OK) {
        fprintf(stderr, "accudisc: full TOC parse: %s\n",
                accudisc_strerror(err));
        return 1;
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

    if (err != ACCUDISC_OK)
        return fail_dev(dev, "CD-Text", err);

    accudisc_cdtext *text = NULL;
    err = accudisc_cdtext_decode(raw, len, &text);
    accudisc_free(raw);
    if (err != ACCUDISC_OK) {
        fprintf(stderr, "accudisc: CD-Text decode: %s\n",
                accudisc_strerror(err));
        return 1;
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

    char mcn[14];
    err = accudisc_scan_mcn(dev, toc.tracks[0].lba, mcn);
    if (err == ACCUDISC_OK)
        printf("mcn %s\n", mcn);
    else if (err == ACCUDISC_ERR_NOTFOUND)
        printf("mcn absent\n");
    else
        return fail_dev(dev, "mcn scan", err);

    for (uint8_t i = 0; i < toc.track_count; i++) {
        const accudisc_track *t = &toc.tracks[i];
        char isrc[13];

        if (!ACCUDISC_TRACK_IS_AUDIO(t))
            continue;
        err = accudisc_scan_isrc(dev, t->lba, isrc);
        if (err == ACCUDISC_OK)
            printf("track %u isrc %s\n", t->number, isrc);
        else if (err == ACCUDISC_ERR_NOTFOUND)
            printf("track %u isrc absent\n", t->number);
        else
            return fail_dev(dev, "isrc scan", err);
    }
    return 0;
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
    if (!ctx->quiet && (now - ctx->last_prog >= 0.25 ||
                        ctx->done == ctx->total)) {
        fprintf(stderr, "\r  %u / %u sectors (%.1f%%) ", ctx->done,
                ctx->total, 100.0 * ctx->done / ctx->total);
        ctx->last_prog = now;
    }
    return 0;
}

/* Condensed terminal disc map from the status map: each cell is the WORST
 * state in its bucket (hard > C2 > pending > ok). */
static void render_map(const uint8_t *map, uint32_t count)
{
    enum { WIDTH = 64 };
    uint32_t bucket = (count + WIDTH - 1) / WIDTH;

    fprintf(stderr, "  disc map (%u sectors/cell): '.' ok, '!' C2, "
                    "'X' hard, ' ' pending\n  [", bucket);
    for (uint32_t cell = 0; cell * bucket < count; cell++) {
        uint32_t from = cell * bucket;
        uint32_t to = from + bucket < count ? from + bucket : count;
        char ch = '.';
        int worst = -1;

        for (uint32_t i = from; i < to; i++) {
            uint8_t st = ACCUDISC_MAP_STATE(map[i]);
            int rank = st == ACCUDISC_MAP_HARD    ? 3
                       : st == ACCUDISC_MAP_C2      ? 2
                       : st == ACCUDISC_MAP_PENDING ? 1 : 0;
            if (rank > worst) {
                worst = rank;
                ch = st == ACCUDISC_MAP_HARD      ? 'X'
                     : st == ACCUDISC_MAP_C2      ? '!'
                     : st == ACCUDISC_MAP_PENDING ? ' ' : '.';
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
    long start = 0, count = -1;
    int want_map = 0;

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
                return 2;
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
        else if (!strcmp(a, "--speed") && i + 1 < argc)
            req.speed_x = (uint16_t)strtol(argv[++i], NULL, 0);
        else if (!strcmp(a, "--map"))
            want_map = 1;
        else if (!strcmp(a, "-q"))
            ctx.quiet = 1;
        else {
            usage(stderr);
            return 2;
        }
    }
    if (sub_path && req.sub == ACCUDISC_SUB_NONE) {
        fprintf(stderr, "accudisc: --subf requires --sub raw|q\n");
        return 2;
    }
    if (c2_path && req.c2 == ACCUDISC_C2_NONE) {
        fprintf(stderr, "accudisc: --c2f requires C2 (drop --no-c2)\n");
        return 2;
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

    uint8_t *map = calloc(req.count, 1);
    if (!map) {
        fprintf(stderr, "accudisc: out of memory\n");
        return 1;
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
        fail_dev(dev, "read", err);
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
    if (want_map)
        render_map(map, req.count);
    ret = 0;

out:
    if (ctx.pcm)
        fclose(ctx.pcm);
    if (ctx.c2f)
        fclose(ctx.c2f);
    if (ctx.subf)
        fclose(ctx.subf);
    free(map);
    return ret;
}

/* ---- entry -------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const char *device = "/dev/sr0";
    int i = 1;

    for (; i < argc; i++) {
        const char *a = argv[i];

        if (!strcmp(a, "--device") && i + 1 < argc)
            device = argv[++i];
        else if (!strcmp(a, "--version") || !strcmp(a, "-V")) {
            printf("accudisc %s\n", accudisc_version_string());
            return 0;
        } else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage(stdout);
            return 0;
        } else
            break;
    }
    if (i >= argc) {
        usage(stderr);
        return 2;
    }

    const char *command = argv[i++];
    if (!strcmp(command, "version")) {
        printf("accudisc %s\n", accudisc_version_string());
        return 0;
    }

    int err = 0;
    accudisc_device *dev = accudisc_open(device, 0, &err);
    if (!dev) {
        fprintf(stderr, "accudisc: open %s: %s\n", device,
                accudisc_strerror(err));
        return 1;
    }

    int rc;
    if (!strcmp(command, "info"))
        rc = cmd_info(dev);
    else if (!strcmp(command, "toc"))
        rc = cmd_toc(dev);
    else if (!strcmp(command, "fulltoc"))
        rc = i < argc ? dump_blob(dev, argv[i], "full TOC",
                                  accudisc_read_full_toc)
                      : cmd_fulltoc_parsed(dev);
    else if (!strcmp(command, "cdtext") && i < argc)
        rc = dump_blob(dev, argv[i], "CD-Text", accudisc_read_cdtext);
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
        rc = cmd_read(dev, argc - i, argv + i);
    else {
        fprintf(stderr, "accudisc: unknown command '%s'\n", command);
        usage(stderr);
        rc = 2;
    }

    accudisc_close(dev);
    return rc;
}
