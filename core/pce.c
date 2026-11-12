/* PCE64 system: memory map, I/O page, timer, interrupts, joypad, frame loop. */
#include "pce.h"
#ifdef PCE_VTRACE
#include <debug.h>
#endif
#ifdef PCE_PROF
uint64_t (*pce_ticks)(void);
uint64_t prof_cpu, prof_render, prof_psg, prof_cb;
#define PT() pce_ticks()
#else
#define PT() 0
#endif

static void build_rompages_from(pce_t *p, int x0)
{
    uint32_t banks = p->rom_size >> 13;
    for (int x = x0; x < 128; x++) {
        uint32_t b;
        if (p->mapper == MAPPER_SF2) {
            if (x < 0x40) b = x;
            else b = 0x40 + (uint32_t)p->sf2bank * 0x40 + (x - 0x40);
            if (b >= banks) b %= banks;
        } else if (p->rom_size == 0x60000) {
            b = (x < 0x40) ? (x & 0x1F) : ((x & 0x0F) + 32);
        } else if (p->rom_size == 0xC0000) {
            /* 768K (Darius Plus): the first 512K, then the last 256K twice */
            b = (x < 0x40) ? x : ((x & 0x1F) + 0x40);
        } else if (p->rom_size == 0x30000) {
            /* 192K: same split idea as 384K */
            b = (x < 0x40) ? (x & 0x0F) : ((x & 0x07) + 16);
        } else {
            b = x % banks;
        }
        p->rompage[x] = p->rom + (b << 13);
    }
}

static void build_rompages(pce_t *p) { build_rompages_from(p, 0); }

static void build_banks_range(pce_t *p, int b0, int b1)
{
    for (int bank = b0; bank < b1; bank++) {
        uint8_t *r, *w = NULL;
        uint32_t k = 0;
        if (bank >= 0x40 && bank < 0x44 && p->cart_ram_on) {
            /* card RAM: plain memory (code there is interpreted) */
            r = w = p->cart_ram + ((bank - 0x40) << 13);
        } else if (bank < 0x80) {
            r = p->rompage[bank];
            /* recompiler block key: ROM banks (and the SF2 bank for its window) */
            k = ((uint32_t)bank << 16) | 0x01000000u;
            if (p->mapper == MAPPER_SF2 && bank >= 0x40) k |= (uint32_t)(p->sf2bank + 1) << 25;
        } else if (bank >= 0xF8 && bank <= 0xFB) {
            /* RAM (all four banks mirror it): translated, checked on block entry */
            r = w = p->ram;
            k = (0xF8u << 16) | 0x01000000u;
        } else if (bank == 0xFF) r = NULL;     /* I/O */
        else r = p->ffpage;
        p->bank_rmap[bank] = r; p->bank_wmap[bank] = w; p->bank_jkey[bank] = k;
    }
}

static void build_banks(pce_t *p) { build_banks_range(p, 0, 256); }

/* after copying a machine: point its tables at its own memory */
void pce_relocate(pce_t *p)
{
    p->vec_ok = 0;
    build_banks(p);
    for (int i = 0; i < 8; i++) pce_map(p, i);
}

void pce_map(pce_t *p, int slot)
{
    uint8_t bank = p->mpr[slot];
    p->rmap[slot] = p->bank_rmap[bank];
    p->wmap[slot] = p->bank_wmap[bank];
    p->jkey[slot] = p->bank_jkey[bank];      /* 0: interpret */
    if (slot == 7) p->vec_ok = 0;
}

int pce_load(pce_t *p, const uint8_t *blob, uint32_t blob_size)
{
    if (blob_size < PCE64_HDR_SIZE || memcmp(blob, PCE64_MAGIC, 8)) return -1;
    uint32_t size = be32(blob + 12);
    if (size + PCE64_HDR_SIZE > blob_size || size < 0x2000) return -2;
    p->rom = (uint8_t *)blob + PCE64_HDR_SIZE;
    p->rom_size = size;
    p->flags = be32(blob + 16);
    p->mapper = be32(blob + 20);
    /* Populous carries RAM on the card (recognised as other emulators do) */
    p->cart_ram_on = size > 0x1F2E && !memcmp(p->rom + 0x1F26, "POPULOUS", 8);
    return 0;
}

