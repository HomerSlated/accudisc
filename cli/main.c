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
        "  disc           pre-flight guard: is the loaded disc rippable audio,\n"
        "                 a blank to burn, or neither (exit 3)\n"
        "  toc            list tracks (LBA); prefers the full TOC and degrades\n"
        "                 to format 0 on an unreadable lead-in, reporting which\n"
        "  fulltoc [FILE] parsed session structure, or raw dump to FILE\n"
        "  cdtext FILE    dump raw CD-Text packs to FILE (no file if absent)\n"
        "  text           decode and print CD-Text (block 0)\n"
        "  scan           MCN and per-track ISRCs from the Q subchannel\n"
        "  pregaps        per-track index/pregap map from Q (CRC-gated);\n"
        "                 reports pregap Nf / gapless / UNKNOWN per boundary\n"
        "  features [FLAG] probe drive capability. --c2: C2 boolean (exit 0\n"
        "                 iff clearly usable) — the only flag that gates exit.\n"
        "                 --stream: Accurate Stream. --rotation: CAV/CLV drive\n"
        "                 type (disc-free). --all / bare: everything, exit 0\n"
        "  speed [X]      report read speed + rotation (CAV/CLV/...); with X,\n"
        "                 set an Nx ceiling first (SET STREAMING, else SET CD\n"
        "                 SPEED). [--exact] pins the rate (CLV; may be refused).\n"
        "                 [--start L --count N] REQUESTS that range (a perf hint;\n"
        "                 locality is drive-dependent — whole-disc on all drives\n"
        "                 tested so far). A standalone set persists (not restored)\n"
        "  speed-uncap [on|off]\n"
        "                 report or set the vendor read-speed uncap\n"
        "                 (Plextor: SpeedRead) — needs --driver; persistent\n"
        "                 drive state. No argument = report only\n"
        "  media          identify recordable media from ATIP:\n"
        "                 manufacturer, ATIP code, capacity, CD-R/RW\n"
        "  speeds         probe which speed settings the drive really\n"
        "                 honours: [--start L] [--ladder LIST] — timed\n"
        "                 streaming reads per rung; page 2A vs measured\n"
        "  c2lag          probe the drive's C2-bitmap/audio alignment\n"
        "                 [--start L] [--count N] [--speed X] — point it at\n"
        "                 a DAMAGED span (C2 must fire); report-only\n"
        "  stop           spin the spindle down (no eject)\n"
        "  eject          open the tray / unload the disc\n"
        "  load           close the tray / load the disc\n"
        "  read           read CD-DA sectors (see read options)\n"
        "  write          burn an audio session DAO from a cdrdao .toc:\n"
        "                 --toc FILE --bin FILE [--simulate] [--byteswap]\n"
        "                 [--speed X] [--progress-fd N]  (needs blank disc)\n"
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
        "  --start LBA    first sector (default: start of the audio session)\n"
        "  --count N      sectors to read (default: to that session's lead-out)\n"
        "  --session N    which session to read; default is the single audio\n"
        "                 session, and is required when more than one has audio\n"
        "  --tracks A-B   read tracks A through B (or --track N for one); both\n"
        "                 must be in the same session. Overrides --session\n"
        "  --no-c2        do not request C2 pointers (default: requested)\n"
        "  --c2beb        request C2 + block-error bits (296 B) variant\n"
        "  --sub raw|q    also capture subchannel (raw P-W or formatted Q)\n"
        "  --any          expected sector type ANY (default CD-DA)\n"
        "  --force        bypass the audio-range guard (data tracks, session\n"
        "                 seams) — the drive will reject what it cannot read\n"
        "  --fulltoc FILE also dump the raw full TOC (single-spin capture)\n"
        "  --cdtext FILE  also dump raw CD-Text packs; absence is noted\n"
        "                 but does not affect the read's exit code\n"
        "  --pcm FILE     write raw s16le PCM here\n"
        "  --c2f FILE     write the C2 bitmap stream here\n"
        "  --subf FILE    write the subchannel stream here (needs --sub)\n"
        "  --cdg FILE     write CD+G packs here: R-W de-interleaved and\n"
        "                 Reed-Solomon corrected, 24 bytes/pack (needs --sub raw)\n"
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
        "  --uncap        lift the vendor read-speed cap for this read only,\n"
        "                 restoring the drive's prior setting afterwards\n"
        "                 (needs --driver; Plextor: SpeedRead)\n"
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

/* Pre-flight guard: which of burn / rip is legal for the loaded disc. The
 * token is authoritative; the exit code is the coarse branch. */
static int cmd_disc(accudisc_device *dev)
{
    accudisc_disc_probe p;
    int err = accudisc_probe_disc(dev, &p);

    if (err != ACCUDISC_OK)
        return fail_dev(dev, "probe disc", err);

    printf("disc kind=%s profile=0x%04x", accudisc_disc_kind_str(p.kind),
           p.profile);
    if (p.disc_status == ACCUDISC_DISC_STATUS_UNKNOWN)
        printf(" disc_status=-1");
    else
        printf(" disc_status=%u", p.disc_status);
    if (p.erasable == ACCUDISC_DISC_STATUS_UNKNOWN)
        printf(" erasable=-1");
    else
        printf(" erasable=%u", p.erasable);
    printf(" audio_tracks=%u data_tracks=%u reason=%s", p.audio_tracks,
           p.data_tracks, accudisc_disc_reason_str(p.reason));
    /* Only meaningful without a disc, so it is emitted only there. */
    if (p.reason == ACCUDISC_DISC_WHY_NO_MEDIUM)
        printf(" tray=%s", accudisc_tray_state_str(p.tray));
    putchar('\n');

    /* 0 = actionable (the caller reads kind= to pick the path), 3 =
     * classified but neither path is legal. Refusing is a successful
     * classification, not a failure — exit 2 is reserved for not being able
     * to classify at all, which accudisc_probe_disc reports as an error. */
    return p.kind == ACCUDISC_DISC_NEITHER ? 3 : 0;
}

