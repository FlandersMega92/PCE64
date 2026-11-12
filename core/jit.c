/* HuC6280 -> MIPS (VR4300) recompiler.
 *
 * Straight-line runs of 6280 code in ROM are translated into MIPS code and
 * cached by (logical PC, ROM bank). Interrupts and the cycle budget are
 * checked at exactly the points where the interpreter checks them (taken
 * branches, jumps, calls, returns, CLI, ...), so for the same input the
 * recompiled CPU produces the same machine state as the interpreter; the
 * N64 test builds compare per-frame state hashes to verify that.
 *
 * Register use while 6280 code runs (all callee-saved, so C helpers keep them):
 *   s0 A   s1 X   s2 Y   s3 S   s4 NZ   s5 C (0/1)   s6 V (0/nonzero)
 *   s7 remaining cycles   fp pointer to pce_t   v0 next PC between blocks
 * NZ: N = (NZ & 0x8080) != 0, Z = (NZ & 0xFF) == 0.
 */
#if defined(__mips__) && defined(PCE_JIT)
#include "pce.h"
#if defined(PCE_VTRACE) && defined(__mips__)
#include <debug.h>
#endif
#include "cpu_tab.h"
#include <stddef.h>
#ifdef JIT_TRACE
#include <libdragon.h>
int jit_trace;
#define JT(...) do { if (jit_trace) debugf(__VA_ARGS__); } while (0)
#else
#define JT(...) do { } while (0)
#endif
void data_cache_hit_writeback(volatile const void *addr, unsigned long length);
void inst_cache_hit_invalidate(volatile void *addr, unsigned long length);

/* ---- MIPS encoding ---- */
enum { ZERO = 0, AT = 1, V0 = 2, V1 = 3, A0 = 4, A1 = 5, A2 = 6, A3 = 7,
       T0 = 8, T1 = 9, T2 = 10, T3 = 11, T4 = 12, T5 = 13, T6 = 14, T7 = 15,
       S0 = 16, S1, S2, S3, S4, S5, S6, S7, T8 = 24, T9 = 25, SP = 29, FP = 30, RA = 31 };
#define RA_ S0
#define RX_ S1
#define RY_ S2
#define RS_ S3
#define RNZ S4
#define RC_ S5
#define RV_ S6
#define RCY S7

#define R(rs, rt, rd, sa, fn) ((uint32_t)(((rs) << 21) | ((rt) << 16) | ((rd) << 11) | ((sa) << 6) | (fn)))
#define I(op, rs, rt, im) ((uint32_t)(((op) << 26) | ((rs) << 21) | ((rt) << 16) | ((im) & 0xFFFF)))

static uint32_t *cp;   /* emit pointer */
static inline void E(uint32_t w) { *cp++ = w; }
#define ADDU(d, s, t)  E(R(s, t, d, 0, 0x21))
#define SUBU(d, s, t)  E(R(s, t, d, 0, 0x23))
#define AND(d, s, t)   E(R(s, t, d, 0, 0x24))
#define OR(d, s, t)    E(R(s, t, d, 0, 0x25))
#define XOR(d, s, t)   E(R(s, t, d, 0, 0x26))
#define NOR(d, s, t)   E(R(s, t, d, 0, 0x27))
#define SLTU(d, s, t)  E(R(s, t, d, 0, 0x2B))
#define SLT(d, s, t)   E(R(s, t, d, 0, 0x2A))
#define SLLV(d, t, s)  E(R(s, t, d, 0, 0x04))
#define MULTU(s, t)    E(R(s, t, 0, 0, 0x19))
#define MFHI(d)        E(R(0, 0, d, 0, 0x10))
#define SLL(d, t, sa)  E(R(0, t, d, sa, 0x00))
#define SRL(d, t, sa)  E(R(0, t, d, sa, 0x02))
#define JR(s)          E(R(s, 0, 0, 0, 0x08))
#define JALR(s)        E(R(s, 0, RA, 0, 0x09))
#define MOVE(d, s)     ADDU(d, s, ZERO)
#define NOP()          E(0)
#define ADDIU(t, s, im) E(I(0x09, s, t, im))
#define SLTIU(t, s, im) E(I(0x0B, s, t, im))
#define ANDI(t, s, im) E(I(0x0C, s, t, im))
#define ORI(t, s, im)  E(I(0x0D, s, t, im))
#define XORI(t, s, im) E(I(0x0E, s, t, im))
#define LUI(t, im)     E(I(0x0F, 0, t, im))
#define LBU(t, off, s) E(I(0x24, s, t, off))
#define LHU(t, off, s) E(I(0x25, s, t, off))
#define LW(t, off, s)  E(I(0x23, s, t, off))
#define SB(t, off, s)  E(I(0x28, s, t, off))
#define SH(t, off, s)  E(I(0x29, s, t, off))
#define SW(t, off, s)  E(I(0x2B, s, t, off))
#define SD(t, off, s)  E(I(0x3F, s, t, off))
#define LD(t, off, s)  E(I(0x37, s, t, off))
#define J_(addr)       E((0x02u << 26) | (((uint32_t)(addr) >> 2) & 0x3FFFFFF))
#define JAL(addr)      E((0x03u << 26) | (((uint32_t)(addr) >> 2) & 0x3FFFFFF))
/* branches: emitted with offset 0, patched with patch_to() */
#define BEQ(s, t)      (E(I(0x04, s, t, 0)), cp - 1)
#define BNE(s, t)      (E(I(0x05, s, t, 0)), cp - 1)
#define BLEZ(s)        (E(I(0x06, s, 0, 0)), cp - 1)
#define BLTZ(s)        (E(I(0x01, s, 0, 0)), cp - 1)
#define BGEZ(s)        (E(I(0x01, s, 1, 0)), cp - 1)
#define BLTZAL(s)      (E(I(0x01, s, 0x10, 0)), cp - 1)
static inline void patch_to(uint32_t *br, uint32_t *target)
{
    int32_t off = (int32_t)(target - (br + 1));
    *br = (*br & 0xFFFF0000u) | ((uint32_t)off & 0xFFFF);
}
static inline void LI(int r, uint32_t v)
{
    if (v <= 0xFFFF) ORI(r, ZERO, v);
    else { LUI(r, v >> 16); if (v & 0xFFFF) ORI(r, r, v & 0xFFFF); }
}

#define OFF(f) ((int)offsetof(pce_t, f))
#define O_RAM OFF(ram)

/* ---- code cache ---- */
#define CODE_SIZE (1024 * 1024)
#define TAB_BITS 14
#define TAB_SIZE (1 << TAB_BITS)
#ifdef JIT_DYNAMIC
static uint32_t *code_buf;              /* given by jit_init_buffer */
#else
static uint32_t code_buf_static[CODE_SIZE / 4] __attribute__((aligned(65536)));
static uint32_t *code_buf = code_buf_static;
#endif
static unsigned code_size = CODE_SIZE;
static uint32_t *code_end;
static uint32_t *ccp;               /* cold pointer: grows down from the end of the current 64 KB page */
#define PAGE_WORDS (65536 / 4)
static struct { uint32_t key; uint32_t *code; } tab[TAB_SIZE] __attribute__((aligned(16)));
/* Every block of the current generation, by key (open addressing): the
 * dispatcher's table is direct mapped, and two blocks that share a slot would
 * otherwise be translated again each time the other one ran. */
#define ALL_BITS 15
#define ALL_SIZE (1 << ALL_BITS)
static struct { uint32_t key; uint32_t *code; } allb[ALL_SIZE];
static unsigned nall;
unsigned jit_stat_replace;
void jit_flush(void);
unsigned jit_stat_refill, jit_stat_mid, jit_stat_tw, jit_stat_twb, jit_stat_hotb, jit_stat_forced;
static uint32_t *all_find(uint32_t key)
{
    unsigned h = (key * 2654435761u) >> (32 - ALL_BITS);
    for (;;) {
        if (allb[h].key == key) return allb[h].code;
        if (!allb[h].key) return NULL;
        h = (h + 1) & (ALL_SIZE - 1);
    }
}
static void all_put(uint32_t key, uint32_t *code)
{
    unsigned h = (key * 2654435761u) >> (32 - ALL_BITS);
    for (;;) {
        if (allb[h].key == key) { allb[h].code = code; return; }
        if (!allb[h].key) {
            if (nall >= ALL_SIZE * 3 / 4) return;       /* full: such blocks may be translated again */
            allb[h].key = key; allb[h].code = code; nall++; return;
        }
        h = (h + 1) & (ALL_SIZE - 1);
    }
}
/* entries into the middle of a checked twin (where a run stopped at an exact
   instruction resumes): key|pc -> stub; translated twins record them */
#define MID_BITS 13
#define MID_MAGIC 0xFFFFFFFFu
#define MID_SIZE (1 << MID_BITS)
static struct { uint32_t key; uint32_t *code; } midb[MID_SIZE];
static unsigned nmid;
static uint32_t *mid_find(uint32_t key)
{
    unsigned h = (key * 2654435761u) >> (32 - MID_BITS);
    for (;;) {
        if (midb[h].key == key) return midb[h].code;
        if (!midb[h].key) return NULL;
        h = (h + 1) & (MID_SIZE - 1);
    }
}
static void mid_put(uint32_t key, uint32_t *code)
{
    unsigned h = (key * 2654435761u) >> (32 - MID_BITS);
    for (;;) {
        if (midb[h].key == key) { midb[h].code = code; return; }
        if (!midb[h].key) {
            if (nmid >= MID_SIZE * 3 / 4) return;
            midb[h].key = key; midb[h].code = code; nmid++; return;
        }
        h = (h + 1) & (MID_SIZE - 1);
    }
}
static struct { uint32_t *body; unsigned pc, prefix; } twm[64]; static int ntwm;
static uint32_t *entry_fn, *loop_check, *loop_nocheck, *interp_common, *interp_mem, *exit_lbl, *ram_inval_rt;
static uint32_t *rd_dyn_rt, *wr_dyn_rt;    /* shared byte read/write at the address in t9 */
static uint32_t *rts_rt, *push16_rt;        /* shared RTS, and push of the 16-bit value in t1 */
/* I/O register writes: a1 = I/O address (low 13 bits), a2 = value, t8 = cycles
   of the block after the instruction. The common cases are done right here
   (these routines stay in the I-cache, the C path does not); the rest go to
   io_slow_rt, which calls pce_io_write. */
static uint32_t *io_rd_slow_rt, *vdcst_rt;
static uint32_t *io_slow_rt, *psg_wr_rt, *vdc0_rt, *vdc2_rt, *vdc3_rt, *vce4_rt, *vce5_rt, *irqack_rt;
static pce_t *tr_p;                         /* machine being translated for */
_Static_assert(offsetof(pce_t, bx_n) < 0x8000, "bx_n needs a 16-bit offset");
_Static_assert(offsetof(pce_t, tm_cycles) < 0x8000 && offsetof(pce_t, tm_pos) < 0x8000 && offsetof(pce_t, vdc_ev) < 0x8000, "event fields need 16-bit offsets");
_Static_assert(offsetof(pce_t, iobuf) < 0x8000 && offsetof(pce_t, irq_lines) < 0x8000 && offsetof(pce_t, chunk_end) < 0x8000 &&
               offsetof(pce_t, cycles_extra) < 0x8000, "I/O fast paths need 16-bit offsets");
_Static_assert(offsetof(vdc_t, wlatch) - offsetof(vdc_t, reg) < 0x7000 && offsetof(vdc_t, bgdirty) + 2048 - offsetof(vdc_t, reg) < 0x7000, "vdc fields near reg");
_Static_assert(offsetof(vdc_t, sc_dirty) + 512 - offsetof(vdc_t, bat_words) < 0x7000 && offsetof(vdc_t, sc_frozen) + 512 - offsetof(vdc_t, bat_words) < 0x7000 &&
               offsetof(vdc_t, cell_used) + 512 - offsetof(vdc_t, bat_words) < 0x7000, "vdc fields near bat_words");
_Static_assert(offsetof(psg_t, log) - offsetof(psg_t, nlog) + 4 * PSG_LOG_MAX < 0x7000, "psg log near nlog");
_Static_assert(offsetof(pce_t, ram) % 4 == 0, "ram must be word aligned");
_Static_assert(offsetof(pce_t, dbg_pc) < 0x8000 && offsetof(pce_t, vec_ok) < 0x8000 && offsetof(pce_t, bank_jkey) + 1024 < 0x8000, "fields used by generated code need 16-bit offsets");
_Static_assert(offsetof(pce_t, ram) + 0x2000 < 0x8000, "ram must be reachable with 16-bit offsets");
static uint32_t *blocks_start;
/* The runtime (dispatcher, shared routines) sits at the start of the code
 * buffer, so it uses I-cache lines 0 .. rt_lines. Translated code leaves that
 * part of every 16 KB stretch alone (the cache is 16 KB, direct mapped), so
 * blocks never evict the runtime. Blocks are kept short enough to fit. */
#define ICACHE_SPAN 0x4000
#define BLOCK_MAX_BYTES 3072
static unsigned rt_bytes;
/* Pinned code: blocks of the timer interrupt handler (which games run
 * thousands of times a second, between other code that would evict it) go to
 * a part of the I-cache no other block uses, right after the runtime. Their
 * cold stubs go to the end of the first 64 KB page (in branch range). */
#ifndef JIT_PIN_BYTES
#define JIT_PIN_BYTES 0      /* measured: no gain (C code shares the sets) */
#endif
#define PIN_COLD_BYTES 8192
static unsigned resv_bytes;              /* I-cache window ordinary blocks leave alone: runtime + pinned */
static unsigned win_lo, win_hi, hot_lo, rt_lo;
static uint32_t *pin_cp, *pin_end, *pin_ccp, *pin_cold_lo;
static int pinning;                      /* translating into the pinned region */
unsigned jit_stat_pin;
unsigned jit_stat_tr, jit_stat_flush, jit_stat_bytes, jit_stat_ins, jit_stat_hot, jit_stat_cold, jit_stat_inval;
#define RAM_INVAL_MAX 16
#ifdef JIT_OPSTATS
unsigned jst_n[256], jst_hot[256], jst_cold[256];
#endif
static uint8_t ram_inval_n[0x2000];     /* code changes seen per RAM offset */
/* Code is translated once it has run a few times; until then the interpreter
 * runs it. Code that runs once (start-up, level set-up) is never translated,
 * and a burst of new code costs its translation over several frames. */
#ifndef JIT_HOT
#define JIT_HOT 3
#endif
unsigned jit_hot = JIT_HOT;                  /* runs before a block is translated */
static uint8_t heat[4096];
static int jit_force;                   /* translate now (RAM code that changed) */
static uint32_t *chain_rt, *chain_rt_c;  /* runtime: translate the target and patch the jump (unchecked / checked entry) */
static uint32_t *chain_rt_t;             /* runtime, reached by jal: the block's checked twin, patching the jal */
static uint32_t *twin_out;
static int tr_twin;                      /* translating a checked twin: a budget test after every instruction */

/* ---- C helpers called from generated code ---- */
#ifdef JIT_WATCH
#include <libdragon.h>
pce_t *jw_p; int jw_done;
#define JW(tag, x) do { if (jw_p && !jw_done && jw_p->vdc.vram[0xab9]) { jw_done = 1; debugf("WATCH %s pc=%04x x=%lx S=%02x\n", tag, jw_p->PC, (unsigned long)(x), jw_p->S); } } while (0)
#else
#define JW(tag, x) do { } while (0)
#endif
static PCE_HOT uint32_t helper_rd(pce_t *p, uint32_t a) { JW("rd", a); return pce_slow_read(p, a); }
static PCE_HOT void helper_wr(pce_t *p, uint32_t a, uint32_t v) { JW("wr", a); pce_slow_write(p, a, v); JW("wr-after", a | v << 16); }

static inline void p_from_j(pce_t *p)
{
    p->P = ((p->jnz & 0x8080) ? 0x80 : 0) | (p->jv ? 0x40 : 0) | (p->jd ? 0x08 : 0) |
           (p->ji ? 0x04 : 0) | ((p->jnz & 0xFF) ? 0 : 0x02) | (p->jc ? 0x01 : 0);
}
static inline void j_from_p(pce_t *p)
{
    uint8_t P = p->P;
    p->jnz = (P & 0x02) ? ((P & 0x80) ? 0x8000 : 0) : ((P & 0x80) ? 0x80 : 1);
    p->jc = P & 1; p->jv = P & 0x40; p->jd = (P & 0x08) != 0; p->ji = (P & 0x04) != 0;
}

/* interrupt entry: returns the new PC (the dispatcher subtracts the 8 cycles) */
static PCE_HOT1 uint32_t helper_irq(pce_t *p)
{
    uint8_t l = p->irq_lines;
    p->jpin = l & 4;
    unsigned vec = (l & 4) ? 0xFFFA : (l & 2) ? 0xFFF8 : 0xFFF6;
    p_from_j(p);
    unsigned pc = p->PC;
    p->ram[0x100 | p->S] = pc >> 8; p->S--;
    p->ram[0x100 | p->S] = pc & 0xFF; p->S--;
    p->ram[0x100 | p->S] = p->P & ~0x10; p->S--;
    p->ji = 1; p->jd = 0;
    if (!p->vec_ok) {
        /* vectors of a ROM bank never change: read them once per mapping */
        const uint8_t *r = p->rmap[7];
        if (!r || p->mpr[7] >= 0x80) {
            uint8_t lo = p->rmap[vec >> 13] ? p->rmap[vec >> 13][vec & 0x1FFF] : pce_slow_read(p, vec);
            uint8_t hi = p->rmap[(vec + 1) >> 13] ? p->rmap[(vec + 1) >> 13][(vec + 1) & 0x1FFF] : pce_slow_read(p, vec + 1);
            return lo | hi << 8;
        }
        p->irqvec[0] = r[0x1FFA] | r[0x1FFB] << 8;
        p->irqvec[1] = r[0x1FF8] | r[0x1FF9] << 8;
        p->irqvec[2] = r[0x1FF6] | r[0x1FF7] << 8;
        p->vec_ok = 1;
    }
    return p->irqvec[(l & 4) ? 0 : (l & 2) ? 1 : 2];
}