void pce_reset(pce_t *p)
{
    memset(p->ram, 0, sizeof(p->ram));
    memset(p->cart_ram, 0, sizeof(p->cart_ram));
    memset(p->ffpage, 0xFF, sizeof(p->ffpage));
    p->sf2bank = 0;
    build_rompages(p);
    build_banks(p);
    p->irq_mask = 0; p->irq_pend = 0; p->irq_lines = 0;
    p->tm_reload = p->tm_count = p->tm_on = 0; p->tm_cycles = 0;
    p->pad_sel = 1; p->pad_clr = 0; p->pad_idx = 0;
    p->iobuf = 0;
    p->line = 0;
    p->frame = 0;
    p->cycles_extra = 0;
    p->bx_n = 0;
    p->run_debt = 0;
    p->tm_pos = 0; p->in_cpu = 0;
    p->idle_skip = 1;
#ifdef RENDER_AT
    p->render_at = RENDER_AT;
#else
    if (!p->render_at) p->render_at = 80;
#endif
    vdc_reset(p);
    cpu_reset(p);
}

/* CLI with an interrupt pending: end the run just after the next instruction
   (one CPU cycle from now: whatever instruction follows uses it up) */
PCE_HOT1 void pce_irq_soon(pce_t *p)
{
    if (!p->in_cpu) return;
    int t = pce_now(p) + (p->speed ? 1 : 4);
    int d = p->chunk_end - t;
    if (d > 0) { p->chunk_end = t; p->cyc_left -= d; p->cut_delta += d; p->ev_exact = 1; }
}

PCE_HOT1 void pce_update_irq(pce_t *p)
{
    p->irq_lines = p->irq_pend & ~p->irq_mask & 7;
    /* a pending interrupt ends the current run (the next run takes it on entry) */
    if (p->irq_lines && p->in_cpu) {
        /* the CPU samples its interrupt lines before an instruction's last
           cycle: a change made by the instruction itself (unmasking, CLI)
           is seen once the next instruction has run (one CPU cycle on) */
        int t = pce_now(p) + (p->speed ? 1 : 4);
        int d = p->chunk_end - t;
        if (d > 0) { p->chunk_end = t; p->cyc_left -= d; p->cut_delta += d; p->ev_exact = 1; }
    }
}

/* ---- I/O page (physical bank $FF) ---- */

static uint8_t pad_read(pce_t *p)
{
    uint8_t v = 0x30 | 0x80; /* bit7 = 1: no CD-ROM; bits 4-5 unused (1) */
    if (!(p->flags & GF_REGION_US)) v |= 0x40; /* bit6: 1 = Japan PC Engine, 0 = TurboGrafx */
    /* the pad's 74HC157 drives all four lines low while CLR is high */
    if (p->pad_clr) return v;
    uint8_t b;
    if (p->flags & GF_NO_MULTITAP) b = p->pad[0];
    else if (p->pad_idx < 5) b = p->pad[p->pad_idx];
    else return v | 0x0F;                                     /* tap ports 6-8: nothing */
    uint8_t n = p->pad_sel ? (b >> 4) & 0x0F                  /* U R D L */
                           : b & 0x0F;                        /* I II SEL RUN */
    return v | (~n & 0x0F);
}

static inline void timer_sync(pce_t *p, int pos);
static inline void io_sync(pce_t *p)
{
#ifdef PCE_VERIFY
    if (p->vlog && p->in_cpu && p->rn < 64) { p->rlog[p->rn][0] = pce_now(p); p->rlog[p->rn][1] = p->dbg_io; p->rlog[p->rn][2] = p->dbg_pc; p->rn++; }
#endif
    if (p->in_cpu) { int now = pce_now(p); if (now >= p->next_due) pce_catch_up(p, now); }
}
static inline void io_sync_timer(pce_t *p)
{
    if (p->in_cpu) timer_sync(p, pce_now(p));
}

