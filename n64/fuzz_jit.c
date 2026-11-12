/* N64 test: recompiler vs interpreter on random code and random state.
 * Both run the same cycle budget; because the recompiler stops at the same
 * check points as the interpreter, the resulting machines must be identical. */
#include <libdragon.h>
#include <malloc.h>
#include "pce.h"
#include <stddef.h>

static pce_t A __attribute__((aligned(16))), B __attribute__((aligned(16))), C0 __attribute__((aligned(16))), C __attribute__((aligned(16)));
static uint8_t *blob;
#define LSTRIDE (MAX_WIDTH + 32)
static uint8_t lbg[SCREEN_LINES * LSTRIDE], lspr[SCREEN_LINES * LSTRIDE];
static uint16_t sext[2 * SCREEN_LINES];
static uint32_t rs = 0x1234567;
static int getenv_trace = 0;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static void remap(pce_t *p) { pce_relocate(p); }
/* the background tile cache is derived data the fuzz never uses */
static void copy_machine(pce_t *d, const pce_t *s)
{
    size_t a = offsetof(pce_t, vdc.bgcache), b = a + sizeof s->vdc.bgcache;
    memcpy(d, s, a);
    memcpy((uint8_t *)d + b, (const uint8_t *)s + b, sizeof *s - b);
}

/* the rest of the machine: I/O state (everything after the RAM, minus the tile cache and the pointers) */
static int same_io(const pce_t *a, const pce_t *b)
{
    size_t s0 = offsetof(pce_t, iobuf), c0 = offsetof(pce_t, vdc.bgcache), c1 = c0 + sizeof a->vdc.bgcache;
    size_t e = offsetof(pce_t, psg.log);
    if (memcmp((const uint8_t *)a + s0, (const uint8_t *)b + s0, c0 - s0)) {
        for (size_t i = s0; i < c0; i++) if (((const uint8_t *)a)[i] != ((const uint8_t *)b)[i]) {
            size_t ov = offsetof(pce_t, vdc), vr = offsetof(pce_t, vdc.reg), bd = offsetof(pce_t, vdc.bgdirty);
            debugf("   io diff before bgcache at +%u: vram word %d, reg byte %d, bgdirty %d (%02x/%02x)\n", (unsigned)i,
                   (int)(i - ov) / 2, (int)(i - vr), (int)(i - bd), ((const uint8_t *)a)[i], ((const uint8_t *)b)[i]);
            break;
        }
        return 1;
    }
    if (memcmp((const uint8_t *)a + c1, (const uint8_t *)b + c1, e - c1)) {
        for (size_t i = c1; i < e; i++) if (((const uint8_t *)a)[i] != ((const uint8_t *)b)[i]) { debugf("   io diff at +%u (vdc %u vce %u psg %u)\n", (unsigned)i, (unsigned)offsetof(pce_t, vdc), (unsigned)offsetof(pce_t, vce), (unsigned)offsetof(pce_t, psg)); break; }
        return 1;
    }
    if (memcmp(a->psg.log, b->psg.log, a->psg.nlog * sizeof a->psg.log[0])) { debugf("   psg log differs\n"); return 1; }
    if (a->irq_pend != b->irq_pend || a->irq_lines != b->irq_lines || memcmp(a->pal_native, b->pal_native, sizeof a->pal_native)) { debugf("   irq/pal_native differ\n"); return 1; }
    return 0;
}

