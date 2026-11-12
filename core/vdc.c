/* HuC6270 VDC + HuC6260 VCE, scanline renderer.
 * Output is one row of 9-bit color indices per line (0-255 BG, 256-511 sprites),
 * which the frontend turns into pixels with its own palette table. */
#include "pce.h"

#define SNAP_MAX 64     /* sprite cells snapshotted per frame (vdc_t snap_*) */
#define COLD __attribute__((noinline, cold))
#ifdef PCE_PROF
extern uint64_t (*pce_ticks)(void);
uint64_t prof_bg, prof_spr, prof_prep;
#define VPT() pce_ticks()
#else
#define VPT() 0
#endif

enum { R_MAWR = 0, R_MARR = 1, R_VWR = 2, R_CR = 5, R_RCR = 6, R_BXR = 7, R_BYR = 8,
       R_MWR = 9, R_HSR = 10, R_HDR = 11, R_VPR = 12, R_VDW = 13, R_VCR = 14,
       R_DCR = 15, R_SOUR = 16, R_DESR = 17, R_LENR = 18, R_SATB = 19 };

enum { ST_CR = 0x01, ST_OR = 0x02, ST_RR = 0x04, ST_DS = 0x08, ST_DV = 0x10, ST_VD = 0x20 };

static const int incs[4] = { 1, 32, 64, 128 };

static void bat_recount(vdc_t *v);
static void vis_reset(vdc_t *v);
void vdc_reset(pce_t *p)
{
    vdc_t *v = &p->vdc;
    memset(v->vram, 0, sizeof(v->vram));
    memset(v->satb, 0, sizeof(v->satb));
    memset(v->reg, 0, sizeof(v->reg));
    v->sel = 0; v->status = 0; v->rdbuf = 0; v->wlatch = 0;
    v->yoff = 0; v->raster = 0; v->satb_pending = 0; v->ds_line = -1; v->rr_late = -1;
    v->b_n = 0;
    memset(v->bgdirty, 1, sizeof(v->bgdirty));
    v->reg[R_VPR] = 0x0F02; v->reg[R_VDW] = 0x00EF; v->reg[R_VCR] = 0x0003;
    v->reg[R_HDR] = 0x001F; v->reg[R_HSR] = 0x0202;
    v->disp_start = 20; v->disp_end = 260;
    memset(&p->vce, 0, sizeof(p->vce));
    p->vce.dirty = 1;
    v->pl_full = 1; v->pl_mwr = 0xFFFF; v->bg_pend = v->spr_pend = 0;
    v->spr_first = v->spr_last = -1;
    memset(v->sc_frozen, 0, sizeof v->sc_frozen);
    v->snap_n = 0;
    vis_reset(v);
    v->pl_tdirty_any = 0; v->pl_dn = 0;
    memset(v->pl_tdirty, 0, sizeof v->pl_tdirty);
    memset(v->pl_cdirty, 0, sizeof v->pl_cdirty);
    memset(v->sc_dirty, 1, sizeof v->sc_dirty);
    memset(v->cell_used, 0, sizeof v->cell_used);
    bat_recount(v);
    for (int i = 0; i < 512; i++) p->pal_native[i] = p->colortab ? p->colortab[0] : 0;
}

static void vdc_irq(pce_t *p, int bit)
{
    /* the VDC only raises a status bit when its interrupt is enabled */
    vdc_t *v = &p->vdc;
    uint16_t cr = v->reg[R_CR], dcr = v->reg[R_DCR];
    int en;
    switch (bit) {
    case ST_CR: en = cr & 0x01; break;
    case ST_OR: en = cr & 0x02; break;
    case ST_RR: en = cr & 0x04; break;
    case ST_VD: en = cr & 0x08; break;
    case ST_DS: en = dcr & 0x01; break;
    case ST_DV: en = dcr & 0x02; break;
    default: en = 0;
    }
    if (!en) return;
    if (bit == ST_RR) p->rcr_irqs++;
    v->status |= bit;
    p->irq_pend |= 2;
    pce_update_irq(p);
}

static void band_flush(pce_t *p);
static void vram_track(pce_t *p, unsigned addr, unsigned old, unsigned data);

static inline void vram_write(pce_t *p, uint16_t addr, uint16_t data)
{
    vdc_t *v = &p->vdc;
    if (addr & 0x8000) return;
    uint16_t old = v->vram[addr];
#ifdef PCE_VERIFY
    if (p->vlog && p->vwlog && p->vwn < 8192) { p->vwlog[p->vwn][0] = addr; p->vwlog[p->vwn][1] = data; p->vwlog[p->vwn][2] = p->in_cpu ? pce_now(p) : -1; p->vwlog[p->vwn][3] = p->dbg_pc; p->vwn++; }
#endif
    if (old == data) return;
    if (p->deferred) vram_track(p, addr, old, data);
    else if (v->b_n) { EMUX_BEGIN(PS_VRAMW); band_flush(p); EMUX_END(PS_VRAMW); }      /* lines already shown keep the old data */
    v->vram[addr] = data;
    v->bgdirty[addr >> 4] = 1;
}

static void vram_dma(pce_t *p)
{
    vdc_t *v = &p->vdc;
    uint16_t dcr = v->reg[R_DCR];
    int sinc = (dcr & 4) ? -1 : 1, dinc = (dcr & 8) ? -1 : 1;
    uint32_t len = (uint32_t)v->reg[R_LENR] + 1;
    uint16_t s = v->reg[R_SOUR], d = v->reg[R_DESR];
    while (len--) {
        vram_write(p, d, v->vram[s & 0x7FFF]);
        s += sinc; d += dinc;
    }
    v->reg[R_SOUR] = s; v->reg[R_DESR] = d; v->reg[R_LENR] = 0xFFFF;
    vdc_irq(p, ST_DV);
}

uint8_t vdc_read(pce_t *p, int a)
{
    vdc_t *v = &p->vdc;
    switch (a & 3) {
    case 0: {
        uint8_t s = v->status & 0x7F;
        v->status &= ~0x3F;
        p->irq_pend &= ~2; pce_update_irq(p);
        return s;
    }
    case 2: return v->rdbuf & 0xFF;
    case 3: {
        uint8_t r = v->rdbuf >> 8;
        if (v->sel == R_VWR) {
            v->reg[R_MARR] += incs[(v->reg[R_CR] >> 11) & 3];
            v->rdbuf = v->vram[v->reg[R_MARR] & 0x7FFF];
        }
        return r;
    }
    }
    return 0;
}

static void mwr_changed(pce_t *p, uint16_t old);
PCE_HOT void vdc_write(pce_t *p, int a, uint8_t val)
{
    vdc_t *v = &p->vdc;
    uint16_t oldv = v->reg[v->sel];
    switch (a & 3) {
    case 0: v->sel = val & 0x1F; return;
    case 1: return;
    case 2:
        if (v->sel == R_VWR) { v->wlatch = (v->wlatch & 0xFF00) | val; return; }
        v->reg[v->sel] = (v->reg[v->sel] & 0xFF00) | val;
        break;
    case 3:
        if (v->sel == R_VWR) {
            uint16_t d = (v->wlatch & 0xFF) | (val << 8);
            vram_write(p, v->reg[R_MAWR], d);
            v->reg[R_MAWR] += incs[(v->reg[R_CR] >> 11) & 3];
            return;
        }
        v->reg[v->sel] = (v->reg[v->sel] & 0x00FF) | (val << 8);
        break;
    }
#ifdef SCROLL_LOG
    if ((v->sel == R_BXR || v->sel == R_BYR || v->sel == R_RCR) && p->frame >= SCROLL_LOG && p->frame < SCROLL_LOG + 3) {
        int now = p->in_cpu ? pce_now(p) : p->line * CYCLES_PER_LINE;
        fprintf(stderr, "SCR f%lu reg%d=%03x line %d +%d (a%d)\n", (unsigned long)p->frame, v->sel, v->reg[v->sel], now / CYCLES_PER_LINE, now % CYCLES_PER_LINE, a & 3);
    }
#endif
    switch (v->sel) {
    case R_CR: case R_RCR: pce_reschedule(p); break;
    case R_MARR: if ((a & 3) == 3) v->rdbuf = v->vram[v->reg[R_MARR] & 0x7FFF]; break;
    case R_BYR: v->yoff = v->reg[R_BYR] & 0x1FF; break;
    case R_MWR: if (p->deferred && v->reg[R_MWR] != oldv) mwr_changed(p, oldv); break;
    case R_LENR: if ((a & 3) == 3) vram_dma(p); break;
    case R_SATB: v->satb_pending = 1; break;
    default: break;
    }
}

/* a block transfer into the data port with VWR selected: one VRAM word */
PCE_HOT void vdc_vram_word(pce_t *p, uint8_t lo, uint8_t hi)
{
    vdc_t *v = &p->vdc;
    v->wlatch = (v->wlatch & 0xFF00) | lo;
    vram_write(p, v->reg[R_MAWR], lo | hi << 8);
    v->reg[R_MAWR] += incs[(v->reg[R_CR] >> 11) & 3];
}

uint8_t vce_read(pce_t *p, int a)
{
    vce_t *c = &p->vce;
    switch (a & 7) {
    case 4: return c->pal[c->addr] & 0xFF;
    case 5: { uint8_t r = (c->pal[c->addr] >> 8) | 0xFE; c->addr = (c->addr + 1) & 0x1FF; return r; }
    default: return 0xFF;
    }
}

/* The backdrop colour ($000; $100 outside the display) is drawn per line.
 * A line takes it when its state is latched, but the colour is only used once
 * its pixels go out, a little later: a write early in the line, after the
 * latch, still changes most of that line, so it is credited to it. */
