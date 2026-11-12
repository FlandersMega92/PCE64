/* PCE64 - PC Engine / TurboGrafx-16 emulator core (portable C).
 * Shared by the host test harness and the N64 frontend. */
#ifndef PCE_H
#define PCE_H
#include <stdint.h>
#include <string.h>

#ifdef __mips__
#define PCE_HOT __attribute__((section(".text.pcehot")))
/* the per-line part of it, first (an I-cache window the recompiler can keep
   its code out of: JIT_HOTWIN) */
#ifdef NO_HOT1
#define PCE_HOT1 PCE_HOT
#else
#define PCE_HOT1 __attribute__((section(".text.pcehot1")))
#endif
/* the rest of the emulator's per-frame code, right after it: its place in
   the I-cache then does not move when unrelated code changes (a stats build
   behaves like a release build) */
#define PCE_WARM __attribute__((section(".text.pcewarm")))
#else
#define PCE_HOT
#define PCE_HOT1
#define PCE_WARM
#endif

/* ---- profiling with ares emux profile slots (no cost to guest timing) ---- */
#if defined(PCE_EMUX) && defined(__mips__)
#define EMUX_BEGIN(slot) do { register long _r asm("t0") = (slot); \
    asm volatile(".word 0x42000028 | (8 << 20) | (1 << 6)" :: "r"(_r)); } while (0)
#define EMUX_END(slot) do { register long _r asm("t0") = (slot); \
    asm volatile(".word 0x42000028 | (8 << 20) | (2 << 6)" :: "r"(_r)); } while (0)
static inline unsigned long long emux_read(long slot, long code)
{
    register long _d asm("t0") = slot;
    register long long _t asm("t1") = code;
    asm volatile(".word 0x42000029 | (8 << 20) | (9 << 15)" : "+r"(_t) : "r"(_d));
    return _t;
}
#else
#define EMUX_BEGIN(slot) do { } while (0)
#define EMUX_END(slot) do { } while (0)
#endif
enum { PS_CPU = 1, PS_BG, PS_SPR, PS_PSG, PS_LINE, PS_FRAME, PS_POST, PS_AUDIO, PS_VRAMW, PS_IDLE, PS_IO, PS_NSLOTS };

/* ---- game blob produced by tools/pce64_convert.py ---- */
#define PCE64_MAGIC "PCE64GM1"
#define PCE64_HDR_SIZE 64
enum { MAPPER_NORMAL = 0, MAPPER_SF2 = 1 };
#define GF_REGION_US 0x01
#define GF_NO_SPRLIMIT 0x02
#define GF_NO_MULTITAP 0x04   /* one pad, no TurboTap (for the odd game that breaks with one) */

typedef struct {
    char magic[8];
    uint32_t version;   /* big endian in file */
    uint32_t rom_size;
    uint32_t flags;
    uint32_t mapper;
    uint32_t crc32;
    uint32_t reserved;
    char title[32];
} pce64_hdr_t;

/* ---- joypad bits (our own, active high) ---- */
enum {
    PAD_I = 0x01, PAD_II = 0x02, PAD_SELECT = 0x04, PAD_RUN = 0x08,
    PAD_UP = 0x10, PAD_RIGHT = 0x20, PAD_DOWN = 0x40, PAD_LEFT = 0x80,
};

#define LINES_PER_FRAME 263
#ifndef RCR_LEAD
#define RCR_LEAD 0        /* lines the raster counter reaches $40 before the display */
#endif
#define SATB_DMA_LINES 4
#define VD_AT 64           /* cycles into the first vblank line: the vblank interrupt (start of HDW) */
#define EXACT_NEAR 600     /* cycles: a VDC event this close after the timer makes a stop exact */      /* the SATB DMA ends this many lines into the vertical blank */
#ifdef PCE_TPROF
/* time accumulators (N64 ticks) for measurement builds */
#include <stdint.h>
extern uint64_t (*spk_ticks)(void);
extern uint64_t tprof[8];
#define TP_BEGIN(n) uint64_t _tp##n = spk_ticks()
#define TP_END(n, i) (tprof[i] += spk_ticks() - _tp##n)
#else
#define TP_BEGIN(n) ((void)0)
#define TP_END(n, i) ((void)0)
#endif
#define CYCLES_PER_LINE 455      /* 7.16 MHz CPU cycles per scanline (1365 master / 3) */
#define SCREEN_LINES 240         /* lines handed to the frontend */
#define SCREEN_FIRST 20          /* first VCE line shown: the standard 240-line frame (VDS 15, VSW 2) starts here */
#define MAX_WIDTH 512