/* biased random code: mostly instructions the recompiler handles, branches with short offsets */
static void gen_code(uint8_t *pg, int n)
{
    static const uint8_t common[] = {
        0xA9, 0xA5, 0xB5, 0xAD, 0xBD, 0xB9, 0xA1, 0xB1, 0xB2, 0xA2, 0xA6, 0xAE, 0xA0, 0xA4, 0xAC,
        0x85, 0x95, 0x8D, 0x9D, 0x99, 0x81, 0x91, 0x92, 0x86, 0x84, 0x64, 0x9C,
        0x09, 0x05, 0x29, 0x25, 0x49, 0x45, 0x69, 0x65, 0x6D, 0xE9, 0xE5, 0xC9, 0xC5, 0xE0, 0xC0, 0x89, 0x24,
        0x0A, 0x4A, 0x2A, 0x6A, 0x06, 0x46, 0x26, 0x66, 0x0E, 0xE6, 0xC6, 0xEE, 0xFE, 0x1A, 0x3A,
        0xE8, 0xC8, 0xCA, 0x88, 0xAA, 0xA8, 0x8A, 0x98, 0x48, 0x68, 0xDA, 0xFA, 0x5A, 0x7A,
        0x18, 0x38, 0x58, 0x78, 0xB8, 0xD8, 0xF8, 0xF4, 0x22, 0x42, 0x02, 0x62, 0x82, 0xC2,
        0x10, 0x30, 0x50, 0x70, 0x90, 0xB0, 0xD0, 0xF0, 0x80, 0x0F, 0x8F, 0x07, 0x87, 0x04, 0x14, 0x83, 0x93,
        0x20, 0x60, 0x4C, 0x43, 0x08, 0x28, 0xEA, 0x54, 0xD4, 0x40, 0x00, 0x7C, 0x6C, 0x44 };
    for (int i = 0; i < n; i++) {
        pg[i] = (rnd() & 3) ? common[rnd() % sizeof common] : (uint8_t)rnd();
    }
    /* keep block transfers short: a 64 KB transfer into the VDC that keeps
       restarting VRAM DMA takes billions of steps */
    for (int i = 0; i + 6 < n; i++)
        if (pg[i] == 0x73 || pg[i] == 0xC3 || pg[i] == 0xD3 || pg[i] == 0xE3 || pg[i] == 0xF3) { pg[i + 5] |= 1; pg[i + 5] &= 0x1F; pg[i + 6] = 0; }
}