static int cmd_toc(accudisc_device *dev)
{
    accudisc_toc toc;
    accudisc_toc_info info;
    int err = accudisc_read_toc_src(dev, &toc, &info);

    if (err != ACCUDISC_OK)
        return fail_dev(dev, "read toc", err);
    for (uint8_t i = 0; i < toc.track_count; i++) {
        const accudisc_track *t = &toc.tracks[i];
        /* session is appended, never inserted: the first five fields are the
         * frozen contract and stay where existing parsers expect them. */
        printf("track %u lba %u sectors %u %s", t->number, t->lba,
               t->sectors, ACCUDISC_TRACK_IS_AUDIO(t) ? "audio" : "data");
        if (t->session)
            printf(" session %u", t->session);
        /* Appended, and only when non-zero: sectors before this track's INDEX
         * 01 that belong to it (ECMA-130 §20). TOC-derivable only for the
         * first track, where the program area's start at LBA 0 supplies the
         * other edge. Distinct from the `pregaps` token, which reports whether
         * SUBCHANNEL index data was collected for every track. */
        if (t->pregap)
            printf(" pregap %u", t->pregap);
        putchar('\n');
    }
    for (uint8_t i = 0; i < toc.session_count; i++) {
        const accudisc_session *s = &toc.sessions[i];
        printf("session %u tracks %u-%u audio %u data %u leadout %u\n",
               s->number, s->first_track, s->last_track, s->audio_tracks,
               s->data_tracks, s->leadout_lba);
    }
    printf("leadout lba %u\n", toc.leadout_lba);

    /* Acquisition path. Pregaps are deliberately reported as absent here: they
     * live in the program-area Q subchannel, never in the lead-in, so neither
     * READ TOC format supplies them (see `pregaps`). */
    printf("source=%s degrade=%s pregaps=none",
           accudisc_toc_source_str(info.source),
           accudisc_toc_degrade_str(info.degrade));
    if (info.source == ACCUDISC_TOC_SRC_FULLTOC)
        printf(" sessions=%u..%u disc_type=0x%02x", info.first_session,
               info.last_session, info.disc_type);
    /* A COUNT, not a range — the distinction matters. On a degrade this is the
     * only session structure still reachable, and it comes from a different
     * opcode (READ DISC INFORMATION) than the TOC. 0 = nobody could say. */
    printf(" session_count=%u", info.session_count);
    /* Structural defects in the lead-in, as a comma-separated slug list.
     * Absent entirely on a well-formed disc, so nothing changes for the
     * overwhelmingly common case; present it means the TOC contradicts itself
     * and is most likely copy-protected. */
    if (toc.anomalies) {
        int first = 1;

        printf(" anomalies=");
        for (unsigned b = 0; b < 16; b++) {
            if (!(toc.anomalies & (1u << b)))
                continue;
            printf("%s%s", first ? "" : ",",
                   accudisc_toc_anomaly_str(1u << b));
            first = 0;
        }
        if (toc.anomalies & ACCUDISC_TOC_ANOM_UNTRUSTED_GEOMETRY)
            printf(" toc_trusted=0");
    }
    putchar('\n');

    if (info.degrade != ACCUDISC_TOC_DEGRADE_NONE)
        fprintf(stderr,
                "accudisc: full TOC unavailable (%s: %s) — geometry from "
                "READ TOC format 0; session structure unavailable\n",
                accudisc_toc_degrade_str(info.degrade),
                accudisc_strerror(info.degrade_err));

    /* Deliberately exit 0 on a degrade. `toc` promises track geometry, and a
     * degrade still delivers it in full — nothing the caller asked for is
     * missing, only the session structure that format 0 never carried anyway.
     * (Contrast `fulltoc`, where the caller asked for the lead-in itself and
     * exit 3 means they did not get it.) Making a marginal lead-in fail
     * `toc` would regress exactly the discs this fallback exists to serve.
     * The health signal rides on degrade=, which is strictly more informative
     * than an exit code: it separates unreadable from absent from malformed. */
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

/* Collect the raw 96-byte subchannel of each delivered sector into a flat
 * buffer indexed by (lba - base), for the index/pregap decoder. */
struct sub_collector {
    uint8_t *buf;
    uint32_t base;
    uint32_t count;
};

static int sub_collect_sink(void *user, const accudisc_chunk *c)
{
    struct sub_collector *sc = user;

    if (!c->sub_len)
        return 0;
    for (uint32_t s = 0; s < c->nsec; s++) {
        uint32_t lba = c->lba + s;
        if (lba < sc->base || lba - sc->base >= sc->count)
            continue;
        const uint8_t *sec = c->data + (size_t)s * c->sector_len;
        memcpy(sc->buf + (size_t)(lba - sc->base) * 96,
               sec + c->audio_len + c->c2_len, 96);
    }
    return 0;
}

static const char *pregap_state_str(uint8_t st)
{
    switch (st) {
    case ACCUDISC_PREGAP_NONE:    return "gapless";
    case ACCUDISC_PREGAP_PRESENT: return "pregap";
    case ACCUDISC_PREGAP_UNKNOWN: return "UNKNOWN";
    default:                      return "no-data";
    }
}

/* Read each track boundary's neighbourhood and decode the index/pregap map.
 * Boundaries are read separately (seeking defeats the drive cache between
 * them); each window is the 400 sectors before the track start plus a few
 * after to catch the index-1 frame itself. */
#define PREGAP_WINDOW 400u
#define PREGAP_TAIL   4u

static int cmd_pregaps(accudisc_device *dev)
{
    accudisc_toc toc;
    int err = accudisc_read_toc(dev, &toc);

    if (err != ACCUDISC_OK)
        return fail_dev(dev, "read toc", err);

    uint8_t *buf = malloc((size_t)(PREGAP_WINDOW + PREGAP_TAIL) * 96);
    if (!buf) {
        fprintf(stderr, "accudisc: out of memory\n");
        return 1;
    }

    int ret = 0;
    for (uint8_t i = 0; i < toc.track_count; i++) {
        const accudisc_track *t = &toc.tracks[i];
        uint32_t L = t->lba;
        uint32_t start = L > PREGAP_WINDOW ? L - PREGAP_WINDOW : 0;
        uint32_t count = L - start + PREGAP_TAIL;

        struct sub_collector sc = { buf, start, count };
        accudisc_read_req req = {
            .lba = start, .count = count, .sub = ACCUDISC_SUB_RAW,
        };
        err = accudisc_read_cdda(dev, &req, sub_collect_sink, &sc, NULL);
        if (err != ACCUDISC_OK) {
            free(buf);
            return fail_dev(dev, "boundary read", err);
        }

        /* Single-track TOC so the decoder reports just this boundary. */
        accudisc_toc one = { .first_track = t->number, .last_track = t->number,
                             .track_count = 1 };
        one.tracks[0] = *t;
        accudisc_index_map m;
        accudisc_index_map_decode(buf, start, count, &one, &m, 1);

        printf("track %2u  index1 %7d  %-7s", m.track, m.index1_lba,
               pregap_state_str(m.pregap_state));
        if (m.pregap_state == ACCUDISC_PREGAP_PRESENT)
            printf("  %3uf  index0 %7d  [%u ok, %u bad]",
                   m.pregap_frames, m.index0_lba, m.crc_ok, m.crc_bad);
        else if (m.pregap_state == ACCUDISC_PREGAP_UNKNOWN)
            printf("  (damaged approach: %u bad frames near boundary)",
                   m.crc_bad);
        if (m.q_index1_lba >= 0 && m.q_index1_lba != m.index1_lba)
            printf("  [!] Q index1 %d != TOC", m.q_index1_lba);
        putchar('\n');

        if (m.pregap_state == ACCUDISC_PREGAP_UNKNOWN)
            ret = 3; /* at least one boundary needs recovery to resolve */
    }

    free(buf);
    return ret;
}

static const char *rotation_str(accudisc_rotation r)
{
    switch (r) {
    case ACCUDISC_ROTATION_CLV:  return "CLV (constant linear velocity)";
    case ACCUDISC_ROTATION_CAV:  return "CAV (constant angular velocity)";
    case ACCUDISC_ROTATION_PCAV: return "P-CAV (CAV then capped flat)";
    case ACCUDISC_ROTATION_ZCLV: return "Z-CLV (zoned CLV)";
    default:                     return "unknown";
    }
}

/* Probe drive capabilities. Split by flag:
 *   --c2        the C2 boolean question — the ONLY flag that gates the exit
 *               code (0 = clearly usable, 1 = not/unverified, 2 = probe error)
 *   --stream    Accurate Stream / positional determinism (informational)
 *   --rotation  CAV/CLV/... drive type from the nominal curve, disc-free
 *               (informational). Answers "is this a CAV drive", not "what will
 *               it do with this disc" — so it does NOT print the current speed.
 *   --all / bare  everything, informational, exit 0 (2 on probe error)
 */
static int cmd_features(accudisc_device *dev, int argc, char **argv)
{
    int c2 = 0, stream = 0, rotation = 0, all = 0;

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--c2")) c2 = 1;
        else if (!strcmp(argv[i], "--stream")) stream = 1;
        else if (!strcmp(argv[i], "--rotation")) rotation = 1;
        else if (!strcmp(argv[i], "--all")) all = 1;
        else { usage(stderr); return 1; }
    }
    if (!c2 && !stream && !rotation && !all)
        all = 1; /* bare features = show everything */

    accudisc_features f;
    int have_f = 0;
    if (c2 || all) {
        int err = accudisc_probe_features(dev, &f);
        if (err != ACCUDISC_OK)
            return fail_dev(dev, "probe", err);
        have_f = 1;
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
    }

    if (stream || all) {
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
    }

    if (rotation || all) {
        /* Nominal curve is disc-independent (RPM-derived), so this classifies
         * the DRIVE, not the loaded disc — a CAV drive reports CAV with an empty
         * tray. No current speed here; that is `speed`'s job. */
        accudisc_perf_desc pd[16];
        uint32_t n = 0;
        if (accudisc_get_performance(dev, pd, 16, &n) == ACCUDISC_OK && n > 0)
            printf("rotation %s (nominal, RPM-derived)\n",
                   rotation_str(accudisc_classify_rotation(pd, n)));
        else
            printf("rotation unknown (GET PERFORMANCE rejected)\n");
    }

    /* Only --c2 carries a pass/fail meaning; everything else is informational. */
    if (c2)
        return have_f && f.c2_verdict == ACCUDISC_C2_SUPPORTED ? 0 : 1;
    return 0;
}

