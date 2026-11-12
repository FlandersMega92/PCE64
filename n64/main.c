/* PCE64 N64 frontend (libdragon).
 *
 * The core composes each scanline into two 8-bit layers (background and
 * sprites). The RDP turns them into pixels with two 256-entry palettes and
 * draws them into the framebuffer, so the CPU never touches 16-bit pixels.
 * Layers are double-buffered so the RDP can draw frame N while the CPU
 * emulates frame N+1. */
#include <libdragon.h>
#include <malloc.h>
#include "pce.h"

#define AUDIO_RATE 32000
#define BLOB_SCAN_START 0x110000
#define BLOB_SCAN_STEP 0x10000
#define ROM_MAX 0x4000000
#define NSETS 2

static pce_t P __attribute__((aligned(8192)));   /* a fixed place in the 8 KB data cache, whatever the code size */
static uint16_t colortab[512];
static int fbw = 0, img_w = 0, img_x = 0;
static int16_t *abuf;
#ifdef PCE_STATS
/* vertical blanks seen: 60 per 60 frames means every frame was shown on time */
static volatile unsigned vi_count;
static void vi_tick(void) { vi_count++; }
#endif
#ifndef SPIKE_US
#define SPIKE_US 18000
#endif
#if defined(PCE_SPIKES) || defined(PCE_TPROF)
uint64_t (*spk_ticks)(void); uint64_t spk_plane, spk_sprr;
#endif
#ifdef PCE_TPROF
uint64_t tprof[8];
#endif
#ifdef PCE_SPIKES
#endif
#ifdef PSG_LOGHASH
uint32_t psg_log_hash = 2166136261u;
#endif

typedef struct {
    uint8_t *bg, *spr;          /* pixel (0,0) of each layer */
    uint8_t *bg_mem, *spr_mem;
    int stride;
    uint16_t *ext;
    uint16_t *tlut_bg, *tlut_spr;
    rspq_block_t *block;
    int block_w;
    rspq_syncpoint_t sync;
    int busy;
    uint16_t tl_pal[512];       /* palette the TLUTs were built from */
    int tl_ok;
} layerset_t;
static layerset_t sets[NSETS];

/* ---------- video ---------- */

static void make_colortab(void)
{
    for (int c = 0; c < 512; c++) {
        int g = (c >> 6) & 7, r = (c >> 3) & 7, b = c & 7;
        int r5 = (r << 2) | (r >> 1), g5 = (g << 2) | (g >> 1), b5 = (b << 2) | (b >> 1);
        colortab[c] = (r5 << 11) | (g5 << 6) | (b5 << 1) | 1;
    }
}

/* framebuffer width for a PCE line width: the PCE active picture is about 47.6 us
   wide in every dot-clock mode, the N64 picture about 52 us, so pad by ~9%. */
static int fb_width_for(int w)
{
    int f = (w * 109 + 50) / 100;
    return (f + 7) & ~7;
}

static void sets_init(void)
{
    for (int i = 0; i < NSETS; i++) {
        layerset_t *s = &sets[i];
        size_t sz = (size_t)(MAX_WIDTH + 32) * (SCREEN_LINES + 1);
        s->bg_mem = memalign(64, sz);
        s->spr_mem = memalign(64, sz);
        memset(s->bg_mem, 0, sz);
        memset(s->spr_mem, 0, sz);
        data_cache_hit_writeback_invalidate(s->bg_mem, sz);
        data_cache_hit_writeback_invalidate(s->spr_mem, sz);
        s->ext = calloc(2 * SCREEN_LINES, sizeof(uint16_t));
        s->tlut_bg = memalign(16, 256 * 2);
        s->tlut_spr = memalign(16, 256 * 2);
        s->block = NULL;
        s->busy = 0;
        s->bg = s->bg_mem + 16;
        s->spr = s->spr_mem + 16;
        s->stride = 256 + 32;
    }
}

/* layer rows use a stride of w+32 (pixel 0 at offset 16), so the RDP blit
   and the cache writeback both cover one contiguous region */
static void sets_set_width(int w)
{
    for (int i = 0; i < NSETS; i++) {
        layerset_t *s = &sets[i];
        if (s->busy) { rspq_syncpoint_wait(s->sync); s->busy = 0; }
        s->stride = (w + 32 + 15) & ~15;
        size_t sz = (size_t)(MAX_WIDTH + 32) * (SCREEN_LINES + 1);
        memset(s->spr_mem, 0, sz);
        memset(s->bg_mem, 0, sz);
        memset(s->ext, 0, 2 * SCREEN_LINES * sizeof(uint16_t));
        if (s->block) { rspq_block_free(s->block); s->block = NULL; }
    }
}

/* libdragon's 240p presets open a 237-line video window (V_VIDEO 37..511
   half-lines on NTSC), which drops the last three rows of the frame. The PC
   Engine's standard frame is 240 lines, so widen the window to 480 half-lines
   around the same centre: the whole frame is shown, at the position a real
   console puts it, and a CRT's overscan trims it the way it trims a PC Engine. */
static void vi_full_height(void)
{
    volatile uint32_t *v_video = (volatile uint32_t *)0xA4400028;
    /* libdragon's presets: NTSC and MPAL 0x025..0x1FF, PAL 0x05F..0x239 */
    int c = get_tv_type() == TV_PAL ? (0x05F + 0x239) / 2 : (0x025 + 0x1FF) / 2;
    int s = c - SCREEN_LINES, e = c + SCREEN_LINES;
    *v_video = ((uint32_t)s << 16) | (uint32_t)e;
}

static void set_video(int w)
{
    if (fbw) { rspq_wait(); display_close(); }
    fbw = fb_width_for(w);
    img_w = w;
    img_x = ((fbw - w) / 2) & ~3;
    resolution_t res = { .width = fbw, .height = SCREEN_LINES, .interlaced = false };
    display_init(res, DEPTH_16_BPP, 3, GAMMA_NONE, FILTERS_RESAMPLE);
    vi_full_height();
    for (int i = 0; i < 3; i++) {
        surface_t *s = display_get();
        memset(s->buffer, 0, s->stride * s->height);
        display_show(s);
    }
    sets_set_width(w);
}