#define BD_LATE 190
static void backdrop_write(pce_t *p)
{
    vce_t *c = &p->vce;
    if (!p->in_cpu) return;
    int L = p->line, y = L - SCREEN_FIRST;
    if (y < 0 || y >= SCREEN_LINES) return;
    int pos = pce_now(p) - L * CYCLES_PER_LINE;
    if (pos < p->render_at || pos >= p->render_at + BD_LATE) return;
    int disp = L >= p->vdc.disp_start && L < p->vdc.disp_end;
    if ((c->addr == 0) == disp) p->line_bd[y] = c->pal[c->addr];
}

void vce_write(pce_t *p, int a, uint8_t v)
{
    vce_t *c = &p->vce;
    switch (a & 7) {
    case 0: c->cr = v; break;
    case 2: c->addr = (c->addr & 0x100) | v; break;
    case 3: c->addr = (c->addr & 0xFF) | ((v & 1) << 8); break;
    case 4: c->pal[c->addr] = (c->pal[c->addr] & 0x100) | v; c->dirty = 1;
            if (p->colortab) p->pal_native[c->addr] = p->colortab[c->pal[c->addr]];
            if (!(c->addr & 0xFF)) backdrop_write(p);
            break;
    case 5: c->pal[c->addr] = (c->pal[c->addr] & 0xFF) | ((v & 1) << 8); c->dirty = 1;
            if (p->colortab) p->pal_native[c->addr] = p->colortab[c->pal[c->addr]];
            if (!(c->addr & 0xFF)) backdrop_write(p);
            c->addr = (c->addr + 1) & 0x1FF; break;
    }
}

/* ---------------- frame / line timing ---------------- */

PCE_WARM void vdc_frame_start(pce_t *p)
{
    vdc_t *v = &p->vdc;
    if (p->deferred) { p->rl->nband = 0; p->rl->nspr = p->rl->nspr_behind = 0; p->rl->used_plane = 0; }
    int vsw = v->reg[R_VPR] & 0x1F;
    int vds = v->reg[R_VPR] >> 8;
    int vdw = (v->reg[R_VDW] & 0x1FF) + 1;
    v->disp_start = vsw + vds + 3;
    v->disp_end = v->disp_start + vdw;
    if (v->disp_end > LINES_PER_FRAME - 1) v->disp_end = LINES_PER_FRAME - 1;
    v->raster0 = (v->raster + 1) & 0x3FF;   /* (the counter runs on from the last frame) */
    if (v->ds_line >= LINES_PER_FRAME) v->ds_line -= LINES_PER_FRAME;   /* a SATB DMA still running */
    else v->ds_line = -1;
}

static void rl_finalize(pce_t *p);
/* end of the display: the frame is complete; vertical blank starts */
PCE_WARM static COLD void display_end(pce_t *p)
{
    vdc_t *v = &p->vdc;
    if (v->b_n) band_flush(p);
    if (p->deferred) rl_finalize(p);
    memcpy(p->vce.snap, p->vce.pal, sizeof(p->vce.snap));
    /* vertical blank: the vblank interrupt; the SATB DMA runs now and
       raises its own interrupt a few lines later, when the 256 words are
       through (games tell the two apart by the status bits they see) */
    if (v->satb_pending || (v->reg[R_DCR] & 0x10)) {
        uint16_t s = v->reg[R_SATB];
        for (int i = 0; i < 256; i++) v->satb[i] = v->vram[(uint16_t)(s + i) & 0x7FFF];
        v->satb_pending = 0;
        v->ds_line = v->disp_end + SATB_DMA_LINES;     /* (may fall in the next frame) */
    }
}

PCE_HOT1 void vdc_line_start(pce_t *p, int line)
{
    vdc_t *v = &p->vdc;
    /* the raster counter is set to $40 RCR_LEAD lines before the display:
       its interrupt comes that much ahead of the line it names (MAME) */
    if (line == v->disp_start - RCR_LEAD) v->raster = 0x40;
    else v->raster = (v->raster + 1) & 0x3FF;
    if (line == v->disp_start) v->yoff = v->reg[R_BYR] & 0x1FF;
    else v->yoff++;
    /* on every line: games set RCR past the display to get an interrupt in
       the vertical blank. On the first line of the vertical blank the raster
       interrupt comes first and the vblank one a little later (vdc_vd_late):
       Air Zonk carries its chain of splits through the vblank that way, and
       Cadash's handlers keep the vblank status from being overwritten */
    if (v->raster == (v->reg[R_RCR] & 0x3FF))
        vdc_irq(p, ST_RR);
    if (line == v->disp_end) {
        TP_BEGIN(2); display_end(p); TP_END(2, 2);
        v->rr_late = line * CYCLES_PER_LINE + VD_AT;      /* the vblank interrupt, a little into the line */
    }
    if (line == v->ds_line) { v->ds_line = -1; vdc_irq(p, ST_DS); }
}

/* the vblank interrupt, a little into the first vblank line */
void vdc_rr_late(pce_t *p)
{
    p->vdc.rr_late = -1;
    vdc_irq(p, ST_VD);
}

/* ---------------- rendering ---------------- */

static uint64_t expand8[256];   /* bit i (MSB first) -> byte i = 1 */
static uint64_t expand8r[256];  /* same, mirrored (for x-flipped sprites) */
static uint64_t palrep[16];     /* palette number << 4 in every byte */
static int tabs_ready;

static COLD void tabs_init(void)
{
    for (int b = 0; b < 256; b++) {
        uint8_t t[8], u[8];
        for (int i = 0; i < 8; i++) { t[i] = (b >> (7 - i)) & 1; u[i] = (b >> i) & 1; }
        memcpy(&expand8[b], t, 8);
        memcpy(&expand8r[b], u, 8);
    }
    for (int i = 0; i < 16; i++) palrep[i] = 0x0101010101010101ull * (uint64_t)(i << 4);
    tabs_ready = 1;
}

static inline uint64_t ld8(const uint8_t *p) { return ((const struct __attribute__((packed, may_alias)) { uint64_t v; } *)p)->v; }
typedef struct __attribute__((packed, may_alias)) { uint64_t v; } u64u;
static inline void st8(uint8_t *p, uint64_t v) { ((u64u *)p)->v = v; }
/* 0xFF in every byte that is nonzero in its low nibble */
static inline uint64_t nzmask(uint64_t v)
{
    uint64_t n = (v | (v >> 1) | (v >> 2) | (v >> 3)) & 0x0101010101010101ull;
    return (n << 8) - n;
}
/* 8 pixels from four bitplane bytes */
static inline uint64_t planes(const uint64_t *E, unsigned b0, unsigned b1, unsigned b2, unsigned b3)
{
    return E[b0] | (E[b1] << 1) | (E[b2] << 2) | (E[b3] << 3);
}

static const int mapw_tab[4] = { 32, 64, 128, 128 };

static inline int spr_height(int attr) { int c = (attr >> 12) & 3; return c == 0 ? 16 : (c == 1 ? 32 : 64); }