#ifdef PCE_IOSTAT
unsigned iostat_w[64], iostat_r[8];
#endif
PCE_HOT uint8_t pce_io_read(pce_t *p, uint32_t phys)
{
    uint16_t a = phys & 0x1FFF;
#ifdef PCE_IOSTAT
    iostat_r[(a >> 10) & 7]++;
#endif
#ifdef PCE_VERIFY
    p->dbg_io = a;
#endif
    if (a < 0x800) io_sync(p);
    else if ((a >> 10) == 3) io_sync_timer(p);
    switch (a >> 10) {
    case 0: pce_penalty(p, 1); return vdc_read(p, a);
    case 1: pce_penalty(p, 1); return vce_read(p, a);
    case 2: return p->iobuf;                                   /* PSG is write-only */
    case 3: p->iobuf = (p->iobuf & 0x80) | (p->tm_count & 0x7F); return p->iobuf;
    case 4: p->iobuf = pad_read(p); return p->iobuf;
    case 5:
        if ((a & 3) == 2) { p->iobuf = (p->iobuf & 0xF8) | (p->irq_mask & 7); return p->iobuf; }
        if ((a & 3) == 3) { p->iobuf = (p->iobuf & 0xF8) | (p->irq_pend & 7); return p->iobuf; }
        return p->iobuf;
    default: return 0xFF;
    }
}

PCE_HOT void pce_io_write(pce_t *p, uint32_t phys, uint8_t v)
{
    uint16_t a = phys & 0x1FFF;
#ifdef PCE_IOSTAT
    if (a < 0x400) iostat_w[((a & 3) << 4) | (p->vdc.sel & 15)]++; else iostat_w[48 + ((a >> 10) & 7)]++;
    { int now = p->in_cpu ? pce_now(p) : 0; if (now >= p->next_due) iostat_w[63]++; }
#endif
#ifdef PCE_VERIFY
    p->dbg_io = a;
#endif
#ifdef IO_COUNT
    { extern long ioc_w[8], ioc_a[0x2000]; ioc_w[(a >> 10) & 7]++; ioc_a[a]++; }
#endif
    if (a < 0x800) io_sync(p);
    else if ((a >> 10) == 3) io_sync_timer(p);
    switch (a >> 10) {
    case 0: pce_penalty(p, 1); vdc_write(p, a, v); break;
    case 1: pce_penalty(p, 1); vce_write(p, a, v); break;
    case 2: p->iobuf = v; psg_write(p, a & 0x0F, v); break;
    case 3:
        p->iobuf = v;
        if (a & 1) {
            if ((v & 1) && !p->tm_on) { p->tm_count = p->tm_reload; p->tm_cycles = 0; }
            p->tm_on = v & 1;
            pce_reschedule(p);
        } else p->tm_reload = v & 0x7F;
        break;
    case 4:
        p->iobuf = v;
        /* multitap: SEL rising moves to the next port, CLR high selects port 1 */
        if ((v & 1) && !p->pad_sel) p->pad_idx = (p->pad_idx + 1) & 7;
        if (v & 2) p->pad_idx = 0;
        p->pad_sel = v & 1;
        p->pad_clr = (v >> 1) & 1;
        break;
    case 5:
        p->iobuf = v;
        if ((a & 3) == 2) { p->irq_mask = v & 7; pce_update_irq(p); }
        else if ((a & 3) == 3) { p->irq_pend &= ~4; pce_update_irq(p); }
        break;
    default: break;
    }
}

PCE_HOT uint8_t pce_slow_read(pce_t *p, uint16_t addr)
{
    uint8_t bank = p->mpr[addr >> 13];
    if (bank == 0xFF) return pce_io_read(p, 0x1FE000 | (addr & 0x1FFF));
    return 0xFF;
}