static void record_block(layerset_t *s)
{
    surface_t bgs = surface_make_linear(s->bg, FMT_CI8, img_w, SCREEN_LINES);
    bgs.stride = s->stride;
    surface_t sps = surface_make_linear(s->spr, FMT_CI8, img_w, SCREEN_LINES);
    sps.stride = s->stride;
    rspq_block_begin();
    rdpq_set_mode_copy(false);
    rdpq_mode_tlut(TLUT_RGBA16);
    rdpq_tex_upload_tlut(s->tlut_bg, 0, 256);
    rdpq_tex_blit(&bgs, img_x, 0, NULL);
    rdpq_set_mode_copy(true);
    rdpq_mode_tlut(TLUT_RGBA16);
    rdpq_tex_upload_tlut(s->tlut_spr, 0, 256);
    rdpq_tex_blit(&sps, img_x, 0, NULL);
    s->block = rspq_block_end();
    s->block_w = img_w;
}

static void build_tluts(layerset_t *s)
{
    const uint16_t *pal = P.vce.snap;
    uint16_t *tb = s->tlut_bg, *ts = s->tlut_spr;
    /* bg: entry 0 of every palette is the backdrop (transparent bg pixels keep
       their palette bits); sprites: 0 is transparent, 0x10 marks overscan */
    uint16_t back = colortab[pal[0] & 0x1FF];
    s->tl_ok = 0;
    for (int i = 0; i < 256; i++) tb[i] = (i & 15) ? colortab[pal[i] & 0x1FF] : back;
    for (int i = 0; i < 256; i++) ts[i] = colortab[pal[256 + i] & 0x1FF];
    ts[0] = 0;
    ts[0x10] = colortab[pal[0x100] & 0x1FF];
    data_cache_hit_writeback(tb, 512);
    data_cache_hit_writeback(ts, 512);
}

/* The bg row is about to be fully overwritten: claim its cache lines without
   reading them from RDRAM ("create dirty exclusive"). */
static void row_prep(uint8_t *row, int len)
{
    for (int i = 0; i < len; i += 16)
        asm volatile ("cache 0x0D, 0(%0)" :: "r"(row + i) : "memory");
}

/* ---------- deferred drawing: the RDP draws the frame the core described ---------- */

static rlist_t RL;
static rspq_syncpoint_t last_sync;
static int last_busy;

static void gpu_wait(pce_t *p)
{
    (void)p;
    if (last_busy) { rspq_syncpoint_wait(last_sync); last_busy = 0; }
}
static void cache_wb(const void *ptr, unsigned n) { data_cache_hit_writeback(ptr, n); }

PCE_WARM static void build_tluts_deferred(layerset_t *s)
{
    const uint16_t *pal = P.vce.snap;
    uint16_t *tb = s->tlut_bg, *ts = s->tlut_spr;
    if (s->tl_ok && !memcmp(s->tl_pal, pal, sizeof s->tl_pal)) return;
    memcpy(s->tl_pal, pal, sizeof s->tl_pal); s->tl_ok = 1;
    /* colour 0 of every palette is transparent: the backdrop is filled first */
    for (int i = 0; i < 256; i++) tb[i] = (i & 15) ? colortab[pal[i] & 0x1FF] : 0;
    for (int i = 0; i < 256; i++) ts[i] = (i & 15) ? colortab[pal[256 + i] & 0x1FF] : 0;
    data_cache_hit_writeback(tb, 512);
    data_cache_hit_writeback(ts, 512);
}

PCE_WARM static void sprite_mode(layerset_t *s)
{
    rdpq_set_mode_standard();
    rdpq_mode_combiner(RDPQ_COMBINER_TEX);
    rdpq_mode_alphacompare(1);
    rdpq_mode_tlut(TLUT_RGBA16);
    rdpq_tex_upload_tlut(s->tlut_spr, 0, 256);
}

/* a sprite strip: its rows of the 32-pixel-wide 4-bit pattern block are
   loaded as 8-bit data (16 bytes a row) and drawn through a CI4 tile */
PCE_WARM static void draw_strip(const rl_spr_t *e)
{
    int W = (e->flags & RS_W32) ? 32 : 16, so = (e->flags & RS_RIGHT) ? 16 : 0;
    int t0 = (e->flags & RS_YF) ? e->h - e->ry - e->n : e->ry;
    rdpq_set_texture_image_raw(0, PhysicalAddr(P.sprcache + e->pair * 256), FMT_CI8, 16, e->h);
    rdpq_set_tile(TILE7, FMT_CI8, 0, 16, NULL);
    rdpq_load_tile(TILE7, 0, t0, 16, t0 + e->n);
    rdpq_set_tile(TILE0, FMT_CI4, 0, 16, &(rdpq_tileparms_t){ .palette = e->pal });
    rdpq_set_tile_size(TILE0, 0, t0, 32, t0 + e->n);
    int x0 = img_x + e->x, x1 = x0 + W, y0 = e->y, y1 = y0 + e->n;
    /* reversed coordinates mirror the texture (rdpq: x0 > x1 covers x1+1 .. x0) */
    if (e->flags & RS_XF) { int t = x0; x0 = x1 - 1; x1 = t - 1; }
    if (e->flags & RS_YF) { int t = y0; y0 = y1 - 1; y1 = t - 1; }
    rdpq_texture_rectangle(TILE0, x0, y0, x1, y1, so, t0);
}

PCE_WARM static void blit_rows(uint8_t *layer, int stride, int y0, int n, int w)
{
    surface_t sub = surface_make_linear(layer + y0 * stride, FMT_CI8, w, n);
    sub.stride = stride;
    rdpq_tex_blit(&sub, img_x, y0, NULL);
}