typedef struct pce_s pce_t;

/* ---- deferred rendering ----
 * In deferred mode the core does not draw pixels. For each frame it records
 * what to draw (bands of background lines, sprite strips) and keeps two
 * images the frontend's GPU draws from: the background plane (the whole BAT
 * as 8-bit pixels, palette << 4 | colour) and a sprite pattern cache (4-bit
 * pixels, two 16x16 cells side by side per 32x16 block). Lines whose VRAM
 * data changes after they were shown are drawn by the CPU into the layers
 * instead, so the result matches the line-by-line renderer. */
#define RL_MAX_SPR 1024
enum { RB_BG = 1, RB_LAYER = 2 };                  /* band flags */
enum { RS_XF = 1, RS_YF = 2, RS_FRONT = 4, RS_W32 = 8, RS_RIGHT = 16 };
enum { RL_DISP = 1, RL_SPRL = 2 };                 /* line flags */
typedef struct { int16_t y0, n; uint16_t bx, by, w, cr, mwr; uint8_t flags; } rl_band_t;
typedef struct {
    int16_t x, y, n;        /* screen position of the strip's first line, line count */
    uint8_t ry, h;          /* sprite row of the first line (before flipping), sprite height */
    uint16_t pair;          /* first 32x16 block in the pattern cache */
    uint8_t pal, flags;
} rl_spr_t;
typedef struct {
    int nband;
    rl_band_t band[SCREEN_LINES];
    int nspr, nspr_behind;  /* behind-background strips first, each group back to front */
    rl_spr_t spr[RL_MAX_SPR];
    uint8_t line[SCREEN_LINES];
    int plane_w, plane_h;   /* background plane size in pixels */
    int used_plane;         /* some band draws from the plane */
} rlist_t;
#define PLANE_MAX_W 1024
#define PLANE_MAX_H 512
#define SPRCACHE_PAIRS 260  /* 256 + 4 wrap-around copies */

typedef struct {
    uint16_t vram[0x8000];
    uint16_t satb[256];
    uint16_t reg[32];
    uint8_t sel;
    uint16_t status;
    uint16_t rdbuf;
    uint16_t wlatch;
    int yoff;              /* current BG y (latched from BYR) */
    int raster;            /* raster counter: $40 on the first display line, then +1 every line
                              (blanking included) until the next frame's first display line */
    int raster0;           /* its value on line 0 of this frame */
    int satb_pending;
    int ds_line;           /* line the SATB DMA finishes on (its interrupt), or -1 */
    int rr_late;           /* frame position of the vblank interrupt (a little into the first vblank line), or -1 */
    /* open band of lines waiting to be drawn */
    int b_y0, b_n, b_w;
    unsigned b_yoff0;
    uint16_t b_bxr, b_mwr, b_cr;
    uint8_t bgdirty[2048];
    uint8_t bgcache[2048 * 64] __attribute__((aligned(16)));
    int burst;             /* display disabled this frame */
    int disp_start, disp_end; /* VCE lines of active display */
    uint8_t sl_n[LINES_PER_FRAME], sl_cells[LINES_PER_FRAME], sl_ovf[LINES_PER_FRAME];
    uint64_t sl_frame;     /* frame the sprite lists were built for */
    uint8_t sl_idx[LINES_PER_FRAME][64];
    struct { int16_t d0, n; uint8_t i; } sr[512]; /* deferred: runs of display lines per sprite, in index order */
    int nsr, sl_built;
    /* deferred rendering */
    unsigned bat_words;    /* BAT size in words for the current MWR */
    uint16_t pl_mwr;       /* MWR the plane was built for */
    int pl_full;           /* plane needs a full rebuild */
    int bg_pend, spr_pend; /* this frame has lines drawn later from the plane / sprite cache */
    int pl_tdirty_any, pl_dn;
    uint16_t tref[2048];   /* BAT cells using each tile */
    uint8_t pl_tdirty[2048];
    uint8_t pl_cdirty[8192];
    unsigned pl_cmax;      /* largest BAT this frame (cells marked in pl_cdirty lie below it) */
    uint16_t pl_dlist[1024];
    uint8_t sc_dirty[512]; /* sprite cells changed since cached */
    uint8_t cell_used[512];/* sprite cells used by this frame's sprites */
    uint16_t cell_line[512], cell_last[512]; /* display lines showing the cell */
    uint8_t sc_frozen[512];/* cached already for this frame: newer data waits */
    int snap_n;            /* frozen cells' patterns as the waiting lines saw them (see snap_swap) */
    uint16_t snap_cell[64];
    uint16_t snap_data[64][64];
    int spr_first, spr_last; /* display lines with sprites waiting for the GPU, or -1 */
    uint32_t tvis[64];     /* bitmap: tile shown by a line waiting for the plane (kept up to date lazily) */
    int vis_nb, vis_open_y0, vis_open_n;  /* what tvis covers: bands before vis_nb, open band lines before vis_open_n */
} vdc_t;