PCE_HOT void pce_slow_write(pce_t *p, uint16_t addr, uint8_t v)
{
    uint8_t bank = p->mpr[addr >> 13];
    if (bank == 0xFF) { pce_io_write(p, 0x1FE000 | (addr & 0x1FFF), v); return; }
    if (bank < 0x80 && p->mapper == MAPPER_SF2 && (addr & 0x1FFC) == 0x1FF0) {
        int nb = addr & 3;
        if (nb != p->sf2bank) {
            p->sf2bank = nb;
            /* only the window at banks $40-$7F moves */
            build_rompages_from(p, 0x40);
            build_banks_range(p, 0x40, 0x80);
            for (int i = 0; i < 8; i++) if ((p->mpr[i] & 0xC0) == 0x40) pce_map(p, i);
        }
    }
}

/* ---- timer (advanced lazily to a frame position) ---- */
static inline void timer_advance(pce_t *p, int cyc)
{
    if (!p->tm_on) return;
    p->tm_cycles += cyc;
    if (p->tm_cycles < 1024) return;
    int n = p->tm_cycles >> 10;
    p->tm_cycles &= 1023;
    /* n ticks: count down, reloading (and interrupting) after zero */
    if (n <= p->tm_count) { p->tm_count -= n; return; }
    n -= p->tm_count + 1;
    unsigned period = p->tm_reload + 1u;
    p->tm_count = p->tm_reload - (n % period);
    p->irq_pend |= 4;
    pce_update_irq(p);
}
static inline void timer_sync(pce_t *p, int pos)
{
    if (pos > p->tm_pos) { timer_advance(p, pos - p->tm_pos); p->tm_pos = pos; }
}

#define FRAME_CYCLES (LINES_PER_FRAME * CYCLES_PER_LINE)

/* Line starts (raster counter, interrupts, vblank) and render latches up to
 * frame position pos, in time order. */
static void catch_up_(pce_t *p, int pos);
PCE_HOT1 void pce_catch_up(pce_t *p, int pos)
{
    TP_BEGIN(3);
    catch_up_(p, pos);
    TP_END(3, 3);
}
static inline __attribute__((always_inline)) void catch_up_(pce_t *p, int pos)
{
    for (;;) {
        int rl = p->vdc.rr_late;
        if (rl >= 0 && rl <= pos) {
            int tn = p->next_ls <= p->next_lt ? p->next_ls * CYCLES_PER_LINE : p->next_lt * CYCLES_PER_LINE + p->render_at;
            if (rl < tn) { vdc_rr_late(p); continue; }
        }
        if (p->next_ls <= p->next_lt) {
            if (p->next_ls >= LINES_PER_FRAME) { p->next_due = 0x7FFFFFFF; break; }
            if (p->next_ls * CYCLES_PER_LINE > pos) { p->next_due = p->next_ls * CYCLES_PER_LINE; break; }
            if (p->next_ls == p->next_lt && pos >= (p->next_ls + 1) * CYCLES_PER_LINE + p->render_at) {
                /* two or more whole lines with nothing special: in one go */
                int L = p->next_ls;
                int k = (pos - p->render_at) / CYCLES_PER_LINE - L + 1;
                if (vdc_line_plain(p, L)) {
                    int lim = vdc_special_after(p, L);
                    if (k > lim - L) k = lim - L;
                    if (k >= 2) {
                        vdc_lines(p, L, k);
                        p->line = L + k - 1;
                        p->next_ls += k; p->next_lt += k;
                        continue;
                    }
                }
            }
            p->line = p->next_ls;
            vdc_line_start(p, p->next_ls);
            p->next_ls++;
        } else {
            int t = p->next_lt * CYCLES_PER_LINE + p->render_at;
            if (t > pos) { p->next_due = t; break; }
            vdc_line_hook(p, p->next_lt);
            if (!p->skip_render || p->deferred) {
                EMUX_BEGIN(PS_LINE);
                vdc_line(p, p->next_lt);
                EMUX_END(PS_LINE);
            }
            p->next_lt++;
        }
    }
    if (p->vdc.rr_late >= 0 && p->vdc.rr_late < p->next_due) p->next_due = p->vdc.rr_late;
}