PCE_WARM static void draw_frame(layerset_t *s, surface_t *fb)
{
    rlist_t *rl = &RL;
    build_tluts_deferred(s);
    /* the CPU drew some rows itself: make them visible to the RDP */
    for (int i = 0; i < rl->nband; i++)
        if ((rl->band[i].flags & (RB_BG | RB_LAYER)) == (RB_BG | RB_LAYER))
            data_cache_hit_writeback(s->bg + rl->band[i].y0 * s->stride - 16, rl->band[i].n * s->stride);
    for (int y = 0; y < SCREEN_LINES; y++)
        if (rl->line[y] & RL_SPRL) data_cache_hit_writeback(s->spr + y * s->stride - 16, s->stride);

    rdpq_attach(fb, NULL);
    rdpq_set_scissor(img_x, 0, img_x + img_w, SCREEN_LINES);
    /* backdrop: VCE colour 0 on display lines, $100 outside the display */
    /* (games change the backdrop colour between lines for gradients) */
    for (int y = 0; y < SCREEN_LINES; ) {
        uint16_t c = P.line_bd[y] & 0x1FF;
        int y1 = y + 1;
        while (y1 < SCREEN_LINES && (P.line_bd[y1] & 0x1FF) == c) y1++;
        rdpq_set_mode_fill(color_from_packed16(colortab[c]));
        rdpq_fill_rectangle(img_x, y, img_x + img_w, y1);
        y = y1;
    }
    if (rl->nspr_behind) {
        sprite_mode(s);
        for (int i = 0; i < rl->nspr_behind; i++) draw_strip(&rl->spr[i]);
    }
    rdpq_set_mode_copy(true);
    rdpq_mode_tlut(TLUT_RGBA16);
    rdpq_tex_upload_tlut(s->tlut_bg, 0, 256);
    int cur_pitch = -1, plane_set = 0;
    for (int i = 0; i < rl->nband; i++) {
        const rl_band_t *b = &rl->band[i];
        if (!(b->flags & RB_BG)) continue;
        int w = b->w < img_w ? b->w : img_w;
        if (b->flags & RB_LAYER) { blit_rows(s->bg, s->stride, b->y0, b->n, w); cur_pitch = -1; plane_set = 0; continue; }
        int W = rl->plane_w, H = rl->plane_h;
        if (!plane_set) { rdpq_set_texture_image_raw(0, PhysicalAddr(P.plane), FMT_CI8, W, H); plane_set = 1; }
        for (int r = 0; r < b->n; ) {
            int pr = (b->by + r) % H;
            int rows = b->n - r;
            if (pr + rows > H) rows = H - pr;
            /* up to two pieces across (the plane wraps), loaded in TMEM-sized slices */
            for (int x = 0, sx = b->bx % W; x < w; ) {
                int seg = W - sx < w - x ? W - sx : w - x;
                int sa = sx & ~7, bw = ((sx + seg + 7) & ~7) - sa;
                int rmax = 2048 / bw;
                if (bw != cur_pitch) {
                    rdpq_set_tile(TILE7, FMT_CI8, 0, bw, NULL);
                    rdpq_set_tile(TILE0, FMT_CI8, 0, bw, NULL);
                    cur_pitch = bw;
                }
                for (int k = 0; k < rows; ) {
                    int nr = rows - k < rmax ? rows - k : rmax;
                    /* the _fx form: a 1024-pixel-wide plane (128-tile map) ends at
                       s1 = 1024, which the hardware takes (exclusive end) but
                       rdpq_load_tile's assert rejects */
                    rdpq_load_tile_fx(TILE7, sa * 4, (pr + k) * 4, (sa + bw) * 4, (pr + k + nr) * 4);
                    rdpq_set_tile_size(TILE0, sa, pr + k, sa + bw, pr + k + nr);
                    rdpq_texture_rectangle(TILE0, img_x + x, b->y0 + r + k, img_x + x + seg, b->y0 + r + k + nr, sx, pr + k);
                    k += nr;
                }
                x += seg; sx = 0;
            }
            r += rows;
        }
    }
    if (rl->nspr > rl->nspr_behind) {
        sprite_mode(s);
        for (int i = rl->nspr_behind; i < rl->nspr; i++) draw_strip(&rl->spr[i]);
    }
    /* lines whose sprites the CPU drew */
    int sprl = 0;
    for (int y = 0; y < SCREEN_LINES; ) {
        if (!(rl->line[y] & RL_SPRL)) { y++; continue; }
        int y1 = y + 1;
        while (y1 < SCREEN_LINES && (rl->line[y1] & RL_SPRL)) y1++;
        if (!sprl) {
            rdpq_set_mode_copy(true);
            rdpq_mode_tlut(TLUT_RGBA16);
            rdpq_tex_upload_tlut(s->tlut_spr, 0, 256);
            sprl = 1;
        }
        blit_rows(s->spr, s->stride, y, y1 - y, img_w);
        y = y1;
    }
    rdpq_detach_show();
}

static void fatal_screen(const char *l1, const char *l2, const char *l3);

/* ---------- game loading ---------- */