/* per-frame sprite lists: the SATB copy only changes in vblank */
PCE_WARM static COLD void build_sprite_lists(pce_t *p)
{
    vdc_t *v = &p->vdc;
    int lines = v->disp_end - v->disp_start;
    if (lines > LINES_PER_FRAME) lines = LINES_PER_FRAME;
    if (lines < 0) lines = 0;
    memset(v->sl_n, 0, lines);
    memset(v->sl_cells, 0, lines);
    memset(v->sl_ovf, 0, lines);
    v->sl_frame = p->frame;
    if (p->deferred) { memset(v->cell_used, 0, sizeof v->cell_used); v->spr_first = v->spr_last = -1; v->nsr = 0; v->sl_built = 0; }
    int nolimit = p->flags & GF_NO_SPRLIMIT;
    if (p->deferred && lines > 0) {
        /* usually no line has more than 16 cells of sprites: then nothing is
           dropped, every sprite is one run, and the per-line counts are
           prefix sums (same result as the line-by-line loop below) */
        static int16_t dc[LINES_PER_FRAME + 1], dn[LINES_PER_FRAME + 1];
        memset(dc, 0, (lines + 1) * sizeof dc[0]);
        memset(dn, 0, (lines + 1) * sizeof dn[0]);
        for (int i = 0; i < 64; i++) {
            const uint16_t *s = &v->satb[i * 4];
            int sy = (s[0] & 0x3FF) - 64, h = spr_height(s[3]), sx = (s[1] & 0x3FF) - 32, w = (s[3] & 0x100) ? 2 : 1;
            int y0 = sy < 0 ? 0 : sy, y1 = sy + h > lines ? lines : sy + h;
            if (y0 >= y1) continue;
            dc[y0] += w; dc[y1] -= w;
            if (!(sx >= MAX_WIDTH || sx + w * 16 <= 0)) { dn[y0]++; dn[y1]--; }
        }
        int c = 0, k = 0, over = 0;
        for (int y = 0; y < lines; y++) {
            c += dc[y]; k += dn[y];
            if (c > 16) { over = 1; break; }
            v->sl_cells[y] = c; v->sl_n[y] = k;
        }
        if (!over) {
            for (int i = 0; i < 64; i++) {
                const uint16_t *s = &v->satb[i * 4];
                int sy = (s[0] & 0x3FF) - 64, h = spr_height(s[3]), sx = (s[1] & 0x3FF) - 32, w = (s[3] & 0x100) ? 2 : 1;
                int y0 = sy < 0 ? 0 : sy, y1 = sy + h > lines ? lines : sy + h;
                if (y0 >= y1 || sx >= MAX_WIDTH || sx + w * 16 <= 0) continue;
                if (v->nsr < 512) { int r = v->nsr++; v->sr[r].d0 = y0; v->sr[r].n = y1 - y0; v->sr[r].i = i; }
                int pat = (s[2] >> 1) & 0x3FF;
                if (w == 2) pat &= ~1;
                if (h == 32) pat &= ~2; else if (h == 64) pat &= ~6;
                int yf = s[3] & 0x8000;
                for (int cy = 0; cy < h / 16; cy++) {
                    int rb = yf ? h / 16 - 1 - cy : cy;
                    int a = sy + rb * 16, b = a + 15;
                    if (a < y0) a = y0;
                    if (b > y1 - 1) b = y1 - 1;
                    if (a > b) continue;
                    for (int cx = 0; cx < w; cx++) {
                        unsigned cc = (pat + cy * 2 + cx) & 0x1FF;
                        if (!v->cell_used[cc]) { v->cell_used[cc] = 1; v->cell_line[cc] = a; v->cell_last[cc] = b; }
                        else { if (a < v->cell_line[cc]) v->cell_line[cc] = a; if (b > v->cell_last[cc]) v->cell_last[cc] = b; }
                    }
                }
            }
            return;
        }
        memset(v->sl_n, 0, lines);
        memset(v->sl_cells, 0, lines);
    }
    for (int i = 0; i < 64; i++) {
        const uint16_t *s = &v->satb[i * 4];
        int sy = (s[0] & 0x3FF) - 64;
        int h = spr_height(s[3]);
        int sx = (s[1] & 0x3FF) - 32;
        int w = (s[3] & 0x100) ? 2 : 1;
        int y0 = sy < 0 ? 0 : sy, y1 = sy + h > lines ? lines : sy + h;
        int offscreen = sx >= MAX_WIDTH || sx + w * 16 <= 0;
        int pat = (s[2] >> 1) & 0x3FF;
        if (w == 2) pat &= ~1;
        if (h == 32) pat &= ~2; else if (h == 64) pat &= ~6;
        int yf = s[3] & 0x8000;
        int shown = 0;
        if (p->deferred) {
            /* runs of lines instead of per-line lists (built later if the CPU needs them) */
            int open = -1;
            for (int y = y0; y < y1; y++) {
                if (v->sl_cells[y] + w > 16) { v->sl_ovf[y] = 1; if (!nolimit) { open = -1; continue; } }
                v->sl_cells[y] += w;
                if (!offscreen && v->sl_n[y] < 64) {
                    v->sl_n[y]++; shown = 1;
                    if (open >= 0) v->sr[open].n++;
                    else if (v->nsr < 512) { open = v->nsr++; v->sr[open].d0 = y; v->sr[open].n = 1; v->sr[open].i = i; }
                } else open = -1;
            }
        } else
        for (int y = y0; y < y1; y++) {
            if (v->sl_cells[y] + w > 16) { v->sl_ovf[y] = 1; if (!nolimit) continue; }
            v->sl_cells[y] += w;
            if (!offscreen && v->sl_n[y] < 64) { v->sl_idx[y][v->sl_n[y]++] = i; shown = 1; }
        }
        if (shown && p->deferred) {
            /* lines that may show each cell, for the hazard checks (whole
               16-line blocks: a superset when lines were dropped) */
            for (int cy = 0; cy < h / 16; cy++) {
                int rb = yf ? h / 16 - 1 - cy : cy;          /* block of sprite lines showing row cy */
                int a = sy + rb * 16, b = a + 15;
                if (a < y0) a = y0;
                if (b > y1 - 1) b = y1 - 1;
                if (a > b) continue;
                for (int cx = 0; cx < w; cx++) {
                    unsigned c = (pat + cy * 2 + cx) & 0x1FF;
                    if (!v->cell_used[c]) { v->cell_used[c] = 1; v->cell_line[c] = a; v->cell_last[c] = b; }
                    else { if (a < v->cell_line[c]) v->cell_line[c] = a; if (b > v->cell_last[c]) v->cell_last[c] = b; }
                }
            }
        }
    }
}

void vdc_line_hook(pce_t *p, int line)
{
    if (line == p->vdc.disp_start) build_sprite_lists(p);
}

static uint8_t claim[MAX_WIDTH + 64];
static uint8_t claim0[MAX_WIDTH + 64];
static int spr_irqs = 1;    /* render_sprites raises the sprite interrupts */
#ifdef RL_STATS
long rls_mask, rls_snap, rls_coll, rls_bgh, rls_sprh, rls_bands, rls_layer, rls_spr, rls_cells, rls_plcells, rls_full, rls_frames, rls_sprl;
#define RLS(x) (x)
#else
#define RLS(x) ((void)0)
#endif

PCE_WARM static void decode_bg(vdc_t *v, unsigned t)
{
    const uint16_t *src = &v->vram[t * 16];
    uint64_t *dst = (uint64_t *)&v->bgcache[t * 64];
    for (int r = 0; r < 8; r++) {
        unsigned w0 = src[r], w1 = src[r + 8];
        dst[r] = planes(expand8, w0 & 0xFF, w0 >> 8, w1 & 0xFF, w1 >> 8);
    }
    v->bgdirty[t] = 0;
}

static void render_sprites(pce_t *p, int y, uint8_t *bg, uint8_t *spr, uint16_t *ext, int width, uint16_t cr);

/* CPU background: screen lines y0 .. y0+n-1 into the bg layer, drawn a tile
 * row (up to 8 lines) at a time */
PCE_WARM static void cpu_bg_rows(pce_t *p, int y0, int n, int width, unsigned bxr, unsigned yoff0, uint16_t mwr, uint16_t cr)
{
    vdc_t *v = &p->vdc;
    int stride = p->layer_stride;
    uint8_t *bg0 = p->layer_bg + y0 * stride;
    EMUX_BEGIN(PS_BG);
    if (cr & 0x80) {
        int mapw = mapw_tab[(mwr >> 4) & 3];
        unsigned hmask = ((mwr & 0x40) ? 64 : 32) * 8 - 1;
        unsigned mmask = mapw - 1;
        int bx = bxr;
        int ncols = (width + (bx & 7) + 7) >> 3;
        int ly = 0;
        while (ly < n) {
            unsigned by = (yoff0 + ly) & hmask;
            int r0 = by & 7;
            int k = 8 - r0;
            if (k > n - ly) k = n - ly;
            const uint16_t *bat = &v->vram[(by >> 3) * mapw];
            uint8_t *d = bg0 + ly * stride - (bx & 7);
            if (p->row_prep)
                for (int i = 0; i < k; i++) p->row_prep(bg0 + (ly + i) * stride - 16, stride);
            unsigned tx = (bx >> 3) & mmask;
            for (int c = 0; c < ncols; c++, d += 8, tx = (tx + 1) & mmask) {
                unsigned e = bat[tx];
                unsigned t = e & 0x7FF;
                if (v->bgdirty[t]) decode_bg(v, t);
                const uint64_t *src = (const uint64_t *)&v->bgcache[t * 64] + r0;
                uint64_t prep = palrep[e >> 12];
                uint8_t *dd = d;
                for (int j = 0; j < k; j++, dd += stride) st8(dd, src[j] | prep);
            }
            ly += k;
        }
    } else {
        for (int i = 0; i < n; i++) {
            if (p->row_prep) p->row_prep(bg0 + i * stride - 16, stride);
            memset(bg0 + i * stride, 0, width);
        }
    }
    EMUX_END(PS_BG);
}

/* deferred mode keeps sprite runs; the CPU renderer wants per-line lists */
PCE_WARM static void build_line_lists(vdc_t *v)
{
    int lines = v->disp_end - v->disp_start;
    if (lines > LINES_PER_FRAME) lines = LINES_PER_FRAME;
    if (lines > 0) memset(v->sl_n, 0, lines);
    for (int k = 0; k < v->nsr; k++)
        for (int d = v->sr[k].d0; d < v->sr[k].d0 + v->sr[k].n; d++) v->sl_idx[d][v->sl_n[d]++] = v->sr[k].i;
    v->sl_built = 1;
}

/* CPU sprites for one screen line into the sprite layer (bg row already drawn) */
PCE_WARM static void cpu_spr_line(pce_t *p, int sy, int width, uint16_t cr)
{
    vdc_t *v = &p->vdc;
    if (p->deferred && !v->sl_built) build_line_lists(v);
    int stride = p->layer_stride;
    uint16_t *ext = p->spr_ext + 2 * sy;
    uint8_t *spr = p->layer_spr + sy * stride;
    if (ext[1] > ext[0]) memset(spr + ext[0], 0, ext[1] - ext[0]);
    ext[0] = ext[1] = 0;
    if (cr & 0x40)
        render_sprites(p, sy + SCREEN_FIRST - v->disp_start, p->layer_bg + sy * stride, spr, ext, width, cr);
}

/* line-by-line renderer: draw the open band now */
static void band_flush_cpu(pce_t *p)
{
    vdc_t *v = &p->vdc;
    int n = v->b_n;
    v->b_n = 0;
    cpu_bg_rows(p, v->b_y0, n, v->b_w, v->b_bxr, v->b_yoff0, v->b_mwr, v->b_cr);
    EMUX_BEGIN(PS_SPR);
    if (v->b_cr & 0x40) {
        int stride = p->layer_stride;
        for (int i = 0; i < n; i++) {
            int sy = v->b_y0 + i;
            render_sprites(p, sy + SCREEN_FIRST - v->disp_start, p->layer_bg + sy * stride,
                           p->layer_spr + sy * stride, p->spr_ext + 2 * sy, v->b_w, v->b_cr);
        }
    }
    EMUX_END(PS_SPR);
}

/* ---------------- deferred rendering ---------------- */

static void bat_recount(vdc_t *v)
{
    unsigned mwr = v->reg[R_MWR];
    v->bat_words = (unsigned)mapw_tab[(mwr >> 4) & 3] * ((mwr & 0x40) ? 64 : 32);
    memset(v->tref, 0, sizeof v->tref);
    for (unsigned a = 0; a < v->bat_words; a++) v->tref[v->vram[a] & 0x7FF]++;
}