/* Achievable-speed probe: which --speed / --ladder values are real on
 * this drive+bus+disc, by timed streaming reads. */
static int cmd_speeds(accudisc_device *dev, int argc, char **argv)
{
    uint16_t cand[16];
    uint8_t ncand = 0;
    long start = -1;

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--start") && i + 1 < argc)
            start = strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--ladder") && i + 1 < argc) {
            char *p = argv[++i];
            while (*p && ncand < 16) {
                cand[ncand++] = (uint16_t)strtol(p, &p, 10);
                if (*p == ',')
                    p++;
                else
                    break;
            }
        } else {
            usage(stderr);
            return 1;
        }
    }

    accudisc_toc toc;
    int err = accudisc_read_toc(dev, &toc);
    if (err != ACCUDISC_OK)
        return fail_dev(dev, "read toc", err);

    if (ncand == 0) {
        /* Default rungs: the common ladder, capped at the drive's page 2A
         * maximum (the claim is good enough to pick candidates; the timed
         * read below is what judges them). */
        static const uint16_t common[] = {52, 48, 40, 32, 24, 16, 8, 4};
        unsigned max_kbps = 0, cur_kbps = 0;
        unsigned max_x = accudisc_get_speed(dev, &max_kbps, &cur_kbps)
                             == ACCUDISC_OK ? max_kbps / 176 : 52;

        for (size_t i = 0; i < sizeof(common) / sizeof(common[0]); i++)
            if (common[i] <= max_x)
                cand[ncand++] = common[i];
        if (ncand == 0)
            cand[ncand++] = (uint16_t)(max_x ? max_x : 1);
    }

    /* Middle half of the disc: representative CAV radius, headroom for
     * one fresh window per rung. */
    uint32_t lba = start >= 0 ? (uint32_t)start : toc.leadout_lba / 4;
    uint32_t count = toc.leadout_lba > lba ? toc.leadout_lba - lba : 0;
    if (count > toc.leadout_lba / 2 && start < 0)
        count = toc.leadout_lba / 2;

    accudisc_speed_rung rungs[16];
    err = accudisc_probe_speed_ladder(dev, lba, count, cand, ncand, rungs);
    if (err != ACCUDISC_OK)
        return fail_dev(dev, "speed probe", err);

    for (uint8_t i = 0; i < ncand; i++)
        printf("speed req=%u page2a=%u measured=%u.%02u\n",
               rungs[i].requested_x, rungs[i].reported_x,
               rungs[i].measured_cx / 100, rungs[i].measured_cx % 100);
    return 0;
}