/* interpret code the recompiler does not translate; returns cycles used */
#ifdef RUN_HIST
unsigned rh_pc[16], rh_n[16];
static void run_hist(unsigned pc) { for (int i = 0; i < 16; i++) { if (rh_n[i] && rh_pc[i] == pc) { rh_n[i]++; return; } if (!rh_n[i]) { rh_pc[i] = pc; rh_n[i] = 1; return; } } }
#endif
static PCE_HOT uint32_t helper_run_x(pce_t *p, int early);
static PCE_HOT uint32_t helper_run(pce_t *p) { return helper_run_x(p, 1); }
/* right after taking an interrupt: the handler's first instruction runs
   whatever the budget (as in the interpreter) */
static PCE_HOT uint32_t helper_run_first(pce_t *p) { return helper_run_x(p, 0); }
static PCE_HOT uint32_t helper_run_x(pce_t *p, int early)
{
#ifdef RUN_HIST
    run_hist(p->PC);
#endif
    if (__builtin_expect(early && p->cyc_left <= 0 && p->ev_exact, 0)) {
        /* reached between check points with the budget gone at an event that
           needs the exact instruction: stop here */
        p->jchk = 1; p->jexit = 0;
        return 0;
    }
    p_from_j(p);
    JW("run", 0);
    uint8_t sp0 = p->speed;
    JT("  ramrun %04x left %d\n", p->PC, p->cyc_left);
#ifdef JIT_COUNTS
    uint32_t tc0; __asm__ volatile("mfc0 %0, $9" : "=r"(tc0));
#endif
    int used = cpu_ramrun(p, p->cyc_left);
#ifdef JIT_COUNTS
    { extern unsigned jit_run_cyc, jit_run_ticks; uint32_t tc1; __asm__ volatile("mfc0 %0, $9" : "=r"(tc1)); jit_run_ticks += tc1 - tc0; jit_run_cyc += used; }
#endif
    JT("  ramrun -> %04x used %d chk %d\n", p->PC, used, p->jchk);
    j_from_p(p);
    p->jexit = (p->speed != sp0);
    return used;
}

/* decimal-mode ADC/SBC: returns A | C << 8 */
static uint32_t helper_adc_dec(uint32_t a, uint32_t m, uint32_t c)
{
    unsigned lo = (a & 0x0F) + (m & 0x0F) + (c ? 1 : 0);
    unsigned hi = (a & 0xF0) + (m & 0xF0);
    if (lo > 9) lo += 6;
    hi += (lo > 0x0F) ? 0x10 : 0;
    if (hi > 0x90) hi += 0x60;
    return ((lo & 0x0F) | (hi & 0xF0)) | ((hi > 0xFF) << 8);
}
static uint32_t helper_sbc_dec(uint32_t a, uint32_t m, uint32_t c)
{
    int cc = c ? 0 : 1;
    int lo = (a & 0x0F) - (m & 0x0F) - cc;
    int hi = (a & 0xF0) - (m & 0xF0);
    if (lo < 0) { lo -= 6; hi -= 0x10; }
    if (hi < 0) hi -= 0x60;
    unsigned nc = !(((int)a - (int)m - cc) < 0);
    return ((lo & 0x0F) | (hi & 0xF0)) | (nc << 8);
}


/* the interpreter's idle-loop test (cpu.c), reached on a taken short backward branch */
int cpu_idle_loop(pce_t *p, unsigned target, unsigned br);
static uint32_t helper_idle(pce_t *p, uint32_t target, uint32_t br)
{
    if (cpu_idle_loop(p, target, br)) { p->idle_yes = target; return 1; }
    p->idle_last = target;
    return 0;
}

uint32_t *jit_translate(pce_t *p, uint32_t pc);
uint32_t *jit_lookup(pce_t *p, uint32_t pc);
uint32_t *jit_chain(pce_t *p, uint32_t pc, uint32_t *jins, uint32_t chk);
uint32_t *jit_ram_inval(pce_t *p, uint32_t pc, uint32_t *entry);

/* ---- register sync around C calls that need the CPU state ---- */
static void emit_sync_out(void)
{
    SB(RA_, OFF(A), FP); SB(RX_, OFF(X), FP); SB(RY_, OFF(Y), FP); SB(RS_, OFF(S), FP);
    SW(RNZ, OFF(jnz), FP); SB(RC_, OFF(jc), FP); SB(RV_, OFF(jv), FP);
}
static void emit_sync_in(void)
{
    LBU(RA_, OFF(A), FP); LBU(RX_, OFF(X), FP); LBU(RY_, OFF(Y), FP); LBU(RS_, OFF(S), FP);
    LW(RNZ, OFF(jnz), FP); LBU(RC_, OFF(jc), FP); LBU(RV_, OFF(jv), FP);
}

#ifdef JIT_COUNTS
unsigned jit_cnt[8], jit_run_cyc, jit_run_ticks;
#define COUNT_AT(i) do { LI(T5, (uint32_t)&jit_cnt[i]); LW(T6, 0, T5); ADDIU(T6, T6, 1); SW(T6, 0, T5); } while (0)
#else
#define COUNT_AT(i) do { } while (0)
#endif
#define FRAME 176
/* reg = fp + off, for fields beyond 16-bit offsets */
static void emit_fp_addr(int reg, uint32_t off)
{
    LUI(reg, off >> 16); ORI(reg, reg, off & 0xFFFF); ADDU(reg, reg, FP);
}
/* the access penalty (pce_penalty): straight off the running budget */
static void emit_cycles_extra_inc(void)
{
    ADDIU(RCY, RCY, -1);
}
/* VDC data write to BXR or BYR (selected register in T1, &reg[0] in T2):
   just the register (and BYR's line counter) when the display is up to
   date; anything else falls through to the slow path. Raster effects write
   these once or twice a line. */
static void emit_scroll_fast(int lo)
{
    ADDIU(T3, T1, -7);
    SLTIU(T3, T3, 2);
    uint32_t *b_no = BEQ(T3, ZERO); LW(T3, OFF(chunk_end), FP);
    ADDU(T7, S7, T8); SUBU(T3, T3, T7);            /* now */
    emit_fp_addr(T4, OFF(next_due));
    LW(T4, 0, T4);
    SLT(T5, T3, T4);
    uint32_t *b_no2 = BEQ(T5, ZERO); SLL(T3, T1, 1);
    ADDU(T3, T3, T2);
    SB(A2, lo ? 1 : 0, T3);
    ADDIU(T4, T1, -8);
    uint32_t *b_done = BNE(T4, ZERO); NOP();
    LHU(T4, 0, T3); ANDI(T4, T4, 0x1FF);           /* BYR: the line counter restarts from it */
    emit_fp_addr(T5, OFF(vdc.yoff));
    SW(T4, 0, T5);
    patch_to(b_done, cp);
    emit_cycles_extra_inc();
    JR(RA); NOP();
    patch_to(b_no, cp); patch_to(b_no2, cp);
}
/* VDC data write to RCR: the register, then C works out the next event
   (pce_reschedule) without going through the whole I/O write path */
static void emit_rcr_fast(int lo)
{
    ADDIU(T3, T1, -6);
    uint32_t *b_no = BNE(T3, ZERO); LW(T3, OFF(chunk_end), FP);
    ADDU(T7, S7, T8); SUBU(T3, T3, T7);            /* now */
    emit_fp_addr(T4, OFF(next_due));
    LW(T4, 0, T4);
    SLT(T5, T3, T4);
    uint32_t *b_no2 = BEQ(T5, ZERO); NOP();
    SB(A2, lo ? 13 : 12, T2);                      /* reg[6] */
    emit_cycles_extra_inc();
    SW(RA, 48, SP); SW(T8, 52, SP);
    ADDU(T7, S7, T8); SW(T7, OFF(cyc_left), FP);
    MOVE(A0, FP); JAL(pce_reschedule); NOP();
    LW(T8, 52, SP); LW(T7, OFF(cyc_left), FP); SUBU(S7, T7, T8);
    LW(RA, 48, SP);
    JR(RA); NOP();
    patch_to(b_no, cp); patch_to(b_no2, cp);
}
static void emit_io_routines(void)
{
    /* the general case */
    io_slow_rt = cp;
    COUNT_AT(7);
    SW(RA, 48, SP); SW(T8, 52, SP);
    ADDU(T7, S7, T8); SW(T7, OFF(cyc_left), FP);
    MOVE(A0, FP); JAL(pce_io_write); NOP();
    LW(T8, 52, SP); LW(T7, OFF(cyc_left), FP); SUBU(S7, T7, T8);
    LW(RA, 48, SP);
    JR(RA); NOP();

    /* PSG ($0800-$0BFF): log the write with its line (psg_write) */
    psg_wr_rt = cp;
    {
        const unsigned onl = OFF(psg.nlog), olog = OFF(psg.log) - OFF(psg.nlog);
        SB(A2, OFF(iobuf), FP);
        emit_fp_addr(T0, onl);
        LW(T1, 0, T0);
        SLTIU(T2, T1, PSG_LOG_MAX);
        uint32_t *b_full = BEQ(T2, ZERO); LW(T3, OFF(chunk_end), FP);
        ADDU(T7, S7, T8); SUBU(T3, T3, T7);            /* now */
        uint32_t *b_neg = BLTZ(T3); LUI(T4, 0x0090);
        ORI(T4, T4, 0x0901); MULTU(T3, T4); MFHI(T4); /* now / 455 */
        SLTIU(T2, T4, LINES_PER_FRAME);
        uint32_t *b_ok = BNE(T2, ZERO); NOP();
        ORI(T4, ZERO, LINES_PER_FRAME - 1);
        patch_to(b_ok, cp);
        SLL(T4, T4, 16); ANDI(T5, A1, 0x0F); SLL(T5, T5, 8); OR(T4, T4, T5);
        ANDI(T5, A2, 0xFF); OR(T4, T4, T5);
        SLL(T5, T1, 2); ADDU(T5, T5, T0); SW(T4, olog, T5);
        ADDIU(T1, T1, 1);
        JR(RA); SW(T1, 0, T0);
        patch_to(b_full, cp); patch_to(b_neg, cp);
        J_(io_slow_rt); NOP();
    }

    const unsigned oreg = OFF(vdc.reg);
    const int o_sel = OFF(vdc.sel) - oreg, o_wl = OFF(vdc.wlatch) - oreg, o_bgd = OFF(vdc.bgdirty) - oreg;
    /* VDC address port: select a register */
    vdc0_rt = cp;
    emit_cycles_extra_inc();
    emit_fp_addr(T2, oreg);
    ANDI(T1, A2, 0x1F);
    JR(RA); SB(T1, o_sel, T2);

    /* VDC data low: the VRAM write latch, or MAWR low (no side effects, nothing
       the display uses: no need to bring the display up to date first) */
    vdc2_rt = cp;
    {
        emit_fp_addr(T2, oreg);
        LBU(T1, o_sel, T2);
        ADDIU(T3, T1, -2);
        uint32_t *b_nvwr = BNE(T3, ZERO); NOP();
        emit_cycles_extra_inc();
        JR(RA); SB(A2, o_wl + 1, T2);
        patch_to(b_nvwr, cp);
        uint32_t *b_slow = BNE(T1, ZERO); NOP();
        emit_cycles_extra_inc();
        JR(RA); SB(A2, 1, T2);                      /* reg[0] low byte */
        patch_to(b_slow, cp);
        emit_scroll_fast(1);
        emit_rcr_fast(1);
        J_(io_slow_rt); NOP();
    }

    /* VDC data high: MAWR high, or a VRAM word (vram_write + vram_track when
       no lines wait to be drawn and the display is up to date) */
    vdc3_rt = cp;
    {
        const unsigned obw = OFF(vdc.bat_words);
        const int o_bgp = OFF(vdc.bg_pend) - obw, o_spp = OFF(vdc.spr_pend) - obw,
                  o_tref = OFF(vdc.tref) - obw, o_ptd = OFF(vdc.pl_tdirty) - obw, o_ptda = OFF(vdc.pl_tdirty_any) - obw,
                  o_pcd = OFF(vdc.pl_cdirty) - obw, o_pdl = OFF(vdc.pl_dlist) - obw, o_pdn = OFF(vdc.pl_dn) - obw,
                  o_scd = OFF(vdc.sc_dirty) - obw, o_cu = OFF(vdc.cell_used) - obw, o_scf = OFF(vdc.sc_frozen) - obw;
        emit_fp_addr(T2, oreg);
        LBU(T1, o_sel, T2);
        uint32_t *b_nmawr = BNE(T1, ZERO); NOP();
        emit_cycles_extra_inc();
        JR(RA); SB(A2, 0, T2);                      /* reg[0] high byte */
        patch_to(b_nmawr, cp);
        ADDIU(T3, T1, -2);
        uint32_t *b_slow1 = BNE(T3, ZERO); LW(T3, OFF(chunk_end), FP);
        ADDU(T7, S7, T8); SUBU(T3, T3, T7);            /* now */
        emit_fp_addr(T4, OFF(next_due));
        LW(T4, 0, T4);
        SLT(T5, T3, T4);
        uint32_t *b_slow2 = BEQ(T5, ZERO); NOP();      /* lines to process first */
        emit_fp_addr(T6, obw);
        LHU(T4, 0, T2);                                /* addr = MAWR */
        /* lines wait for the GPU: C checks for hazards, unless this word
           cannot be one (not the BAT, a tile the BAT does not use; a
           sprite cell no waiting sprite shows) */
        /* (a1 and a2 stay as they are for io_slow_rt) */
        LW(T5, o_bgp, T6);
        uint32_t *b_bgok = BEQ(T5, ZERO); ANDI(T7, T4, 0x8000);
        uint32_t *b_bgok2 = BNE(T7, ZERO); LW(T7, 0, T6);      /* no write at all */
        SLTU(T7, T4, T7);
        uint32_t *b_slow3 = BNE(T7, ZERO); SRL(T3, T4, 4);     /* BAT word */
        SLL(T3, T3, 1); ADDU(T3, T3, T6); LHU(T7, o_tref, T3);
        uint32_t *b_slow4 = BNE(T7, ZERO); NOP();              /* a tile in use */
        patch_to(b_bgok, cp); patch_to(b_bgok2, cp);
        LW(T5, o_spp, T6);
        uint32_t *b_spok = BEQ(T5, ZERO); SRL(T3, T4, 6);
        ANDI(T3, T3, 0x1FF); ADDU(T3, T3, T6);
        LBU(T7, o_cu, T3);
        uint32_t *b_spok2 = BEQ(T7, ZERO); LBU(T7, o_scf, T3);
        uint32_t *b_slow5 = BEQ(T7, ZERO); NOP();              /* shown and not cached yet */
        patch_to(b_spok, cp); patch_to(b_spok2, cp);
        /* commit */
        LBU(A0, o_wl + 1, T2); ANDI(T5, A2, 0xFF); SLL(T5, T5, 8); OR(T5, T5, A0);   /* d */
        LHU(T3, 10, T2); SRL(T3, T3, 11); ANDI(T3, T3, 3);
        uint32_t *b_inc1 = BEQ(T3, ZERO); ORI(V0, ZERO, 1);
        ORI(V0, ZERO, 16); SLLV(V0, V0, T3);
        patch_to(b_inc1, cp);
        ADDU(V1, T4, V0); SH(V1, 0, T2);               /* MAWR += increment */
        ANDI(V1, T4, 0x8000);
        uint32_t *b_done1 = BNE(V1, ZERO); NOP();
        emit_fp_addr(V1, OFF(vdc.vram));
        SLL(A0, T4, 1); ADDU(V1, V1, A0);
        LHU(A3, 0, V1);                                /* old */
        uint32_t *b_done2 = BEQ(A3, T5); ORI(V0, ZERO, 1);
        SH(T5, 0, V1);
        SRL(A0, T4, 4); ADDU(A0, A0, T2); SB(V0, o_bgd, A0);
        SRL(A0, T4, 6); ADDU(A0, A0, T6); SB(V0, o_scd, A0);
        /* BAT word: tile use counts, plane cell dirty */
        LW(A1, 0, T6); SLTU(A1, T4, A1);
        uint32_t *b_nbat = BEQ(A1, ZERO); NOP();
        ANDI(A0, A3, 0x7FF); SLL(A0, A0, 1); ADDU(A0, A0, T6); LHU(A1, o_tref, A0); ADDIU(A1, A1, -1); SH(A1, o_tref, A0);
        ANDI(A0, T5, 0x7FF); SLL(A0, A0, 1); ADDU(A0, A0, T6); LHU(A1, o_tref, A0); ADDIU(A1, A1, 1); SH(A1, o_tref, A0);
        ADDU(A0, T4, T6); LBU(A1, o_pcd, A0);
        uint32_t *b_marked = BNE(A1, ZERO); NOP();
        SB(V0, o_pcd, A0);
        LW(A1, o_pdn, T6); SLTIU(A0, A1, 1024);
        uint32_t *b_nolist = BEQ(A0, ZERO); SLL(A0, A1, 1);
        ADDU(A0, A0, T6); SH(T4, o_pdl, A0);
        patch_to(b_nolist, cp);
        ADDIU(A1, A1, 1); SW(A1, o_pdn, T6);
        patch_to(b_nbat, cp); patch_to(b_marked, cp);
        /* pattern data of a tile the BAT uses */
        SRL(A0, T4, 4); SLL(A1, A0, 1); ADDU(A1, A1, T6); LHU(A1, o_tref, A1);
        uint32_t *b_nref = BEQ(A1, ZERO); NOP();
        ADDU(A1, A0, T6); SB(V0, o_ptd, A1); SW(V0, o_ptda, T6);
        patch_to(b_nref, cp);
        patch_to(b_done1, cp); patch_to(b_done2, cp);
        emit_cycles_extra_inc();
        JR(RA); NOP();
        patch_to(b_slow2, cp); patch_to(b_slow3, cp); patch_to(b_slow4, cp); patch_to(b_slow5, cp);
        J_(io_slow_rt); NOP();
        patch_to(b_slow1, cp);
        emit_scroll_fast(0);
        emit_rcr_fast(0);
        J_(io_slow_rt); NOP();
    }

    /* VCE colour data: palette entry (and its frontend colour) */
    for (int hi = 0; hi < 2; hi++) {
        if (hi) vce5_rt = cp; else vce4_rt = cp;
        const unsigned opal = OFF(vce.pal);
        const int o_addr = OFF(vce.addr) - opal, o_dirty = OFF(vce.dirty) - opal;
        LW(T3, OFF(chunk_end), FP);
        ADDU(T7, S7, T8); SUBU(T3, T3, T7);            /* now */
        emit_fp_addr(T4, OFF(next_due));
        LW(T4, 0, T4);
        SLT(T5, T3, T4);
        uint32_t *b_slow = BEQ(T5, ZERO); NOP();       /* lines to process first (palette snapshot) */
        emit_fp_addr(T6, OFF(colortab));
        LW(T6, 0, T6);
        uint32_t *b_slow2 = BEQ(T6, ZERO); NOP();
        emit_fp_addr(T2, opal);
        LHU(T3, o_addr, T2);
        ANDI(V0, T3, 0xFF);
        uint32_t *b_slow3 = BEQ(V0, ZERO); NOP();      /* backdrop / overscan colour: C records it per line */
        SLL(T4, T3, 1); ADDU(T4, T4, T2); LHU(T5, 0, T4);
        if (!hi) { ANDI(T5, T5, 0x100); ANDI(V0, A2, 0xFF); OR(T5, T5, V0); }
        else { ANDI(T5, T5, 0xFF); ANDI(V0, A2, 1); SLL(V0, V0, 8); OR(T5, T5, V0); }
        SH(T5, 0, T4);
        ORI(V0, ZERO, 1); SB(V0, o_dirty, T2);
        SLL(V0, T5, 1); ADDU(V0, V0, T6); LHU(V0, 0, V0);  /* colortab[pal] */
        emit_fp_addr(V1, OFF(pal_native));
        SLL(A0, T3, 1); ADDU(V1, V1, A0); SH(V0, 0, V1);
        if (hi) { ADDIU(T3, T3, 1); ANDI(T3, T3, 0x1FF); SH(T3, o_addr, T2); }
        emit_cycles_extra_inc();
        JR(RA); NOP();
        patch_to(b_slow, cp); patch_to(b_slow2, cp); patch_to(b_slow3, cp);
        J_(io_slow_rt); NOP();
    }

    /* $1403: timer interrupt acknowledge */
    irqack_rt = cp;
    {
        SB(A2, OFF(iobuf), FP);
        LBU(T0, OFF(irq_pend), FP); ANDI(T0, T0, 3); SB(T0, OFF(irq_pend), FP);
        LBU(T1, OFF(irq_mask), FP); NOR(T1, T1, ZERO); AND(T1, T1, T0); ANDI(T1, T1, 7);
        uint32_t *b_slow = BNE(T1, ZERO); NOP();       /* another one still pending: C cuts the budget */
        JR(RA); SB(T1, OFF(irq_lines), FP);
        patch_to(b_slow, cp);
        J_(io_slow_rt); NOP();
    }

    /* I/O read, the general case: address in a1, value back in v0 */
    io_rd_slow_rt = cp;
    SW(RA, 48, SP); SW(T8, 52, SP);
    ADDU(T7, S7, T8); SW(T7, OFF(cyc_left), FP);
    MOVE(A0, FP); JAL(pce_io_read); NOP();
    LW(T8, 52, SP); LW(T7, OFF(cyc_left), FP); SUBU(S7, T7, T8);
    LW(RA, 48, SP);
    JR(RA); NOP();

    /* VDC status read (interrupt handlers do one per interrupt): the status,
       acknowledging the VDC interrupt, when the display is up to date and no
       other interrupt stays pending (else C, which also cuts the budget) */
    vdcst_rt = cp;
    {
        LW(T3, OFF(chunk_end), FP);
        ADDU(T7, S7, T8); SUBU(T3, T3, T7);            /* now */
        emit_fp_addr(T4, OFF(next_due));
        LW(T4, 0, T4);
        SLT(T5, T3, T4);
        uint32_t *b_slow = BEQ(T5, ZERO); NOP();
        LBU(T0, OFF(irq_pend), FP); ANDI(T0, T0, 0xFD);
        LBU(T1, OFF(irq_mask), FP); NOR(T1, T1, ZERO); AND(T1, T1, T0); ANDI(T1, T1, 7);
        uint32_t *b_slow2 = BNE(T1, ZERO); NOP();
        SB(T0, OFF(irq_pend), FP); SB(T1, OFF(irq_lines), FP);
        emit_fp_addr(T2, OFF(vdc.status));
        LHU(T3, 0, T2); ANDI(V0, T3, 0x7F); ANDI(T3, T3, 0xFFC0); SH(T3, 0, T2);
        emit_cycles_extra_inc();
        JR(RA); NOP();
        patch_to(b_slow, cp); patch_to(b_slow2, cp);
        J_(io_rd_slow_rt); NOP();
    }
}