typedef struct {
    uint16_t pal[512];
    uint16_t addr;
    uint8_t cr;
    uint8_t dirty;         /* palette changed since frontend last looked */
    uint16_t snap[512];    /* palette as it was at the end of the visible frame */
} vce_t;

typedef struct {
    uint8_t wave[32];
    uint8_t wpos;
    uint16_t freq;
    uint8_t ctrl;
    uint8_t bal;
    uint8_t noise;
    uint8_t dda;
    uint32_t phase;        /* 16.16 position in the waveform */
    uint32_t nphase;
    uint32_t lfsr;
    int noise_out;
    int32_t al, ar;        /* cached amplitudes */
    int32_t lutl[32], lutr[32]; /* sample value -> contribution */
    uint64_t tt[32];       /* wave position -> both contributions, packed (see psg.c) */
    uint32_t step;         /* cached phase step per output sample */
    uint8_t dirty;
    uint8_t tdirty;        /* tt needs rebuilding (wave or volume changed) */
} psg_ch_t;

#define PSG_LOG_MAX 4096
typedef struct {
    psg_ch_t ch[6];
    int16_t outbuf[2 * 640];   /* next to the channel tables: no cache conflicts while mixing */
    uint8_t sel, mainbal, lfofreq, lfoctrl;
    int rate;              /* output rate */
    int16_t *buf;          /* stereo interleaved */
    int bufpos, bufcap;
    uint32_t frac;         /* sample timing accumulator */
    uint32_t step;         /* output samples per line in 16.16 */
    uint8_t active;        /* channels that make sound */
    int nlog;
    struct { uint16_t line; uint8_t reg, val; } log[PSG_LOG_MAX];
    uint8_t anydirty;
    uint32_t kstep;        /* (PSG clock << 16) / output rate */
} psg_t;