/* C2/audio alignment probe. The result is a factual drive property (like
 * the read offset): print it, record it, never apply it here. */
static int cmd_c2lag(accudisc_device *dev, int argc, char **argv)
{
    long start = 0, count = -1;
    unsigned speed = 0;

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--start") && i + 1 < argc)
            start = strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--count") && i + 1 < argc)
            count = strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--speed") && i + 1 < argc)
            speed = (unsigned)strtol(argv[++i], NULL, 0);
        else {
            usage(stderr);
            return 1;
        }
    }
    if (count < 0) {
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
    if (speed)
        accudisc_set_speed(dev, speed);

    accudisc_c2_lag lag;
    int err = accudisc_probe_c2_lag(dev, (uint32_t)start, (uint32_t)count,
                                    &lag);
    if (err == ACCUDISC_ERR_NOTFOUND) {
        if (lag.sectors_active == 0)
            fprintf(stderr, "accudisc: c2lag: inconclusive — no C2 fired "
                            "anywhere in this span (try a damaged span, or "
                            "a higher --speed to make flags fire)\n");
        else
            fprintf(stderr, "accudisc: c2lag: inconclusive — %u C2-active "
                            "sectors but the reread evidence is too thin "
                            "(flags=%u diffs=%u peak=%u); try a larger "
                            "span\n",
                    lag.sectors_active, lag.flags_used, lag.diff_bytes,
                    lag.peak_milli);
        return 3;
    }
    if (err != ACCUDISC_OK)
        return fail_dev(dev, "c2lag", err);

    printf("c2lag pairs=%d peak=%u runner=%u flags=%u diffs=%u active=%u\n",
           lag.lag_pairs, lag.peak_milli, lag.runner_milli, lag.flags_used,
           lag.diff_bytes, lag.sectors_active);
    return 0;
}

/* Report the drive's read-speed state, optionally setting it first. A bare
 * speed X is the user's request for that end state, so it PERSISTS — the SOP is
 * "honour the instruction, don't pop what you didn't push." --exact / --start /
 * --count route through SET STREAMING only (no SET CD SPEED fallback). */
static int cmd_speed(accudisc_device *dev, int argc, char **argv)
{
    long want_x = -1, start = -1, count = -1;
    int exact = 0;

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--exact"))
            exact = 1;
        else if (!strcmp(argv[i], "--start") && i + 1 < argc)
            start = strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--count") && i + 1 < argc)
            count = strtol(argv[++i], NULL, 0);
        else if (argv[i][0] != '-' && want_x < 0)
            want_x = strtol(argv[i], NULL, 0);
        else {
            usage(stderr);
            return 1;
        }
    }

    int ranged = (start >= 0 || count >= 0 || exact);
    if (ranged && want_x < 0) {
        fprintf(stderr, "accudisc: --exact/--start/--count require a speed\n");
        return 1;
    }
    if (count >= 0 && start < 0) {
        fprintf(stderr, "accudisc: --count requires --start\n");
        return 1;
    }

    if (want_x >= 0) {
        int rc;
        if (ranged) {
            int32_t s = start >= 0 ? (int32_t)start : 0;
            int32_t e = count >= 0 ? (int32_t)(start + count)
                                   : (int32_t)0xFFFFFFFFu; /* whole disc */
            rc = accudisc_set_speed_range(dev, (unsigned)want_x, s, e,
                                          exact ? ACCUDISC_SPEED_EXACT : 0);
        } else {
            rc = accudisc_set_speed(dev, (unsigned)want_x);
        }
        if (rc != ACCUDISC_OK) {
            /* An Illegal Request to --exact IS the answer: the drive will not
             * pin the exact rate at this speed (no CLV here). Report it, then
             * still surface the error status. */
            accudisc_sense sn;
            accudisc_last_sense(dev, &sn);
            if (exact && sn.valid && sn.key == 0x05)
                printf("exact %ldx refused (Illegal Request) — drive will not "
                       "force CLV here\n", want_x);
            return fail_dev(dev, "set speed", rc);
        }
    }

    /* Honoured value: page 2A current is what the drive actually adopted. */
    unsigned max_kbps = 0, cur_kbps = 0;
    if (accudisc_get_speed(dev, &max_kbps, &cur_kbps) == ACCUDISC_OK)
        printf("page2A     max %ux (%u kB/s)  current %ux (%u kB/s)\n",
               max_kbps / 176, max_kbps, cur_kbps / 176, cur_kbps);
    else
        printf("page2A     unavailable\n");

    /* Nominal curve + rotation classification (disc-independent, RPM-derived;
     * a drive that rejects GET PERFORMANCE reports unknown, never inferred). */
    accudisc_perf_desc pd[16];
    uint32_t n = 0;
    if (accudisc_get_performance(dev, pd, 16, &n) == ACCUDISC_OK && n > 0) {
        printf("rotation   %s\n", rotation_str(accudisc_classify_rotation(pd, n)));
        for (uint32_t i = 0; i < n; i++)
            printf("  curve[%u] lba %u..%u  %.1fx..%.1fx (nominal)\n", i,
                   pd[i].start_lba, pd[i].end_lba,
                   pd[i].start_kbps / 176.4, pd[i].end_kbps / 176.4);
    } else {
        printf("rotation   unknown (GET PERFORMANCE rejected)\n");
    }
    return 0;
}