/* the fast routine for a write to I/O address a (low 13 bits), or NULL */
static uint32_t *io_route(unsigned a)
{
    switch (a >> 10) {
#ifdef JIT_NO_VDC3
    case 0: switch (a & 3) { case 0: return vdc0_rt; case 2: return vdc2_rt; } return NULL;
#else
    case 0: switch (a & 3) { case 0: return vdc0_rt; case 2: return vdc2_rt; case 3: return tr_p->deferred ? vdc3_rt : NULL; } return NULL;
#endif
    case 1: return (a & 7) == 4 ? vce4_rt : (a & 7) == 5 ? vce5_rt : NULL;
    case 2: return psg_wr_rt;
    case 5: return (a & 3) == 3 ? irqack_rt : NULL;
    default: return NULL;
    }
}

static void emit_runtime(void)
{
    cp = code_buf;
    /* int entry(pce_t *p, int cycles): returns the cycles left */
    entry_fn = cp;
    ADDIU(SP, SP, -FRAME);
    SD(RA, FRAME - 8, SP); SD(FP, FRAME - 16, SP);
    SD(S7, FRAME - 24, SP); SD(S6, FRAME - 32, SP); SD(S5, FRAME - 40, SP); SD(S4, FRAME - 48, SP);
    SD(S3, FRAME - 56, SP); SD(S2, FRAME - 64, SP); SD(S1, FRAME - 72, SP); SD(S0, FRAME - 80, SP);
    MOVE(FP, A0);
    MOVE(RCY, A1);
    emit_sync_in();
    LHU(V0, OFF(PC), FP);

    /* check point: budget, then interrupts */
    loop_check = cp;
    COUNT_AT(0);
    uint32_t *b_exit = BLEZ(RCY); NOP();
    LBU(T0, OFF(irq_lines), FP);
    uint32_t *b1 = BEQ(T0, ZERO); NOP();
    LBU(T1, OFF(ji), FP);
    uint32_t *b2 = BNE(T1, ZERO); NOP();
    /* take the interrupt (helper_irq), here when the vectors are known */
    LBU(T1, OFF(vec_ok), FP);
    uint32_t *b_cirq = BEQ(T1, ZERO); NOP();
    {
        /* P from the split flags (I is clear here, B is pushed clear) */
        ANDI(T2, RNZ, 0x8080); SLTU(T2, ZERO, T2); SLL(T2, T2, 7);
        SLTU(T3, ZERO, RV_); SLL(T3, T3, 6); OR(T2, T2, T3);
        LBU(T3, OFF(jd), FP); SLTU(T3, ZERO, T3); SLL(T3, T3, 3); OR(T2, T2, T3);
        ANDI(T3, RNZ, 0xFF); SLTIU(T3, T3, 1); SLL(T3, T3, 1); OR(T2, T2, T3);
        SLTU(T3, ZERO, RC_); OR(T2, T2, T3);
        SRL(T5, V0, 8);
        ADDU(T4, RS_, FP); SB(T5, O_RAM + 0x100, T4); ADDIU(RS_, RS_, -1); ANDI(RS_, RS_, 0xFF);
        ADDU(T4, RS_, FP); SB(V0, O_RAM + 0x100, T4); ADDIU(RS_, RS_, -1); ANDI(RS_, RS_, 0xFF);
        ADDU(T4, RS_, FP); SB(T2, O_RAM + 0x100, T4); ADDIU(RS_, RS_, -1); ANDI(RS_, RS_, 0xFF);
        ORI(T5, ZERO, 1); SB(T5, OFF(ji), FP); SB(ZERO, OFF(jd), FP);
        ANDI(T5, T0, 4);
        SB(T5, OFF(jpin), FP);
        uint32_t *b_v0 = BNE(T5, ZERO); LHU(V0, OFF(irqvec), FP);
        ANDI(T5, T0, 2);
        uint32_t *b_v1 = BNE(T5, ZERO); LHU(V0, OFF(irqvec) + 2, FP);
        LHU(V0, OFF(irqvec) + 4, FP);
        patch_to(b_v0, cp); patch_to(b_v1, cp);
    }
    uint32_t *b_irqd = BEQ(ZERO, ZERO); ADDIU(RCY, RCY, -8);
    patch_to(b_cirq, cp);
    SH(V0, OFF(PC), FP);
    emit_sync_out();
    MOVE(A0, FP); JAL(helper_irq); NOP();
    LBU(RS_, OFF(S), FP);
    ADDIU(RCY, RCY, -8);
    patch_to(b_irqd, cp);
    /* the budget gone already: the interpreter runs the handler's first
       instruction (then stops, if the event needs the exact instruction) */
    uint32_t *b_ifirst = BLEZ(RCY); NOP();
    /* fall into the lookup with v0 = vector */

    loop_nocheck = cp;
    patch_to(b1, cp); patch_to(b2, cp);
    COUNT_AT(1);
    SRL(T0, V0, 13);
    SLL(T0, T0, 2);
    ADDU(T0, T0, FP);
    LW(T1, OFF(jkey), T0);
    uint32_t *b_mem = BEQ(T1, ZERO); NOP();
    OR(T2, T1, V0);                          /* key */
    SRL(T3, T1, 9);
    XOR(T3, T3, V0);
    ANDI(T3, T3, TAB_SIZE - 1);
    SLL(T3, T3, 3);
    LI(T4, (uint32_t)tab);
    ADDU(T4, T4, T3);
    LW(T5, 0, T4);
    uint32_t *b_miss = BNE(T5, T2); LW(T6, 4, T4);
    JR(T6); NOP();
    /* miss or RAM/IO page: translate (or get the interpreter stub) */
    patch_to(b_miss, cp);
    MOVE(A0, FP); MOVE(A1, V0); JAL(jit_lookup); NOP();
    JR(V0); NOP();

    /* Out of budget. What the frame loop (pce_run_frame) would do next is
       done here when it is only the timer and the next event: no display
       lines to process, not the end of the frame. Else: exit to C. */
    uint32_t *to_exit[8]; int nte = 0;
    patch_to(b_exit, cp);
#ifndef JIT_NO_EVT
    {
        const uint32_t FRAME_CYC = LINES_PER_FRAME * CYCLES_PER_LINE;
        LW(T1, OFF(jit_noevt), FP); LW(T2, OFF(bx_n), FP); OR(T1, T1, T2);   /* tests, or a block transfer to go on with */
        to_exit[nte++] = BNE(T1, ZERO); LW(T0, OFF(chunk_end), FP);
        SUBU(T0, T0, RCY);
        LW(T1, OFF(cycles_extra), FP); ADDU(T0, T0, T1);          /* fc: frame position */
        LW(T2, OFF(vdc_ev), FP); SLT(T3, T0, T2);
        to_exit[nte++] = BEQ(T3, ZERO); LUI(T2, FRAME_CYC >> 16);
        ORI(T2, T2, FRAME_CYC & 0xFFFF); SLT(T3, T0, T2);
        to_exit[nte++] = BEQ(T3, ZERO); NOP();
        /* timer_sync(fc) */
        LW(T2, OFF(tm_pos), FP); SLT(T3, T2, T0);
        uint32_t *b_nosync = BEQ(T3, ZERO); LBU(T4, OFF(tm_on), FP);
        uint32_t *b_toff = BEQ(T4, ZERO); SUBU(T5, T0, T2);
        LW(T6, OFF(tm_cycles), FP); ADDU(T6, T6, T5);
        SLTIU(T3, T6, 1024);
        uint32_t *b_notick = BNE(T3, ZERO); SRL(T7, T6, 10);  /* n ticks */
        ANDI(T6, T6, 1023);
        LBU(A0, OFF(tm_count), FP); SLTU(T3, A0, T7);
        uint32_t *b_under = BNE(T3, ZERO); NOP();
        SUBU(A0, A0, T7); SB(A0, OFF(tm_count), FP);
        uint32_t *b_sdone = BEQ(ZERO, ZERO); NOP();
        patch_to(b_under, cp);
        SUBU(T7, T7, A0); ADDIU(T7, T7, -1);                    /* ticks after reaching zero */
        LBU(A1, OFF(tm_reload), FP); ADDIU(A2, A1, 1);
        SLTU(T3, T7, A2);
        to_exit[nte++] = BEQ(T3, ZERO); NOP();                  /* more than one period: C */
        SUBU(A0, A1, T7); SB(A0, OFF(tm_count), FP);
        LBU(A3, OFF(irq_pend), FP); ORI(A3, A3, 4); SB(A3, OFF(irq_pend), FP);
        LBU(A1, OFF(irq_mask), FP); NOR(A1, A1, ZERO); AND(A1, A1, A3); ANDI(A1, A1, 7);
        SB(A1, OFF(irq_lines), FP);
        patch_to(b_sdone, cp); patch_to(b_notick, cp);
        SW(T6, OFF(tm_cycles), FP);
        patch_to(b_toff, cp);
        SW(T0, OFF(tm_pos), FP);
        patch_to(b_nosync, cp);
        /* next_event */
        LW(T2, OFF(ev_other), FP); SLT(T3, T0, T2);
        LBU(A3, OFF(ev_other_vdc), FP);        /* exact: a VDC event, unless the timer comes first ... */
        to_exit[nte++] = BEQ(T3, ZERO); LBU(T4, OFF(tm_on), FP);  /* not ahead (out of date): C */
        uint32_t *b_noti = BEQ(T4, ZERO); LW(T5, OFF(tm_pos), FP);
        LW(T6, OFF(tm_cycles), FP); LBU(A0, OFF(tm_count), FP);
        ADDIU(T5, T5, 1024); SUBU(T5, T5, T6); SLL(A0, A0, 10); ADDU(T5, T5, A0);
        SLT(T3, T5, T2);
        uint32_t *b_later = BEQ(T3, ZERO); NOP();
        {   /* ... and the VDC event is not close behind it */
            LW(T7, OFF(ev_other), FP); SUBU(T7, T7, T5); SLTIU(T7, T7, EXACT_NEAR); AND(A3, A3, T7);
        }
        MOVE(T2, T5);
        patch_to(b_later, cp); patch_to(b_noti, cp);
        SLT(T3, T0, T2);
        to_exit[nte++] = BEQ(T3, ZERO); NOP();
        /* the next run */
        SW(T2, OFF(chunk_end), FP); SUBU(RCY, T2, T0); SW(RCY, OFF(cyc_left), FP);
        SB(A3, OFF(ev_exact), FP);
        SW(ZERO, OFF(cycles_extra), FP);
        COUNT_AT(2);
        J_(loop_check); NOP();
    }
#endif

    /* exit: store the state, return the cycles left */
    exit_lbl = cp;
    for (int i = 0; i < nte; i++) patch_to(to_exit[i], cp);
    SH(V0, OFF(PC), FP);
    emit_sync_out();
    MOVE(V0, RCY);
    LD(RA, FRAME - 8, SP); LD(FP, FRAME - 16, SP);
    LD(S7, FRAME - 24, SP); LD(S6, FRAME - 32, SP); LD(S5, FRAME - 40, SP); LD(S4, FRAME - 48, SP);
    LD(S3, FRAME - 56, SP); LD(S2, FRAME - 64, SP); LD(S1, FRAME - 72, SP); LD(S0, FRAME - 80, SP);
    JR(RA); ADDIU(SP, SP, FRAME);

    /* interpreter fallback: v0 = PC */
    interp_common = cp;
    patch_to(b_mem, cp);                    /* RAM / I/O pages come straight here */
    COUNT_AT(3);
    SH(V0, OFF(PC), FP);
    SW(RCY, OFF(cyc_left), FP);
    emit_sync_out();
    MOVE(A0, FP); JAL(helper_run); NOP();
    SUBU(RCY, RCY, V0);
    emit_sync_in();
    LHU(V0, OFF(PC), FP);
    LBU(T0, OFF(jexit), FP);
    uint32_t *b_x = BNE(T0, ZERO); NOP();
    LBU(T0, OFF(jchk), FP);
    uint32_t *b_c = BNE(T0, ZERO); NOP();
    J_(loop_nocheck); NOP();
    { uint32_t *x = cp; J_(exit_lbl); NOP(); patch_to(b_x, x); }
    { uint32_t *x = cp; J_(loop_check); NOP(); patch_to(b_c, x); }

    /* interpreter, right after an interrupt with the budget gone: v0 = PC */
    patch_to(b_ifirst, cp);
    SH(V0, OFF(PC), FP);
    SW(RCY, OFF(cyc_left), FP);
    emit_sync_out();
    MOVE(A0, FP); JAL(helper_run_first); NOP();
    SUBU(RCY, RCY, V0);
    emit_sync_in();
    LHU(V0, OFF(PC), FP);
    LBU(T0, OFF(jexit), FP);
    { uint32_t *b_x = BNE(T0, ZERO); NOP();
      LBU(T0, OFF(jchk), FP);
      uint32_t *b_c = BNE(T0, ZERO); NOP();
      J_(loop_nocheck); NOP();
      patch_to(b_x, cp); J_(exit_lbl); NOP();
      patch_to(b_c, cp); J_(loop_check); NOP(); }

    /* chain: t9 = address of the j to patch, v0 = target PC; returns with
       v0 = target PC (loop_check / the interpreter entry use it) */
    chain_rt_t = cp;
    ADDIU(T9, RA, -8);                       /* the jal to patch */
    uint32_t *b_ct = BEQ(ZERO, ZERO); ORI(A3, ZERO, 2);
    chain_rt_c = cp;
    uint32_t *b_cc = BEQ(ZERO, ZERO); ORI(A3, ZERO, 1);
    chain_rt = cp;
    MOVE(A3, ZERO);
    patch_to(b_cc, cp); patch_to(b_ct, cp);
    COUNT_AT(6);
    SW(V0, 44, SP);
    MOVE(A0, FP); MOVE(A1, V0); JAL(jit_chain); MOVE(A2, T9);
    MOVE(T6, V0); LW(V0, 44, SP);
    JR(T6); NOP();

    /* RAM / I/O page code: PC was stored by jit_translate */
    interp_mem = cp;
    LHU(V0, OFF(PC), FP);
    J_(interp_common); NOP();

    /* shared memory access for computed addresses: t9 = address, t8 = cycles of
       the block after this instruction (for the I/O position). Read: value in
       t1; write: value in a2. Keeps t9. */
    rd_dyn_rt = cp;
    {
        SRL(T0, T9, 13); SLL(T0, T0, 2); ADDU(T0, T0, FP); LW(T0, OFF(rmap), T0);
        uint32_t *bs = BEQ(T0, ZERO); ANDI(T1, T9, 0x1FFF);
        ADDU(T0, T0, T1); JR(RA); LBU(T1, 0, T0);
        patch_to(bs, cp);
        COUNT_AT(4);
        SW(RA, 48, SP); SW(T9, 40, SP); SW(T8, 52, SP);
        ADDU(T7, S7, T8); SW(T7, OFF(cyc_left), FP);
        MOVE(A0, FP); JAL(helper_rd); MOVE(A1, T9);
        MOVE(T1, V0);
        LW(T8, 52, SP); LW(T7, OFF(cyc_left), FP); SUBU(S7, T7, T8);
        LW(T9, 40, SP); LW(RA, 48, SP);
        JR(RA); NOP();
    }
    wr_dyn_rt = cp;
    {
        SRL(T0, T9, 13); SLL(T0, T0, 2); ADDU(T0, T0, FP); LW(T0, OFF(wmap), T0);
        uint32_t *bs = BEQ(T0, ZERO); ANDI(A3, T9, 0x1FFF);
        ADDU(T0, T0, A3); JR(RA); SB(A2, 0, T0);
        patch_to(bs, cp);
        COUNT_AT(5);
        SW(RA, 48, SP); SW(T9, 40, SP); SW(T8, 52, SP);
        ADDU(T7, S7, T8); SW(T7, OFF(cyc_left), FP);
        MOVE(A0, FP); JAL(helper_wr); MOVE(A1, T9);
        LW(T8, 52, SP); LW(T7, OFF(cyc_left), FP); SUBU(S7, T7, T8);
        LW(T9, 40, SP); LW(RA, 48, SP);
        JR(RA); NOP();
    }

    /* RTS: pull the return address and go through the dispatcher */
    rts_rt = cp;
    ADDIU(RS_, RS_, 1); ANDI(RS_, RS_, 0xFF); ADDU(T0, RS_, FP); LBU(T1, O_RAM + 0x100, T0);
    ADDIU(RS_, RS_, 1); ANDI(RS_, RS_, 0xFF); ADDU(T0, RS_, FP); LBU(T2, O_RAM + 0x100, T0);
    SLL(T2, T2, 8); OR(V0, T1, T2); ADDIU(V0, V0, 1);
    J_(loop_check); ANDI(V0, V0, 0xFFFF);
    /* push t1 (high byte first), return to ra */
    push16_rt = cp;
    SRL(T2, T1, 8); ADDU(T0, RS_, FP); SB(T2, O_RAM + 0x100, T0); ADDIU(RS_, RS_, -1); ANDI(RS_, RS_, 0xFF);
    ADDU(T0, RS_, FP); SB(T1, O_RAM + 0x100, T0); ADDIU(RS_, RS_, -1);
    JR(RA); ANDI(RS_, RS_, 0xFF);

    /* a RAM block's code bytes changed: a1 = PC, a2 = block entry */
    ram_inval_rt = cp;
    MOVE(A0, FP); JAL(jit_ram_inval); NOP();
    JR(V0); NOP();

    emit_io_routines();

    blocks_start = cp;
    rt_bytes = (((uint8_t *)cp - (uint8_t *)code_buf) + 31) & ~31u;
    resv_bytes = rt_bytes + JIT_PIN_BYTES;
    rt_lo = win_lo = (uintptr_t)code_buf & (ICACHE_SPAN - 1);
    win_hi = win_lo + resv_bytes;
}