static void cpu_bg_band(pce_t *p, rl_band_t *b)
{
    cpu_bg_rows(p, b->y0, b->n, b->w, b->bx, b->by, b->mwr, b->cr);
}

/* plane areas shown by lines that wait for the GPU: tiles in a bitmap, BAT
   cells by the bands' geometry */
static void vis_reset(vdc_t *v) { memset(v->tvis, 0, sizeof v->tvis); v->vis_nb = 0; v->vis_open_n = 0; v->vis_open_y0 = -1; }
static void vis_mark_row(vdc_t *v, unsigned row, unsigned bxr, int w, uint16_t mwr)
{
    unsigned mapw = mapw_tab[(mwr >> 4) & 3];
    unsigned c0 = (bxr >> 3), c1 = (bxr + w - 1) >> 3;
    const uint16_t *bat = &v->vram[row * mapw];
    for (unsigned cx = c0; cx <= c1; cx++) {
        unsigned t = bat[cx & (mapw - 1)] & 0x7FF;
        v->tvis[t >> 5] |= 1u << (t & 31);
    }
}
/* mark the tiles of band lines n0 .. n-1 */
static void vis_mark_lines(vdc_t *v, unsigned bx, unsigned by, int n0, int n, int w, uint16_t mwr)
{
    unsigned maph = (mwr & 0x40) ? 64 : 32;
    for (unsigned r = (by + n0) >> 3; r <= (by + n - 1) >> 3; r++) vis_mark_row(v, r & (maph - 1), bx, w, mwr);
}
/* bring tvis up to date with the lines latched since the last look */
static void vis_update(pce_t *p)
{
    vdc_t *v = &p->vdc;
    rlist_t *rl = p->rl;
    for (; v->vis_nb < rl->nband; v->vis_nb++) {
        rl_band_t *b = &rl->band[v->vis_nb];
        if ((b->flags & (RB_BG | RB_LAYER)) == RB_BG) vis_mark_lines(v, b->bx, b->by, 0, b->n, b->w, b->mwr);
    }
    if (v->b_n && (v->b_cr & 0x80)) {
        int n0 = (v->vis_open_y0 == v->b_y0) ? v->vis_open_n : 0;
        if (n0 < v->b_n) vis_mark_lines(v, v->b_bxr, v->b_yoff0, n0, v->b_n, v->b_w, v->b_mwr);
        v->vis_open_y0 = v->b_y0; v->vis_open_n = v->b_n;
    }
}
static int cell_in_band(unsigned c, unsigned bx, unsigned by, int n, int w, uint16_t mwr)
{
    unsigned mapw = mapw_tab[(mwr >> 4) & 3], maph = (mwr & 0x40) ? 64 : 32;
    unsigned cx = c & (mapw - 1), cy = (c / mapw) & (maph - 1);
    unsigned r0 = (by >> 3) & (maph - 1), nr = ((by & 7) + n + 7) >> 3;
    if (nr < maph && ((cy - r0) & (maph - 1)) >= nr) return 0;
    unsigned c0 = (bx >> 3) & (mapw - 1), nc = ((bx & 7) + w + 7) >> 3;
    return nc >= mapw || ((cx - c0) & (mapw - 1)) < nc;
}
static int bat_visible(pce_t *p, unsigned c)
{
    vdc_t *v = &p->vdc;
    rlist_t *rl = p->rl;
    for (int i = 0; i < rl->nband; i++) {
        rl_band_t *b = &rl->band[i];
        if ((b->flags & (RB_BG | RB_LAYER)) == RB_BG && cell_in_band(c, b->bx, b->by, b->n, b->w, b->mwr)) return 1;
    }
    return v->b_n && (v->b_cr & 0x80) && cell_in_band(c, v->b_bxr, v->b_yoff0, v->b_n, v->b_w, v->b_mwr);
}

/* does a band show tile t? */
static int band_has_tile(vdc_t *v, const rl_band_t *b, unsigned t)
{
    unsigned mapw = mapw_tab[(b->mwr >> 4) & 3], maph = (b->mwr & 0x40) ? 64 : 32;
    unsigned c0 = b->bx >> 3, c1 = (b->bx + b->w - 1) >> 3;
    for (unsigned r = (unsigned)b->by >> 3; r <= ((unsigned)b->by + b->n - 1) >> 3; r++) {
        const uint16_t *bat = &v->vram[(r & (maph - 1)) * mapw];
        for (unsigned cx = c0; cx <= c1; cx++)
            if ((bat[cx & (mapw - 1)] & 0x7FF) == t) return 1;
    }
    return 0;
}

/* The word at VRAM address a, which a waiting band shows (as a BAT cell or
 * as tile data), is about to change: the bands that show it are drawn now,
 * from the data they saw. The others keep waiting for the GPU. */
static COLD void bg_hazard_at(pce_t *p, unsigned a)
{
    vdc_t *v = &p->vdc;
    rlist_t *rl = p->rl;
    RLS(rls_bgh++);
    if (v->b_n) band_flush(p);
    int bat = a < v->bat_words, left = 0;
    unsigned t = (a >> 4) & 0x7FF;
    int tile = v->tref[t] != 0;         /* (a BAT word can also be part of a tile in use) */
    for (int i = 0; i < rl->nband; i++) {
        rl_band_t *b = &rl->band[i];
        if ((b->flags & (RB_BG | RB_LAYER)) != RB_BG) continue;
        if ((bat && cell_in_band(a, b->bx, b->by, b->n, b->w, b->mwr)) || (tile && band_has_tile(v, b, t))) {
            cpu_bg_band(p, b); b->flags |= RB_LAYER;
        } else left = 1;
    }
    v->bg_pend = left;
    vis_reset(v);           /* rebuilt from the bands still waiting */
}

/* VRAM data that the plane holds changed while lines that draw from the
 * plane are waiting: draw those lines now, from the data they saw */
static COLD void bg_hazard(pce_t *p)
{
    vdc_t *v = &p->vdc;
    rlist_t *rl = p->rl;
    RLS(rls_bgh++);
    if (v->b_n) band_flush(p);
    for (int i = 0; i < rl->nband; i++) {
        rl_band_t *b = &rl->band[i];
        if ((b->flags & (RB_BG | RB_LAYER)) == RB_BG) { cpu_bg_band(p, b); b->flags |= RB_LAYER; }
    }
    v->bg_pend = 0;
    vis_reset(v);
}

/* Cells snapshotted this frame (their sprite-cache image frozen with the
 * pattern the waiting lines saw) also keep that pattern here: if a hazard
 * later has the CPU draw those lines, it must draw them from the old data,
 * not from what has since been written to VRAM. */

static void snap_swap(vdc_t *v)
{
    for (int i = 0; i < v->snap_n; i++) {
        uint16_t *a = &v->vram[v->snap_cell[i] << 6], *b = v->snap_data[i];
        for (int k = 0; k < 64; k++) { uint16_t t = a[k]; a[k] = b[k]; b[k] = t; }
    }
}

/* split band i after its first k lines: the rest becomes a new band at the end */
static void band_split(rlist_t *rl, int i, int k)
{
    rl_band_t *b = &rl->band[i], *c = &rl->band[rl->nband++];
    *c = *b;
    c->y0 = b->y0 + k; c->n = b->n - k; c->by = (b->by + k) & 0x1FF;
    b->n = k;
}

/* Sprite pattern cell c, which waiting lines show, is about to change, and
 * lines not shown yet show it too: the waiting lines that show it get their
 * sprites drawn by the CPU now (each in a band of its own, so that only their
 * background is drawn by the CPU as well). Other waiting lines stay with the
 * GPU. */
static COLD void spr_hazard_at(pce_t *p, unsigned c)
{
    vdc_t *v = &p->vdc;
    rlist_t *rl = p->rl;
    RLS(rls_sprh++);
    if (v->b_n) band_flush(p);
    int off = v->disp_start - SCREEN_FIRST;             /* screen row of display line 0 */
    int d0 = v->cell_line[c] > v->spr_first ? v->cell_line[c] : v->spr_first;
    int d1 = v->cell_last[c] < v->spr_last ? v->cell_last[c] : v->spr_last;
    int y0 = d0 + off, y1 = d1 + off + 1;
    if (y0 < 0) y0 = 0;
    if (y1 > SCREEN_LINES) y1 = SCREEN_LINES;
    if (y0 >= y1) return;
    int any = 0;
    for (int i = 0; i < rl->nband; i++) {
        rl_band_t *b = &rl->band[i];
        if (!(b->cr & 0x40)) continue;
        int a = b->y0, e = b->y0 + b->n;
        if (e <= y0 || a >= y1) continue;
        if (a < y0) { band_split(rl, i, y0 - a); continue; }        /* the rest: looked at later */
        if (e > y1) band_split(rl, i, y1 - a);
        b = &rl->band[i];
        if (!(b->flags & RB_LAYER)) { cpu_bg_band(p, b); b->flags |= RB_LAYER; }
        any = 1;
    }
    if (!any) return;
    snap_swap(v); spr_irqs = 0;
    for (int i = 0; i < rl->nband; i++) {
        rl_band_t *b = &rl->band[i];
        if (!(b->cr & 0x40) || !(b->flags & RB_LAYER) || b->y0 + b->n <= y0 || b->y0 >= y1) continue;
        for (int y = b->y0; y < b->y0 + b->n; y++)
            if ((rl->line[y] & (RL_SPRL | RL_DISP)) == RL_DISP) { cpu_spr_line(p, y, b->w, b->cr); rl->line[y] |= RL_SPRL; }
    }
    spr_irqs = 1; snap_swap(v);
}