/* Report the read-speed uncap, or set it. The resulting mode-page-2A ceiling
 * is printed alongside, since that is the observable the setting moves. */
static int cmd_speed_uncap(accudisc_device *dev, int argc, char **argv)
{
    unsigned max_kbps = 0, cur_kbps = 0;
    int on = 0, err;

    if (argc > 1) {
        usage(stderr);
        return 1;
    }
    if (argc == 1) {
        if (!strcmp(argv[0], "on"))
            on = 1;
        else if (strcmp(argv[0], "off") != 0) {
            usage(stderr);
            return 1;
        }
        err = accudisc_speed_uncap_set(dev, on);
        if (err == ACCUDISC_ERR_UNSUPPORTED) {
            fprintf(stderr, "accudisc: read-speed uncap unsupported via %s — "
                            "a vendor driver is required (--driver auto)\n",
                    accudisc_access_method(dev));
            return 2;
        }
        if (err != ACCUDISC_OK)
            return fail_dev(dev, "speed-uncap set", err);
    }

    err = accudisc_speed_uncap_get(dev, &on);
    if (err == ACCUDISC_ERR_UNSUPPORTED) {
        fprintf(stderr, "accudisc: read-speed uncap unsupported via %s — a "
                        "vendor driver is required (--driver auto)\n",
                accudisc_access_method(dev));
        return 2;
    }
    if (err != ACCUDISC_OK)
        return fail_dev(dev, "speed-uncap get", err);

    if (accudisc_get_speed(dev, &max_kbps, &cur_kbps) != ACCUDISC_OK)
        max_kbps = 0;
    printf("speed-uncap %s max_kbps %u max_x %u\n", on ? "on" : "off",
           max_kbps, max_kbps / 176);
    return 0;
}

static int cmd_media(accudisc_device *dev)
{
    accudisc_atip atip;
    int err = accudisc_read_atip(dev, &atip);

    if (err == ACCUDISC_ERR_NOTFOUND) {
        /* No ATIP: pressed disc, or no recordable media loaded. */
        printf("atip absent\n");
        return 3;
    }
    if (err != ACCUDISC_OK)
        return fail_dev(dev, "READ ATIP", err);

    /* Machine line; `manufacturer=` is last so its value may contain spaces
     * (empty when the code is not in the catalog). */
    printf("atip leadin=%02u:%02u:%02u leadout=%02u:%02u:%02u type=%s "
           "manufacturer=%s\n",
           atip.lead_in_min, atip.lead_in_sec, atip.lead_in_frame,
           atip.lead_out_min, atip.lead_out_sec, atip.lead_out_frame,
           atip.erasable ? "CD-RW" : "CD-R",
           atip.manufacturer ? atip.manufacturer : "");
    return 0;
}

struct write_prog {
    int prog_fd;      /* machine progress fd, or -1 */
    uint32_t total;
    uint32_t last_emit;
};

static void write_progress(void *user, uint32_t done, uint32_t total)
{
    struct write_prog *wp = user;

    wp->total = total;
    fprintf(stderr, "\rwriting %u/%u (%.1f%%)   ", done, total,
            total ? 100.0 * done / total : 0.0);
    if (wp->prog_fd >= 0 && (done - wp->last_emit >= 4096 || done == total)) {
        dprintf(wp->prog_fd, "progress %u %u\n", done, total);
        wp->last_emit = done;
    }
}

static int cmd_write(accudisc_device *dev, int argc, char **argv)
{
    const char *toc = NULL, *bin = NULL;
    accudisc_write_opts o = {0};
    struct write_prog wp = { -1, 0, 0 };

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--toc") && i + 1 < argc)
            toc = argv[++i];
        else if (!strcmp(argv[i], "--bin") && i + 1 < argc)
            bin = argv[++i];
        else if (!strcmp(argv[i], "--simulate"))
            o.simulate = 1;
        else if (!strcmp(argv[i], "--byteswap"))
            o.byteswap = 1;
        else if (!strcmp(argv[i], "--speed") && i + 1 < argc)
            o.speed = (int)strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--progress-fd") && i + 1 < argc)
            wp.prog_fd = (int)strtol(argv[++i], NULL, 0);
        else {
            usage(stderr);
            return 1;
        }
    }
    if (!toc || !bin) {
        fprintf(stderr, "accudisc: write needs --toc FILE and --bin FILE\n");
        return 1;
    }

    int err = accudisc_write(dev, toc, bin, &o, write_progress, &wp);
    fprintf(stderr, "\n");
    if (err == ACCUDISC_ERR_UNSUPPORTED) {
        fprintf(stderr, "accudisc: write: disc is not blank\n");
        return 3;
    }
    if (err != ACCUDISC_OK)
        return fail_dev(dev, "write", err);

    const char *mode = o.simulate ? "simulate" : "burn";
    printf("write done sectors=%u mode=%s\n", wp.total, mode);
    if (wp.prog_fd >= 0)
        dprintf(wp.prog_fd, "summary sectors=%u mode=%s result=ok\n",
                wp.total, mode);
    return 0;
}

/* ---- read ------------------------------------------------------------- */

struct read_ctx {
    FILE *pcm, *c2f, *subf, *cdgf;
    accudisc_rw *rw; /* R-W decoder state; NULL unless --cdg */
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
        /* CD+G: de-interleave and RS-correct the R-W stream into 24-byte
         * packs. Streamed rather than buffered — the decoder carries the
         * 8-pack window it needs, so a whole-disc rip costs no extra memory. */
        if (ctx->rw && c->sub_len == ACCUDISC_BYTES_SUB_RAW) {
            accudisc_rw_pack pk[ACCUDISC_RW_PACKS_PER_SEC];
            unsigned got = 0;

            if (accudisc_rw_feed(ctx->rw,
                                 sec + c->audio_len + c->c2_len, pk,
                                 ACCUDISC_RW_PACKS_PER_SEC, &got) == ACCUDISC_OK)
                for (unsigned p = 0; p < got; p++)
                    fwrite(pk[p].symbol, 1, ACCUDISC_RW_PACK_SYMBOLS,
                           ctx->cdgf);
        }
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
    const char *cdg_path = NULL;
    const char *map_path = NULL, *fulltoc_path = NULL, *cdtext_path = NULL;
    long start = 0, count = -1;
    int have_start = 0, want_session = -1, force = 0;
    int want_first_track = 0, want_last_track = 0;
    int want_map = 0;
    int uncap = 0, uncap_prior = -1; /* -1 = never touched, nothing to restore */
    uint16_t ladder[8];