static uint8_t *find_and_load_game(uint32_t *size_out, char *title)
{
    for (uint32_t off = BLOB_SCAN_START; off < ROM_MAX; off += BLOB_SCAN_STEP) {
        uint32_t m0 = io_read(0x10000000 + off);
        uint32_t m1 = io_read(0x10000000 + off + 4);
        if (m0 != 0x50434536 || m1 != 0x34474D31) continue; /* "PCE64GM1" */
        uint8_t hdr[PCE64_HDR_SIZE];
        for (int i = 0; i < PCE64_HDR_SIZE; i += 4) {
            uint32_t w = io_read(0x10000000 + off + i);
            hdr[i] = w >> 24; hdr[i + 1] = w >> 16; hdr[i + 2] = w >> 8; hdr[i + 3] = w;
        }
        uint32_t rs = be32(hdr + 12);
        if (rs < 0x2000 || rs > 0x280000) return NULL;
        uint32_t total = (rs + PCE64_HDR_SIZE + 15) & ~15;
        uint8_t *blob = memalign(16, total);
        if (!blob) fatal_screen("Not enough memory for this game.",
                                "It needs the 8 MB Expansion Pak.", NULL);
        /* flush our cache lines first so nothing stale lands on top of the DMA */
        data_cache_hit_writeback_invalidate(blob, total);
        dma_read(blob, off, total);
        data_cache_hit_invalidate(blob, total);
        memcpy(title, hdr + 32, 31);
        title[31] = 0;
        *size_out = total;
        return blob;
    }
    return NULL;
}

static void fatal_screen(const char *l1, const char *l2, const char *l3)
{
    display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_RESAMPLE);
    for (;;) {
        surface_t *s = display_get();
        graphics_fill_screen(s, graphics_make_color(0, 0, 40, 255));
        graphics_set_color(graphics_make_color(255, 255, 255, 255), 0);
        graphics_draw_text(s, 24, 60, "PCE64 - PC Engine emulator");
        graphics_draw_text(s, 24, 100, l1);
        if (l2) graphics_draw_text(s, 24, 116, l2);
        if (l3) graphics_draw_text(s, 24, 132, l3);
        display_show(s);
    }
}

/* ---------- input ---------- */

static uint8_t pad_bits(joypad_inputs_t in, int tick)
{
    joypad_buttons_t b = in.btn;
    uint8_t v = 0;
    if (b.a) v |= PAD_I;
    if (b.b) v |= PAD_II;
    if (b.c_right && tick) v |= PAD_I;    /* turbo I */
    if (b.c_down && tick) v |= PAD_II;    /* turbo II */
    if (b.start) v |= PAD_RUN;
    if (b.z || b.l || b.r) v |= PAD_SELECT;
    if (b.d_up || in.stick_y > 40) v |= PAD_UP;
    if (b.d_down || in.stick_y < -40) v |= PAD_DOWN;
    if (b.d_left || in.stick_x < -40) v |= PAD_LEFT;
    if (b.d_right || in.stick_x > 40) v |= PAD_RIGHT;
    return v;
}

/* N64 controllers 1-4 are PC Engine pads 1-4 on a multitap */
static void read_pads(uint8_t pads[4], int *reset_combo)
{
    joypad_poll();
    static int turbo = 0;
    turbo++;
    int tick = (turbo >> 1) & 1;   /* 15 Hz turbo */
    for (int i = 0; i < 4; i++) pads[i] = pad_bits(joypad_get_inputs(JOYPAD_PORT_1 + i), tick);
    joypad_buttons_t b = joypad_get_buttons(JOYPAD_PORT_1);
    *reset_combo = b.start && b.z && b.l && b.r;
}

