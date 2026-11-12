/* PCE64 host harness: runs the core on a PC for exactness and screenshots.
 *   pce_host GAME.blob FRAMES OUTDIR [--shots a,b,c] [--input FILE] [--audio out.raw] [--hash-every N]
 * Input file lines: "<frame> <buttons>" where buttons is a list of
 *   I II SEL RUN U D L R, or "-" for none. State holds until the next line. */
#ifdef PSG_LOGHASH
#include <stdint.h>
uint32_t psg_log_hash = 2166136261u;
#endif
#include "../core/pce.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static pce_t P;
#define LSTRIDE (MAX_WIDTH + 32)
static uint8_t lbg[SCREEN_LINES * LSTRIDE], lspr[SCREEN_LINES * LSTRIDE];
static uint16_t sext[2 * SCREEN_LINES];

static void rgb_of(uint16_t c, uint8_t *o)
{
    int g = (c >> 6) & 7, r = (c >> 3) & 7, b = c & 7;
    o[0] = r * 255 / 7; o[1] = g * 255 / 7; o[2] = b * 255 / 7;
}

/* deferred mode: compose the frame the way the N64 frontend has the RDP draw it */
static rlist_t RL;
static uint8_t *plane, *sprcache;
static int deferred;
static void compose_line(int y, uint16_t *idx, int w)
{
    rlist_t *rl = &RL;
    uint16_t base = (rl->line[y] & RL_DISP) ? 0 : 0x100;
    for (int x = 0; x < w; x++) idx[x] = base;
    for (int pass = 0; pass < 3; pass++) {
        if (pass == 1) {
            for (int i = 0; i < rl->nband; i++) {
                rl_band_t *b = &rl->band[i];
                if (!(b->flags & RB_BG) || y < b->y0 || y >= b->y0 + b->n) continue;
                int row = y - b->y0;
                for (int x = 0; x < b->w && x < w; x++) {
                    uint8_t v = (b->flags & RB_LAYER) ? lbg[y * LSTRIDE + 16 + x]
                        : plane[((b->by + row) % rl->plane_h) * rl->plane_w + (b->bx + x) % rl->plane_w];
                    if (v & 15) idx[x] = v;
                }
            }
            continue;
        }
        int a = pass == 0 ? 0 : rl->nspr_behind, e = pass == 0 ? rl->nspr_behind : rl->nspr;
        for (int i = a; i < e; i++) {
            rl_spr_t *s = &rl->spr[i];
            if (y < s->y || y >= s->y + s->n) continue;
            int ry = s->ry + (y - s->y);
            int ty = (s->flags & RS_YF) ? s->h - 1 - ry : ry;
            const uint8_t *row = sprcache + s->pair * 256 + ty * 16;
            int W = (s->flags & RS_W32) ? 32 : 16, so = (s->flags & RS_RIGHT) ? 16 : 0;
            for (int k = 0; k < W; k++) {
                int x = s->x + k;
                if (x < 0 || x >= w) continue;
                int col = (s->flags & RS_XF) ? so + W - 1 - k : so + k;
                int c = (row[col >> 1] >> ((col & 1) ? 0 : 4)) & 15;
                if (c) idx[x] = 0x100 | s->pal << 4 | c;
            }
        }
    }
    if (rl->line[y] & RL_SPRL)
        for (int x = 0; x < w; x++) { uint8_t s = lspr[y * LSTRIDE + 16 + x]; if (s) idx[x] = 0x100 | s; }
}

/* colour of a composed pixel: the backdrop and overscan colours are per line */
static uint16_t pix_color(int y, uint16_t idx)
{
    return (idx == 0 || idx == 0x100) ? P.line_bd[y] : P.vce.snap[idx];
}