    ctx.prog_fd = -1;
    req.c2 = ACCUDISC_C2_PTRS;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];

        if (!strcmp(a, "--start") && i + 1 < argc) {
            start = strtol(argv[++i], NULL, 0);
            have_start = 1;
        } else if (!strcmp(a, "--session") && i + 1 < argc)
            want_session = (int)strtol(argv[++i], NULL, 0);
        else if ((!strcmp(a, "--track") || !strcmp(a, "--tracks")) &&
                 i + 1 < argc) {
            /* "N" or "A-B". One spelling accepts both so --track 3 and
             * --tracks 2-11 both read naturally. */
            const char *v = argv[++i];
            char *end = NULL;
            long f = strtol(v, &end, 10);

            want_first_track = (int)f;
            if (end && *end == '-' && end[1])
                want_last_track = (int)strtol(end + 1, &end, 10);
            else
                want_last_track = (int)f;
            if (!end || *end || f < 1 || want_last_track < want_first_track ||
                want_last_track > 99) {
                fprintf(stderr, "accudisc: bad --tracks '%s' (want N or A-B)\n",
                        v);
                return 1;
            }
        } else if (!strcmp(a, "--count") && i + 1 < argc)
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
        else if (!strcmp(a, "--force"))
            force = 1;
        else if (!strcmp(a, "--fulltoc") && i + 1 < argc)
            fulltoc_path = argv[++i];
        else if (!strcmp(a, "--cdtext") && i + 1 < argc)
            cdtext_path = argv[++i];
        else if (!strcmp(a, "--pcm") && i + 1 < argc)
            pcm_path = argv[++i];
        else if (!strcmp(a, "--c2f") && i + 1 < argc)
            c2_path = argv[++i];
        else if (!strcmp(a, "--cdg") && i + 1 < argc)
            cdg_path = argv[++i];
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
        else if (!strcmp(a, "--uncap"))
            uncap = 1;
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
    if (cdg_path && req.sub != ACCUDISC_SUB_RAW) {
        /* CD+G lives in R-W, which only the raw 96-byte form carries; the
         * deinterleaved Q form has thrown those channels away already. */
        fprintf(stderr, "accudisc: --cdg requires --sub raw\n");
        return 1;
    }
    if (sub_path && req.sub == ACCUDISC_SUB_NONE) {
        fprintf(stderr, "accudisc: --subf requires --sub raw|q\n");
        return 1;
    }
    if (c2_path && req.c2 == ACCUDISC_C2_NONE) {
        fprintf(stderr, "accudisc: --c2f requires C2 (drop --no-c2)\n");
        return 1;
    }

    /* Resolve the range against the TOC, then refuse ranges that cannot be
     * read as CD-DA. --force is the deliberate escape hatch; it is kept
     * separate from --any, which only selects the READ CD expected sector
     * type. Conflating them would mean you could not ask for a CD-DA read of
     * a data track even to observe how the drive rejects it.
     *
     * The default is ONE SESSION, not the whole disc. "Whole disc" is wrong on
     * anything multi-session: between session 1's last track and session 2's
     * first sit a lead-out, a lead-in and a pregap that hold no payload. */
    if (!force || count < 0) {
        accudisc_toc toc;
        accudisc_range_check chk;
        accudisc_toc_info info = {0};
        int err = accudisc_read_toc_src(dev, &toc, &info);

        if (err != ACCUDISC_OK)
            return fail_dev(dev, "read toc", err);

        if (want_first_track) {
            uint32_t tlba = 0, tcount = 0;

            err = accudisc_toc_track_range(&toc, (uint8_t)want_first_track,
                                           (uint8_t)want_last_track, &tlba,
                                           &tcount);
            if (err == ACCUDISC_ERR_NOTFOUND) {
                fprintf(stderr,
                        "accudisc: tracks %d-%d: not on this disc (has 1-%u)\n",
                        want_first_track, want_last_track,
                        toc.track_count ? toc.tracks[toc.track_count - 1].number
                                        : 0);
                return 1;
            }
            if (err == ACCUDISC_ERR_UNSUPPORTED) {
                fprintf(stderr,
                        "accudisc: tracks %d and %d are in different sessions; "
                        "a span across a session seam includes the lead-out "
                        "and lead-in between them\n",
                        want_first_track, want_last_track);
                return 1;
            }
            if (err != ACCUDISC_OK) {
                fprintf(stderr, "accudisc: tracks %d-%d: no usable extent\n",
                        want_first_track, want_last_track);
                return 1;
            }
            if (!have_start)
                start = (long)tlba;
            if (count < 0)
                count = (long)(tlba + tcount) - start;
            if (!ctx.quiet)
                fprintf(stderr, "accudisc: tracks %d-%d, lba %ld count %ld\n",
                        want_first_track, want_last_track, start, count);
        } else if (want_session < 0 && !have_start && count < 0) {
            /* Neither a range nor a session named: pick the audio session, if
             * there is exactly one. */
            int s = accudisc_toc_default_audio_session(&toc);

            if (s == ACCUDISC_ERR_UNSUPPORTED) {
                fprintf(stderr,
                        "accudisc: more than one session contains audio; "
                        "choose one with --session N\n");
                for (uint8_t i = 0; i < toc.session_count; i++)
                    fprintf(stderr, "  session %u tracks %u-%u audio %u data %u\n",
                            toc.sessions[i].number, toc.sessions[i].first_track,
                            toc.sessions[i].last_track,
                            toc.sessions[i].audio_tracks,
                            toc.sessions[i].data_tracks);
                return 1;
            }
            if (s == ACCUDISC_ERR_NOTFOUND) {
                fprintf(stderr, "accudisc: no session contains audio tracks "
                                "(%u track%s, all marked data)\n",
                        toc.track_count, toc.track_count == 1 ? "" : "s");
                /* CTRL is the TOC's only statement about track type, and it
                 * can be a deliberate lie: SunnComm MediaCloQ presents a disc
                 * whose audio a CD player happily plays as DATA tracks to a
                 * computer drive, which is the whole protection. Refusing is
                 * correct — we report what the disc claims — but a user whose
                 * disc plays fine in a hi-fi deserves to know why we disagree
                 * and what the way past is. */
                fprintf(stderr,
                        "accudisc: if this disc plays in an audio CD player, "
                        "its CTRL bits may be misreporting track type — some\n"
                        "accudisc: copy-protection schemes do this "
                        "deliberately. --force reads it anyway.\n");
                return 1;
            }
            if (s > 0)
                want_session = s;
            /* ERR_INVAL = no session structure (degraded lead-in): fall
             * through to the flat whole-disc default, which the range guard
             * still vets. */
        }

        if (want_session > 0) {
            uint32_t slba = 0, scount = 0;

            /* The AUDIO span, not the whole session. On a Mixed Mode CD the
             * session also holds a data track; the whole-session range would
             * include it and the guard below would (correctly) refuse the lot. */
            err = accudisc_toc_session_audio_range(&toc, (uint8_t)want_session,
                                                   &slba, &scount);
            if (err == ACCUDISC_ERR_UNSUPPORTED) {
                fprintf(stderr,
                        "accudisc: session %d has audio tracks either side of "
                        "a data track; no single range covers them\n",
                        want_session);
                fprintf(stderr,
                        "accudisc: select explicitly, e.g. --tracks A-B\n");
                return 1;
            }
            if (err != ACCUDISC_OK) {
                fprintf(stderr,
                        "accudisc: session %d not on this disc, or has no "
                        "audio tracks\n", want_session);
                return 1;
            }
            if (!have_start)
                start = (long)slba;
            if (count < 0)
                count = (long)(slba + scount) - start;
            if (!ctx.quiet)
                fprintf(stderr, "accudisc: session %d, lba %ld count %ld\n",
                        want_session, start, count);
        } else if (count < 0) { /* no session structure: through lead-out */
            if ((uint32_t)start >= toc.leadout_lba) {
                fprintf(stderr, "accudisc: start %ld >= lead-out %u\n", start,
                        toc.leadout_lba);
                return 1;
            }
            count = (long)toc.leadout_lba - start;
        }

        if (count <= 0) {
            fprintf(stderr, "accudisc: empty range (count %ld)\n", count);
            return 1;
        }

        if (!force &&
            accudisc_check_audio_range(&toc, (uint32_t)start, (uint32_t)count,
                                       &chk) != ACCUDISC_OK) {
            fprintf(stderr,
                    "accudisc: refusing lba %ld count %ld: %s at lba %u",
                    start, count, accudisc_range_reason_str(chk.reason),
                    chk.first_bad_lba);
            if (chk.track)
                fprintf(stderr, " (track %u)", chk.track);
            fprintf(stderr, "\n");
            fprintf(stderr,
                    "accudisc: these sectors are not readable as CD-DA; "
                    "--force overrides\n");
            return 1;
        }
    }

    req.lba = (uint32_t)start;
    req.count = (uint32_t)count;
    ctx.total = req.count;

    /* Inline lead-in capture: fold the metadata dumps into this invocation
     * so a full capture is one spin-up instead of three. Absence (exit 3
     * from dump_blob) is a note, not a failure; real I/O errors abort. */
    if (fulltoc_path) {
        int frc = dump_blob(dev, fulltoc_path, "full TOC",
                            accudisc_read_full_toc);
        if (frc != 0 && frc != 3)
            return frc;
    }
    if (cdtext_path) {
        int crc = dump_blob(dev, cdtext_path, "CD-Text",
                            accudisc_read_cdtext);
        if (crc != 0 && crc != 3)
            return crc;
    }

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
    if (cdg_path) {
        if (!(ctx.cdgf = fopen(cdg_path, "wb"))) {
            perror(cdg_path);
            goto out; /* ret already 1; bare return would leak map/pcm/c2f */
        }
        if (!(ctx.rw = accudisc_rw_open())) {
            fprintf(stderr, "accudisc: out of memory\n");
            goto out;
        }
    }
    if (sub_path && !(ctx.subf = fopen(sub_path, "wb"))) {
        perror(sub_path);
        goto out;
    }

    /* SpeedRead is an audio-only accelerator: it pins the drive's CAV RPM to
     * the outer-edge target across the whole disc, which corrupts the Q
     * subchannel on inner/mid tracks (measured 0% Q-CRC there; see
     * drivers/plextor/FEATURES.md). Never lift the cap while capturing sub. */
    if (uncap && req.sub != ACCUDISC_SUB_NONE) {
        fprintf(stderr, "accudisc: --uncap cannot be combined with --sub — "
                        "SpeedRead corrupts the Q subchannel\n");
        ret = 1;
        goto out;
    }

    /* Warn (driver-free) if SpeedRead already looks enabled for a subchannel
     * read: on a Plextor, mode-page-2A max read speed above 40x means the
     * uncap is on — persistent drive state a prior session may have left set.
     * We do not change it (caller's drive); we make the silent loss visible. */
    if (req.sub != ACCUDISC_SUB_NONE && !ctx.quiet) {
        accudisc_drive_id id;
        unsigned mk = 0, ck = 0;
        if (accudisc_drive_identify(dev, &id) == ACCUDISC_OK &&
            !strcmp(id.vendor, "PLEXTOR") &&
            accudisc_get_speed(dev, &mk, &ck) == ACCUDISC_OK && mk / 176 > 40)
            fprintf(stderr,
                "accudisc: WARNING: SpeedRead appears enabled (max read %ux) "
                "with --sub requested;\n"
                "  the Q subchannel will be corrupted on inner/mid tracks. "
                "Disable it first:\n"
                "  accudisc speed-uncap off --driver plextor\n",
                mk / 176);
    }

    /* --uncap: lift the firmware read-speed cap for this read only. The
     * setting is persistent drive state, so the prior value is restored
     * afterwards — a read must not silently reconfigure the drive. */
    if (uncap) {
        int err = accudisc_speed_uncap_get(dev, &uncap_prior);

        if (err == ACCUDISC_ERR_UNSUPPORTED) {
            fprintf(stderr, "accudisc: --uncap unsupported via %s — a vendor "
                            "driver is required (--driver auto)\n",
                    accudisc_access_method(dev));
            ret = 2;
            goto out;
        }
        if (err != ACCUDISC_OK) {
            ret = fail_dev(dev, "speed-uncap get", err);
            goto out;
        }
        if ((err = accudisc_speed_uncap_set(dev, 1)) != ACCUDISC_OK) {
            ret = fail_dev(dev, "speed-uncap set", err);
            goto out;
        }
        if (!ctx.quiet)
            fprintf(stderr, "read-speed uncap: on (was %s)\n",
                    uncap_prior ? "on" : "off");
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
    if (st.subq_total)
        fprintf(stderr, "  subchannel Q     : %llu/%llu CRC-ok (%.2f%%), "
                        "%llu bad\n",
                (unsigned long long)st.subq_ok,
                (unsigned long long)st.subq_total,
                100.0 * (double)st.subq_ok / (double)st.subq_total,
                (unsigned long long)(st.subq_total - st.subq_ok));
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
                "rereads=%llu slips=%llu subq_total=%llu subq_ok=%llu "
                "subq_bad=%llu\n",
                (unsigned long long)st.hard_errors,
                (unsigned long long)st.sectors_flagged,
                (unsigned long long)st.sectors_recovered,
                (unsigned long long)st.sectors_suspect,
                (unsigned long long)st.rereads,
                (unsigned long long)st.slips,
                (unsigned long long)st.subq_total,
                (unsigned long long)st.subq_ok,
                (unsigned long long)(st.subq_total - st.subq_ok));
    /* Exit 3 = delivered but degraded: the caller should gate before
     * trusting the image (relative signals only — see the header). */
    ret = (st.hard_errors || st.sectors_suspect || st.sectors_flagged) ? 3
                                                                       : 0;