struct pce_s {
    /* CPU (kept at the start: the recompiler addresses these with 16-bit offsets) */
    uint8_t A, X, Y, S, P;
    uint16_t PC;
    uint8_t mpr[8];
    uint8_t speed;         /* 1 = high (7.16 MHz), 0 = low */
    int halted;
    int cycles_extra;
    /* interrupts */
    uint8_t irq_mask;      /* $1402 */
    uint8_t irq_pend;      /* bit0 IRQ2, bit1 IRQ1 (VDC), bit2 timer */
    uint8_t irq_lines;     /* pend & ~mask */
    /* idle loop skipping */
    int idle_skip;
    unsigned idle_last, idle_yes;
    /* recompiler state: flags kept apart (N/Z lazily in jnz), per-slot block keys */
    uint32_t jnz;
    uint8_t jc, jv, jd, ji;
    uint8_t jchk, jexit;
    uint8_t jpin;          /* in the timer interrupt handler: translated code is pinned in the I-cache */
    /* block transfer in progress (it goes on at the start of the next run) */
    uint32_t bx_n;
    uint16_t bx_src, bx_dst;
    uint8_t bx_op, bx_alt;
    uint32_t jkey[8];
    /* event-driven timing: frame position = chunk_end - cyc_left while the CPU runs */
    int chunk_end, cyc_left, cut_delta, in_cpu, step_base;
    int chunk_start;       /* frame position the current run started at */
    int jit_noevt;         /* recompiler: always return at the end of the budget (tests) */
    int ev_other;          /* next event that is not the timer (next_event) */
    uint8_t ev_other_vdc;  /* ev_other is a VDC event (not the end of the frame) */
    uint8_t ev_pick_exact; /* next_event: the event it returned needs the exact instruction */
    uint8_t ev_exact;      /* the running budget ends at such an event: the CPU stops at the
                              first instruction boundary past it; otherwise (the timer, the end
                              of the frame) at the next check point (jump, taken branch, ...) */
    int vdc_ev;            /* frame position of the next VDC event (catch up there) */
    int tm_pos;            /* frame position the timer has been advanced to */
    int next_due;          /* frame position of the next line start / render latch */
    unsigned dbg_pc, dbg_io;   /* verification builds: where the last I/O came from */
    /* memory map per MPR slot */
    uint8_t *rmap[8];
    uint8_t *wmap[8];
    /* physical banks */
    uint8_t *rom;
    uint32_t rom_size;
    int mapper;
    int sf2bank;
    uint8_t *rompage[128];
    /* what each physical bank maps to (pce_map / TAM copy from here) */
    uint8_t *bank_rmap[256];
    uint8_t *bank_wmap[256];
    uint32_t bank_jkey[256];
    uint16_t irqvec[3];    /* IRQ2, IRQ1, TIMER vectors of the bank in slot 7 */
    uint8_t vec_ok;        /* irqvec is current (slot 7 holds ROM) */
    uint8_t ram[0x2000] __attribute__((aligned(16)));
    int cart_ram_on;
    uint8_t ffpage[0x2000];
    uint8_t iobuf;
    /* timer */
    uint8_t tm_reload, tm_count, tm_on;
    int tm_cycles;
    /* joypad */
    uint8_t pad[5];
    uint8_t pad_sel, pad_clr, pad_idx;
    uint32_t flags;
    /* video */
    vdc_t vdc;
    vce_t vce;
    psg_t psg;
    int line;              /* current VCE line */
    uint64_t frame;
    unsigned rcr_irqs;     /* raster interrupts raised (the frontend's I-cache policy looks at the rate) */
    int width;             /* rendered width of the current frame */
    int line_width[SCREEN_LINES];
    uint16_t line_bd[SCREEN_LINES];   /* backdrop colour each line was shown with (VCE $000, or $100 outside the display) */
    /* line callback for frontends that convert per line */
    void (*line_cb)(pce_t *, int y, const uint8_t *bg, const uint8_t *spr, int w);
    const uint16_t *colortab;  /* optional: 9-bit VCE color -> frontend pixel */
    uint16_t pal_native[512];  /* colortab applied to the palette, kept current */
    int skip_render;           /* run the frame without drawing it */
    void *user;
    int run_debt;
    int force_interp;
    struct vlog_s { int fc, end, used, pc; uint32_t a, ram; } *vlog; int vn; int vtrace, vtrace_run1; int rlog[64][3]; int rn; int (*vwlog)[4]; int vwn;
    int next_ls, next_lt;  /* next line start / render latch to process */
    int render_at;         /* CPU cycles into a line when the VDC latches it */
    /* layers: rows of 8-bit indices; bg 0..255 (0x10 = overscan), spr 0 = none.
       Pointers are to pixel (0,0); rows need 16 bytes of slack on both sides. */
    uint8_t *layer_bg, *layer_spr;
    int layer_stride;
    void (*row_prep)(uint8_t *row, int len);  /* called before a bg row is overwritten */
    uint16_t *spr_ext;     /* 2 per screen line, owned by whoever owns the layers */
    /* deferred rendering (frontend sets these before pce_reset) */
    int deferred;
    rlist_t *rl;
    uint8_t *plane;        /* PLANE_MAX_W * PLANE_MAX_H bytes, row stride = plane_w */
    uint8_t *sprcache;     /* SPRCACHE_PAIRS * 256 bytes */
    uint8_t *sprcache_w;   /* optional: an uncached alias of it to write through (no cache writeback needed) */
    uint8_t *plane_w;      /* optional: an uncached alias of the plane, for scattered cells */
    void (*gpu_wait)(pce_t *p);                 /* GPU done reading plane / cache */
    void (*cache_wb)(const void *p, unsigned n);/* written data must reach memory */
    /* last: large, and the recompiler addresses the fields above with 16-bit offsets */
    uint8_t cart_ram[0x8000] __attribute__((aligned(16)));   /* Populous: 32 KB of RAM on the card, banks $40-$43 */
};