static void sprcache_cell(pce_t *p, unsigned c);
static COLD void spr_snapshot(pce_t *p, unsigned c)
{
    vdc_t *v = &p->vdc;
    RLS(rls_snap++);
    TP_BEGIN(31);
    if (p->gpu_wait) p->gpu_wait(p);
    TP_END(31, 4);
    if (v->sc_dirty[c]) sprcache_cell(p, c);
    v->sc_frozen[c] = 1;
    v->snap_cell[v->snap_n] = c;
    memcpy(v->snap_data[v->snap_n], &v->vram[c << 6], sizeof v->snap_data[0]);
    v->snap_n++;
}

static inline void pl_mark_cell(vdc_t *v, unsigned c)
{
    if (v->pl_cdirty[c]) return;
    v->pl_cdirty[c] = 1;
    if (v->pl_dn < 1024) v->pl_dlist[v->pl_dn] = c;
    v->pl_dn++;
}

static PCE_HOT void vram_track(pce_t *p, unsigned addr, unsigned old, unsigned data)
{
    vdc_t *v = &p->vdc;
    unsigned t = addr >> 4;
    int bat = addr < v->bat_words;
    if (v->bg_pend) {
        int hit = bat && bat_visible(p, addr);
        if (!hit && v->tref[t & 0x7FF]) { vis_update(p); hit = (v->tvis[(t & 0x7FF) >> 5] >> (t & 31)) & 1; }
        if (hit) bg_hazard_at(p, addr);
    }
    unsigned c = addr >> 6;
    if (v->spr_pend && v->cell_used[c] && !v->sc_frozen[c] &&
        (int)v->cell_line[c] <= v->spr_last && (int)v->cell_last[c] >= v->spr_first) {
        /* a waiting line shows this cell: if no later line does, cache the old
           pattern now; otherwise the waiting lines' sprites are drawn now */
        if ((int)v->cell_last[c] <= v->spr_last && v->snap_n < SNAP_MAX) spr_snapshot(p, c);
        else spr_hazard_at(p, c);
    }
    if (bat) {
        v->tref[old & 0x7FF]--; v->tref[data & 0x7FF]++;
        pl_mark_cell(v, addr);
    }
    if (v->tref[t]) { v->pl_tdirty[t] = 1; v->pl_tdirty_any = 1; }
    v->sc_dirty[addr >> 6] = 1;
}

/* nw words of a block transfer into the data port (VWR selected) from s:
 * vdc_vram_word for each, with the common tracking done here */
PCE_HOT void vdc_vram_words(pce_t *p, const uint8_t *s, unsigned nw)
{
    vdc_t *v = &p->vdc;
    if (!nw) return;
#ifdef PCE_VERIFY
    for (unsigned k = 0; k < nw; k++, s += 2) vdc_vram_word(p, s[0], s[1]);
#else
    unsigned inc = incs[(v->reg[R_CR] >> 11) & 3];
    uint16_t a = v->reg[R_MAWR];
    for (unsigned k = 0; k < nw; k++, s += 2, a += inc) {
        if (a & 0x8000) continue;
        uint16_t d = s[0] | s[1] << 8, old = v->vram[a];
        if (old == d) continue;
        if (p->deferred) {
            if ((v->bg_pend && (a < v->bat_words || v->tref[a >> 4])) ||
                (v->spr_pend && v->cell_used[a >> 6] && !v->sc_frozen[a >> 6])) vram_track(p, a, old, d);
            else {
                if (a < v->bat_words) { v->tref[old & 0x7FF]--; v->tref[d & 0x7FF]++; pl_mark_cell(v, a); }
                if (v->tref[a >> 4]) { v->pl_tdirty[a >> 4] = 1; v->pl_tdirty_any = 1; }
                v->sc_dirty[a >> 6] = 1;
            }
        } else if (v->b_n) band_flush(p);
        v->vram[a] = d;
        v->bgdirty[a >> 4] = 1;
    }
    v->reg[R_MAWR] = a;
    v->wlatch = (v->wlatch & 0xFF00) | s[-2];
#endif
}

static void mwr_changed(pce_t *p, uint16_t old)
{
    vdc_t *v = &p->vdc;
    (void)old;
    /* bands waiting for the plane were latched with the old map size
       (each band keeps its own copy of MWR for the CPU path) */
    if (v->bg_pend) bg_hazard(p);
    bat_recount(v);         /* the plane is rebuilt if the final MWR differs from its own */
    if (v->bat_words > v->pl_cmax) v->pl_cmax = v->bat_words;
}

/* the plane is read by the GPU: written data must reach memory (N64: the
   cache line is written back; "create" claims a line about to be fully
   written without reading it first) */
#if defined(__mips__)
#define LINE_WB(a)     __asm__ volatile ("cache 0x19, 0(%0)" :: "r"(a) : "memory")
#define LINE_CREATE(a) __asm__ volatile ("cache 0x0D, 0(%0)" :: "r"(a) : "memory")
#else
#define LINE_WB(a)     ((void)(a))
#define LINE_CREATE(a) ((void)(a))
#endif

PCE_WARM static void plane_cell(pce_t *p, unsigned c, unsigned mapw, unsigned W)
{
    vdc_t *v = &p->vdc;
    unsigned cx = c & (mapw - 1), cy = c / mapw;
    unsigned e = v->vram[c], t = e & 0x7FF;
    if (v->bgdirty[t]) decode_bg(v, t);
    const uint64_t *tc = (const uint64_t *)&v->bgcache[t * 64];
    uint64_t prep = palrep[e >> 12];
    int unc = p->plane_w != NULL;
    uint8_t *d = (unc ? p->plane_w : p->plane) + (cy * 8) * W + cx * 8;
    int wb = p->cache_wb != NULL && !unc;
    RLS(rls_plcells++);
    for (int r = 0; r < 8; r++, d += W) {
        *(uint64_t *)d = tc[r] | prep;          /* uncached: no line to read in or write back */
        if (wb) LINE_WB(d);
    }
}

/* many cells: row by row (each plane row written left to right, a tile's
   pattern stays in the cache for its 8 rows); need = cells to draw, NULL = all */
PCE_WARM static void plane_rows(pce_t *p, unsigned mapw, unsigned maph, unsigned W, const uint8_t *need)
{
    vdc_t *v = &p->vdc;
    int wb = p->cache_wb != NULL;
    for (unsigned cy = 0; cy < maph; cy++) {
        const uint16_t *bat = &v->vram[cy * mapw];
        const uint8_t *nd = need ? need + cy * mapw : NULL;
        if (nd) {
            /* mostly changed: the whole row (claiming its cache lines is
               cheaper than reading them for the cells that stay) */
            unsigned k = 0;
            for (unsigned cx = 0; cx < mapw; cx++) k += nd[cx];
            if (!k) continue;
            if (k * 2 > mapw) nd = NULL;
        }
        /* the cells' tiles, decoded (8-bit pixels, no palette yet) */
        for (unsigned cx = 0; cx < mapw; cx++)
            if (!nd || nd[cx]) { unsigned t = bat[cx] & 0x7FF; if (v->bgdirty[t]) decode_bg(v, t); }
        const uint8_t *bgc = v->bgcache;
        uint8_t *row = p->plane + (cy * 8) * W;
#ifndef OLD_ROWS
        if (!nd) {
            /* two cells (one 16-byte cache line of each pixel row) at a
               time: each tile's pattern is read once, and each line is
               claimed, filled and written back in one go */
            for (unsigned cx = 0; cx < mapw; cx += 2) {
                unsigned e0 = bat[cx], e1 = bat[cx + 1];
                const uint64_t *t0 = (const uint64_t *)(bgc + (e0 & 0x7FF) * 64), *t1 = (const uint64_t *)(bgc + (e1 & 0x7FF) * 64);
                uint64_t p0 = palrep[e0 >> 12], p1 = palrep[e1 >> 12];
                uint8_t *d = row + cx * 8;
                for (int r = 0; r < 8; r++, d += W) {
                    if (wb) LINE_CREATE(d);
                    ((uint64_t *)d)[0] = t0[r] | p0;
                    ((uint64_t *)d)[1] = t1[r] | p1;
                    if (wb) LINE_WB(d);
                }
            }
        } else {
            /* scattered cells: through the uncached alias when there is one */
            uint8_t *urow = p->plane_w ? row - p->plane + p->plane_w : row;
            int wbs = wb && !p->plane_w;
            for (unsigned cx = 0; cx < mapw; cx++) {
                if (!nd[cx]) continue;
                unsigned e = bat[cx];
                const uint64_t *t = (const uint64_t *)(bgc + (e & 0x7FF) * 64);
                uint64_t pr = palrep[e >> 12];
                uint8_t *d = urow + cx * 8;
                for (int r = 0; r < 8; r++, d += W) {
                    *(uint64_t *)d = t[r] | pr;
                    if (wbs) LINE_WB(d);
                }
            }
        }
#else
        for (int r = 0; r < 8; r++, row += W) {
            if (!nd) {
                if (wb) for (unsigned x = 0; x < W; x += 16) LINE_CREATE(row + x);
                for (unsigned cx = 0; cx < mapw; cx++) {
                    unsigned e = bat[cx];
                    *(uint64_t *)(row + cx * 8) = ((const uint64_t *)(bgc + (e & 0x7FF) * 64))[r] | palrep[e >> 12];
                }
                if (wb) for (unsigned x = 0; x < W; x += 16) LINE_WB(row + x);
            } else {
                uint8_t *urow = p->plane_w ? row - p->plane + p->plane_w : row;
                int wbs = wb && !p->plane_w;
                for (unsigned cx = 0; cx < mapw; cx++) {
                    if (!nd[cx]) continue;
                    unsigned e = bat[cx];
                    *(uint64_t *)(urow + cx * 8) = ((const uint64_t *)(bgc + (e & 0x7FF) * 64))[r] | palrep[e >> 12];
                    if (wbs) LINE_WB(urow + cx * 8);
                }
            }
        }
#endif
#ifdef RL_STATS
        for (unsigned cx = 0; cx < mapw; cx++) if (!nd || nd[cx]) rls_plcells++;
#endif
    }
}