/* The I-cache window (index range) no block uses: the runtime, and right
   before it (jit_set_hot) the emulator's per-line C code */
void jit_set_hot(unsigned lo) { hot_lo = lo & (ICACHE_SPAN - 1) & ~31u; }
/* keep translated code out of the per-line C code's lines too (on), or only
   out of the runtime's: the code already translated is dropped */
void jit_window(int on)
{
    unsigned lo = (on && hot_lo && hot_lo < rt_lo) ? hot_lo : rt_lo;
    if (lo != win_lo) { win_lo = lo; jit_flush(); }
}
/* first address at or after c where a block may start */
static uint32_t *block_place_n(uint32_t *c, unsigned need)
{
    uintptr_t a = (uintptr_t)c;
    for (;;) {
        unsigned o = a & (ICACHE_SPAN - 1), e = o + need;
        if (o < win_hi && e > win_lo) { a += win_hi - o; continue; }
        if (e > ICACHE_SPAN + win_lo) { a += ICACHE_SPAN + win_hi - o; continue; }
        return (uint32_t *)a;
    }
}
/* does code at c of that many bytes stay out of the window? */
static int place_ok(uint32_t *c, unsigned bytes)
{
    unsigned o = (uintptr_t)c & (ICACHE_SPAN - 1), e = o + bytes;
    return !(o < win_hi && e > win_lo) && e <= ICACHE_SPAN + win_lo;
}
/* The current 64 KB page: hot code grows up from its start around the window's
   image in each 16 KB of addresses, the cold stubs fill those images once the
   hot code is past them (cold_alloc), and what does not fit there grows down
   from the page's end (ccp). */
static uint32_t *page_start, *page_top;
static uint32_t *gap_cur, *gap_end; static int gap_k;
static void page_begin(void) { page_start = cp; page_top = ccp; gap_cur = gap_end = NULL; gap_k = 0; }

static void cold_begin(uint32_t *br);
static void cold_end(void);

/* I/O calls publish the exact budget position: s7 plus the cycles of the
 * block's later instructions (known at block end, so patched in then). */
static uint32_t stubbuf[8192];
static unsigned cur_prefix;
static struct { uint32_t *hot; int stub; unsigned prefix; int sgn, delta; } iofix[1024];
static int niofix;
/* the next instruction's immediate becomes sgn * (cycles of the block after
   the current instruction) + delta, once the block's length is known */
/* in a cold stub: unless the budget ends at an event that needs the exact
   instruction (ev_exact), carry on at back (the old check points suffice) */
static void emit_exact_or_back(uint32_t *back)
{
    LBU(T1, OFF(ev_exact), FP);
    uint32_t *b_ex = BNE(T1, ZERO); NOP();
    J_(back); NOP();
    patch_to(b_ex, cp);
}
static int insn_io;     /* the instruction being translated may write I/O (which can cut the budget) */
static void fix_rest(int sgn, int delta)
{
    if (cp >= stubbuf && cp < stubbuf + 8192) { iofix[niofix].hot = NULL; iofix[niofix].stub = cp - stubbuf; }
    else { iofix[niofix].hot = cp; iofix[niofix].stub = -1; }
    iofix[niofix].prefix = cur_prefix;
    iofix[niofix].sgn = sgn; iofix[niofix].delta = delta;
    niofix++;
}
static void io_fix_here(void) { fix_rest(0, 0); }   /* sign from the register (see cold_flush) */
static unsigned cur_lpc;
static void emit_io_pre(void)
{
#ifdef PCE_VERIFY
    LI(T7, cur_lpc); SW(T7, OFF(dbg_pc), FP);
#endif
    io_fix_here(); ADDIU(T7, S7, 0); SW(T7, OFF(cyc_left), FP);
}
static void emit_io_post(void) { LW(T7, OFF(cyc_left), FP); io_fix_here(); ADDIU(S7, T7, 0); }

/* ---- memory access emitters (value in/out in `r`, address const or in T9) ----
 * hot path: lw map; beq -> cold stub; nop; access. */
static void emit_rd_const(int r, unsigned ea)
{
    if (tr_p->mpr[ea >> 13] == 0xFF && (ea & 0x1FFF) < 0x400 && (ea & 3) == 0) {
        /* VDC status: the fast routine while the I/O page is mapped there */
        LBU(T0, OFF(mpr) + (ea >> 13), FP);
        XORI(T0, T0, 0xFF);
        uint32_t *bs = BNE(T0, ZERO); NOP();
#ifdef PCE_VERIFY
        LI(T7, cur_lpc); SW(T7, OFF(dbg_pc), FP);
#endif
        ORI(A1, ZERO, ea & 0x1FFF);
        JAL(vdcst_rt); io_fix_here(); ORI(T8, ZERO, 0);
        MOVE(r, V0);
        uint32_t *back = cp;
        cold_begin(bs);
        LW(T0, OFF(rmap) + 4 * (ea >> 13), FP);
        uint32_t *bc = BEQ(T0, ZERO); NOP();
        J_(back); LBU(r, ea & 0x1FFF, T0);
        patch_to(bc, cp);
        emit_io_pre();
        MOVE(A0, FP); LI(A1, ea); JAL(helper_rd); NOP();
        MOVE(r, V0);
        emit_io_post();
        J_(back); NOP();
        cold_end();
        return;
    }
    LW(T0, OFF(rmap) + 4 * (ea >> 13), FP);
    uint32_t *bs = BEQ(T0, ZERO); NOP();
    LBU(r, ea & 0x1FFF, T0);
    uint32_t *back = cp;
    cold_begin(bs);
    emit_io_pre();
    MOVE(A0, FP); LI(A1, ea); JAL(helper_rd); NOP();
    MOVE(r, V0);
    emit_io_post();
    J_(back); NOP();
    cold_end();
}
static void emit_rd_dyn(int r)      /* address in T9 (kept) */
{
#ifdef PCE_VERIFY
    LI(T7, cur_lpc); SW(T7, OFF(dbg_pc), FP);
#endif
    JAL(rd_dyn_rt); io_fix_here(); ORI(T8, ZERO, 0);
    if (r != T1) MOVE(r, T1);
}
static void emit_wr_const(int r, unsigned ea)
{
    insn_io = 1;
    uint32_t *rt = tr_p->mpr[ea >> 13] == 0xFF ? io_route(ea & 0x1FFF) : NULL;
    if (rt) {
        /* I/O page now (and almost surely later): the fast routine, after a check */
        LBU(T0, OFF(mpr) + (ea >> 13), FP);
        XORI(T0, T0, 0xFF);
        uint32_t *bs = BNE(T0, ZERO); MOVE(A2, r);
#ifdef PCE_VERIFY
        LI(T7, cur_lpc); SW(T7, OFF(dbg_pc), FP);
#endif
        ORI(A1, ZERO, ea & 0x1FFF);
        JAL(rt); io_fix_here(); ORI(T8, ZERO, 0);
        uint32_t *back = cp;
        cold_begin(bs);
        /* mapped elsewhere this time: the general path (RAM, or C) */
        LW(T0, OFF(wmap) + 4 * (ea >> 13), FP);
        uint32_t *bc = BEQ(T0, ZERO); NOP();
        J_(back); SB(A2, ea & 0x1FFF, T0);
        patch_to(bc, cp);
        emit_io_pre();
        MOVE(A0, FP); LI(A1, ea); JAL(helper_wr); NOP();
        emit_io_post();
        J_(back); NOP();
        cold_end();
        return;
    }
    LW(T0, OFF(wmap) + 4 * (ea >> 13), FP);
    uint32_t *bs = BEQ(T0, ZERO); NOP();
    SB(r, ea & 0x1FFF, T0);
    uint32_t *back = cp;
    cold_begin(bs);
    MOVE(A2, r);
    emit_io_pre();
    MOVE(A0, FP); LI(A1, ea); JAL(helper_wr); NOP();
    emit_io_post();
    J_(back); NOP();
    cold_end();
}
static void emit_wr_dyn(int r)      /* address in T9 */
{
    insn_io = 1;
#ifdef PCE_VERIFY
    LI(T7, cur_lpc); SW(T7, OFF(dbg_pc), FP);
#endif
    if (r != A2) MOVE(A2, r);
    JAL(wr_dyn_rt); io_fix_here(); ORI(T8, ZERO, 0);
}
/* indexed absolute that stays inside one 8 KB slot: the slot is known */
static int ix_static, ix_reg;
static unsigned ix_ea;
static void emit_rd_ix(int r, unsigned ea, int idx)
{
    LW(T0, OFF(rmap) + 4 * (ea >> 13), FP);
    uint32_t *bs = BEQ(T0, ZERO); ADDU(T0, T0, idx);
    LBU(r, ea & 0x1FFF, T0);
    uint32_t *back = cp;
    cold_begin(bs);
    LI(T9, ea); ADDU(T9, T9, idx);
    SW(T9, 40, SP);
    emit_io_pre();
    MOVE(A0, FP); MOVE(A1, T9); JAL(helper_rd); NOP();
    MOVE(r, V0); LW(T9, 40, SP);
    emit_io_post();
    J_(back); NOP();
    cold_end();
}
static void emit_wr_ix(int r, unsigned ea, int idx)
{
    insn_io = 1;
    LW(T0, OFF(wmap) + 4 * (ea >> 13), FP);
    uint32_t *bs = BEQ(T0, ZERO); ADDU(T0, T0, idx);
    SB(r, ea & 0x1FFF, T0);
    uint32_t *back = cp;
    cold_begin(bs);
    MOVE(A2, r);
    LI(T9, ea); ADDU(T9, T9, idx);
    emit_io_pre();
    MOVE(A0, FP); MOVE(A1, T9); JAL(helper_wr); NOP();
    emit_io_post();
    J_(back); NOP();
    cold_end();
}
/* zero page: ram[z] */
static void emit_zp_ea_x(unsigned z, int idx)   /* T9 = ram offset (z+idx)&0xFF, T0 = fp + T9 */
{
    ADDIU(T9, idx, z); ANDI(T9, T9, 0xFF); ADDU(T0, T9, FP);
}

static int cur_nz_dead;
static void emit_setnz(int r) { if (!cur_nz_dead) MOVE(RNZ, r); }

/* Slow paths are emitted into a side buffer while a block is translated and
 * appended after the block's exit, so the hot path stays short and dense. */