static uint32_t image_hash(void)
{
    uint32_t h = 2166136261u;
    for (int y = 0; y < SCREEN_LINES; y++) {
        int w = P.line_width[y];
        static uint16_t lidx[MAX_WIDTH];
        if (deferred) compose_line(y, lidx, w);
        else for (int x = 0; x < w; x++) {
            uint8_t b = lbg[y * LSTRIDE + 16 + x], s = lspr[y * LSTRIDE + 16 + x];
            lidx[x] = s == 0x10 ? 0x100 : s ? (0x100 | s) : (b & 0x0F) ? b : 0;
        }
        for (int x = 0; x < w; x++) { h ^= pix_color(y, lidx[x]); h *= 16777619u; }
    }
    return h;
}

static void write_ppm(const char *path)
{
    int w = 0;
    for (int y = 0; y < SCREEN_LINES; y++) if (P.line_width[y] > w) w = P.line_width[y];
    if (!w) w = 256;
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, SCREEN_LINES);
    for (int y = 0; y < SCREEN_LINES; y++)
        for (int x = 0; x < w; x++) {
            uint8_t px[3] = { 0, 0, 0 };
            if (x < P.line_width[y]) {
                uint16_t idx;
                if (deferred) {
                    static uint16_t lidx[MAX_WIDTH]; static int ly = -1;
                    if (ly != y) { compose_line(y, lidx, P.line_width[y]); ly = y; }
                    idx = lidx[x];
                    if (y == SCREEN_LINES - 1 && x == P.line_width[y] - 1) ly = -1;
                } else {
                    uint8_t b = lbg[y * LSTRIDE + 16 + x], s = lspr[y * LSTRIDE + 16 + x];
                    idx = s == 0x10 ? 0x100 : s ? (0x100 | s) : (b & 0x0F) ? b : 0;
                }
                rgb_of(pix_color(y, idx), px);
            }
            fwrite(px, 1, 3, f);
        }
    fclose(f);
}

typedef struct { long frame; uint8_t pad; } inev_t;