PCE_WARM static void plane_update(pce_t *p)
{
    vdc_t *v = &p->vdc;
    rlist_t *rl = p->rl;
    uint16_t mwr = v->reg[R_MWR];
    unsigned mapw = mapw_tab[(mwr >> 4) & 3], maph = (mwr & 0x40) ? 64 : 32;
    unsigned W = mapw * 8, n = mapw * maph;
    rl->plane_w = W; rl->plane_h = maph * 8;
    if (mwr != v->pl_mwr) v->pl_full = 1;
    if (v->pl_full) {
        RLS(rls_full++);
        plane_rows(p, mapw, maph, W, NULL);
        memset(v->pl_cdirty, 0, sizeof v->pl_cdirty);   /* (the map size changed: all of it) */
        v->pl_full = 0; v->pl_mwr = mwr;
    } else if (v->pl_tdirty_any || v->pl_dn > 1024) {
        unsigned cnt = 0;
        for (unsigned c = 0; c < n; c++)
            if (v->pl_cdirty[c] || v->pl_tdirty[v->vram[c] & 0x7FF]) { v->pl_cdirty[c] = 1; cnt++; }
        if (cnt > 48) plane_rows(p, mapw, maph, W, v->pl_cdirty);
        else for (unsigned c = 0; c < n; c++) if (v->pl_cdirty[c]) plane_cell(p, c, mapw, W);
        memset(v->pl_cdirty, 0, n > v->pl_cmax ? n : v->pl_cmax);   /* (no cell past these is marked) */
    } else if (v->pl_dn > 48) {
        plane_rows(p, mapw, maph, W, v->pl_cdirty);
        for (int i = 0; i < v->pl_dn; i++) v->pl_cdirty[v->pl_dlist[i]] = 0;
    } else {
        for (int i = 0; i < v->pl_dn; i++) {
            unsigned c = v->pl_dlist[i];
            if (c < n) plane_cell(p, c, mapw, W);
            v->pl_cdirty[c] = 0;
        }
    }
    if (v->pl_tdirty_any) memset(v->pl_tdirty, 0, sizeof v->pl_tdirty);
    v->pl_tdirty_any = 0; v->pl_dn = 0; v->pl_cmax = 0;
}

/* 8 one-byte pixels (in memory order) -> 4 bytes, first pixel in the high nibble */
static inline void put4(uint8_t *d, uint64_t x)
{
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    x = (x | (x >> 4)) & 0x00FF00FF00FF00FFull;
    x = (x | (x >> 8)) & 0x0000FFFF0000FFFFull;
    x = (x | (x >> 16)) & 0x00000000FFFFFFFFull;
    *(uint32_t *)d = (uint32_t)x;
#else
    uint8_t t[8]; memcpy(t, &x, 8);
    for (int j = 0; j < 4; j++) d[j] = t[2 * j] << 4 | t[2 * j + 1];
#endif
}

PCE_WARM static void sprcache_cell(pce_t *p, unsigned c)
{
    vdc_t *v = &p->vdc;
    const uint16_t *sp = &v->vram[c << 6];
    unsigned pair = c >> 1;
    int unc = p->sprcache_w != NULL;
    uint8_t *base = unc ? p->sprcache_w : p->sprcache;
    uint8_t *d = base + pair * 256 + (c & 1) * 8;
    int wrap = pair < SPRCACHE_PAIRS - 256;
    for (int r = 0; r < 16; r++, d += 16) {
        unsigned p0 = sp[r], p1 = sp[r + 16], p2 = sp[r + 32], p3 = sp[r + 48];
        uint8_t row[8];
        put4(row, planes(expand8, p0 >> 8, p1 >> 8, p2 >> 8, p3 >> 8));
        put4(row + 4, planes(expand8, p0 & 0xFF, p1 & 0xFF, p2 & 0xFF, p3 & 0xFF));
        uint64_t x; memcpy(&x, row, 8);
        *(uint64_t *)d = x;                         /* one store a row (uncached on the N64) */
        if (wrap) *(uint64_t *)(d + 256 * 256) = x;
    }
    if (p->cache_wb && !unc) {
        p->cache_wb(p->sprcache + pair * 256, 256);
        if (wrap) p->cache_wb(p->sprcache + (pair + 256) * 256, 256);
    }
    v->sc_dirty[c] = 0;
    RLS(rls_cells++);
}

/* sprite strips: runs of consecutive lines on which a sprite is shown */
PCE_WARM static void sprite_runs(pce_t *p)
{
    vdc_t *v = &p->vdc;
    rlist_t *rl = p->rl;
    static struct { int16_t y, n; uint8_t i; } tmp[RL_MAX_SPR];
    int16_t oy[64], on[64];
    uint8_t onl[SCREEN_LINES];
    int nt = 0;
    memset(on, 0, sizeof on);
    memset(onl, 0, sizeof onl);
    for (int i = 0; i < rl->nband; i++) {
        rl_band_t *b = &rl->band[i];
        if (b->cr & 0x40) memset(onl + b->y0, 1, b->n);
    }
    /* the stretches of lines whose sprites the GPU draws (usually one) */
    static int16_t sg0[SCREEN_LINES], sg1[SCREEN_LINES];
    int nsg = 0;
    for (int y = 0; y < SCREEN_LINES; ) {
        while (y < SCREEN_LINES && (!onl[y] || (rl->line[y] & RL_SPRL))) y++;
        int a = y;
        while (y < SCREEN_LINES && onl[y] && !(rl->line[y] & RL_SPRL)) y++;
        if (y > a) { sg0[nsg] = a; sg1[nsg] = y; nsg++; }
    }
    /* split each sprite's runs where its lines are not drawn by the GPU */
    for (int r = 0; r < v->nsr; r++) {
        int i = v->sr[r].i;
        int y = v->sr[r].d0 + v->disp_start - SCREEN_FIRST, ye = y + v->sr[r].n;
        for (int g = 0; g < nsg; g++) {
            int a = y > sg0[g] ? y : sg0[g], b = ye < sg1[g] ? ye : sg1[g];
            if (b > a && nt < RL_MAX_SPR) { tmp[nt].y = a; tmp[nt].n = b - a; tmp[nt].i = i; nt++; }
        }
    }
    (void)oy; (void)on;

    /* strips per sprite, in creation order */
    int16_t head[64], next[RL_MAX_SPR], tail[64];
    for (int i = 0; i < 64; i++) head[i] = -1;
    for (int k = 0; k < nt; k++) {
        int i = tmp[k].i;
        next[k] = -1;
        if (head[i] < 0) head[i] = k; else next[tail[i]] = k;
        tail[i] = k;
    }
    /* behind-background strips, then in-front ones; each back (index 63) to front */
    int no = 0;
    for (int front = 0; front < 2; front++) {
        if (front) rl->nspr_behind = no;
        for (int i = 63; i >= 0; i--) {
            if (head[i] < 0) continue;
            const uint16_t *s = &v->satb[i * 4];
            int attr = s[3];
            if (!(attr & 0x80) != !front) continue;
            for (int k = head[i]; k >= 0; k = next[k]) {
                rl_spr_t *e = &rl->spr[no++];
                int sy = (s[0] & 0x3FF) - 64;
                int h = spr_height(attr);
                int wcells = (attr & 0x100) ? 2 : 1;
                int pat = (s[2] >> 1) & 0x3FF;
                if (wcells == 2) pat &= ~1;
                if (h == 32) pat &= ~2; else if (h == 64) pat &= ~6;
                pat &= 0x1FF;
                e->x = (s[1] & 0x3FF) - 32;
                e->y = tmp[k].y; e->n = tmp[k].n;
                e->ry = tmp[k].y + SCREEN_FIRST - v->disp_start - sy;
                e->h = h;
                e->pair = pat >> 1;
                e->pal = attr & 0xF;
                e->flags = ((attr & 0x800) ? RS_XF : 0) | ((attr & 0x8000) ? RS_YF : 0) |
                           ((attr & 0x80) ? RS_FRONT : 0) | (wcells == 2 ? RS_W32 : ((pat & 1) ? RS_RIGHT : 0));
                for (int cy = 0; cy < h / 16; cy++)
                    for (int cx = 0; cx < wcells; cx++) {
                        unsigned c = (pat + cy * 2 + cx) & 0x1FF;
                        if (v->sc_dirty[c] && !v->sc_frozen[c]) sprcache_cell(p, c);
                    }
            }
        }
    }
    rl->nspr = no;
}


/* A behind-background sprite with a higher priority (lower SATB index) than an
 * in-front one hides it where they overlap: the VDC picks the first opaque
 * sprite pixel, then that sprite's priority decides against the background.
 * Games use this to mask sprites (text that is "not there yet", objects
 * passing behind scenery). The GPU draws behind and in-front sprites as two
 * layers, so lines where such a pair overlaps are drawn by the CPU. */