static struct { uint32_t *j; int at; } chain_fix[256];
static int nchain;
static int nstub;
static struct { uint32_t *br; int at; } pend[512];
static int npend;
static uint32_t *hot_save;
static void cold_begin(uint32_t *br) { pend[npend].br = br; pend[npend].at = nstub; npend++; hot_save = cp; cp = stubbuf + nstub; }
static void cold_end(void) { nstub = cp - stubbuf; cp = hot_save; }
static unsigned blk_total;
static uint32_t *last_stub; static int last_nstub;
static uint32_t *last_rc; static int last_nrc;     /* RAM entry check */
static uint32_t *cold_alloc(int n)
{
    if (!pinning && n > 0 && win_lo < win_hi) {
        for (;;) {
            if (gap_cur && gap_cur + n <= gap_end) { uint32_t *r = gap_cur; gap_cur += n; return r; }
            if (gap_k >= 4) break;
            uintptr_t base = (uintptr_t)page_top - 65536 + (unsigned)gap_k * ICACHE_SPAN;
            uint32_t *lo = (uint32_t *)(base + win_lo), *hi = (uint32_t *)(base + win_hi);
            if (lo < page_start) { gap_k++; continue; }        /* (the runtime's own place) */
            if (hi > cp) break;                                            /* the hot code is not past it yet */
            gap_cur = lo; gap_end = hi; gap_k++;
        }
    }
    ccp -= n;
    return ccp;
}
static void cold_flush(void)
{
    /* stubs go to the cold end of the page, away from the hot code */
    uint32_t *stub_base = cold_alloc(nstub);
    for (int i = 0; i < npend; i++) patch_to(pend[i].br, stub_base + pend[i].at);
    for (int i = 0; i < nchain; i++) {
        uint32_t *dst = stub_base + chain_fix[i].at;
        *chain_fix[i].j = (0x02u << 26) | (((uint32_t)dst >> 2) & 0x3FFFFFF);
    }
    memcpy(stub_base, stubbuf, nstub * 4);
    last_stub = stub_base; last_nstub = nstub;
    /* now that the stubs are in place, patch the I/O position offsets */
    for (int i = 0; i < niofix; i++) {
        uint32_t *ins = iofix[i].hot ? iofix[i].hot : stub_base + iofix[i].stub;
        int rest = (int)(blk_total - iofix[i].prefix);
        int rt = (*ins >> 16) & 31;
        int sgn = iofix[i].sgn;
        if (!sgn) sgn = (rt == T7 || rt == T8) ? 1 : -1;     /* pre: t7 = s7 + rest; t8 = rest; post: s7 = t7 - rest */
        *ins = (*ins & 0xFFFF0000u) | ((uint32_t)(sgn * rest + iofix[i].delta) & 0xFFFF);
    }
    niofix = 0;
    nstub = 0; npend = 0; nchain = 0;
}

/* ---- block translation ---- */
enum { K_NORMAL, K_END_CHECK, K_END_NOCHECK };

static inline unsigned rd16(const uint8_t *pg, unsigned off) { return pg[off] | pg[off + 1] << 8; }

static int supported(unsigned op)
{
    switch (op_kind[op]) {
    case O_SET: case O_CSL: case O_CSH: case O_BRK:
        return 0;
#ifdef JIT_NO_BLK
    case O_BLK: return 0;
#endif
    default: return 1;
    }
}

static int is_t_alu(unsigned k) { return k == O_ORA || k == O_AND || k == O_EOR || k == O_ADC; }

/* operand into T1: returns 0 if the mode needs no operand */
static void emit_operand(unsigned md, const uint8_t *pg, unsigned off, int read)
{
    switch (md & 15) {
    case M_IMM: LI(T1, pg[off + 1]); break;
    case M_ZP:  if (read) LBU(T1, O_RAM + pg[off + 1], FP); break;
    case M_ZPX: emit_zp_ea_x(pg[off + 1], RX_); if (read) LBU(T1, O_RAM, T0); break;
    case M_ZPY: emit_zp_ea_x(pg[off + 1], RY_); if (read) LBU(T1, O_RAM, T0); break;
    case M_ABS: if (read) emit_rd_const(T1, rd16(pg, off + 1)); break;
    case M_ABX: case M_ABY: {
        unsigned ea = rd16(pg, off + 1);
        int idx = (md & 15) == M_ABX ? RX_ : RY_;
        if ((ea & 0x1FFF) + 0xFF <= 0x1FFF) {
            ix_static = 1; ix_ea = ea; ix_reg = idx;
            if (read) emit_rd_ix(T1, ea, idx);
        } else {
            ix_static = 0;
            LI(T9, ea); ADDU(T9, T9, idx); ANDI(T9, T9, 0xFFFF);
            if (read) emit_rd_dyn(T1);
        }
        break; }
    case M_IZX: case M_IZY: case M_IZP: {
        unsigned z = pg[off + 1];
        if ((md & 15) == M_IZX) {
            emit_zp_ea_x(z, RX_);              /* T9 = (z+X)&0xFF, T0 = fp+T9 */
            LBU(T9, O_RAM, T0);
            ADDIU(T0, T0, 1);
            /* (z+X+1)&0xFF */
            SUBU(T0, T0, FP); ANDI(T0, T0, 0xFF); ADDU(T0, T0, FP);
            LBU(T2, O_RAM, T0);
        } else {
            LBU(T9, O_RAM + z, FP);
            LBU(T2, O_RAM + ((z + 1) & 0xFF), FP);
        }
        SLL(T2, T2, 8); OR(T9, T9, T2);
        if ((md & 15) == M_IZY) { ADDU(T9, T9, RY_); ANDI(T9, T9, 0xFFFF); }
        if (read) emit_rd_dyn(T1);
        break; }
    }
}

/* load straight into register r (common modes; others through T1) */
static void emit_load(unsigned md, const uint8_t *pg, unsigned off, int r)
{
    switch (md & 15) {
    case M_IMM: LI(r, pg[off + 1]); return;
    case M_ZP: LBU(r, O_RAM + pg[off + 1], FP); return;
    case M_ZPX: emit_zp_ea_x(pg[off + 1], RX_); LBU(r, O_RAM, T0); return;
    case M_ZPY: emit_zp_ea_x(pg[off + 1], RY_); LBU(r, O_RAM, T0); return;
    case M_ABS: emit_rd_const(r, rd16(pg, off + 1)); return;
    case M_ABX: case M_ABY: {
        unsigned ea = rd16(pg, off + 1);
        int idx = (md & 15) == M_ABX ? RX_ : RY_;
        if ((ea & 0x1FFF) + 0xFF <= 0x1FFF && idx != r) { emit_rd_ix(r, ea, idx); return; }
        break; }
    default: break;
    }
    emit_operand(md, pg, off, 1);
    MOVE(r, T1);
}

/* write T-reg r to the operand location set up by emit_operand */
static void emit_store(unsigned md, const uint8_t *pg, unsigned off, int r)
{
    switch (md & 15) {
    case M_ZP: SB(r, O_RAM + pg[off + 1], FP); break;
    case M_ZPX: case M_ZPY: SB(r, O_RAM, T0); break;    /* T0 still fp+offset */
    case M_ABS: emit_wr_const(r, rd16(pg, off + 1)); break;
    case M_ABX: case M_ABY: if (ix_static) { emit_wr_ix(r, ix_ea, ix_reg); break; } /* fall through */
    default: emit_wr_dyn(r); break;
    }
}

static void emit_push(int r) { ADDU(T0, RS_, FP); SB(r, O_RAM + 0x100, T0); ADDIU(RS_, RS_, -1); ANDI(RS_, RS_, 0xFF); }
static void emit_pull(int r) { ADDIU(RS_, RS_, 1); ANDI(RS_, RS_, 0xFF); ADDU(T0, RS_, FP); LBU(r, O_RAM + 0x100, T0); }

static unsigned blk_slot;           /* 8 KB slot of the block being translated */
static int blk_ram;                 /* it is RAM code */
#define is_ram_blk blk_ram
static unsigned rcx0[8], rcx1[8]; static int nrcx;   /* RAM offsets the entry check leaves out */
static unsigned gen_count;          /* bumped by every flush */

/* Leave the block for a constant target. The jump's delay slot adds back the
 * cycles of the block's instructions after this one (subtracted on entry but
 * not run) plus delta. Targets in the block's own 8 KB page (same bank mapping,
 * same translation key) are chained directly, to the target's checked entry
 * (budget test, as the interpreter does at taken branches) or its plain one. */
static void emit_goto(uint32_t target, int chk, int delta)
{
    target &= 0xFFFF;
    /* A chained ROM or RAM block in this slot tests on entry (a budget gone
       here takes it to its twin's checked entry, or the interpreter, which
       stops right away); other targets need the test here. */
    if (!chk && (tr_twin || (target & 0xFFFF) >> 13 != blk_slot)) {
        /* not a check point, but the boundary is one when the budget ends
           at an event that needs the exact instruction */
        fix_rest(1, delta); ADDIU(T0, RCY, 0);
        uint32_t *b_gone = BLEZ(T0); NOP();
        uint32_t *back = cp;
        cold_begin(b_gone);
        emit_exact_or_back(back);
        MOVE(RCY, T0); LI(V0, target); J_(loop_check); NOP();
        cold_end();
    }
    if ((target >> 13) != blk_slot) {
        LI(V0, target);
        J_(chk ? loop_check : loop_nocheck);
        fix_rest(1, delta); ADDIU(RCY, RCY, 0);
        return;
    }
    /* j <chain stub>, patched to j <target block> on first use */
    uint32_t *jins = cp;
    J_(0);
    fix_rest(1, delta); ADDIU(RCY, RCY, 0);
    cold_begin(NULL);
    npend--;                                 /* not a branch: fixed up below */
    uint32_t *stub_at = (uint32_t *)(intptr_t)nstub;
    LI(T9, (uint32_t)jins);
    LI(V0, target);
    J_(chk ? chain_rt_c : chain_rt); NOP();
    cold_end();
    /* remember to point the j at the stub once the stubs are placed */
    chain_fix[nchain].j = jins; chain_fix[nchain].at = (int)(intptr_t)stub_at; nchain++;
}

/* could the code from toff up to boff be an idle loop? (opcodes only; the
   runtime test also checks where the data comes from) */
static int idle_shape(const uint8_t *pg, unsigned toff, unsigned boff)
{
    unsigned a = toff; int guard = 0;
    while (a < boff && guard++ < 6) {
        switch (pg[a]) {
        case 0xA9: case 0xA2: case 0xA0: case 0xC9: case 0xE0: case 0xC0: case 0x29: case 0x09: case 0x49: case 0x89:
        case 0xA5: case 0xA6: case 0xA4: case 0xC5: case 0xE4: case 0xC4: case 0x25: case 0x05: case 0x45: case 0x24:
            a += 2; break;
        case 0xAD: case 0xAE: case 0xAC: case 0xCD: case 0xEC: case 0xCC: case 0x2D: case 0x0D: case 0x4D: case 0x2C:
        case 0x83: a += 3; break;
        case 0x93: a += 4; break;
        default: return 0;
        }
    }
    return a == boff;
}
static const uint8_t *cur_pg;

/* taken branch: 2 extra cycles; short backward branches run the idle-loop test */
static void emit_take(unsigned target, unsigned brpc, int len, int backward_short)
{
    if (backward_short && (target >> 13) == blk_slot && !idle_shape(cur_pg, target & 0x1FFF, brpc & 0x1FFF)) {
        /* never idle from here: the interpreter's test fails, but it still
           remembers the target (and a target found idle before stays idle) */
        LW(T0, OFF(idle_skip), FP);
        uint32_t *b_noidle = BEQ(T0, ZERO); LI(T1, target);
        LW(T0, OFF(idle_yes), FP);
        uint32_t *b_known = BEQ(T0, T1); NOP();
        uint32_t *b_done = BEQ(ZERO, ZERO); SW(T1, OFF(idle_last), FP);
        patch_to(b_known, cp);
        fix_rest(-1, 2); ADDIU(RCY, ZERO, 0);
        patch_to(b_noidle, cp); patch_to(b_done, cp);
        emit_goto(target, 1, -2);
        return;
    }
    if (backward_short) {
        LW(T0, OFF(idle_skip), FP);
        uint32_t *b_noidle = BEQ(T0, ZERO); NOP();
        LW(T0, OFF(idle_yes), FP);
        LI(T1, target);
        uint32_t *b_known = BEQ(T0, T1); NOP();
        LW(T0, OFF(idle_last), FP);
        uint32_t *b_not = BEQ(T0, T1); NOP();
        MOVE(A0, FP); MOVE(A1, T1); LI(A2, brpc); JAL(helper_idle); NOP();
        uint32_t *b_no2 = BEQ(V0, ZERO); NOP();
        patch_to(b_known, cp);
        /* idle: nothing left of the budget once the jump below has added back
           the rest of the block and taken the branch's 2 cycles */
        fix_rest(-1, 2); ADDIU(RCY, ZERO, 0);
        patch_to(b_noidle, cp); patch_to(b_not, cp); patch_to(b_no2, cp);
    }
    emit_goto(target, 1, -2);
}

static unsigned insn_len(unsigned op)
{
    unsigned md = op_mode[op], k = op_kind[op], len;
    switch (md & 15) {
    case M_IMP: len = 1; break;
    case M_ABS: case M_ABX: case M_ABY: len = 3; break;
    default: len = 2; break;
    }
    switch (k) {
    case O_TST: len = (op == 0x93 || op == 0xB3) ? 4 : 3; break;
    case O_BLK: len = 7; break;
    case O_BBR: case O_BBS: len = 3; break;
    case O_BCC_: case O_BRA: case O_BSR: case O_TAM: case O_TMA: case O_ST0: case O_ST1: case O_ST2: len = 2; break;
    case O_JMP: case O_JMPI: case O_JMPX: case O_JSR: len = 3; break;
    default: break;
    }
    return len;
}

/* Flag liveness inside a block. A block ends at the first control transfer
 * (or an instruction that may end it: CLI, PLP, TAM, stores in RAM blocks);
 * the flags are live at every possible exit. An instruction whose N/Z (or V)
 * result is overwritten before anything reads it need not compute it. */
#define MAXI 28
static uint8_t nz_dead[MAXI + 1], v_dead[MAXI + 1];
static int cur_v_dead;
static void flag_liveness(const uint8_t *pg, unsigned off, int is_ram, unsigned lim)
{
    uint8_t wnz[MAXI], rnz[MAXI], wv[MAXI], rv[MAXI], ex[MAXI];
    int n = 0;
    while (n < MAXI && off + 7 <= 0x1FFF && off + 7 <= lim) {
        unsigned op = pg[off], k = op_kind[op];
        if (!supported(op)) break;
        int x = 0, w = 0, r = 0, vw = 0, vr = 0;
        switch (k) {
        case O_LDA: case O_LDX: case O_LDY: case O_ORA: case O_AND: case O_EOR:
        case O_INA: case O_DEA: case O_INX: case O_INY: case O_DEX: case O_DEY:
        case O_TAX: case O_TAY: case O_TXA: case O_TYA: case O_TSX: case O_PLA: case O_PLX: case O_PLY:
        case O_ASLA: case O_LSRA: case O_ROLA: case O_RORA:
        case O_CMP: case O_CPX: case O_CPY:
            w = 1; break;
        case O_ADC: case O_SBC: w = 1; vw = 2; break;     /* V only in binary mode: does not kill V */
        case O_BIT: case O_TSB: case O_TRB: case O_TST: w = 1; vw = 1; break;
        case O_ASL: case O_LSR: case O_ROL: case O_ROR: case O_INC: case O_DEC: w = 1; break;
        case O_CLV: vw = 1; break;
        case O_PHP: r = vr = 1; break;
        case O_BCC_: r = vr = 1; x = 1; break;      /* (reads one flag; keep it simple) */
        case O_BBR: case O_BBS: case O_BRA: case O_JMP: case O_JMPI: case O_JMPX: case O_JSR: case O_BSR:
        case O_RTS: case O_RTI: case O_PLP: case O_CLI: case O_TAM:
            x = 1; break;
        case O_BLK: x = 1; break;
        default: break;
        }
        {
            /* a write that may reach I/O can end the block early (its budget
               cut): flags must be up to date there */
            unsigned m = op_mode[op] & 15;
            int wr = (k == O_STA || k == O_STX || k == O_STY || k == O_STZ || k == O_TSB || k == O_TRB ||
                      k == O_ASL || k == O_LSR || k == O_ROL || k == O_ROR || k == O_INC || k == O_DEC);
            if ((wr && m != M_ZP && m != M_ZPX && m != M_ZPY) || k == O_ST0 || k == O_ST1 || k == O_ST2) x = 1;
        }
        if (is_ram && (k == O_STA || k == O_STX || k == O_STY || k == O_STZ || k == O_TSB || k == O_TRB ||
                       k == O_ASL || k == O_LSR || k == O_ROL || k == O_ROR || k == O_INC || k == O_DEC ||
                       k == O_RMB || k == O_SMB || k == O_PHA || k == O_PHX || k == O_PHY || k == O_PHP)) x = 1;
        if (tr_twin) x = 1;          /* a twin may leave after any instruction */
        wnz[n] = w; rnz[n] = r; wv[n] = vw; rv[n] = vr; ex[n] = x;
        n++;
        off += insn_len(op);
        if (x && k != O_TAM && k != O_STA && k != O_STX && k != O_STY && k != O_STZ) {
            /* control transfers end the block for sure */
            if (k == O_BCC_ || k == O_BBR || k == O_BBS || k == O_BRA || k == O_JMP || k == O_JMPI ||
                k == O_JMPX || k == O_JSR || k == O_BSR || k == O_RTS || k == O_RTI || k == O_BLK) break;
        }
    }
    int lnz = 1, lv = 1;                 /* live at the end */
    for (int i = n - 1; i >= 0; i--) {
        if (ex[i]) { lnz = 1; lv = 1; }  /* may leave the block after this one */
        nz_dead[i] = wnz[i] && !lnz;
        v_dead[i] = wv[i] && !lv;
        if (wnz[i]) lnz = 0;
        if (rnz[i]) lnz = 1;
        if (wv[i] == 1) lv = 0;
        if (rv[i]) lv = 1;
    }
    for (int i = n; i <= MAXI; i++) nz_dead[i] = v_dead[i] = 0;
}