/* Next point where the CPU must stop: a line start that raises an interrupt
 * (raster match, vblank), a timer underflow, or the end of the frame. */
static PCE_HOT1 int next_event(pce_t *p)
{
    int ev = FRAME_CYCLES;
    vdc_t *v = &p->vdc;
    int de = v->disp_end;
    if (de >= p->next_ls && de * CYCLES_PER_LINE < ev) ev = de * CYCLES_PER_LINE;
    if (v->reg[5] & 0x04) {
        int rl = vdc_rcr_line(p, p->next_ls);
        if (rl >= 0 && rl * CYCLES_PER_LINE < ev) ev = rl * CYCLES_PER_LINE;
    }
    /* the vblank interrupt, a little into the first vblank line */
    if (v->rr_late >= 0 && (v->reg[5] & 0x08) && v->rr_late < ev) ev = v->rr_late;
    /* the end of the SATB DMA, when its interrupt is enabled */
    if (v->ds_line >= p->next_ls && (v->reg[15] & 0x01) && v->ds_line * CYCLES_PER_LINE < ev) ev = v->ds_line * CYCLES_PER_LINE;
    /* sprite collision interrupt: every display line's latch is an event
       (the collision is only known once the line's sprites are drawn) */
    if ((v->reg[5] & 0x41) == 0x41) {
        int L = p->next_lt;
        if (L < v->disp_start) L = v->disp_start;
        if (L < de) { int t = L * CYCLES_PER_LINE + p->render_at; if (t < ev) ev = t; }
    }
    /* sprite overflow interrupt: at the latch of the next line with too many sprites */
    if ((v->reg[5] & 0x42) == 0x42) {
        int L = p->next_lt;
        int o = vdc_next_ovf(p, L);
        if (o >= 0) { int t = o * CYCLES_PER_LINE + p->render_at; if (t < ev) ev = t; }
    }
    /* the VDC only needs bringing up to date at its own events (or when the
       CPU touches it); sprite collision interrupts are raised as lines are
       processed (each line is an event then) */
    p->vdc_ev = ((v->reg[5] & 0x41) == 0x41) ? 0 : ev;
    p->ev_other = ev;
    p->ev_other_vdc = ev < FRAME_CYCLES;
    if (p->tm_on) {
        int t = p->tm_pos + (1024 - p->tm_cycles) + (int)p->tm_count * 1024;
        if (t < ev) ev = t;
    }
    /* VDC events (raster splits, vblank) need the exact instruction; the
       timer alone does not, unless a VDC event follows close behind */
    p->ev_pick_exact = p->ev_other_vdc && p->ev_other - ev < EXACT_NEAR;
    return ev;
}

/* Called by I/O that can move an event earlier: shorten the running budget. */
PCE_HOT1 void pce_reschedule(pce_t *p)
{
    if (!p->in_cpu) return;
    int ev = next_event(p);
    int now = pce_now(p);

    if (ev <= now) ev = now + 1;
    if (ev < p->chunk_end) {
        int d = p->chunk_end - ev;
        p->chunk_end = ev;
        p->cyc_left -= d;
        p->cut_delta += d;
        p->ev_exact = p->ev_pick_exact;
    }
}