PCE_WARM static void sprite_masks(pce_t *p)
{
    vdc_t *v = &p->vdc;
    rlist_t *rl = p->rl;
    int lines = v->disp_end - v->disp_start;
    if (lines > LINES_PER_FRAME) lines = LINES_PER_FRAME;
    if (lines <= 0) return;
    static uint8_t m[LINES_PER_FRAME];
    int lo = lines, hi = 0;
    /* the in-front sprites on screen, in SATB order */
    int16_t fi[64], fy0[64], fy1[64], fx0[64], fx1[64];
    int nf = 0;
    for (int j = 0; j < 64; j++) {
        const uint16_t *b = &v->satb[j * 4];
        if (!(b[3] & 0x80)) continue;
        int by = (b[0] & 0x3FF) - 64, bh = spr_height(b[3]), bx = (b[1] & 0x3FF) - 32, bw = (b[3] & 0x100) ? 32 : 16;
        if (by + bh <= 0 || by >= lines || bx + bw <= 0 || bx >= MAX_WIDTH) continue;
        fi[nf] = j; fy0[nf] = by; fy1[nf] = by + bh; fx0[nf] = bx; fx1[nf] = bx + bw; nf++;
    }
    if (!nf) return;
    for (int i = 0, k0 = 0; i < 63; i++) {
        while (k0 < nf && fi[k0] <= i) k0++;
        if (k0 == nf) break;                           /* no in-front sprite after this one */
        const uint16_t *a = &v->satb[i * 4];
        if (a[3] & 0x80) continue;                     /* in front: no problem */
        int ay = (a[0] & 0x3FF) - 64, ah = spr_height(a[3]), ax = (a[1] & 0x3FF) - 32, aw = (a[3] & 0x100) ? 32 : 16;
        if (ay + ah <= 0 || ay >= lines || ax + aw <= 0 || ax >= MAX_WIDTH) continue;
        for (int k = k0; k < nf; k++) {
            if (fx0[k] >= ax + aw || ax >= fx1[k]) continue;
            int y0 = ay > fy0[k] ? ay : fy0[k], y1 = ay + ah < fy1[k] ? ay + ah : fy1[k];
            if (y0 < 0) y0 = 0;
            if (y1 > lines) y1 = lines;
            if (y0 >= y1) continue;
            if (lo > hi) memset(m, 0, lines);
            memset(m + y0, 1, y1 - y0);
            if (y0 < lo) lo = y0;
            if (y1 > hi) hi = y1;
        }
    }
    if (lo >= hi) return;
    int off = v->disp_start - SCREEN_FIRST;            /* screen row of display line 0 */
#define MASKED(y) ({ int d_ = (y) - off; d_ >= lo && d_ < hi && m[d_] && (rl->line[y] & (RL_SPRL | RL_DISP)) == RL_DISP; })
    /* the masked lines' background is drawn by the CPU: in bands of their
       own (split off the bands they were in), so the rest stays on the GPU */
    int any = 0;
    for (int i = 0; i < rl->nband; i++) {
        rl_band_t *b = &rl->band[i];
        if (!(b->cr & 0x40)) continue;
        int k = 0;
        while (k < b->n && !MASKED(b->y0 + k)) k++;
        if (k == b->n) continue;
        if (k > 0 || !(b->flags & RB_LAYER)) {
            if (k > 0) { band_split(rl, i, k); continue; }   /* the rest: a band of its own, looked at later */
            int r = 1;
            while (r < b->n && MASKED(b->y0 + r)) r++;
            if (r < b->n) band_split(rl, i, r);
            b = &rl->band[i];
            cpu_bg_band(p, b); b->flags |= RB_LAYER;
        }
        any = 1;
    }
    if (!any) return;
    snap_swap(v); spr_irqs = 0;
    for (int i = 0; i < rl->nband; i++) {
        rl_band_t *b = &rl->band[i];
        if (!(b->cr & 0x40) || !(b->flags & RB_LAYER)) continue;
        for (int y = b->y0; y < b->y0 + b->n; y++) {
            if (!MASKED(y)) continue;
            cpu_spr_line(p, y, b->w, b->cr);
            rl->line[y] |= RL_SPRL;
            RLS(rls_mask++);
        }
    }
#undef MASKED
    spr_irqs = 1;
    snap_swap(v);
}

/* display ended: everything this frame shows is known; bring the plane and the
 * pattern cache up to date for the GPU */
PCE_WARM static COLD void rl_finalize(pce_t *p)
{
    vdc_t *v = &p->vdc;
    rlist_t *rl = p->rl;
    if (v->b_n) band_flush(p);
    EMUX_BEGIN(PS_VRAMW);
    TP_BEGIN(32);
    if (p->gpu_wait) p->gpu_wait(p);
    TP_END(32, 0);
    EMUX_END(PS_VRAMW);
    EMUX_BEGIN(PS_IDLE);
    sprite_masks(p);
    int need = 0;
    for (int i = 0; i < rl->nband; i++)
        if ((rl->band[i].flags & (RB_BG | RB_LAYER)) == RB_BG) need = 1;
    rl->used_plane = need;
#ifdef PCE_SPIKES
    extern uint64_t (*spk_ticks)(void); extern uint64_t spk_plane, spk_sprr;
    uint64_t t0 = spk_ticks ? spk_ticks() : 0;
#endif
    TP_BEGIN(6);
    if (need) plane_update(p);
    TP_END(6, 6);
#ifdef PCE_SPIKES
    uint64_t t1 = spk_ticks ? spk_ticks() : 0;
#endif
    sprite_runs(p);
#ifdef PCE_SPIKES
    if (spk_ticks) { spk_plane += t1 - t0; spk_sprr += spk_ticks() - t1; }
#endif
    vis_reset(v);
    v->spr_first = v->spr_last = -1;
    memset(v->sc_frozen, 0, sizeof v->sc_frozen);
    v->snap_n = 0;
    EMUX_END(PS_IDLE);
#ifdef RL_STATS
    rls_frames++; rls_bands += rl->nband; rls_spr += rl->nspr;
    for (int i = 0; i < rl->nband; i++) if (rl->band[i].flags & RB_LAYER) rls_layer++;
    for (int y = 0; y < SCREEN_LINES; y++) if (rl->line[y] & RL_SPRL) rls_sprl++;
#endif
    v->bg_pend = v->spr_pend = 0;
}

/* deferred: record the open band */
PCE_WARM static __attribute__((noinline)) void band_record(pce_t *p)
{
    vdc_t *v = &p->vdc;
    rlist_t *rl = p->rl;
    int n = v->b_n;
    v->b_n = 0;
    rl_band_t *b = &rl->band[rl->nband++];
    b->y0 = v->b_y0; b->n = n; b->bx = v->b_bxr; b->by = v->b_yoff0; b->w = v->b_w; b->cr = v->b_cr;
    b->mwr = v->b_mwr;
    b->flags = (v->b_cr & 0x80) ? RB_BG : 0;
    if ((v->b_cr & 0x41) == 0x41) {
        /* sprite collision interrupt: the CPU has to look at the pixels */
        cpu_bg_band(p, b); b->flags |= RB_LAYER;
        RLS(rls_coll += n);
        for (int y = b->y0; y < b->y0 + n; y++) { cpu_spr_line(p, y, b->w, b->cr); rl->line[y] |= RL_SPRL; }
    } else if ((v->b_cr & 0x42) == 0x42) {
        for (int y = b->y0; y < b->y0 + n; y++) {
            int d = y + SCREEN_FIRST - v->disp_start;
            if (d >= 0 && d < LINES_PER_FRAME && v->sl_ovf[d]) vdc_irq(p, ST_OR);
        }
    }
}

static void band_flush(pce_t *p)
{
    if (!p->vdc.b_n) return;
    if (p->deferred) band_record(p);
    else band_flush_cpu(p);
}

/* line-by-line renderer: clear the line's sprite row; outside the display
   the line is overscan colour (returns 1) */
static __attribute__((noinline)) int cpu_line_prep(pce_t *p, int y, int w, int disp)
{
    vdc_t *v = &p->vdc;
    int stride = p->layer_stride;
    uint16_t *ext = p->spr_ext + 2 * y;
    uint8_t *spr = p->layer_spr + y * stride;
    if (ext[1] > ext[0]) memset(spr + ext[0], 0, ext[1] - ext[0]);
    ext[0] = ext[1] = 0;
    if (disp) return 0;
    if (v->b_n) band_flush(p);
    uint8_t *bg = p->layer_bg + y * stride;
    if (p->row_prep) p->row_prep(bg - 16, stride);
    memset(bg, 0, w);
    memset(spr, 0x10, w);           /* sprite-layer 0x10 = overscan colour (VCE $100) */
    ext[0] = 0; ext[1] = w;
    return 1;
}

/* Called at the point in each scanline where the VDC latches its state. */
PCE_HOT1 void vdc_line(pce_t *p, int line)
{
    vdc_t *v = &p->vdc;
    int y = line - SCREEN_FIRST;
    if (y < 0 || y >= SCREEN_LINES || !p->layer_bg) return;
    if (!tabs_ready) tabs_init();
    int w = ((v->reg[R_HDR] & 0x7F) + 1) * 8;
    if (w > MAX_WIDTH) w = MAX_WIDTH;
    p->line_width[y] = w;
    int disp = line >= v->disp_start && line < v->disp_end;
    p->line_bd[y] = p->vce.pal[disp ? 0 : 0x100];      /* games change it mid-frame for gradients */
    if (p->deferred) {
        p->rl->line[y] = disp ? RL_DISP : 0;
        if (!disp) { if (v->b_n) band_flush(p); return; }
    } else if (cpu_line_prep(p, y, w, disp)) return;
    uint16_t cr = v->reg[R_CR];
    uint16_t bxr = v->reg[R_BXR] & 0x3FF, mwr = v->reg[R_MWR];
    unsigned yoff = v->yoff & 0x1FF;
    if (v->b_n && v->b_bxr == bxr && v->b_mwr == mwr && v->b_cr == cr && v->b_w == w &&
        ((v->b_yoff0 + v->b_n) & 0x1FF) == yoff) {
        v->b_n++;
    } else {
        if (v->b_n) band_flush(p);
        v->b_y0 = y; v->b_n = 1; v->b_yoff0 = yoff;
        v->b_bxr = bxr; v->b_mwr = mwr; v->b_cr = cr; v->b_w = w;
    }
    if (p->deferred) {
        if (cr & 0x80) v->bg_pend = 1;
        if (cr & 0x40) {
            v->spr_pend = 1; v->spr_last = line - v->disp_start;
            if (v->spr_first < 0) v->spr_first = v->spr_last;
        }
    }
    /* sprite collision / overflow interrupts must fire on their own line */
    if ((cr & 0x41) == 0x41 || y == SCREEN_LINES - 1 || ((cr & 0x42) == 0x42 && vdc_ovf_line(p, line))) band_flush(p);
}