static void flush_icache(void *start, unsigned len)
{
    data_cache_hit_writeback(start, len);
    inst_cache_hit_invalidate(start, len);
}

/* translate one block starting at pc; returns its code */
/* RAM blocks: a store that may hit the block's own later code ends the block
   (the next block checks its code bytes on entry) */
static unsigned ram_b0;             /* RAM offset of a RAM block's first byte, or ~0 */
#define RAM_BLOCK_MAX 0x100
static int ram_hits(unsigned lo, unsigned n)   /* RAM offsets lo .. lo+n-1 (mod 8 KB) */
{
    if (ram_b0 == ~0u) return 0;
    unsigned d = (ram_b0 - lo) & 0x1FFF;      /* block start relative to the range */
    unsigned e = (lo - ram_b0) & 0x1FFF;      /* range start relative to the block */
    return d < n || e < RAM_BLOCK_MAX;
}
static int store_hazard(unsigned md, const uint8_t *pg, unsigned off)
{
    switch (md & 15) {
    case M_ZP: return ram_hits(pg[off + 1], 1);
    case M_ZPX: case M_ZPY: return ram_hits(0, 0x100);
    case M_ABS: return ram_hits(rd16(pg, off + 1) & 0x1FFF, 1);
    case M_ABX: case M_ABY: return ram_hits(rd16(pg, off + 1) & 0x1FFF, 0x100);
    default: return 1;      /* indirect: anywhere */
    }
}

/* entry check for a RAM block: compare its code bytes (RAM offsets b0..b1-1)
   with the bytes it was translated from; emitted at the cold end of the page */
static uint32_t *emit_ram_check(const uint8_t *ram, unsigned b0, unsigned b1, uint32_t pc, uint32_t *body)
{
    uint32_t buf[600];
    uint32_t *save = cp; cp = buf;
    uint32_t *fails[160]; int nf = 0;
    uint32_t *b_chk = BLEZ(RCY); NOP();          /* checked entry; the plain one follows */
    /* the words to compare, with the bytes that matter */
    static uint16_t ww[160]; static uint32_t wm[160]; int nw = 0;
    for (unsigned w = b0 & ~3u; w < b1 && nw < 160; w += 4) {
        uint32_t mask = 0;
        for (unsigned b = 0; b < 4; b++) {
            unsigned o = w + b;
            if (o < b0 || o >= b1) continue;
            int skip = 0;
            for (int x = 0; x < nrcx; x++) if (o >= rcx0[x] && o < rcx1[x]) skip = 1;
            if (!skip) mask |= 0xFFu << (24 - 8 * b);
        }
        if (mask) { ww[nw] = w; wm[nw] = mask; nw++; }
    }
    if (nw) LW(T0, O_RAM + ww[0], FP);
    for (int i = 0; i < nw; i++) {
        unsigned w = ww[i]; uint32_t mask = wm[i];
        uint32_t want = ((uint32_t)ram[w] << 24 | (uint32_t)ram[w + 1] << 16 | (uint32_t)ram[w + 2] << 8 | ram[w + 3]) & mask;
        if (mask != 0xFFFFFFFFu) { LI(T2, mask); AND(T0, T0, T2); }
        LI(T1, want);
        fails[nf++] = BNE(T0, T1);
        if (i + 1 < nw) LW(T0, O_RAM + ww[i + 1], FP);    /* the next word, in the delay slot */
        else NOP();
    }
    J_(body); NOP();
    uint32_t *fail = cp;
    LI(A1, pc);
    uint32_t *entry_lui = cp;
    LUI(A2, 0); ORI(A2, A2, 0);
    J_(ram_inval_rt); NOP();
    uint32_t *xs = cp;
    LI(V0, pc); J_(loop_check); NOP();
    unsigned n = cp - buf;
    cp = save;
    uint32_t *entry = pinning ? cp : ccp - n;    /* pinned: run on every entry, so with the block */
    /* branches are relative: patch them in buf as if placed at entry */
    for (int i = 0; i < nf; i++) {
        int32_t o = (int32_t)(fail - (fails[i] + 1));
        *fails[i] = (*fails[i] & 0xFFFF0000u) | ((uint32_t)o & 0xFFFF);
    }
    { int32_t o = (int32_t)(xs - (b_chk + 1)); *b_chk = (*b_chk & 0xFFFF0000u) | ((uint32_t)o & 0xFFFF); }
    uint32_t ea = (uint32_t)(entry + 1);
    entry_lui[0] = (entry_lui[0] & 0xFFFF0000u) | (ea >> 16);
    entry_lui[1] = (entry_lui[1] & 0xFFFF0000u) | (ea & 0xFFFF);
    /* j targets are absolute within the 256 MB region: fine to copy */
    memcpy(entry, buf, n * 4);
    if (pinning) cp = entry + n; else ccp = entry;
    last_rc = entry; last_nrc = n;
    return entry + 1;
}