#ifdef PCE_PROF
extern uint64_t (*pce_ticks)(void);
extern uint64_t prof_cpu, prof_render, prof_psg, prof_cb, prof_bg, prof_spr, prof_prep;
#endif
#if defined(PCE_PROF) || defined(PCE_SPIKES) || defined(PCE_TPROF)
static uint64_t ticks_fn(void) { return get_ticks(); }
#endif
#ifndef XH_FROM
#define XH_FROM 120
#endif
#ifdef XPROF_HIST
/* ares emux: XPROF slot 1000 toggles the cache-miss histograms (XVSF_HIST) */
#define XPROF(code) do { register long _r asm("t0") = 1000; \
    asm volatile(".word 0x42000028 | (8 << 20) | (" #code " << 6)" :: "r"(_r)); } while (0)
#endif
#ifdef PCE_COUNT
uint64_t cnt_ins, cnt_op[256], cnt_calls;
#endif
#ifdef PCE_SCRIPT
#include "script_input.h"   /* generated: static const struct { uint32_t f; uint8_t p; } script[]; */
#endif

__attribute__((section(".text.pcemain"))) int main(void)
{
#ifdef PCE_STATS
    debug_init_isviewer();
#endif
#ifdef POISON_HEAP
    {   /* test aid: real consoles power on with garbage in RAM, emulators with
           zeros; fill the free heap with garbage to catch zero assumptions */
        static void *blk[64]; int n = 0; uint32_t x = 0x12345678;
        while (n < 64 && (blk[n] = malloc(128 * 1024))) {
            uint32_t *w = blk[n]; for (int i = 0; i < 32768; i++) { x = x * 1664525u + 1013904223u; w[i] = x; }
            data_cache_hit_writeback(blk[n], 128 * 1024); n++;
        }
        while (n) free(blk[--n]);
    }
#endif
    joypad_init();
    make_colortab();
#if defined(PCE_SPIKES) || defined(PCE_TPROF)
    spk_ticks = ticks_fn;
#endif
#ifdef JIT_TRACE
    { extern int jit_trace; jit_trace = 1; }
#endif
#ifdef PCE_PROF
    pce_ticks = ticks_fn;
#endif

#ifdef MICROBENCH
    {
        /* 4 dependent ALU ops + branch per iteration, all in registers */
        uint64_t t0 = get_ticks();
        unsigned a = 1, b = 3;
        for (int i = 0; i < 1000000; i++) { asm volatile("addu %0,%0,%1\n\txor %1,%1,%0\n\tsll %0,%0,1\n\tor %0,%0,%1" : "+r"(a), "+r"(b)); }
        uint64_t t1 = get_ticks();
        debugf("MICROBENCH alu: %llu cpu cycles for 1M iterations (6 instr each) a=%u\n", (t1 - t0) * 2, a);
        static uint8_t buf[65536] __attribute__((aligned(16)));
        volatile uint8_t *vb = buf;
        t0 = get_ticks();
        unsigned s = 0;
        for (int r = 0; r < 16; r++) for (int i = 0; i < 65536; i += 16) s += vb[i];
        t1 = get_ticks();
        debugf("MICROBENCH dmiss: %llu cpu cycles for 65536 line loads s=%u\n", (t1 - t0) * 2, s);
    }
#endif
#ifdef MICROBENCH
    {
        /* synthetic 6280 program in RAM: 20 instructions per loop pass */
        static pce_t Q __attribute__((aligned(16)));
        static uint8_t rom[0x2000 + 64] __attribute__((aligned(16)));
        memcpy(rom, "PCE64GM1", 8); rom[14] = 0x20;  /* 8 KB */
        /* reset vector -> $E000: CSH; LDX #0; loop: LDA $10; ADC $11; STA $12; LDA $12,X; EOR #$55;
           STA $13; INC $14; LDY $15; CMP #3; BNE +0; ... DEX; BNE loop; BRA loop */
        static const uint8_t prog[] = { 0xD4, 0xA2, 0x00,
            0xA5, 0x10, 0x65, 0x11, 0x85, 0x12, 0xB5, 0x12, 0x49, 0x55, 0x85, 0x13, 0xE6, 0x14, 0xA4, 0x15,
            0xC9, 0x03, 0xD0, 0x00, 0xA5, 0x10, 0x65, 0x11, 0x85, 0x12, 0xB5, 0x12, 0x49, 0x55, 0x85, 0x13,
            0xE6, 0x14, 0xA4, 0x15, 0xCA, 0xD0, 0xDB, 0x80, 0xD9 };
        memcpy(rom + 64, prog, sizeof prog);
        rom[64 + 0x1FFE] = 0x00; rom[64 + 0x1FFF] = 0xE0;
        pce_load(&Q, rom, sizeof rom);
        pce_reset(&Q);
        Q.mpr[1] = 0xF8; pce_map(&Q, 1);
        Q.idle_skip = 0;
        uint64_t t0 = get_ticks();
        long cyc = 0;
        for (int i = 0; i < 200; i++) cyc += cpu_run(&Q, 2000);
        uint64_t t1 = get_ticks();
        /* about 3.5 PCE cycles per instruction in this mix */
        debugf("MICROBENCH cpu: %llu N64 cycles for %ld PCE cycles (~%ld instr) -> %llu N64 cycles/instr\n",
               (t1 - t0) * 2, cyc, cyc * 10 / 37, (t1 - t0) * 2 * 37 / (cyc * 10));
    }
#endif
    uint32_t blob_size = 0;
    char title[32];
    uint8_t *blob = find_and_load_game(&blob_size, title);
    if (!blob) fatal_screen("No game found in this ROM.",
                            "Add your own HuCard image with",
                            "pce64_convert.py (see README).");
    if (pce_load(&P, blob, blob_size)) fatal_screen("The game data is damaged.", "Convert the game again.", NULL);

    rdpq_init();
    audio_init(AUDIO_RATE, 4);
    int rate = audio_get_frequency();
    psg_init(&P, rate, NULL, 0);
    abuf = P.psg.outbuf;
#ifdef PCE_NO_PSG
    P.psg.buf = NULL;
#endif
    P.colortab = colortab;
    P.row_prep = row_prep;
    sets_init();
#ifndef PCE_LAYERS_ONLY
    P.deferred = 1;
    P.rl = &RL;
    P.plane = memalign(64, PLANE_MAX_W * PLANE_MAX_H);
    P.sprcache = memalign(64, SPRCACHE_PAIRS * 256);
    if (!P.plane || !P.sprcache) fatal_screen("Not enough memory.", NULL, NULL);
    memset(P.plane, 0, PLANE_MAX_W * PLANE_MAX_H);
    memset(P.sprcache, 0, SPRCACHE_PAIRS * 256);
    data_cache_hit_writeback_invalidate(P.plane, PLANE_MAX_W * PLANE_MAX_H);
    data_cache_hit_writeback_invalidate(P.sprcache, SPRCACHE_PAIRS * 256);
    P.sprcache_w = UncachedAddr(P.sprcache);     /* written a row at a time, read only by the RDP */
    P.plane_w = UncachedAddr(P.plane);           /* scattered cells (whole rows go through the cache) */
    P.gpu_wait = gpu_wait;
    P.cache_wb = cache_wb;
#endif
    pce_reset(&P);
#ifdef PCE_JIT
    {
        /* The recompiled code cache takes what memory is left (more room means
           fewer flushes). The widest display is set up first so that the
           display can always be set up again in the space it frees. */
        set_video(512);
#ifdef JIT_BUF_KB
        static const unsigned sizes[] = { JIT_BUF_KB << 10 };
#else
        static const unsigned sizes[] = { 3u << 20, 5u << 19, 2u << 20, 3u << 19, 1u << 20, 3u << 18, 1u << 19 };
#endif
        void *keep = malloc(128 * 1024);            /* margin for later small allocations */
        void *cb = NULL; unsigned cs = 0;
        for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0] && !cb; i++) { cs = sizes[i]; cb = memalign(65536, cs); }
        free(keep);
        if (!cb) fatal_screen("Not enough memory.", NULL, NULL);
        {
            /* the runtime right after the per-line C code in the I-cache, so
               that translated code can be kept out of both (jit_window) */
            extern char __pcehot_start[], __pcehot1_end[];
            unsigned off = ((uintptr_t)__pcehot1_end + 31) & 0x3FE0;
            jit_set_hot((uintptr_t)__pcehot_start);
            jit_init_buffer((char *)cb + off, cs - off);
        }
#ifdef PCE_STATS
        { extern unsigned jit_rt_bytes(void); debugf("PCE64 JIT code cache %u KB, runtime %u bytes\n", cs >> 10, jit_rt_bytes()); }
#endif
    }