/* pce.c */
int pce_load(pce_t *p, const uint8_t *blob, uint32_t blob_size);
void pce_reset(pce_t *p);
void pce_run_frame(pce_t *p);
void pce_update_irq(pce_t *p);
void pce_catch_up(pce_t *p, int pos);
void pce_reschedule(pce_t *p);
static inline int pce_now(pce_t *p) { return p->chunk_end - p->cyc_left; }
uint8_t pce_io_read(pce_t *p, uint32_t phys);
void pce_io_write(pce_t *p, uint32_t phys, uint8_t v);
uint8_t pce_slow_read(pce_t *p, uint16_t addr);
void pce_slow_write(pce_t *p, uint16_t addr, uint8_t v);
void pce_map(pce_t *p, int slot);
void pce_relocate(pce_t *p);
uint32_t pce_hash(pce_t *p);

/* cpu.c */
void cpu_reset(pce_t *p);
int cpu_run(pce_t *p, int cycles);  /* returns cycles actually used */
void pce_irq_soon(pce_t *p);
int cpu_step(pce_t *p, int *checkpoint); /* exactly one instruction (cpu_step.c) */
int cpu_ramrun(pce_t *p, int cycles);   /* recompiler fallback (cpu_ramrun.c) */
void cpu_bx_continue(pce_t *p);
/* VDC/VCE access penalty: CPU time taken now (the running budget shrinks, so
   events within the run still fall at the right place) */
static inline void pce_penalty(pce_t *p, int n) { if (p->in_cpu) p->cyc_left -= n; else p->cycles_extra += n; }
void cpu_blk(pce_t *p, unsigned pc);           /* recompiler: a block transfer instruction */         /* a long block transfer goes on (budget in p->cyc_left) */

/* jit.c (MIPS only) */
int jit_init(void);
int jit_init_buffer(void *buf, unsigned bytes);
int jit_run(pce_t *p, int cycles);
void jit_flush(void);
void jit_set_hot(unsigned lo);
void jit_window(int on);

/* vdc.c */
void vdc_reset(pce_t *p);
uint8_t vdc_read(pce_t *p, int a);
void vdc_write(pce_t *p, int a, uint8_t v);
void vdc_vram_word(pce_t *p, uint8_t lo, uint8_t hi);
void vdc_vram_words(pce_t *p, const uint8_t *s, unsigned nw);
uint8_t vce_read(pce_t *p, int a);
void vce_write(pce_t *p, int a, uint8_t v);
void vdc_frame_start(pce_t *p);
void vdc_line_start(pce_t *p, int line);
void vdc_line(pce_t *p, int line);
void vdc_frame_end(pce_t *p);
void vdc_line_hook(pce_t *p, int line);
int vdc_ovf_line(pce_t *p, int L);
int vdc_next_ovf(pce_t *p, int L);
void vdc_lines(pce_t *p, int L, int k);
int vdc_special_after(pce_t *p, int L);
int vdc_line_plain(pce_t *p, int L);
void vdc_rr_late(pce_t *p);
/* first line >= from (this frame) on which the raster counter equals RCR, or -1;
   the counter keeps running through the vertical blank, so a match can fall
   after the display or before the next frame's display starts */
static inline int vdc_rcr_line(pce_t *p, int from)
{
    const vdc_t *v = &p->vdc;
    int rcr = v->reg[6] & 0x3FF, r0 = v->disp_start - RCR_LEAD;   /* the line the counter is $40 on */
    if (from < r0) {
        int L = (rcr - v->raster0) & 0x3FF;
        if (L >= from && L < r0) return L;
        from = r0;
    }
    int L = r0 + rcr - 0x40;
    if (rcr >= 0x40 && L >= from && L < LINES_PER_FRAME) return L;
    return -1;
}

/* psg.c */
void psg_init(pce_t *p, int rate, int16_t *buf, int cap);
void psg_write(pce_t *p, int a, uint8_t v);
void psg_frame(pce_t *p);

static inline uint32_t be32(const uint8_t *b) { return (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]; }

#endif