static uint32_t *translate_block(pce_t *p, uint32_t pc)
{
    unsigned slot = pc >> 13;
    const uint8_t *pg = p->rmap[slot];
    int is_ram = p->mpr[slot] >= 0x80;
    blk_ram = is_ram;
    ram_b0 = is_ram ? (pc & 0x1FFF) : ~0u;
    if (!is_ram && !tr_twin) *cp++ = 0;      /* where its instructions start (see below) */
    uint32_t *start = cp;
    ntwm = 0;
    tr_p = p;
    blk_slot = slot;
    cur_pg = pg;
    nstub = 0; npend = 0; nchain = 0; niofix = 0; cur_prefix = 0; blk_total = 0; nrcx = 0; insn_io = 0;
    unsigned off = pc & 0x1FFF;
    unsigned lpc = pc;

    /* checked entry (start): budget test, whose delay slot is the plain
       entry (start + 4) */
    uint32_t *b_chk = BLEZ(RCY);
    if (!supported(pg[off]) || off + 7 > 0x1FFF) {
        /* first instruction needs the interpreter */
        ORI(V0, ZERO, pc);
        /* at the end of the page every instruction is a check point (the
           interpreter tests the budget before each one there), however the
           code got here: the plain entry tests too */
        uint32_t *b_chk2 = NULL;
        if (off + 7 > 0x1FFF) { b_chk2 = BLEZ(RCY); NOP(); }
        J_(interp_common); NOP();
        cold_begin(b_chk);
        J_(loop_check); NOP();
        cold_end();
        if (b_chk2) { cold_begin(b_chk2); J_(loop_check); NOP(); cold_end(); }
        blk_total = 0;
        cold_flush();
        last_nrc = 0;
        return start + 1;
    }
    uint32_t *cyc_patch = cp;
    NOP();                               /* addiu s7,s7,-N patched at the end */
    cold_begin(b_chk);                   /* out of budget: undo that and leave */
    fix_rest(1, 0); ADDIU(RCY, RCY, 0);
    LI(V0, pc); J_(loop_check); NOP();
    cold_end();
    {
        /* the budget runs out inside this block: interpret it, so that the
           CPU stops at the exact instruction (interrupts are taken on time) */
        if (!tr_twin) {
            uint32_t *b_step = BLTZ(RCY); NOP();
            uint32_t *back = cp;
            cold_begin(b_step);
            emit_exact_or_back(back);
            fix_rest(1, 0); ADDIU(RCY, RCY, 0);
            /* ROM code: the block's checked twin (translated on first use,
               then this jal is patched to jump straight to it); RAM code
               may change under a twin: the interpreter */
            if (is_ram) { LI(V0, pc); J_(interp_common); NOP(); }
            else {
                /* (the budget gone right here: stop, no twin needed) */
                uint32_t *b_here = BLEZ(RCY); ORI(V0, ZERO, pc);
                JAL(chain_rt_t); LI(V0, pc);
                patch_to(b_here, cp);
                J_(loop_check); NOP();
            }
            cold_end();
        }
    }
    unsigned total = 0;
    int ninst = 0;
    flag_liveness(pg, off, is_ram, is_ram ? ram_b0 + RAM_BLOCK_MAX : 0x1FFF);
    for (;;) {
        if (off + 7 > 0x1FFF) { emit_goto(lpc, 1, 0); break; }     /* page end: a check point */
        if (ninst >= MAXI || (is_ram && off + 7 - ram_b0 > RAM_BLOCK_MAX)) { emit_goto(lpc, 0, 0); break; }
        unsigned op = pg[off];
        if (!supported(op)) { emit_goto(lpc, 0, 0); break; }
        unsigned md = op_mode[op], k = op_kind[op];
        unsigned len;
        switch (md & 15) {
        case M_IMP: len = 1; break;
        case M_ABS: case M_ABX: case M_ABY: len = 3; break;
        default: len = 2; break;
        }
        /* operands of the special opcodes */
        switch (k) {
        case O_TST: len = (op == 0x93 || op == 0xB3) ? 4 : 3; break;
        case O_BLK: len = 7; break;
        case O_BBR: case O_BBS: len = 3; break;
        case O_BCC_: case O_BRA: case O_BSR: case O_TAM: case O_TMA: case O_ST0: case O_ST1: case O_ST2: len = 2; break;
        case O_JMP: case O_JMPI: case O_JMPX: case O_JSR: len = 3; break;
        default: break;
        }
        unsigned next = (lpc + len) & 0xFFFF;
#ifdef JIT_OPSTATS
        uint32_t *st_cp = cp; int st_ns = nstub;
#endif
        if (!is_ram && ninst > 0 && ntwm < 64) { twm[ntwm].body = cp; twm[ntwm].pc = lpc; twm[ntwm].prefix = total; ntwm++; }
        total += k == O_BLK ? 0 : op_cyc[op];    /* (a transfer counts its own cycles) */
        cur_prefix = total;
        cur_nz_dead = nz_dead[ninst]; cur_v_dead = v_dead[ninst];
        jit_stat_ins++;
        cur_lpc = (lpc + len) & 0xFFFF;
        ninst++;
        int end = K_NORMAL;
        int read = md & 16;
        (void)is_t_alu;

        switch (k) {
        /* ---- loads / stores ---- */
        case O_LDA: emit_load(md, pg, off, RA_); emit_setnz(RA_); break;
        case O_LDX: emit_load(md, pg, off, RX_); emit_setnz(RX_); break;
        case O_LDY: emit_load(md, pg, off, RY_); emit_setnz(RY_); break;
        case O_STA: emit_operand(md, pg, off, 0); emit_store(md, pg, off, RA_); if (store_hazard(md, pg, off)) end = K_END_NOCHECK; break;
        case O_STX: emit_operand(md, pg, off, 0); emit_store(md, pg, off, RX_); if (store_hazard(md, pg, off)) end = K_END_NOCHECK; break;
        case O_STY: emit_operand(md, pg, off, 0); emit_store(md, pg, off, RY_); if (store_hazard(md, pg, off)) end = K_END_NOCHECK; break;
        case O_STZ: emit_operand(md, pg, off, 0); emit_store(md, pg, off, ZERO); if (store_hazard(md, pg, off)) end = K_END_NOCHECK; break;
        /* ---- ALU ---- */
        case O_ORA: case O_AND: case O_EOR:
            if ((md & 15) == M_IMM) {
                unsigned im = pg[off + 1];
                if (k == O_ORA) ORI(RA_, RA_, im); else if (k == O_AND) ANDI(RA_, RA_, im); else XORI(RA_, RA_, im);
            } else {
                emit_operand(md, pg, off, 1);
                if (k == O_ORA) OR(RA_, RA_, T1); else if (k == O_AND) AND(RA_, RA_, T1); else XOR(RA_, RA_, T1);
            }
            emit_setnz(RA_);
            break;
        case O_ADC: case O_SBC: {
            emit_operand(md, pg, off, 1);
            LBU(T2, OFF(jd), FP);
            uint32_t *b_dec = BNE(T2, ZERO); NOP();
            uint32_t *dec_back;
            if (k == O_ADC) {
                ADDU(T3, RA_, T1); ADDU(T3, T3, RC_);
                if (!cur_v_dead) { XOR(T4, RA_, T1); NOR(T4, T4, ZERO); XOR(T5, RA_, T3); AND(T4, T4, T5); ANDI(RV_, T4, 0x80); }
                SRL(RC_, T3, 8);
            } else {
                SUBU(T3, RA_, T1); XORI(T4, RC_, 1); SUBU(T3, T3, T4);
                if (!cur_v_dead) { XOR(T4, RA_, T1); XOR(T5, RA_, T3); AND(T4, T4, T5); ANDI(RV_, T4, 0x80); }
                ANDI(T5, T3, 0xFFFF);  /* borrow shows as high bits set */
                SLTIU(RC_, T5, 0x100);
            }
            ANDI(RA_, T3, 0xFF);
            dec_back = cp;
            emit_setnz(RA_);
            /* decimal mode: rare, out of line */
            cold_begin(b_dec);
            MOVE(A0, RA_); MOVE(A1, T1); MOVE(A2, RC_);
            JAL(k == O_ADC ? (void *)helper_adc_dec : (void *)helper_sbc_dec); NOP();
            ANDI(RA_, V0, 0xFF); SRL(RC_, V0, 8);
            ADDIU(RCY, RCY, -1);
            {
                /* the extra decimal cycle is not in the block's precharge: if
                   the budget now runs out before the block ends, stop after
                   this instruction (every boundary is a check point) */
                uint32_t *b_ok = BGEZ(RCY); NOP();
                LBU(T1, OFF(ev_exact), FP);
                uint32_t *b_ok2 = BEQ(T1, ZERO); NOP();
                int nzd = cur_nz_dead; cur_nz_dead = 0;
                emit_setnz(RA_);
                cur_nz_dead = nzd;
                fix_rest(1, 0); ADDIU(RCY, RCY, 0);
                LI(V0, cur_lpc); J_(loop_check); NOP();
                patch_to(b_ok, cp); patch_to(b_ok2, cp);
            }
            J_(dec_back); NOP();
            cold_end();
            break; }
        case O_CMP: case O_CPX: case O_CPY: {
            int r = k == O_CMP ? RA_ : k == O_CPX ? RX_ : RY_;
            if ((md & 15) == M_IMM) {
                unsigned im = pg[off + 1];
                SLTIU(T4, r, im); XORI(RC_, T4, 1);        /* C: r >= m */
                if (!cur_nz_dead) { ADDIU(T3, r, -(int)im); ANDI(RNZ, T3, 0xFF); }
                break;
            }
            emit_operand(md, pg, off, 1);
            SUBU(T3, r, T1);
            ANDI(T4, T3, 0xFFFF);
            SLTIU(RC_, T4, 0x100);
            if (!cur_nz_dead) ANDI(RNZ, T3, 0xFF);
            break; }
        case O_BIT:
            emit_operand(md, pg, off, 1);
            AND(T2, RA_, T1); ANDI(T3, T1, 0x80); SLL(T3, T3, 8); OR(RNZ, T2, T3);
            ANDI(RV_, T1, 0x40);
            break;
        case O_TSB: case O_TRB:
            emit_operand(md, pg, off, 1);
            AND(T2, RA_, T1); ANDI(T3, T1, 0x80); SLL(T3, T3, 8); OR(RNZ, T2, T3);
            ANDI(RV_, T1, 0x40);
            if (k == O_TSB) OR(T1, T1, RA_);
            else { NOR(T2, RA_, ZERO); AND(T1, T1, T2); }
            emit_store(md, pg, off, T1);
            if (store_hazard(md, pg, off)) end = K_END_NOCHECK;
            break;
        case O_TST: {
            unsigned imm = pg[off + 1];
            if (op == 0x83) LBU(T1, O_RAM + pg[off + 2], FP);
            else if (op == 0xA3) { emit_zp_ea_x(pg[off + 2], RX_); LBU(T1, O_RAM, T0); }
            else if (op == 0x93) emit_rd_const(T1, rd16(pg, off + 2));
            else { LI(T9, rd16(pg, off + 2)); ADDU(T9, T9, RX_); ANDI(T9, T9, 0xFFFF); emit_rd_dyn(T1); }
            ANDI(T2, T1, imm); ANDI(T3, T1, 0x80); SLL(T3, T3, 8); OR(RNZ, T2, T3);
            ANDI(RV_, T1, 0x40);
            break; }
        /* ---- read-modify-write ---- */
        case O_ASL: case O_LSR: case O_ROL: case O_ROR: case O_INC: case O_DEC: {
            emit_operand(md, pg, off, 1);
            switch (k) {
            case O_ASL: SRL(RC_, T1, 7); SLL(T1, T1, 1); ANDI(T1, T1, 0xFF); break;
            case O_LSR: ANDI(RC_, T1, 1); SRL(T1, T1, 1); break;
            case O_ROL: SLL(T1, T1, 1); OR(T1, T1, RC_); SRL(RC_, T1, 8); ANDI(T1, T1, 0xFF); break;
            case O_ROR: SLL(T2, RC_, 8); ANDI(RC_, T1, 1); OR(T1, T1, T2); SRL(T1, T1, 1); break;
            case O_INC: ADDIU(T1, T1, 1); ANDI(T1, T1, 0xFF); break;
            case O_DEC: ADDIU(T1, T1, -1); ANDI(T1, T1, 0xFF); break;
            }
            MOVE(RNZ, T1);
            emit_store(md, pg, off, T1);
            if (store_hazard(md, pg, off)) end = K_END_NOCHECK;
            break; }
        case O_RMB: case O_SMB: {
            unsigned z = pg[off + 1], bit = 1u << ((op >> 4) & 7);
            LBU(T1, O_RAM + z, FP);
            if (k == O_RMB) ANDI(T1, T1, 0xFF & ~bit); else ORI(T1, T1, bit);
            SB(T1, O_RAM + z, FP);
            if (ram_hits(z, 1)) end = K_END_NOCHECK;
            break; }
        case O_ASLA: SRL(RC_, RA_, 7); SLL(RA_, RA_, 1); ANDI(RA_, RA_, 0xFF); emit_setnz(RA_); break;
        case O_LSRA: ANDI(RC_, RA_, 1); SRL(RA_, RA_, 1); emit_setnz(RA_); break;
        case O_ROLA: SLL(RA_, RA_, 1); OR(RA_, RA_, RC_); SRL(RC_, RA_, 8); ANDI(RA_, RA_, 0xFF); emit_setnz(RA_); break;
        case O_RORA: SLL(T2, RC_, 8); ANDI(RC_, RA_, 1); OR(RA_, RA_, T2); SRL(RA_, RA_, 1); emit_setnz(RA_); break;
        case O_INA: ADDIU(RA_, RA_, 1); ANDI(RA_, RA_, 0xFF); emit_setnz(RA_); break;
        case O_DEA: ADDIU(RA_, RA_, -1); ANDI(RA_, RA_, 0xFF); emit_setnz(RA_); break;
        case O_INX: ADDIU(RX_, RX_, 1); ANDI(RX_, RX_, 0xFF); emit_setnz(RX_); break;
        case O_INY: ADDIU(RY_, RY_, 1); ANDI(RY_, RY_, 0xFF); emit_setnz(RY_); break;
        case O_DEX: ADDIU(RX_, RX_, -1); ANDI(RX_, RX_, 0xFF); emit_setnz(RX_); break;
        case O_DEY: ADDIU(RY_, RY_, -1); ANDI(RY_, RY_, 0xFF); emit_setnz(RY_); break;
        /* ---- stack / transfers / flags ---- */
        case O_PHA: emit_push(RA_); if (ram_hits(0x100, 0x100)) end = K_END_NOCHECK; break;
        case O_PHX: emit_push(RX_); if (ram_hits(0x100, 0x100)) end = K_END_NOCHECK; break;
        case O_PHY: emit_push(RY_); if (ram_hits(0x100, 0x100)) end = K_END_NOCHECK; break;
        case O_PLA: emit_pull(RA_); emit_setnz(RA_); break;
        case O_PLX: emit_pull(RX_); emit_setnz(RX_); break;
        case O_PLY: emit_pull(RY_); emit_setnz(RY_); break;
        case O_TAX: MOVE(RX_, RA_); emit_setnz(RX_); break;
        case O_TAY: MOVE(RY_, RA_); emit_setnz(RY_); break;
        case O_TXA: MOVE(RA_, RX_); emit_setnz(RA_); break;
        case O_TYA: MOVE(RA_, RY_); emit_setnz(RA_); break;
        case O_TSX: MOVE(RX_, RS_); emit_setnz(RX_); break;
        case O_TXS: MOVE(RS_, RX_); break;
        case O_SAX: MOVE(T1, RA_); MOVE(RA_, RX_); MOVE(RX_, T1); break;
        case O_SAY: MOVE(T1, RA_); MOVE(RA_, RY_); MOVE(RY_, T1); break;
        case O_SXY: MOVE(T1, RX_); MOVE(RX_, RY_); MOVE(RY_, T1); break;
        case O_CLA: MOVE(RA_, ZERO); break;
        case O_CLX: MOVE(RX_, ZERO); break;
        case O_CLY: MOVE(RY_, ZERO); break;
        case O_CLC: MOVE(RC_, ZERO); break;
        case O_SEC: LI(RC_, 1); break;
        case O_CLV: MOVE(RV_, ZERO); break;
        case O_CLD: SB(ZERO, OFF(jd), FP); break;
        case O_SED: LI(T1, 1); SB(T1, OFF(jd), FP); break;
        case O_SEI: LI(T1, 1); SB(T1, OFF(ji), FP); break;
        case O_CLI:
            SB(ZERO, OFF(ji), FP);
            if (pg[off + 1] == 0x78) { end = K_END_CHECK; break; }      /* CLI SEI: at once */
            {   /* a pending interrupt is taken after the next instruction (pce_irq_soon) */
                LBU(T0, OFF(irq_lines), FP);
                uint32_t *b_none = BEQ(T0, ZERO); NOP();
                emit_io_pre(); MOVE(A0, FP); JAL(pce_irq_soon); NOP(); emit_io_post();
                patch_to(b_none, cp);
            }
            insn_io = 1;
            break;
        case O_NOP: break;
        /* ---- HuC6280 specials ---- */
        case O_TAM:
            {
                unsigned m = pg[off + 1];
                if (m) {
                    /* the bank's mappings come from the per-bank tables */
                    SLL(T0, RA_, 2); ADDU(T0, T0, FP);
                    LW(T1, OFF(bank_rmap), T0); LW(T2, OFF(bank_wmap), T0); LW(T3, OFF(bank_jkey), T0);
                    for (int i = 0; i < 8; i++) if (m & (1u << i)) {
                        SB(RA_, OFF(mpr) + i, FP);
                        SW(T1, OFF(rmap) + 4 * i, FP); SW(T2, OFF(wmap) + 4 * i, FP); SW(T3, OFF(jkey) + 4 * i, FP);
                    }
                    ADDIU(T1, ZERO, -1); SW(T1, OFF(idle_yes), FP);
                    if (m & 0x80) SB(ZERO, OFF(vec_ok), FP);
                }
            }
            /* only a remap of this block's own page has to leave the block */
            if (pg[off + 1] & (1u << blk_slot)) end = K_END_NOCHECK;
            break;
        case O_PHP:
            ANDI(T1, RNZ, 0x8080); SLTU(T1, ZERO, T1); SLL(T1, T1, 7);
            SLTU(T2, ZERO, RV_); SLL(T2, T2, 6); OR(T1, T1, T2);
            LBU(T2, OFF(jd), FP); SLL(T2, T2, 3); OR(T1, T1, T2);
            LBU(T2, OFF(ji), FP); SLL(T2, T2, 2); OR(T1, T1, T2);
            ANDI(T2, RNZ, 0xFF); SLTIU(T2, T2, 1); SLL(T2, T2, 1); OR(T1, T1, T2);
            OR(T1, T1, RC_); ORI(T1, T1, 0x10);
            emit_push(T1);
            if (ram_hits(0x100, 0x100)) end = K_END_NOCHECK;
            break;
        case O_PLP: case O_RTI: {
            emit_pull(T1);
            /* NZ: Z set -> N ? 0x8000 : 0; Z clear -> N ? 0x81 : 1 */
            ANDI(T2, T1, 0x80); ANDI(T3, T1, 0x02);
            uint32_t *bz = BEQ(T3, ZERO); ORI(RNZ, T2, 1);
            SLL(RNZ, T2, 8);
            patch_to(bz, cp);
            ANDI(RC_, T1, 1); ANDI(RV_, T1, 0x40);
            SRL(T2, T1, 3); ANDI(T2, T2, 1); SB(T2, OFF(jd), FP);
            SRL(T2, T1, 2); ANDI(T2, T2, 1); SB(T2, OFF(ji), FP);
            if (k == O_RTI) {
                SB(ZERO, OFF(jpin), FP);
                emit_pull(T1); emit_pull(T2);
                SLL(T2, T2, 8); OR(V0, T1, T2);
                J_(loop_check); NOP();
                end = -1;
            } else end = K_END_CHECK;
            break; }
        case O_TMA: {
            unsigned m = pg[off + 1], s = 0;
            while (s < 8 && !(m & (1u << s))) s++;
            if (s < 8) LBU(RA_, OFF(mpr) + s, FP);
            break; }
        case O_BLK:
            if (ninst > 1) {
                /* the transfer's cycles are not in the block's precharge: the
                   budget may be gone before it (every boundary is a check point) */
                fix_rest(1, 0); ADDIU(T0, RCY, 0);
                uint32_t *b_gone = BLEZ(T0); NOP();
                uint32_t *back = cp;
                cold_begin(b_gone);
                emit_exact_or_back(back);
                fix_rest(1, 0); ADDIU(RCY, RCY, 0);
                LI(V0, lpc); J_(loop_check); NOP();
                cold_end();
            }
            /* in C (cpu_blk), operands read when it runs; then a check point */
            emit_sync_out();
            emit_io_pre();
            MOVE(A0, FP); LI(A1, lpc); JAL(cpu_blk); NOP();
            emit_sync_in();
            emit_io_post();
            if (is_ram_blk && nrcx < 8) { rcx0[nrcx] = off + 1; rcx1[nrcx] = off + 7; nrcx++; }   /* operands: not checked on entry */
            end = K_END_CHECK;
            break;
        case O_ST0: case O_ST1: case O_ST2: {
            unsigned a = k == O_ST0 ? 0 : k == O_ST1 ? 2 : 3;
            uint32_t *rt = io_route(a);
#ifdef PCE_VERIFY
            LI(T7, cur_lpc); SW(T7, OFF(dbg_pc), FP);
#endif
            ORI(A1, ZERO, a); ORI(A2, ZERO, pg[off + 1]);
            JAL(rt ? rt : io_slow_rt); io_fix_here(); ORI(T8, ZERO, 0);
            insn_io = 1;
            break; }
        /* ---- control flow: these end the block ---- */
        case O_BCC_: case O_BRA: case O_BBR: case O_BBS: {
            int o = (int8_t)pg[off + (len - 1)];
            unsigned target = (next + o) & 0xFFFF;
            int bshort = o < 0 && o >= -10;
            uint32_t *b_not = NULL;
            if (k == O_BRA) {
                /* always taken */
            } else if (k == O_BBR || k == O_BBS) {
                LBU(T1, O_RAM + pg[off + 1], FP);
                ANDI(T1, T1, 1u << ((op >> 4) & 7));
                b_not = (k == O_BBR) ? BNE(T1, ZERO) : BEQ(T1, ZERO);
                NOP();
            } else {
                int r;
                switch (op >> 6) {
                case 0: ANDI(T1, RNZ, 0x8080); r = T1; break;
                case 1: r = RV_; break;
                case 2: r = RC_; break;
                default: ANDI(T1, RNZ, 0xFF); r = T1; break;
                }
                /* condition true when (flag != 0) == bit5; for Z the register holds !Z */
                int want_set = (op >> 5) & 1;
                if ((op >> 6) == 3) want_set = !want_set;
                b_not = want_set ? BEQ(r, ZERO) : BNE(r, ZERO);
                NOP();
            }
            /* patch the cycle count before the taken path adds its own */
            emit_take(target, (next - len) & 0xFFFF, len, bshort);
            /* not taken: the block goes on */
            if (b_not) patch_to(b_not, cp);
            /* ROM: the block ends (continuing it past the branch duplicates
               code that is also translated from the branch targets: more
               I-cache misses). RAM: it goes on, so the code is checked on
               entry once for the whole stretch, not once per branch. */
#ifndef JIT_SUPERBLOCK
#ifndef JIT_RAM_SUPER
#define JIT_RAM_SUPER 0      /* measured slower: the longer blocks miss more */
#endif
            if (!(JIT_RAM_SUPER && blk_ram)) {
                if (b_not) emit_goto(next, 0, 0);
                end = -1;
            } else if (!b_not) end = -1;
#else
            if (!b_not) end = -1;
#endif
            break; }
        case O_JMP: {
            unsigned t = rd16(pg, off + 1);
            if (t == lpc) {
                LW(T0, OFF(idle_skip), FP);
                uint32_t *b = BEQ(T0, ZERO); NOP();
                /* idle: the interpreter zeroes the budget, then takes the
                   JMP's 4 cycles, ending 4 over (Salamander's IRQ timing
                   depends on it) */
                fix_rest(-1, -4); ADDIU(RCY, ZERO, 0);
                patch_to(b, cp);
            }
            emit_goto(t, 1, 0);
            end = -1;
            break; }
        case O_JMPI: case O_JMPX: {
            LI(T9, rd16(pg, off + 1));
            if (k == O_JMPX) { ADDU(T9, T9, RX_); ANDI(T9, T9, 0xFFFF); }
            emit_rd_dyn(T1);
            SW(T1, 44, SP);
            ADDIU(T9, T9, 1); ANDI(T9, T9, 0xFFFF);
            emit_rd_dyn(T1);
            LW(T2, 44, SP);
            SLL(T1, T1, 8); OR(V0, T1, T2);
            J_(loop_check); NOP();
            end = -1;
            break; }
        case O_JSR: case O_BSR: {
            unsigned ret = (next - 1) & 0xFFFF;
            JAL(push16_rt); ORI(T1, ZERO, ret);
            unsigned t = (k == O_JSR) ? rd16(pg, off + 1) : (next + (int8_t)pg[off + 1]) & 0xFFFF;
            emit_goto(t, 1, 0);
            end = -1;
            break; }
        case O_RTS:
            J_(rts_rt); NOP();
            end = -1;
            break;
        default:
            /* should not happen: supported() lets through only the cases above */
            emit_goto(lpc, 0, 0);
            end = -1;
            total -= op_cyc[op];
            break;
        }
        (void)read;
#ifdef JIT_OPSTATS
        jst_n[op]++; jst_hot[op] += (cp - st_cp) * 4; jst_cold[op] += (nstub - st_ns) * 4;
#endif
        lpc = next;
        off += len;
        if (tr_twin && end == K_NORMAL) {
            /* twin: stop at the first boundary where the budget is gone (the
               twin's shared exit: t0 = budget - 1, v0 = PC, ra = back) */
            fix_rest(1, -1); ADDIU(T0, RCY, 0);
            uint32_t *b_gone = BLTZAL(T0); ORI(V0, ZERO, lpc);
            patch_to(b_gone, twin_out);
        } else if (insn_io && end == K_NORMAL) {
            /* I/O may have cut the budget (an earlier event, a pending
               interrupt): if it now runs out before the block ends, leave */
            uint32_t *b_cut = BLTZ(RCY); NOP();
            uint32_t *back = cp;
            cold_begin(b_cut);
            emit_exact_or_back(back);
            fix_rest(1, 0); ADDIU(RCY, RCY, 0);
            {
                /* the budget gone: a check point. Else (it ends a cycle into
                   the next instruction: a pending interrupt is taken after
                   that one, see pce_update_irq) on without the check */
                uint32_t *b_gone = BLEZ(RCY); LI(V0, lpc);
                J_(loop_nocheck); NOP();
                patch_to(b_gone, cp);
            }
            J_(loop_check); NOP();
            cold_end();
        }
        insn_io = 0;
        if (end == K_END_CHECK) { LI(V0, lpc); J_(loop_check); NOP(); break; }
        if (end == K_END_NOCHECK) {
            /* after TAM the page may map elsewhere: always go through the
               dispatcher (with the budget test when the event needs the exact
               instruction) */
            LI(V0, lpc);
            uint32_t *b_gone = BLEZ(RCY); NOP();
            uint32_t *back = cp;
            cold_begin(b_gone);
            emit_exact_or_back(back);
            J_(loop_check); NOP();
            cold_end();
            J_(loop_nocheck); NOP();
            break;
        }
        if (end == -1) break;
    }
    *cyc_patch = I(0x09, RCY, RCY, -(int)total);
    blk_total = total;
    cold_flush();
    last_nrc = 0;
    if (is_ram) return emit_ram_check(pg, ram_b0, off, pc, start + 1);
    if (!tr_twin && ntwm && !pinning) {
        /* Where each later instruction starts and the cycles from there on:
           a run that stopped inside the block's twin resumes in the block
           itself (jit_lookup). Kept with the cold stubs. */
        int ok = 1;
        for (int i = 0; i < ntwm; i++)
            if (((twm[i].pc - pc) & 0xFFFF) > 255 || twm[i].body - start > 4095 || total - twm[i].prefix > 4095) ok = 0;
        if (ok) {
            uint32_t *info = cold_alloc(ntwm + 1);
            info[0] = ntwm;
            for (int i = 0; i < ntwm; i++)
                info[i + 1] = ((twm[i].pc - pc) & 0xFF) << 24 | (uint32_t)(twm[i].body - start) << 12 | (total - twm[i].prefix);
            start[-1] = (uint32_t)(uintptr_t)info;
        }
    }
    return start + 1;
}

/* Translate the block at pc (or its twin) at the next place the window
   allows: tried right where the hot code stands, and again past the window's
   image when it turns out to reach into it. */