static int load_input(const char *path, inev_t *ev, int max)
{
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); exit(1); }
    char line[256];
    int n = 0;
    while (fgets(line, sizeof line, f) && n < max) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || *s == '\n' || !*s) continue;
        long fr = strtol(s, &s, 10);
        uint8_t pad = 0;
        char tok[16];
        int off;
        while (sscanf(s, "%15s%n", tok, &off) == 1) {
            s += off;
            if (!strcmp(tok, "I")) pad |= PAD_I;
            else if (!strcmp(tok, "II")) pad |= PAD_II;
            else if (!strcmp(tok, "SEL")) pad |= PAD_SELECT;
            else if (!strcmp(tok, "RUN")) pad |= PAD_RUN;
            else if (!strcmp(tok, "U")) pad |= PAD_UP;
            else if (!strcmp(tok, "D")) pad |= PAD_DOWN;
            else if (!strcmp(tok, "L")) pad |= PAD_LEFT;
            else if (!strcmp(tok, "R")) pad |= PAD_RIGHT;
        }
        ev[n].frame = fr; ev[n].pad = pad; n++;
    }
    fclose(f);
    return n;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s GAME.blob FRAMES OUTDIR [--shots a,b] [--input F] [--audio F] [--hash-every N]\n", argv[0]);
        return 1;
    }
    const char *blobp = argv[1];
    long frames = atol(argv[2]);
    const char *outdir = argv[3];
    long shots[256]; int nshots = 0;
    const char *inp = NULL, *audp = NULL;
    int hash_every = 60;
    int no_idle = 0;
    const char *img_hash = NULL;
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--shots") && i + 1 < argc) {
            char *s = argv[++i];
            while (*s && nshots < 256) { shots[nshots++] = strtol(s, &s, 10); if (*s == ',') s++; }
        } else if (!strcmp(argv[i], "--input") && i + 1 < argc) inp = argv[++i];
        else if (!strcmp(argv[i], "--audio") && i + 1 < argc) audp = argv[++i];
        else if (!strcmp(argv[i], "--hash-every") && i + 1 < argc) hash_every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-idle")) no_idle = 1;
        else if (!strcmp(argv[i], "--deferred")) deferred = 1;
        else if (!strcmp(argv[i], "--img-hash") && i + 1 < argc) img_hash = argv[++i];
    }

    FILE *f = fopen(blobp, "rb");
    if (!f) { perror(blobp); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *blob = malloc(sz);
    if (fread(blob, 1, sz, f) != (size_t)sz) { fprintf(stderr, "read error\n"); return 1; }
    fclose(f);

    if (pce_load(&P, blob, sz)) { fprintf(stderr, "not a PCE64 blob\n"); return 1; }
    static int16_t abuf[4096];
    psg_init(&P, 32000, abuf, 4096);
    P.layer_bg = lbg + 16; P.layer_spr = lspr + 16; P.layer_stride = LSTRIDE; P.spr_ext = sext;
    if (deferred) {
        plane = calloc(PLANE_MAX_W, PLANE_MAX_H); sprcache = calloc(SPRCACHE_PAIRS, 256);
        P.deferred = 1; P.rl = &RL; P.plane = plane; P.sprcache = sprcache;
    }
    pce_reset(&P);
    if (no_idle) P.idle_skip = 0;

    static inev_t ev[65536];
    int nev = inp ? load_input(inp, ev, 65536) : 0, ei = 0;

    char path[1024];
    snprintf(path, sizeof path, "%s/hashes.txt", outdir);
    FILE *hf = fopen(path, "w");
    FILE *af = audp ? fopen(audp, "wb") : NULL;

    FILE *ihf = img_hash ? fopen(img_hash, "w") : NULL;
    clock_t t0 = clock();
    for (long fr = 0; fr < frames; fr++) {
        while (ei < nev && ev[ei].frame <= fr) { P.pad[0] = ev[ei].pad; ei++; }
        P.psg.bufpos = 0;
#ifdef PCE_COUNT
        { extern uint64_t cnt_ins, cnt_op[256], cnt_bank[256]; extern uint32_t cnt_pc[65536];
          static long cf = -1; if (cf < 0) { char *e = getenv("COUNT_FROM"); cf = e ? atol(e) : 0; }
          if (fr == cf) { cnt_ins = 0; memset(cnt_op, 0, sizeof cnt_op); memset(cnt_bank, 0, sizeof cnt_bank); memset(cnt_pc, 0, sizeof(uint32_t) * 65536); } }
#endif
#ifdef IO_COUNT
        { static long cf = -1; if (cf < 0) { char *e = getenv("COUNT_FROM"); cf = e ? atol(e) : 4560; } if (fr == cf) { extern long ioc_w[8], ioc_a[0x2000]; memset(ioc_w, 0, sizeof ioc_w); memset(ioc_a, 0, sizeof ioc_a); } }
#endif
        pce_run_frame(&P);
        { static int sl = -1; if (sl < 0) sl = getenv("PCE_SPEEDLOG") != NULL; if (sl && !P.speed) printf("LOWSPEED frame %ld\n", fr + 1); }
        { static int wl = -1, lw = 0; if (wl < 0) wl = getenv("PCE_WLOG") != NULL; if (wl && P.line_width[SCREEN_LINES / 2] != lw) { printf("WIDTH frame %ld %d -> %d\n", fr + 1, lw, P.line_width[SCREEN_LINES / 2]); lw = P.line_width[SCREEN_LINES / 2]; } }
        { static int dl = -1; if (dl < 0) dl = getenv("PCE_DISPLOG") != NULL; if (dl && (fr + 1) % 300 == 0) printf("DISP frame %ld start %d end %d vpr %04x vdw %04x vcr %04x hsr %04x hdr %04x w %d\n", fr + 1, P.vdc.disp_start, P.vdc.disp_end, P.vdc.reg[0x0C], P.vdc.reg[0x0D], P.vdc.reg[0x0E], P.vdc.reg[0x0A], P.vdc.reg[0x0B], P.line_width[100]); }
#ifdef PCE_COUNT
        { extern uint8_t cnt_ramx[0x2000]; static uint8_t prev[0x2000]; static long ch, chf; int n = 0;
          for (int i = 0; i < 0x2000; i++) if (cnt_ramx[i] && prev[i] != P.ram[i]) { n++; extern int cnt_hc[0x2000]; cnt_hc[i]++; if (fr == frames - 1) {} }
          if (fr == frames - 1) { static int hc2; (void)hc2; }
          memcpy(prev, P.ram, 0x2000); if (n) { ch += n; chf++; }
          if (fr == frames - 1) { int ex = 0; for (int i = 0; i < 0x2000; i++) ex += cnt_ramx[i]; printf("RAM code bytes %d, changed bytes total %ld in %ld frames\n", ex, ch, chf);
            extern int cnt_hc[0x2000]; for (int i = 0; i < 0x2000; i++) if (cnt_hc[i]) printf("   %04x changed in %d frames now %02x\n", i, cnt_hc[i], P.ram[i]); } }
#endif
        if (af) fwrite(abuf, 2, P.psg.bufpos, af);
        if (ihf) fprintf(ihf, "%ld %08x\n", fr + 1, image_hash());
        if (hf && hash_every && (fr + 1) % hash_every == 0) {
            fprintf(hf, "frame=%ld %08x\n", fr + 1, pce_hash(&P));
#ifdef PSG_LOGHASH
            { extern uint32_t psg_log_hash; fprintf(hf, "psg=%ld %08x\n", fr + 1, psg_log_hash); }
#endif
        }
        for (int s = 0; s < nshots; s++)
            if (shots[s] == fr + 1) {
                snprintf(path, sizeof path, "%s/shot_%06ld.ppm", outdir, fr + 1);
                write_ppm(path);
            }
    }
    if (getenv("PCE_SHOWCR")) printf("CR=%04x RCR=%04x\n", P.vdc.reg[5], P.vdc.reg[6]);
    { char *e = getenv("PCE_DUMPRAM"); if (e) { FILE *df = fopen(e, "wb"); fwrite(P.ram, 1, 0x2000, df); fclose(df); } }
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    if (hf) fclose(hf);
    if (af) fclose(af);
    printf("%ld frames in %.2fs (%.0f fps) PC=%04x mpr=%02x %02x %02x %02x %02x %02x %02x %02x\n",
           frames, secs, frames / (secs > 0 ? secs : 1), P.PC,
           P.mpr[0], P.mpr[1], P.mpr[2], P.mpr[3], P.mpr[4], P.mpr[5], P.mpr[6], P.mpr[7]);
#ifdef IO_COUNT
    { extern void io_report(long); extern long ioc_w[8]; io_report(frames - (getenv("COUNT_FROM") ? atol(getenv("COUNT_FROM")) : 4560)); }
#endif
#ifdef PSG_STATS
    { extern void psg_report(long); psg_report(frames); }
#endif
#ifdef RL_STATS
    { extern long rls_mask; printf("mask lines %ld\n", rls_mask); } { extern long rls_snap, rls_coll, rls_bgh, rls_sprh, rls_bands, rls_layer, rls_spr, rls_cells, rls_plcells, rls_full, rls_frames, rls_sprl;
      long f = rls_frames ? rls_frames : 1;
      printf("deferred: frames %ld, per frame: bands %.1f (cpu %.2f) strips %.1f sprlines %.2f | hazards bg %ld spr %ld | cells/frame spr %.1f plane %.1f, full rebuilds %ld coll lines %ld snapshots %ld\n",
             rls_frames, (double)rls_bands / f, (double)rls_layer / f, (double)rls_spr / f, (double)rls_sprl / f, rls_bgh, rls_sprh, (double)rls_cells / f, (double)rls_plcells / f, rls_full, rls_coll, rls_snap); }
#endif
#ifdef PCE_COUNT
    { extern void bank_report(int); char *e = getenv("COUNT_FROM"); bank_report(frames - (e ? atol(e) : 0)); }
#endif
    return 0;
}