out:
    if (uncap_prior >= 0)
        accudisc_speed_uncap_set(dev, uncap_prior); /* leave the drive as found */
    if (ctx.pcm)
        fclose(ctx.pcm);
    if (ctx.c2f)
        fclose(ctx.c2f);
    if (ctx.subf)
        fclose(ctx.subf);
    if (ctx.rw) {
        accudisc_rw_stats rs;

        accudisc_rw_get_stats(ctx.rw, &rs);
        if (!ctx.quiet) {
            fprintf(stderr, "\naccudisc cdg summary\n");
            fprintf(stderr, "  packs written    : %llu\n",
                    (unsigned long long)rs.packs);
            fprintf(stderr, "  graphics / zero  : %llu / %llu\n",
                    (unsigned long long)rs.mode_graphics,
                    (unsigned long long)rs.mode_zero);
            fprintf(stderr, "  RS repaired      : %llu symbols (P %llu, Q %llu)\n",
                    (unsigned long long)(rs.p_fixed + rs.q_fixed),
                    (unsigned long long)rs.p_fixed,
                    (unsigned long long)rs.q_fixed);
            fprintf(stderr, "  RS gave up       : %llu packs (P %llu, Q %llu)\n",
                    (unsigned long long)(rs.p_failed + rs.q_failed),
                    (unsigned long long)rs.p_failed,
                    (unsigned long long)rs.q_failed);
            /* An all-zero R-W stream is the normal state of an ordinary audio
             * CD, not a failure — say so, so nobody reads 0 graphics packs as
             * a bug in the decoder. */
            if (rs.packs && !rs.mode_graphics)
                fprintf(stderr,
                        "  note             : no graphics packs — this disc "
                        "carries no CD+G\n");
        }
        accudisc_rw_close(ctx.rw);
    }
    if (ctx.cdgf)
        fclose(ctx.cdgf);
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

    /* Vendor opcodes and WRITE(10)/SEND CUE SHEET are blocked by the kernel's
     * SG_IO filter on read-only fds, so a permitted driver or the write
     * command implies a read-write open. */
    int need_rdwr = driver != NULL || strcmp(command, "write") == 0;
    int err = 0;
    accudisc_device *dev =
        accudisc_open(device, need_rdwr ? ACCUDISC_OPEN_RDWR : 0, &err);
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
    else if (!strcmp(command, "disc"))
        rc = cmd_disc(dev);
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
    else if (!strcmp(command, "pregaps"))
        rc = cmd_pregaps(dev);
    else if (!strcmp(command, "scan"))
        rc = cmd_scan(dev);
    else if (!strcmp(command, "features"))
        rc = cmd_features(dev, nrest, rest);
    else if (!strcmp(command, "speed"))
        rc = cmd_speed(dev, nrest, rest);
    else if (!strcmp(command, "speed-uncap"))
        rc = cmd_speed_uncap(dev, nrest, rest);
    else if (!strcmp(command, "media"))
        rc = cmd_media(dev);
    else if (!strcmp(command, "write"))
        rc = cmd_write(dev, nrest, rest);
    else if (!strcmp(command, "c2lag"))
        rc = cmd_c2lag(dev, nrest, rest);
    else if (!strcmp(command, "speeds"))
        rc = cmd_speeds(dev, nrest, rest);
    else if (!strcmp(command, "stop")) {
        rc = accudisc_spindle_stop(dev);
        if (rc != ACCUDISC_OK)
            rc = fail_dev(dev, "stop", rc);
    } else if (!strcmp(command, "eject")) {
        rc = accudisc_eject(dev);
        if (rc != ACCUDISC_OK)
            rc = fail_dev(dev, "eject", rc);
    } else if (!strcmp(command, "load")) {
        rc = accudisc_load(dev);
        if (rc != ACCUDISC_OK)
            rc = fail_dev(dev, "load", rc);
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