/* Lines L .. L+k-1, none of them special (display start or end, a raster
 * interrupt line), all display or all outside it: their line starts and latches
 * in one go (deferred mode, no per-line sprite interrupts). Same result as
 * vdc_line_start + vdc_line for each line. */
PCE_WARM __attribute__((noinline)) void vdc_lines(pce_t *p, int L, int k)
{
    vdc_t *v = &p->vdc;
    unsigned yoff1 = v->yoff + 1;          /* what line L's latch sees */
    v->raster = (v->raster + k) & 0x3FF; v->yoff += k;
    int y0 = L - SCREEN_FIRST, y1 = y0 + k;
    if (y0 < 0) { yoff1 += -y0; y0 = 0; }
    if (y1 > SCREEN_LINES) y1 = SCREEN_LINES;
    if (y0 >= y1 || !p->layer_bg) return;
    if (!tabs_ready) tabs_init();
    int n = y1 - y0;
    int w = ((v->reg[R_HDR] & 0x7F) + 1) * 8;
    if (w > MAX_WIDTH) w = MAX_WIDTH;
    rlist_t *rl = p->rl;
    int disp = L >= v->disp_start && L < v->disp_end;
    uint16_t bd = p->vce.pal[disp ? 0 : 0x100];
    for (int y = y0; y < y1; y++) { p->line_width[y] = w; p->line_bd[y] = bd; }
    memset(rl->line + y0, disp ? RL_DISP : 0, n);
    if (!disp) { if (v->b_n) band_flush(p); return; }
    uint16_t cr = v->reg[R_CR];
    uint16_t bxr = v->reg[R_BXR] & 0x3FF, mwr = v->reg[R_MWR];
    unsigned yoff = yoff1 & 0x1FF;
    int cont = v->b_n && v->b_bxr == bxr && v->b_mwr == mwr && v->b_cr == cr && v->b_w == w &&
               ((v->b_yoff0 + v->b_n) & 0x1FF) == yoff;
    if (cont) v->b_n += n;
    else {
        if (v->b_n) band_flush(p);
        v->b_y0 = y0; v->b_n = n; v->b_yoff0 = yoff;
        v->b_bxr = bxr; v->b_mwr = mwr; v->b_cr = cr; v->b_w = w;
    }
    if (cr & 0x80) v->bg_pend = 1;
    if (cr & 0x40) {
        v->spr_pend = 1;
        if (v->spr_first < 0) v->spr_first = y0 + SCREEN_FIRST - v->disp_start;
        v->spr_last = y1 - 1 + SCREEN_FIRST - v->disp_start;
    }
    if (y1 == SCREEN_LINES) band_flush(p);
}

/* does display line L have more sprites than the VDC can show (with the
   overflow interrupt on, such a line is an event of its own)? The sprite
   lists are built at the start of the display; before that, none. */
int vdc_ovf_line(pce_t *p, int L)
{
    vdc_t *v = &p->vdc;
    int d = L - v->disp_start;
    return d >= 0 && L < v->disp_end && d < LINES_PER_FRAME && v->sl_ovf[d] && v->sl_frame == p->frame;
}
/* first such line at or after L (or -1) */
int vdc_next_ovf(pce_t *p, int L)
{
    vdc_t *v = &p->vdc;
    /* not built yet for this frame: the display start, where they are, is
       the next point to look */
    if (v->sl_frame != p->frame) return L <= v->disp_start && v->disp_start < v->disp_end ? v->disp_start : -1;
    int d = L - v->disp_start, e = v->disp_end - v->disp_start;
    if (d < 0) d = 0;
    if (e > LINES_PER_FRAME) e = LINES_PER_FRAME;
    if (d >= e) return -1;
    const uint8_t *f = memchr(v->sl_ovf + d, 1, e - d);
    return f ? (int)(f - v->sl_ovf) + v->disp_start : -1;
}

/* first line after L that has to be processed on its own */
int vdc_special_after(pce_t *p, int L)
{
    vdc_t *v = &p->vdc;
    int lim = LINES_PER_FRAME;
    if (v->disp_start > L && v->disp_start < lim) lim = v->disp_start;
    if (v->disp_start - RCR_LEAD > L && v->disp_start - RCR_LEAD < lim) lim = v->disp_start - RCR_LEAD;
    if (v->disp_end > L && v->disp_end < lim) lim = v->disp_end;
    if (v->reg[R_CR] & 0x04) {
        int rl = vdc_rcr_line(p, L + 1);
        if (rl > L && rl < lim) lim = rl;
    }
    if (v->ds_line > L && v->ds_line < lim) lim = v->ds_line;
    if ((v->reg[R_CR] & 0x42) == 0x42) {
        int o = vdc_next_ovf(p, L + 1);
        if (o > L && o < lim) lim = o;
    }
    return lim;
}

int vdc_line_plain(pce_t *p, int L)
{
    vdc_t *v = &p->vdc;
    if (!p->deferred || (v->reg[R_CR] & 0x41) == 0x41) return 0;
    if ((v->reg[R_CR] & 0x42) == 0x42 && vdc_ovf_line(p, L)) return 0;
    if (L == v->disp_start || L == v->disp_end || L == v->disp_start - RCR_LEAD) return 0;
    if ((v->reg[R_CR] & 0x04) && vdc_rcr_line(p, L) == L) return 0;
    if (L == v->ds_line) return 0;
    return 1;
}

PCE_WARM void vdc_frame_end(pce_t *p)
{
    if (p->vdc.b_n) band_flush(p);
}

PCE_WARM static void render_sprites(pce_t *p, int y, uint8_t *bg, uint8_t *spr, uint16_t *ext, int width, uint16_t cr)
{
    vdc_t *v = &p->vdc;
    if (y < 0) return;
    if (spr_irqs && v->sl_ovf[y] && (cr & 0x02)) vdc_irq(p, ST_OR);
    int n = v->sl_n[y];
    if (!n) return;
    int minx = width, maxx = 0;
    int coll = (cr & 0x01) != 0 && spr_irqs;
    uint8_t *cl = claim + 32;
    for (int k = 0; k < n; k++) {
        int i = v->sl_idx[y][k];
        const uint16_t *s = &v->satb[i * 4];
        int sy = (s[0] & 0x3FF) - 64;
        int sx = (s[1] & 0x3FF) - 32;
        int attr = s[3];
        int pat = (s[2] >> 1) & 0x3FF;
        int wcells = (attr & 0x100) ? 2 : 1;
        int h = spr_height(attr);
        if (wcells == 2) pat &= ~1;
        if (h == 32) pat &= ~2; else if (h == 64) pat &= ~6;
        int ry = y - sy;
        if (attr & 0x8000) ry = h - 1 - ry;
        int cy = ry >> 4, r = ry & 15;
        int xflip = attr & 0x800;
        uint64_t prep = palrep[attr & 0xF];
        int front = attr & 0x80;
        const uint64_t *E = xflip ? expand8r : expand8;
        for (int cx = 0; cx < wcells; cx++) {
            int x0 = sx + cx * 16;
            if (x0 >= width || x0 + 16 <= 0) continue;
            if (x0 < -16) continue;
            int cell = (pat + cy * 2 + (xflip ? (wcells - 1 - cx) : cx)) & 0x1FF;
            const uint16_t *sp = &v->vram[(cell << 6) + r];
            unsigned p0 = sp[0], p1 = sp[16], p2 = sp[32], p3 = sp[48];
            /* left half is the high bytes, unless mirrored */
            uint64_t hi = planes(E, p0 >> 8, p1 >> 8, p2 >> 8, p3 >> 8);
            uint64_t lo = planes(E, p0 & 0xFF, p1 & 0xFF, p2 & 0xFF, p3 & 0xFF);
            for (int hh = 0; hh < 2; hh++) {
                int x = x0 + hh * 8;
                uint64_t px = (hh == 0) == !xflip ? hi : lo;
                uint64_t m = nzmask(px);
                if (!m) continue;
                uint64_t c = ld8(cl + x);
                if (coll) {
                    if (i == 0) st8(claim0 + 32 + x, m);
                    else if (m & ld8(claim0 + 32 + x)) vdc_irq(p, ST_CR);
                }
                uint64_t nm = m & ~c;
                st8(cl + x, c | m);
                if (!nm) continue;
                uint64_t vis = front ? nm : (nm & ~nzmask(ld8(bg + x) & 0x0F0F0F0F0F0F0F0Full));
                uint64_t o = ld8(spr + x);
                st8(spr + x, (o & ~vis) | ((px | prep) & vis));
            }
            if (x0 < minx) minx = x0;
            if (x0 + 16 > maxx) maxx = x0 + 16;
        }
    }
    if (minx < -16) minx = -16;
    if (maxx > width + 16) maxx = width + 16;
    if (maxx > minx) {
        memset(cl + minx, 0, maxx - minx);
        if (coll) memset(claim0 + 32 + minx, 0, maxx - minx);
        if (minx < 0) { memset(spr + minx, 0, -minx); minx = 0; }  /* slack */
        if (maxx > width) { memset(spr + width, 0, maxx - width); maxx = width; }
        if (maxx > minx) { ext[0] = minx; ext[1] = maxx; }
    }
}