static uint32_t *translate_placed(pce_t *p, uint32_t pc, int twin, uint32_t **bodyp)
{
    for (int tries = 0;; tries++) {
        cp = block_place_n(cp, tries ? BLOCK_MAX_BYTES : 64);
        if (ccp - cp < 4096) {
            /* next 64 KB page */
            uint32_t *np = (uint32_t *)(((uintptr_t)cp + 65535) & ~(uintptr_t)65535);
            if (np + PAGE_WORDS > code_end) jit_flush();
            else { cp = np; ccp = np + PAGE_WORDS; page_begin(); }
            cp = block_place_n(cp, tries ? BLOCK_MAX_BYTES : 64);
        }
        uint32_t *body = cp, *ccp0 = ccp, *g0 = gap_cur, *g1 = gap_end; int gk = gap_k;
        last_nstub = 0; last_nrc = 0;
        if (twin) {
            /* the exit shared by the twin's per-instruction tests */
            twin_out = cp;
            LBU(T1, OFF(ev_exact), FP);
            { uint32_t *b_ex = BNE(T1, ZERO); NOP();
              JR(RA); NOP();
              patch_to(b_ex, cp); }
            ADDIU(RCY, T0, 1);
            J_(loop_check); NOP();
            tr_twin = 1;
        }
        uint32_t *code = translate_block(p, pc);
        tr_twin = 0;
        if (tries || place_ok(body, (uint8_t *)cp - (uint8_t *)body)) { *bodyp = body; return code; }
        jit_stat_replace++;
        cp = body; ccp = ccp0; gap_cur = g0; gap_end = g1; gap_k = gk;
    }
}
static int pinning_ram_rc(void) { return last_nrc != 0; }
#ifdef JIT_TMAP
#include <libdragon.h>
/* translation map for profiling: where each block's code went */
static struct { uint32_t pc, key, *b, *e, *rc; uint16_t nrc; } tmap[16384]; static int ntmap;
static void tmap_add(uint32_t pc, uint32_t key, uint32_t *b, uint32_t *e)
{
    if (ntmap < 16384) { tmap[ntmap].pc = pc; tmap[ntmap].key = key; tmap[ntmap].b = b; tmap[ntmap].e = e; tmap[ntmap].rc = last_nrc ? last_rc : NULL; tmap[ntmap].nrc = last_nrc; ntmap++; }
}
void jit_tmap_dump(void)
{
    for (int i = 0; i < ntmap; i++) {
        debugf("TMAP %04lx %08lx %08lx %08lx\n", (unsigned long)tmap[i].pc, (unsigned long)tmap[i].key, (unsigned long)tmap[i].b, (unsigned long)tmap[i].e);
        if (tmap[i].rc) debugf("TMAP %04lx %08lx %08lx %08lx rc\n", (unsigned long)tmap[i].pc, (unsigned long)tmap[i].key, (unsigned long)tmap[i].rc, (unsigned long)(tmap[i].rc + tmap[i].nrc));
    }
}
#define TMAP(pc, key, b, e) tmap_add(pc, key, b, e)
#else
#define TMAP(pc, key, b, e) ((void)0)
#endif
uint32_t *jit_translate(pce_t *p, uint32_t pc)
{
    JW("translate", pc);
    pc &= 0xFFFF;
    unsigned slot = pc >> 13;
    uint32_t key = p->jkey[slot];
    if (!key) { p->PC = pc; return interp_mem; }
    unsigned idx = ((key >> 9) ^ pc) & (TAB_SIZE - 1);   /* same hash as the dispatcher */
    key |= pc;
    if (tab[idx].key == key && tab[idx].code[-1] != MID_MAGIC) return tab[idx].code;
    if (!jit_force) {
        uint32_t *c = all_find(key);
        if (c) { jit_stat_refill++; tab[idx].key = key; tab[idx].code = c; return c; }
    }
    if (p->mpr[slot] >= 0x80 && ram_inval_n[pc & 0x1FFF] >= RAM_INVAL_MAX) { p->PC = pc; return interp_mem; }
    if (!jit_force) {
        uint8_t *h = &heat[idx & 4095];
        if (*h < jit_hot) { (*h)++; p->PC = pc; return interp_mem; }
    }
    jit_stat_tr++;
    JT("  translate %04lx key %08lx\n", pc, key);
    EMUX_BEGIN(PS_SPR);
    uint32_t *body;
    uint32_t *code = translate_placed(p, pc, 0, &body);
    unsigned g0 = gen_count;
    EMUX_END(PS_SPR);
    JT("   -> %p body %p..%p\n", code, body, cp);
    jit_stat_hot += (uint8_t *)cp - (uint8_t *)body; jit_stat_cold += last_nstub * 4; jit_stat_hotb += (uint8_t *)cp - (uint8_t *)body + last_nstub * 4;
    if (JIT_PIN_BYTES && p->jpin && !pinning && g0 == gen_count) {
        /* in the timer handler: translate again into the pinned region if it fits */
        unsigned hot = (cp - body) + (pinning_ram_rc() ? last_nrc : 0), cold = last_nstub + (pinning_ram_rc() ? 0 : last_nrc);
        if (pin_cp + hot + 8 <= pin_end && pin_ccp - cold - 8 >= pin_cold_lo) {
            uint32_t *scp = cp, *sccp = ccp;
            cp = pin_cp; ccp = pin_ccp; pinning = 1;
            last_nstub = 0; last_nrc = 0;
            uint32_t *pbody = cp;
            code = translate_block(p, pc);
            pinning = 0;
            flush_icache(pbody, (uint8_t *)cp - (uint8_t *)pbody);
            if (last_nstub) flush_icache(last_stub, last_nstub * 4);
            if (last_nrc) flush_icache(last_rc, last_nrc * 4);
            TMAP(pc, key, pbody, cp);
            pin_cp = cp; pin_ccp = ccp;
            cp = scp; ccp = sccp;
            jit_stat_pin++;
            tab[idx].key = key;
            tab[idx].code = code;
            all_put(key, code);
            return code;
        }
    }
    flush_icache(body, (uint8_t *)cp - (uint8_t *)body);
    if (last_nstub) flush_icache(last_stub, last_nstub * 4);
    if (last_nrc) flush_icache(last_rc, last_nrc * 4);
    TMAP(pc, key, body, cp);
    tab[idx].key = key;
    tab[idx].code = code;
    all_put(key, code);
    return code;
}

/* the dispatcher's miss: as jit_translate, but a PC inside a translated twin
   (a run that stopped at an exact instruction resumes there) runs the rest of
   the twin until it has come up often enough to deserve its own block */
uint32_t *jit_lookup(pce_t *p, uint32_t pc)
{
    pc &= 0xFFFF;
    unsigned slot = pc >> 13;
    uint32_t key = p->jkey[slot];
    if (key && p->mpr[slot] < 0x80 && nmid) {
        unsigned idx = ((key >> 9) ^ pc) & (TAB_SIZE - 1);
        key |= pc;
        if (tab[idx].key != key && !all_find(key)) {
            uint32_t *m = mid_find(key);
            if (m && ((uintptr_t)m & 1)) {
                /* first use: the entry. The budget gone here: stop (when the
                   event needs the exact instruction). Else precharge the
                   cycles from there on and go on in the block, or in its
                   twin when the budget ends before the block does. */
                cp = block_place_n(cp, 96);
                if (ccp - cp < 64) return jit_translate(p, pc);
                uint32_t *rec = (uint32_t *)((uintptr_t)m & ~(uintptr_t)1);
                *cp++ = MID_MAGIC;                  /* marks it (jit_translate) */
                m = cp;
                uint32_t *b1 = BLEZ(RCY); NOP();
                uint32_t *go = cp;
                ADDIU(RCY, RCY, -(int)rec[2]);
                uint32_t *b2 = BLTZ(RCY); NOP();
                uint32_t *gon = cp;
                J_((uint32_t *)(uintptr_t)rec[1]); NOP();
                patch_to(b1, cp);
                LBU(T1, OFF(ev_exact), FP);
                { uint32_t *bn = BEQ(T1, ZERO); NOP(); patch_to(bn, go); }
                J_(loop_check); ORI(V0, ZERO, pc);
                patch_to(b2, cp);
                LBU(T1, OFF(ev_exact), FP);
                { uint32_t *bn = BEQ(T1, ZERO); NOP(); patch_to(bn, gon); }
                J_((uint32_t *)(uintptr_t)rec[0]); NOP();
                flush_icache(m - 1, (uint8_t *)cp - (uint8_t *)(m - 1));
                mid_put(key, m);
            }
            if (m) {
                jit_stat_mid++;
                /* the dispatcher finds it from now on (jit_translate, for
                   chained jumps, passes over it) */
                tab[idx].key = key; tab[idx].code = m;
                return m;
            }
        }
    }
    return jit_translate(p, pc);
}

#ifdef JIT_WATCH
void jit_dump(pce_t *p, uint32_t pc)
{
    uint32_t *c = jit_translate(p, pc) - 1;
    debugf("DUMP %04lx at %p\n", pc, c);
    for (int i = 0; i < 96; i++) debugf("D %08lx %08lx\n", (unsigned long)(c + i), c[i]);
    uint32_t *s = c + 1 + (int16_t)(c[0] & 0xFFFF);
    for (int i = 0; i < 64; i++) debugf("S %08lx %08lx\n", (unsigned long)(s + i), s[i]);

}
#endif

void jit_flush(void)
{
    jit_stat_flush++;
    memset(tab, 0, sizeof tab);
    memset(allb, 0, sizeof allb); nall = 0;
    memset(ram_inval_n, 0, sizeof ram_inval_n);
    memset(heat, 0, sizeof heat);
    memset(midb, 0, sizeof midb); nmid = 0;
    cp = blocks_start;
    ccp = (uint32_t *)(((uintptr_t)cp + 65535) & ~(uintptr_t)65535);
    pin_cp = code_buf + rt_bytes / 4; pin_end = code_buf + resv_bytes / 4;
    pin_ccp = ccp; pin_cold_lo = ccp - PIN_COLD_BYTES / 4;
    if (JIT_PIN_BYTES) ccp = pin_cold_lo;
    if (ccp - cp < 8192) { cp = ccp; ccp += PAGE_WORDS; }
    page_begin();
    gen_count++;
}

/* A RAM block found its code changed: translate the new code (or, for code
   that keeps changing, leave it to the interpreter) and send the old entry,
   which chained jumps may still reach, to the replacement. */
uint32_t *jit_ram_inval(pce_t *p, uint32_t pc, uint32_t *entry)
{
    jit_stat_inval++;
    pc &= 0xFFFF;
    JT("  inval %04lx entry %p n=%d\n", pc, entry, ram_inval_n[pc & 0x1FFF]);
    unsigned off = pc & 0x1FFF;
    if (ram_inval_n[off] < 255) ram_inval_n[off]++;
    uint32_t k0 = p->jkey[pc >> 13];
    unsigned idx = ((k0 >> 9) ^ pc) & (TAB_SIZE - 1);
    if (tab[idx].key == (k0 | pc)) tab[idx].key = 0;
    unsigned g = gen_count;
    jit_force = 1;
    uint32_t *code = jit_translate(p, pc);
    jit_force = 0;
    if (g != gen_count) return code;            /* flushed: the old entry is gone */
    if (code == interp_mem) {
        all_put(k0 | pc, NULL);                 /* not the old block either */
        /* entry[0] is the delay slot of the checked entry's test: keep it */
        entry[1] = (0x02u << 26) | (((uint32_t)interp_common >> 2) & 0x3FFFFFF);
        entry[2] = I(0x0D, ZERO, V0, pc);       /* ori v0, zero, pc (delay slot) */
    } else {
        entry[1] = (0x02u << 26) | (((uint32_t)code >> 2) & 0x3FFFFFF);
        entry[2] = 0;
    }
    flush_icache(entry, 12);
    return code;
}

/* the checked twin of the ROM block at pc (NULL: use the interpreter) */
static uint32_t *jit_translate_twin(pce_t *p, uint32_t pc)
{
    pc &= 0xFFFF;
    unsigned slot = pc >> 13;
    uint32_t key = p->jkey[slot];
    if (!key || p->mpr[slot] >= 0x80) return NULL;
    key = (key | pc) | 0x80000000u;
    uint32_t *c = all_find(key);
    if (c) return c;
    jit_stat_tr++;
    uint32_t *body;
    uint32_t *code = translate_placed(p, pc, 1, &body);
    if (ntwm) {
        /* for a run that stopped at one of the twin's later instructions to
           resume: where that instruction is in the block itself and in the
           twin, and the cycles from there on (the entry is made on first
           use, jit_lookup) */
        uint32_t k0 = key & 0x7FFFFFFFu & ~0xFFFFu;
        uint32_t *nb = all_find((key & 0x7FFFFFFFu)), *info = nb ? (uint32_t *)(uintptr_t)nb[-2] : NULL;
        uint32_t *rec = cold_alloc(3 * ntwm);
        for (int i = 0; i < ntwm; i++, rec += 3) {
            unsigned rest = blk_total - twm[i].prefix;
            rec[0] = (uint32_t)(uintptr_t)twm[i].body; rec[1] = rec[0]; rec[2] = rest;
            if (info)
                for (unsigned j = 1; j <= info[0]; j++)
                    if ((info[j] >> 24) == ((twm[i].pc - pc) & 0xFF) && (info[j] & 0xFFF) == rest)
                        rec[1] = (uint32_t)(uintptr_t)((nb - 1) + ((info[j] >> 12) & 0xFFF));
            mid_put(k0 | twm[i].pc, (uint32_t *)((uintptr_t)rec | 1));
        }
    }
    TMAP(pc | 0x10000, key, body, cp);
    jit_stat_tw++; jit_stat_twb += (uint8_t *)cp - (uint8_t *)body + last_nstub * 4;
    flush_icache(body, (uint8_t *)cp - (uint8_t *)body);
    if (last_nstub) flush_icache(last_stub, last_nstub * 4);
    all_put(key, code);
    return code;
}

uint32_t *jit_chain(pce_t *p, uint32_t pc, uint32_t *jins, uint32_t chk)
{
    JT("  chain %04lx at %p\n", pc, jins);
    unsigned g = gen_count;
    if (chk == 2) {
        uint32_t *code = jit_translate_twin(p, pc);
        if (!code) return interp_common;       /* (v0 = pc) */
        code -= 1;                             /* its checked entry: the budget may end right here */
        if (g == gen_count) {
            *jins = (0x02u << 26) | (((uint32_t)code >> 2) & 0x3FFFFFF);
            flush_icache(jins, 4);
        }
        return code;
    }
    uint32_t *code = jit_translate(p, pc);
    if (code == interp_mem) return chk ? loop_check : interp_mem;
    if (chk) code -= 1;                        /* the checked entry */
    if (g == gen_count) {
        *jins = (0x02u << 26) | (((uint32_t)code >> 2) & 0x3FFFFFF);
        flush_icache(jins, 4);
    }
    return code;
}

static int jit_ready;
/* a bigger code buffer (64 KB aligned) means fewer flushes and re-translation */
int jit_init_buffer(void *buf, unsigned bytes)
{
    code_buf = buf; code_size = bytes;
    return jit_init();
}

unsigned jit_rt_bytes(void) { return rt_bytes; }
int jit_init(void)
{
    emit_runtime();
    code_end = code_buf + code_size / 4;
    flush_icache(code_buf, (uint8_t *)cp - (uint8_t *)code_buf);
    jit_flush();
    jit_ready = 1;
    return 0;
}

PCE_HOT1 __attribute__((noinline)) int jit_run(pce_t *p, int cycles)
{
    if (!jit_ready || !p->speed) return cpu_run(p, cycles);
    if (p->bx_n) {
        /* a block transfer goes on first (interrupts wait for its end) */
        p->cyc_left = cycles;
        cpu_bx_continue(p);
        if (p->bx_n || p->cyc_left <= 0) {
            int u = cycles - p->cyc_left + p->cycles_extra;
            p->cycles_extra = 0;
            return u;
        }
        cycles = p->cyc_left;     /* (the run is measured from chunk_start below) */
    }
    j_from_p(p);
    p->jpin = 0;
    p->jexit = 0;
#ifdef PCE_VTRACE
    if (p->vtrace) debugf("JIT entry pc %04x irq %x ji %d vec_ok %d irqvec %04x %04x %04x mpr7 %02x cycles %d\n", p->PC, p->irq_lines, p->ji, p->vec_ok, p->irqvec[0], p->irqvec[1], p->irqvec[2], p->mpr[7], cycles);
#endif
    int (*fn)(pce_t *, int) = (int (*)(pce_t *, int))entry_fn;
    int left = fn(p, cycles);
#ifdef PCE_VTRACE
    if (p->vtrace) debugf("JIT exit pc %04x left %d S %02x\n", p->PC, left, p->S);
#endif
    p_from_j(p);
    /* the run may have gone on past its budget (events handled in the
       runtime): measure from where it started */
    int used = p->chunk_end - left - p->chunk_start + p->cut_delta;
    if (p->jexit && left > 0) {
        /* the speed changed under the fallback: finish with the interpreter
           (cpu_run also takes the pending I/O penalties) */
        p->jexit = 0;
        /* cpu_run at low speed counts the budget's remainder (left & 3) as
           used; the interpreter's own CSL rescale does not */
        int rem = p->speed ? 0 : (left & 3);
        return used + cpu_run(p, left) - rem;
    }
    used += p->cycles_extra;
    p->cycles_extra = 0;
    return used;
}
#endif

#if !(defined(__mips__) && defined(PCE_JIT))
/* the frontend's statistics read these in every build */
unsigned jit_stat_tr, jit_stat_flush, jit_stat_pin, jit_stat_refill, jit_stat_inval, jit_stat_mid;
#endif