int main(void)
{
    debug_init_isviewer();
    { extern unsigned jit_hot; jit_hot = 0; } jit_init();
    /* 64 KB ROM: 8 banks of random code */
    uint32_t rom_size = 0x10000;
    blob = memalign(16, PCE64_HDR_SIZE + rom_size);
    memset(blob, 0, PCE64_HDR_SIZE);
    memcpy(blob, "PCE64GM1", 8);
    blob[12] = 0; blob[13] = 1; blob[14] = 0; blob[15] = 0;
    pce_load(&A, blob, PCE64_HDR_SIZE + rom_size);
    pce_reset(&A);
    /* deferred rendering state, so the recompiler's VRAM write path is used */
    static uint16_t ctab[512];
    for (int i = 0; i < 512; i++) ctab[i] = i * 37;
    A.deferred = 1;
    A.rl = calloc(1, sizeof(rlist_t));
    A.plane = calloc(1, PLANE_MAX_W * PLANE_MAX_H);
    A.sprcache = calloc(1, SPRCACHE_PAIRS * 256);
    long bad = 0, n = 0, steps_cyc = 0;
    for (int t = 0; t < 12000 && bad < 8; t++) {
        uint8_t *rom = blob + PCE64_HDR_SIZE;
        if ((t & 63) == 0) {
            gen_code(rom, rom_size);
            /* vectors point into bank 0 code */
            rom[0x1FF6] = 0x00; rom[0x1FF7] = 0xE1;
            rom[0x1FF8] = 0x40; rom[0x1FF9] = 0xE1;
            rom[0x1FFA] = 0x80; rom[0x1FFB] = 0xE1;
            data_cache_hit_writeback(rom, rom_size);
            jit_flush();
        }
        for (int i = 0; i < 0x2000; i++) A.ram[i] = rnd();
        for (int i = 0; i + 6 < 0x2000; i++)
            if (A.ram[i] == 0x73 || A.ram[i] == 0xC3 || A.ram[i] == 0xD3 || A.ram[i] == 0xE3 || A.ram[i] == 0xF3) { A.ram[i + 5] = (A.ram[i + 5] & 0x1F) | 1; A.ram[i + 6] = 0; }
        A.A = rnd(); A.X = rnd(); A.Y = rnd(); A.S = rnd(); A.P = rnd() & 0xCF;
        A.speed = 1;
        /* slot 7 = bank 0 (vectors), slot 1 RAM, others ROM banks 1..6 or RAM */
        A.mpr[0] = 0xF8; A.mpr[1] = 0xF8; A.mpr[7] = 0;
        for (int i = 2; i < 7; i++) A.mpr[i] = (rnd() & 1) ? (1 + rnd() % 7) : 0xF8;
        /* the I/O page in slot 0 (and sometimes a second slot) */
        int io = (t % 3) == 1;
        if (io) { A.mpr[0] = 0xFF; if (rnd() & 1) A.mpr[2 + rnd() % 5] = 0xFF; }
        remap(&A);
        A.PC = 0x4000 + (rnd() & 0x9FFF);      /* slots 2..6 */
        if (A.mpr[A.PC >> 13] >= 0x80) { A.mpr[A.PC >> 13] = 3; remap(&A); }
        if ((t & 3) == 0) {
            /* run from RAM: random code there, sometimes a copy of ROM code */
            A.PC = 0x2000 + (rnd() & 0x1FF0);
            if (rnd() & 1) memcpy(&A.ram[A.PC & 0x1FFF], rom + (rnd() & 0xE000) + (A.PC & 0x1FFF), 0x2000 - (A.PC & 0x1FFF));
        }
        if ((t % 5) == 2) {
            /* VDC traffic from RAM: VRAM address and data writes (the
               recompiler's VRAM write routine and its hazard tests) */
            A.mpr[0] = 0xFF; remap(&A);
            A.PC = 0x2000 + 0x400 + (rnd() & 0x7F0);
            uint8_t *q = &A.ram[A.PC & 0x1FFF];
            int n = 0;
            for (int k = 0; k < 24 && n < 0x300; k++) {
                unsigned a = (rnd() & 1) ? (rnd() & 0x7FF) : (rnd() & 0x7FFF);   /* often the BAT */
                q[n++] = 0x03; q[n++] = 0x00; q[n++] = 0x13; q[n++] = a & 0xFF; q[n++] = 0x23; q[n++] = a >> 8;
                static const uint8_t sels[8] = { 2, 2, 2, 2, 6, 7, 8, 6 };
                q[n++] = 0x03; q[n++] = (rnd() & 3) == 0 ? sels[rnd() & 7] : 0x02;   /* VWR, sometimes RCR / BXR / BYR */
                int w = 1 + rnd() % 4;
                for (int i = 0; i < w; i++) {
                    q[n++] = 0x13; q[n++] = rnd(); q[n++] = 0x23; q[n++] = (rnd() & 1) ? (rnd() & 7) : rnd();
                    if ((rnd() & 3) == 0) { q[n++] = 0xAD; q[n++] = 0x00; q[n++] = (rnd() & 1) ? 0x00 : 0x20; }   /* LDA $0000 / $2000: VDC status */
                }
            }
            for (int i = 0; i < 512; i++) {
                A.vdc.cell_used[i] = (rnd() & 3) == 0; A.vdc.sc_frozen[i] = (rnd() & 3) == 0;
                A.vdc.cell_line[i] = rnd() % 240; A.vdc.cell_last[i] = A.vdc.cell_line[i] + rnd() % 64;
            }
            for (int i = 0; i < 2048; i++) A.vdc.tref[i] = (rnd() & 3) ? 0 : 1 + (rnd() & 3);
            A.vdc.spr_first = rnd() % 200; A.vdc.spr_last = A.vdc.spr_first + rnd() % 40;
            /* a few saved patterns (snapshots), or a full pool */
            A.vdc.snap_n = (rnd() & 7) == 0 ? 64 : rnd() % 4;
            for (int i = 0; i < A.vdc.snap_n; i++) { A.vdc.snap_cell[i] = rnd() & 0x1FF; for (int k = 0; k < 64; k++) A.vdc.snap_data[i][k] = rnd(); }
            A.vdc.bat_words = 1024 << (rnd() & 2);
        }
        A.irq_mask = 0; A.irq_pend = (rnd() & 7) == 0 ? 2 : 0; pce_update_irq(&A);
        A.idle_skip = rnd() & 1; A.idle_last = 0xFFFFFFFF; A.idle_yes = 0xFFFFFFFF;
        A.cycles_extra = 0;
        A.psg.nlog = 0;
        A.next_due = 0x7FFFFFFF;            /* no display catch-up (no frame running) */
        A.vdc.bg_pend = (rnd() & 3) == 0; A.vdc.spr_pend = (rnd() & 3) == 0;
        A.colortab = (rnd() & 7) ? ctab : NULL;
        A.vdc.reg[5] &= 0x07FF;          /* VRAM increment stays sane */
        unsigned spc = A.PC; uint8_t *cpg = A.rmap[spc >> 13] + (spc & 0x1FFF);
        copy_machine(&B, &A);
        remap(&B);
        int budget = 1 + rnd() % 300;
        if (getenv_trace && t >= getenv_trace) debugf("t=%d pc=%04x budget=%d\n", t, A.PC, budget);
#ifdef JIT_TRACE
        { extern int jit_trace; jit_trace = (t == JIT_TRACE); }
#endif
        if ((t % 500) == 0) { extern unsigned jit_stat_tr, jit_stat_flush, jit_stat_inval; debugf("FUZZ progress %d tr=%u flush=%u inval=%u ticks=%lu\n", t, jit_stat_tr, jit_stat_flush, jit_stat_inval, (unsigned long)get_ticks()); }
        /* run as the frame loop does, so interrupts raised by I/O cut the budget */
        A.chunk_end = B.chunk_end = budget; A.cyc_left = B.cyc_left = budget;
        A.cut_delta = B.cut_delta = 0; A.in_cpu = B.in_cpu = 1;
        A.chunk_start = B.chunk_start = 0; A.jit_noevt = B.jit_noevt = 1;
        A.ev_exact = B.ev_exact = rnd() & 1;     /* budget ends at a VDC event (exact) or the timer */
        copy_machine(&C0, &A);
        int ua = cpu_run(&A, budget);
        int ub = jit_run(&B, budget);
        n++;
        steps_cyc += ua;
        if (A.A != B.A || A.X != B.X || A.Y != B.Y || A.S != B.S || A.P != B.P || A.PC != B.PC ||
            memcmp(A.ram, B.ram, 0x2000) || memcmp(A.mpr, B.mpr, 8) || ua != ub || A.speed != B.speed ||
            same_io(&A, &B)) {
            int d = -1;
            for (int i = 0; i < 0x2000; i++) if (A.ram[i] != B.ram[i]) { d = i; break; }
            debugf("FUZZ MISMATCH t=%d budget=%d: interp A=%02x X=%02x Y=%02x S=%02x P=%02x PC=%04x cyc=%d | jit A=%02x X=%02x Y=%02x S=%02x P=%02x PC=%04x cyc=%d ramdiff@%x(%02x/%02x)\n",
                   t, budget, A.A, A.X, A.Y, A.S, A.P, A.PC, ua, B.A, B.X, B.Y, B.S, B.P, B.PC, ub,
                   d, d >= 0 ? A.ram[d] : 0, d >= 0 ? B.ram[d] : 0);
            debugf("   start PC=%04x code: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n", spc,
                   cpg[0], cpg[1], cpg[2], cpg[3], cpg[4], cpg[5], cpg[6], cpg[7], cpg[8], cpg[9], cpg[10], cpg[11]);
            /* replay one instruction at a time */
            copy_machine(&C, &C0); remap(&C);
            char buf[600]; int o = 0, tot = 0;
            for (int k = 0; k < 60 && tot < ua; k++) {
                unsigned pc = C.PC; uint8_t opb = C.rmap[pc >> 13] ? C.rmap[pc >> 13][pc & 0x1FFF] : 0;
                int chk; int u = cpu_step(&C, &chk); tot += u;
                o += snprintf(buf + o, sizeof buf - o, " %02x:%d%s", opb, u, chk ? "!" : "");
                if (o > 560) break;
            }
            debugf("   steps:%s\n", buf);
#ifdef JIT_WATCH
            { extern void jit_dump(pce_t *, uint32_t); jit_dump(&B, 0xdff8); jit_dump(&B, 0xdffa); jit_dump(&B, 0xdfe4); }
#endif
            bad++;
        }
    }
    debugf("FUZZ DONE %ld runs, %ld mismatches, %ld cycles\n", n, bad, steps_cyc);
    for (;;) {}
}