#endif
    set_video(256);
    int pending_w = 256, pending_n = 0;
    int cur = 0;
    int skip_run = 0, skip_next = 0;     /* adaptive frame skip */

#ifdef PCE_STATS
    uint64_t t_emu = 0, t_max = 0, t_wait = 0, t_rdp = 0, t_busy = 0, t_bmax = 0;
    int n_over = 0;
    int skipped = 0;
    debugf("PCE64 START %s rate=%d mem=%d\n", title, rate, get_memory_size());
    register_VI_handler(vi_tick);
#endif
#ifdef PCE_SCRIPT
    int si = 0;
    uint8_t spad = 0;
#endif

    for (;;) {
        int reset = 0;
        uint8_t pads[4];
        read_pads(pads, &reset);
        uint8_t pad = pads[0];
#ifdef PCE_SCRIPT
        while (si < (int)(sizeof(script) / sizeof(script[0])) && script[si].f <= P.frame) spad = script[si++].p;
        pad = spad;
        pads[1] = pads[2] = pads[3] = 0;
#endif
        P.pad[0] = pad;
        for (int i = 1; i < 4; i++) P.pad[i] = pads[i];
        if (reset) pce_reset(&P);

        layerset_t *s = &sets[cur];
        uint64_t tf0 = get_ticks();
        surface_t *fb = skip_next ? NULL : display_get();
        P.skip_render = (fb == NULL);
#ifdef PCE_NO_RENDER
        P.skip_render = 1;
#endif
#ifdef PCE_STATS
        uint64_t tw = get_ticks();
#endif
        if (fb && s->busy) { rspq_syncpoint_wait(s->sync); s->busy = 0; }
        P.layer_bg = s->bg; P.layer_spr = s->spr;
        P.layer_stride = s->stride; P.spr_ext = s->ext;
#ifdef PCE_STATS
        if (!fb) skipped++;
        uint64_t t0 = get_ticks();
        t_rdp += t0 - tw;
#endif
#ifdef XPROF_HIST
        if (P.frame == XH_FROM) XPROF(1);
        if (P.frame == XH_FROM + 120) XPROF(2);
#endif
        P.psg.bufpos = 0;
#ifdef FREEZE_AT
        /* test aid: stop at a frame and keep showing it (for screenshots) */
        if (P.frame >= FREEZE_AT) { memset(abuf, 0, 4 * 534); P.psg.bufpos = 2 * 534; } else
#endif
        { TP_BEGIN(7); pce_run_frame(&P); TP_END(7, 7); }
#ifdef PCE_JIT
        {
            /* Games with raster interrupts on many lines spend much of the
               frame in the emulator's per-line code: then translated code is
               kept out of its part of the I-cache. Others are better off
               with the whole cache for their own code. */
            static unsigned fcount, ion;
            if (++fcount == 60) {
                unsigned r = P.rcr_irqs; P.rcr_irqs = 0; fcount = 0;
#ifdef PCE_STATS
                debugf("PCE64 RCR frame=%llu irqs=%u window=%u\n", P.frame, r / 60, ion);
#endif
                if (!ion && r >= 60 * 16) { ion = 1; jit_window(1); }
                else if (ion && r < 60 * 8) { ion = 0; jit_window(0); }
            }
        }
#endif
#ifdef PCE_STATS
        uint64_t t1 = get_ticks();
        uint64_t dt = t1 - t0;
        t_emu += dt; if (dt > t_max) t_max = dt;
#endif
        EMUX_BEGIN(PS_POST);
        if (fb) {
#ifdef RL_DUMP
            if (P.frame >= RL_DUMP && P.frame < RL_DUMP + 6) {
                uint32_t h = 0; for (int y = 0; y < SCREEN_LINES; y++) h = h * 31 + P.line_bd[y];
                uint32_t hb = 0; for (int i = 0; i < RL.nband; i++) { const uint8_t *q = (const uint8_t *)&RL.band[i]; for (unsigned k = 0; k < sizeof RL.band[i]; k++) hb = hb * 31 + q[k]; }
                uint32_t hp = 0, hc = 0; const uint8_t *pu = UncachedAddr(P.plane);
                for (int i = 0; i < RL.plane_w * RL.plane_h; i += 4) { hp = hp * 31 + *(const uint32_t *)(pu + i); hc = hc * 31 + *(const uint32_t *)(P.plane + i); }
                uint32_t rowz[8] = {0}; for (int r = 0; r < 8; r++) { const uint8_t *q = pu + (231 + r * 30) * RL.plane_w; for (int i = 0; i < RL.plane_w; i++) rowz[r] += q[i] != 0; }
                debugf("RLP frame=%llu plane_ram=%08lx plane_cached=%08lx nz rows231+30k: %lu %lu %lu %lu %lu %lu %lu %lu\n", P.frame, (unsigned long)hp, (unsigned long)hc,
                       (unsigned long)rowz[0], (unsigned long)rowz[1], (unsigned long)rowz[2], (unsigned long)rowz[3], (unsigned long)rowz[4], (unsigned long)rowz[5], (unsigned long)rowz[6], (unsigned long)rowz[7]);
                debugf("RLD frame=%llu set=%d nband=%d nspr=%d bd=%08lx bands=%08lx pw=%d ph=%d b0=%d,%d,%d,%d,%x\n", P.frame, cur, RL.nband, RL.nspr, (unsigned long)h, (unsigned long)hb,
                       RL.plane_w, RL.plane_h, RL.band[0].y0, RL.band[0].n, RL.band[0].bx, RL.band[0].by, RL.band[0].flags);
            }
#endif
            TP_BEGIN(5);
            if (P.deferred) { draw_frame(s, fb); TP_END(5, 5); }
            else {
                build_tluts(s);
                data_cache_hit_writeback(s->bg - 16, (size_t)s->stride * SCREEN_LINES);
                data_cache_hit_writeback(s->spr - 16, (size_t)s->stride * SCREEN_LINES);
                if (!s->block || s->block_w != img_w) {
                    if (s->block) rspq_block_free(s->block);
                    record_block(s);
                }
                rdpq_attach(fb, NULL);
                rspq_block_run(s->block);
                rdpq_detach_show();
            }
            /* the RSP waits here until the RDP has drawn the frame, so the
               sync point below means the RDP is done with the layers, the
               plane and the sprite cache (without the fence it only means
               the RSP has passed the commands on, and the RDP may still be
               reading what the next frame's emulation starts to change) */
#ifndef NO_FENCE
            rdpq_fence();
#endif
            s->sync = rspq_syncpoint_new();
            s->busy = 1;
            last_sync = s->sync; last_busy = 1;
            cur = (cur + 1) % NSETS;
        }

        EMUX_END(PS_POST);
        /* optional adaptive frame skip (off in release builds): when a frame
           overran the 60 Hz budget, emulate the next one without drawing it */
#ifdef PCE_FRAMESKIP
        {
            uint64_t dt = get_ticks() - tf0;
            if (!fb) skip_run++; else skip_run = 0;
            skip_next = dt > TICKS_FROM_US(15800) && skip_run < 2;
        }
#else
        (void)tf0; (void)skip_run;
#endif

        /* follow width changes after they have been stable for a few frames */
        int w = P.line_width[SCREEN_LINES / 2];
        if (w && w != img_w) {
            if (w == pending_w) { if (++pending_n >= 8) { set_video(w); pending_n = 0; } }
            else { pending_w = w; pending_n = 1; }
        } else pending_n = 0;

        /* audio paces the emulation */
#ifdef PCE_STATS
        uint64_t ta = get_ticks();
        { uint64_t b = ta - t0; t_busy += b; if (b > t_bmax) t_bmax = b; if (b > TICKS_FROM_US(16600)) n_over++;
#ifdef PCE_SPIKES
          extern unsigned jit_stat_tr, jit_stat_flush, jit_stat_inval;
          extern long rls_bgh, rls_sprh, rls_cells, rls_plcells, rls_snap;
          static unsigned l_tr, l_fl, l_inv; static long l_bgh, l_sprh, l_cells, l_pl, l_snap;
          if (b > TICKS_FROM_US(SPIKE_US))
              debugf("PCE64 SPIKE frame=%llu busy=%lluus emu=%lluus tr=%u flush=%u inval=%u bghaz=%ld sprhaz=%ld sprcells=%ld planecells=%ld snaps=%ld plane=%lluus sprruns=%lluus\n",
                     P.frame, TICKS_TO_US(b), TICKS_TO_US(dt), jit_stat_tr - l_tr, jit_stat_flush - l_fl, jit_stat_inval - l_inv,
                     rls_bgh - l_bgh, rls_sprh - l_sprh, rls_cells - l_cells, rls_plcells - l_pl, rls_snap - l_snap, TICKS_TO_US(spk_plane), TICKS_TO_US(spk_sprr));
          spk_plane = spk_sprr = 0;
          l_tr = jit_stat_tr; l_fl = jit_stat_flush; l_inv = jit_stat_inval; l_bgh = rls_bgh; l_sprh = rls_sprh; l_cells = rls_cells; l_pl = rls_plcells; l_snap = rls_snap;
#endif
        }
#endif
#ifdef JIT_TMAP
        if (P.frame == JIT_TMAP) { extern void jit_tmap_dump(void); jit_tmap_dump(); }
#endif
        EMUX_BEGIN(PS_AUDIO);
        audio_push(abuf, P.psg.bufpos / 2, true);
        EMUX_END(PS_AUDIO);
#ifdef PCE_STATS
        t_wait += get_ticks() - ta;
        if (P.frame % 60 == 0) {
            { extern unsigned jit_stat_tr, jit_stat_flush, jit_stat_pin, jit_stat_refill, jit_stat_inval;
#ifdef PCE_IOSTAT
              { extern unsigned iostat_w[64], iostat_r[8]; debugf("IOSTAT f%llu w:", P.frame); for (int k = 0; k < 64; k++) if (iostat_w[k]) debugf(" %02x=%u", k, iostat_w[k] / 60); debugf(" r:"); for (int k = 0; k < 8; k++) if (iostat_r[k]) debugf(" %d=%u", k, iostat_r[k] / 60); debugf("\n"); memset(iostat_w, 0, sizeof iostat_w); memset(iostat_r, 0, sizeof iostat_r); }
#endif
#ifdef JIT_COUNTS
              { extern unsigned jit_cnt[8], jit_run_cyc, jit_run_ticks; debugf("JCNT f%llu check=%u lookup=%u evt=%u interp=%u chain=%u io=%u interp_cyc=%u interp_us=%u\n", P.frame,
                  jit_cnt[0] / 60, jit_cnt[1] / 60, jit_cnt[2] / 60, jit_cnt[3] / 60, jit_cnt[6] / 60, jit_cnt[7] / 60, jit_run_cyc / 60, (unsigned)(TICKS_TO_US(jit_run_ticks) / 60));
                memset(jit_cnt, 0, sizeof jit_cnt); jit_run_cyc = 0; jit_run_ticks = 0; }
#endif
#ifdef PCE_TPROF
              debugf("TPROF f%llu bx=%llu psg=%llu dispend=%llu catchup=%llu masks=%llu draw=%llu plane=%llu runframe=%llu\n", P.frame,
                     TICKS_TO_US(tprof[0]) / 60, TICKS_TO_US(tprof[1]) / 60, TICKS_TO_US(tprof[2]) / 60, TICKS_TO_US(tprof[3]) / 60,
                     TICKS_TO_US(tprof[4]) / 60, TICKS_TO_US(tprof[5]) / 60, TICKS_TO_US(tprof[6]) / 60, TICKS_TO_US(tprof[7]) / 60);
              memset(tprof, 0, sizeof tprof);
#endif
              debugf("PCE64 BUSY frame=%llu busy=%lluus max=%lluus over16.6=%d tr=%u pin=%u flush=%u refill=%u inval=%u\n", P.frame, TICKS_TO_US(t_busy) / 60, TICKS_TO_US(t_bmax), n_over,
                     jit_stat_tr, jit_stat_pin, jit_stat_flush, jit_stat_refill, jit_stat_inval); }
            t_busy = t_bmax = 0; n_over = 0;
            { static unsigned last_vi; unsigned vi = vi_count; debugf("PCE64 VSYNC frame=%llu vsyncs=%u\n", P.frame, vi - last_vi); last_vi = vi; }
            debugf("PCE64 STATS frame=%llu emu=%lluus max=%lluus wait=%lluus rdpwait=%lluus skipped=%d hash=%08lx\n",
                   P.frame, TICKS_TO_US(t_emu) / 60, TICKS_TO_US(t_max), TICKS_TO_US(t_wait) / 60,
                   TICKS_TO_US(t_rdp) / 60, skipped,
#ifdef PCE_HASH
                   (unsigned long)pce_hash(&P));
#else
                   0ul);
#endif
#ifdef RUN_HIST
            { extern unsigned rh_pc[16], rh_n[16]; char b[400]; int o = 0;
              for (int i = 0; i < 16 && rh_n[i]; i++) o += snprintf(b + o, sizeof b - o, " %04x:%u", rh_pc[i], rh_n[i]);
              debugf("PCE64 RUNS frame=%llu%s\n", P.frame, b); memset(rh_n, 0, sizeof rh_n); }
#endif
#ifdef PSG_LOGHASH
            { extern uint32_t psg_log_hash; debugf("PCE64 PSGHASH frame=%llu %08lx\n", P.frame, (unsigned long)psg_log_hash); }
#endif
#ifdef PCE_PROF
            debugf("PCE64 PROF cpu=%lluus render=%lluus (bg=%llu spr=%llu prep=%llu) psg=%lluus\n", TICKS_TO_US(prof_cpu) / 60,
                   TICKS_TO_US(prof_render) / 60, TICKS_TO_US(prof_bg) / 60, TICKS_TO_US(prof_spr) / 60, TICKS_TO_US(prof_prep) / 60, TICKS_TO_US(prof_psg) / 60);
            prof_cpu = prof_render = prof_cb = prof_psg = prof_bg = prof_spr = prof_prep = 0;
#endif
#ifdef PCE_COUNT
            debugf("PCE64 COUNT ins/frame=%llu calls/frame=%llu\n", cnt_ins / 60, cnt_calls / 60);
            cnt_ins = 0; cnt_calls = 0;
#endif
#if defined(PCE_EMUX) && defined(PCE_JIT)
            { extern unsigned jit_stat_tr, jit_stat_flush, jit_stat_ins, jit_stat_hot, jit_stat_cold, jit_stat_inval; debugf("PCE64 JIT flushes=%u blocks=%u inval=%u ins=%u hot=%u cold=%u bytes/ins=%u\n", jit_stat_flush, jit_stat_tr, jit_stat_inval, jit_stat_ins, jit_stat_hot, jit_stat_cold, jit_stat_ins ? jit_stat_hot / jit_stat_ins : 0); jit_stat_tr = 0; jit_stat_inval = 0; }
#endif
#ifdef JIT_COUNTS
            { extern unsigned jit_cnt[8]; debugf("PCE64 JITCNT check=%u lookups=%u interp=%u rdslow=%u wrslow=%u chain=%u\n",
                jit_cnt[0] / 60, jit_cnt[1] / 60, jit_cnt[3] / 60, jit_cnt[4] / 60, jit_cnt[5] / 60, jit_cnt[6] / 60); memset(jit_cnt, 0, sizeof jit_cnt); }
#endif
#ifdef JIT_OPSTATS
            if (P.frame % 600 == 0) {
                extern unsigned jst_n[256], jst_hot[256], jst_cold[256];
                unsigned tot = 0, totc = 0, used[256] = {0};
                for (int i = 0; i < 256; i++) { tot += jst_hot[i]; totc += jst_cold[i]; }
                debugf("PCE64 OPSTATS hot=%u cold=%u\n", tot, totc);
                for (int k = 0; k < 24; k++) {
                    int bi = -1;
                    for (int i = 0; i < 256; i++) if (!used[i] && (bi < 0 || jst_hot[i] > jst_hot[bi])) bi = i;
                    used[bi] = 1;
                    debugf("  op %02x n=%u hot=%u (%u/ins) cold=%u\n", bi, jst_n[bi], jst_hot[bi], jst_n[bi] ? jst_hot[bi] / jst_n[bi] : 0, jst_cold[bi]);
                }
            }
#endif
#ifdef PCE_EMUX
            {
                static const char *nm[] = { "", "cpu", "catchup", "transl", "psg", "line", "frame", "post", "audio", "gpuwait", "final", "io" };
                char buf[400]; int o = 0;
                for (int s2 = 1; s2 < PS_NSLOTS; s2++) {
                    unsigned long long cy = emux_read(s2, 0x0000), im = emux_read(s2, 0x0011), dm = emux_read(s2, 0x0021);
                    o += snprintf(buf + o, sizeof buf - o, " %s=%llu/%llu/%llu", nm[s2], cy / 60 / 1000, im / 60, dm / 60);
                    asm volatile("" ::: "memory");
                }
                debugf("PCE64 EMUX kcyc/imiss/dmiss per frame:%s\n", buf);
                register long _r asm("t0") = 0;
                asm volatile(".word 0x42000028 | (8 << 20) | (4 << 6)" :: "r"(_r));   /* reset all slots */
            }
#endif
            t_emu = t_max = t_wait = t_rdp = 0; skipped = 0;
        }
#endif
    }
}