PCE_WARM void pce_run_frame(pce_t *p)
{
    EMUX_BEGIN(PS_FRAME);
    vdc_frame_start(p);
    p->next_ls = 0; p->next_lt = 0; p->next_due = 0;
    int fc = p->run_debt;
    p->vdc_ev = 0;
    for (;;) {
        EMUX_BEGIN(PS_BG);
        if (fc >= p->vdc_ev) pce_catch_up(p, fc);
        timer_sync(p, fc);
        EMUX_END(PS_BG);
        if (fc >= FRAME_CYCLES) { if (fc < p->vdc_ev) pce_catch_up(p, fc); break; }
        int ev = next_event(p);
        if (ev <= fc) ev = fc + 1;
#ifdef SCROLL_LOG
        if (p->frame == SCROLL_LOG && fc > 64 * CYCLES_PER_LINE && fc < 224 * CYCLES_PER_LINE)
            fprintf(stderr, "RUN fc %d (%d+%d) ev %d (%d+%d) rcr %03x cr %04x next_ls %d raster %03x\n", fc, fc / CYCLES_PER_LINE, fc % CYCLES_PER_LINE, ev, ev / CYCLES_PER_LINE, ev % CYCLES_PER_LINE, p->vdc.reg[6], p->vdc.reg[5], p->next_ls, p->vdc.raster);
#endif
        p->chunk_end = ev;
        p->cyc_left = ev - fc;
        p->cut_delta = 0;
        p->ev_exact = p->ev_pick_exact;
        p->chunk_start = fc;
        p->in_cpu = 1;
        EMUX_BEGIN(PS_CPU);
#ifdef PCE_VTRACE
        p->vtrace = p->vlog && p->vtrace_run1 && p->vn == p->vtrace_run1 - 1;
        if (p->vtrace) debugf("RUN %d fc %d ev %d pc %04x irq %x P %02x\n", p->vn, fc, ev, p->PC, p->irq_lines, p->P);
#endif
#ifdef PCE_JIT
        int used = p->force_interp ? cpu_run(p, ev - fc) : jit_run(p, ev - fc);
#else
        int used = cpu_run(p, ev - fc);
#endif
#ifdef PCE_VERIFY
        if (p->vlog) { int i = p->vn++; if (i < 4096) { p->vlog[i].fc = fc; p->vlog[i].end = p->chunk_end; p->vlog[i].used = used;
            p->vlog[i].pc = p->PC; p->vlog[i].a = p->A | p->X << 8 | p->Y << 16 | p->S << 24; p->vlog[i].ram = pce_hash(p); } }
#endif
        EMUX_END(PS_CPU);
        p->in_cpu = 0;
        fc += used - p->cut_delta;      /* a cut shortened the budget, not the time */
    }
    p->run_debt = fc - FRAME_CYCLES;
    p->tm_pos -= FRAME_CYCLES;
    if (!p->skip_render || p->deferred) vdc_frame_end(p);
    EMUX_BEGIN(PS_PSG);
    TP_BEGIN(1);
    psg_frame(p);
    TP_END(1, 1);
    EMUX_END(PS_PSG);
    p->frame++;
    EMUX_END(PS_FRAME);
}

uint32_t pce_hash(pce_t *p)
{
    /* FNV-1a over RAM, VRAM, palette and CPU registers */
    uint32_t h = 2166136261u;
#define HB(b) do { h ^= (uint8_t)(b); h *= 16777619u; } while (0)
    for (int i = 0; i < 0x2000; i++) HB(p->ram[i]);
    for (int i = 0; i < 0x8000; i++) { HB(p->vdc.vram[i]); HB(p->vdc.vram[i] >> 8); }
    for (int i = 0; i < 512; i++) { HB(p->vce.pal[i]); HB(p->vce.pal[i] >> 8); }
    HB(p->A); HB(p->X); HB(p->Y); HB(p->S); HB(p->P); HB(p->PC); HB(p->PC >> 8);
    for (int i = 0; i < 8; i++) HB(p->mpr[i]);
#ifdef PCE_VERIFY
    HB(p->tm_count); HB(p->tm_cycles); HB(p->tm_cycles >> 8); HB(p->tm_on); HB(p->irq_pend); HB(p->irq_mask);
    HB(p->speed); HB(p->run_debt); HB(p->run_debt >> 8); HB(p->cycles_extra); HB(p->bx_n); HB(p->bx_n >> 8); HB(p->bx_src); HB(p->bx_dst);
    for (int i = 0; i < 20; i++) { HB(p->vdc.reg[i]); HB(p->vdc.reg[i] >> 8); }
    HB(p->vdc.status); HB(p->vdc.sel); HB(p->vdc.satb_pending);
#endif
#undef HB
    return h;
}
