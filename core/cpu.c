/* HuC6280 interpreter, laid out for the VR4300.
 *
 * - The hot loop has to fit the 16 KB instruction cache, so anything rare
 *   (decimal mode, block transfers, I/O, idle checks) lives in out-of-line
 *   cold helpers.
 * - Opcode fetch goes through a pointer to the current 8 KB code page.
 * - Zero page and stack are always the 8 KB RAM (MPR1 = $F8, as every
 *   HuCard sets it); other accesses go through the per-slot map.
 * - Idle loops (spinning on a RAM flag until an interrupt) are detected on
 *   taken backward branches and fast-forwarded to the next event. */
#include "pce.h"
#include "cpu_tab.h"   /* op_kind: used to recognise T-flag targets */
#include "cpu_disp.h"
#ifdef PCE_TRACE
#include <stdio.h>
#endif

#define FN 0x80
#define FV 0x40
#define FT 0x20
#define FB 0x10
#define FD 0x08
#define FI 0x04
#define FZ 0x02
#define FC 0x01

#define COLD __attribute__((noinline, cold))
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

#if !defined(PCE_STEP) && !defined(PCE_RAMRUN)
void cpu_reset(pce_t *p)
{
    p->A = p->X = p->Y = 0;
    p->S = 0xFF;
    p->P = FI | FZ;
    p->speed = 0;
    p->mpr[7] = 0x00;
    for (int i = 0; i < 7; i++) p->mpr[i] = 0xFF;
    for (int i = 0; i < 8; i++) pce_map(p, i);
    p->PC = p->rmap[7][0x1FFE] | (p->rmap[7][0x1FFF] << 8);
    p->halted = 0;
    p->idle_last = 0xFFFFFFFF;
    p->idle_yes = 0xFFFFFFFF;
}
#endif

static COLD uint8_t rd_slow(pce_t *p, unsigned a) { return pce_slow_read(p, (uint16_t)a); }
static COLD void wr_slow(pce_t *p, unsigned a, uint8_t v) { EMUX_BEGIN(PS_IO); pce_slow_write(p, (uint16_t)a, v); EMUX_END(PS_IO); }

static inline uint8_t rd(pce_t *p, uint8_t *const *rmap, unsigned a)
{
    uint8_t *m = rmap[a >> 13];
    if (LIKELY(m)) return m[a & 0x1FFF];
    return rd_slow(p, a);
}
static inline void wr(pce_t *p, uint8_t *const *wmap, unsigned a, uint8_t v)
{
    uint8_t *m = wmap[a >> 13];
    if (LIKELY(m)) m[a & 0x1FFF] = v;
    else wr_slow(p, a, v);
}

/* ---- cold helpers ---- */

static COLD unsigned adc_dec(unsigned a, unsigned m, unsigned *fc)
{
    unsigned lo = (a & 0x0F) + (m & 0x0F) + (*fc ? 1 : 0);
    unsigned hi = (a & 0xF0) + (m & 0xF0);
    if (lo > 9) lo += 6;
    hi += (lo > 0x0F) ? 0x10 : 0;
    if (hi > 0x90) hi += 0x60;
    *fc = hi > 0xFF;
    return (uint8_t)((lo & 0x0F) | (hi & 0xF0));
}

static COLD unsigned sbc_dec(unsigned a, unsigned m, unsigned *fc)
{
    int c = *fc ? 0 : 1;
    int lo = (a & 0x0F) - (m & 0x0F) - c;
    int hi = (a & 0xF0) - (m & 0xF0);
    if (lo < 0) { lo -= 6; hi -= 0x10; }
    if (hi < 0) hi -= 0x60;
    *fc = !(((int)a - (int)m - c) < 0);
    return (uint8_t)((lo & 0x0F) | (hi & 0xF0));
}

/* block transfer: returns cycles */
/* n bytes of a block transfer from src to dst (alt: TIA/TAI alternation) */
static PCE_HOT void block_xfer(pce_t *p, unsigned op, unsigned src, unsigned dst, uint32_t n, int alt)
{
#ifdef BX_LOG
    if (n >= 2048) fprintf(stderr, "BX frame %lu op %02x src %04x dst %04x len %u\n", (unsigned long)p->frame, op, src, dst, n);
#endif
    uint8_t *const *rmap = p->rmap, *const *wmap = p->wmap;
    uint32_t i = 0;
    if (op == 0xE3 && p->mpr[(dst & 0xFFFF) >> 13] == 0xFF && (dst & 0x1C03) == (alt ? 0x0003u : 0x0002u) && n >= 4 + alt && p->vdc.sel == 2) {
        /* TIA into the VDC data port with VWR selected (the usual VRAM upload):
           the first pair goes the normal way (it brings the VDC up to date; the
           whole transfer happens at one frame position), the rest word by word.
           A transfer resumed (or split) in the middle of a word finishes that
           word first. */
        uint32_t e = 2 + alt;
        for (; i < e; i++, alt ^= 1) { wr(p, wmap, dst & 0xFFFF, rd(p, rmap, src & 0xFFFF)); src++; dst += alt ? -1 : 1; }
        while (i + 1 < n) {
            const uint8_t *m = rmap[(src & 0xFFFF) >> 13];
            unsigned o = src & 0x1FFF, w = (n - i) / 2;
            if (m && o < 0x1FFF) {
                /* a stretch within one 8 KB page of the source */
                unsigned room = (0x2000 - o) / 2;
                if (w > room) w = room;
                vdc_vram_words(p, m + o, w);
            } else {
                uint8_t lo = rd(p, rmap, src & 0xFFFF), hi = rd(p, rmap, (src + 1) & 0xFFFF);
                vdc_vram_word(p, lo, hi);
                w = 1;
            }
            src += 2 * w; i += 2 * w;
            pce_penalty(p, 2 * w);
        }
    }
    for (; i < n; i++) {
        wr(p, wmap, dst & 0xFFFF, rd(p, rmap, src & 0xFFFF));
        switch (op) {
        case 0x73: src++; dst++; break;
        case 0xC3: src--; dst--; break;
        case 0xD3: src++; break;
        case 0xE3: src++; dst += alt ? -1 : 1; break;
        case 0xF3: src += alt ? -1 : 1; dst++; break;
        }
        alt ^= 1;
    }
    p->bx_src = src; p->bx_dst = dst; p->bx_alt = alt;
}

/* A long transfer runs over several runs (several frames for a big VRAM
 * upload, as on the real machine): each run does what fits in its budget.
 * Interrupts wait for the end, as they do on the HuC6280. */
#define BX_COST 6
#if !defined(PCE_STEP) && !defined(PCE_RAMRUN)
/* go on with a transfer: p->cyc_left is the budget (master cycles, high speed) */
/* the recompiler's block transfer (TII TDD TIN TIA TAI at pc, operands read
   now: RAM code may change them): as the interpreter does it, budget in
   p->cyc_left (before the instruction), registers in p */
PCE_HOT void cpu_blk(pce_t *p, unsigned pc)
{
    uint8_t *const *rmap = p->rmap;
    unsigned op = rd(p, rmap, pc & 0xFFFF);
    unsigned src = rd(p, rmap, (pc + 1) & 0xFFFF) | rd(p, rmap, (pc + 2) & 0xFFFF) << 8;
    unsigned dst = rd(p, rmap, (pc + 3) & 0xFFFF) | rd(p, rmap, (pc + 4) & 0xFFFF) << 8;
    unsigned len = rd(p, rmap, (pc + 5) & 0xFFFF) | rd(p, rmap, (pc + 6) & 0xFFFF) << 8;
    int sh = p->speed ? 0 : 2;
    unsigned S = p->S;
    p->ram[0x100 | S] = p->Y; S = (S - 1) & 0xFF;
    p->ram[0x100 | S] = p->A; S = (S - 1) & 0xFF;
    p->ram[0x100 | S] = p->X; S = (S - 1) & 0xFF;
    uint32_t n = len ? len : 0x10000, k = n;
    int room = (p->cyc_left >> sh) - 17;
    if ((int)(BX_COST * n) > room) {
        k = room <= 0 ? 1 : (uint32_t)(room + BX_COST - 1) / BX_COST;
        if (k > n) k = n;
    }
    p->cyc_left -= (17 + BX_COST * (int)k) << sh;
    p->bx_op = op; p->bx_n = n - k;
    block_xfer(p, op, src, dst, k, 0);
    S = (S + 1) & 0xFF; p->X = p->ram[0x100 | S];
    S = (S + 1) & 0xFF; p->A = p->ram[0x100 | S];
    S = (S + 1) & 0xFF; p->Y = p->ram[0x100 | S];
    p->S = S;
}

PCE_HOT void cpu_bx_continue(pce_t *p)
{
    int sh = p->speed ? 0 : 2;
    int units = p->cyc_left >> sh;
    uint32_t k = units <= 0 ? 1 : (uint32_t)(units + BX_COST - 1) / BX_COST;
    if (k > p->bx_n) k = p->bx_n;
    p->cyc_left -= (int)(BX_COST * k) << sh;
    p->bx_n -= k;
    block_xfer(p, p->bx_op, p->bx_src, p->bx_dst, k, p->bx_alt);
}
#endif

/* Is the code from `target` up to `br` (the branch) a pure wait on RAM?
 * Allowed: loads/compares/tests of RAM, ROM or immediates. */
static COLD int idle_loop(pce_t *p, unsigned target, unsigned br)
{
#ifdef PCE_COUNT
    { extern uint64_t cnt_idlechk; cnt_idlechk++; }
#endif
    uint8_t *const *rmap = p->rmap;
    unsigned a = target;
    int guard = 0;
    while (a < br && guard++ < 6) {
        unsigned op = rd(p, rmap, a & 0xFFFF);
        unsigned len, ea = 0, mem = 0;
        switch (op) {
        case 0xA9: case 0xA2: case 0xA0: case 0xC9: case 0xE0: case 0xC0: case 0x29: case 0x09: case 0x49: case 0x89:
        case 0xA5: case 0xA6: case 0xA4: case 0xC5: case 0xE4: case 0xC4: case 0x25: case 0x05: case 0x45: case 0x24:
            len = 2; break;
        case 0xAD: case 0xAE: case 0xAC: case 0xCD: case 0xEC: case 0xCC: case 0x2D: case 0x0D: case 0x4D: case 0x2C:
            len = 3; ea = rd(p, rmap, (a + 1) & 0xFFFF) | rd(p, rmap, (a + 2) & 0xFFFF) << 8; mem = 1; break;
        case 0x83: len = 3; break;              /* TST #,zp */
        case 0x93: len = 4; ea = rd(p, rmap, (a + 2) & 0xFFFF) | rd(p, rmap, (a + 3) & 0xFFFF) << 8; mem = 1; break;
        default: return 0;
        }
        if (mem) {
            uint8_t bank = p->mpr[ea >> 13];
            if (bank >= 0x80 && !(bank >= 0xF8 && bank <= 0xFB)) return 0; /* only RAM or ROM */
        }
        a += len;
    }
    return a == br;
}

#if !defined(PCE_STEP) && !defined(PCE_RAMRUN)
int cpu_idle_loop(pce_t *p, unsigned target, unsigned br) { return idle_loop(p, target, br); }
#endif

/* Slow (I/O) accesses publish the exact frame position and pick up a budget
 * cut made by the I/O (a new raster interrupt line, a timer change). */
#ifdef PCE_VERIFY
#define IODBG() (p->dbg_pc = (unsigned)(pc - pbase) & 0xFFFF)
#else
#define IODBG() ((void)0)
#endif
#ifdef PCE_STEP
#define IOPRE()  (IODBG(), p->cyc_left = p->step_base - (1000 - (cyc << sh)))
#define IOPOST() ((void)0)
#else
#define IOPRE()  (IODBG(), p->cyc_left = cyc << sh)
#define IOPOST() (cyc = p->cyc_left >> sh)
#endif
#define RD(a) ({ unsigned _a = (a); uint8_t *_m = rmap[_a >> 13]; unsigned _r; \
        if (LIKELY(_m)) _r = _m[_a & 0x1FFF]; else { IOPRE(); _r = rd_slow(p, _a); IOPOST(); } _r; })
#define WR(a, v) do { unsigned _a = (a); unsigned _v = (v); uint8_t *_m = wmap[_a >> 13]; \
        if (LIKELY(_m)) _m[_a & 0x1FFF] = _v; else { IOPRE(); wr_slow(p, _a, _v); IOPOST(); } } while (0)
#define ZPW(z)   ({ unsigned _z = (uint8_t)(z); ram[_z] | ram[(uint8_t)(_z + 1)] << 8; })
#define PUSH(v)  do { ram[0x100 | S] = (v); S = (uint8_t)(S - 1); } while (0)
#define PULL()   (S = (uint8_t)(S + 1), ram[0x100 | S])

#define SETNZ(v) do { fn = fz = (v); } while (0)
#define GETP() ((fn & 0x80) | (fv ? FV : 0) | (fd ? FD : 0) | (fi ? FI : 0) | ((fz & 0xFF) ? 0 : FZ) | (fc ? FC : 0))
#define SETP(v) do { unsigned _p = (v); fn = _p & 0x80; fv = _p & FV; fd = _p & FD; fi = _p & FI; fz = !(_p & FZ); fc = _p & FC; } while (0)

#define ADC_(m) do { unsigned _m = (m); \
        if (LIKELY(!fd)) { unsigned _s = A + _m + (fc ? 1 : 0); \
            fv = (~(A ^ _m) & (A ^ _s) & 0x80); fc = _s > 0xFF; A = (uint8_t)_s; } \
        else { A = adc_dec(A, _m, &fc); cyc--; } SETNZ(A); } while (0)
#define SBC_(m) do { unsigned _m = (m); \
        if (LIKELY(!fd)) { unsigned _s = A - _m - (fc ? 0 : 1); \
            fv = ((A ^ _m) & (A ^ _s) & 0x80); fc = _s < 0x100; A = (uint8_t)_s; } \
        else { A = sbc_dec(A, _m, &fc); cyc--; } SETNZ(A); } while (0)
#define ORA_(m) do { A |= (m); SETNZ(A); } while (0)
#define AND_(m) do { A &= (m); SETNZ(A); } while (0)
#define EOR_(m) do { A ^= (m); SETNZ(A); } while (0)
#define CMP(r, m) do { unsigned _t = (r) - (uint8_t)(m); fc = _t < 0x100; SETNZ((uint8_t)_t); } while (0)
#define ASL(v) ({ unsigned _v = (v); fc = _v & 0x80; _v = (uint8_t)(_v << 1); SETNZ(_v); _v; })
#define LSR(v) ({ unsigned _v = (v); fc = _v & 1; _v >>= 1; SETNZ(_v); _v; })
#define ROL(v) ({ unsigned _v = (v); unsigned _c = fc ? 1 : 0; fc = _v & 0x80; _v = (uint8_t)((_v << 1) | _c); SETNZ(_v); _v; })
#define ROR(v) ({ unsigned _v = (v); unsigned _c = fc ? 0x80 : 0; fc = _v & 1; _v = (_v >> 1) | _c; SETNZ(_v); _v; })
#define INCV(v) ({ unsigned _v = (uint8_t)((v) + 1); SETNZ(_v); _v; })
#define DECV(v) ({ unsigned _v = (uint8_t)((v) - 1); SETNZ(_v); _v; })

/* ---- program counter: a pointer into the current 8 KB code page ----
 * pbase + logical PC == pc. Instructions starting in the last 7 bytes of a
 * page (which may straddle into the next one) are fetched through a small
 * buffer by `slowfetch`. */
#define LPC() ((unsigned)(pc - pbase) & 0xFFFF)
#ifdef PCE_RAMRUN
#define JUMP_LEAVE(t) (leave |= p->jkey[(t) >> 13])
#else
#define JUMP_LEAVE(t) ((void)0)
#endif
#define JUMP(t) do { unsigned _t = (t) & 0xFFFF; const uint8_t *_m = rmap[_t >> 13]; \
        if (UNLIKELY(!_m)) _m = p->ffpage; \
        JUMP_LEAVE(_t); \
        pstart = _m; pbase = _m - (_t & 0xE000); pc = pbase + _t; pend = _m + 0x1FF9; } while (0)

#ifdef PCE_COUNT
#define COUNT() do { extern uint64_t cnt_ins, cnt_op[256], cnt_bank[256]; extern uint32_t cnt_pc[65536]; cnt_ins++; cnt_op[pc[-1]]++; unsigned _l = LPC() - 1; cnt_bank[p->mpr[(_l & 0xFFFF) >> 13]]++; cnt_pc[_l & 0xFFFF]++; if (p->mpr[(_l & 0xFFFF) >> 13] >= 0xF8 && p->mpr[(_l & 0xFFFF) >> 13] <= 0xFB) { extern uint8_t cnt_ramx[0x2000]; for (int _i = 0; _i < 3; _i++) cnt_ramx[(_l + _i) & 0x1FFF] = 1; } } while (0)
#elif defined(PCE_VTRACE) && !defined(PCE_STEP) && !defined(PCE_RAMRUN)
#include <debug.h>
#define COUNT() do { if (p->vtrace) debugf("T %04x op %02x cyc %d irq %x P %02x S %02x\n", (LPC() - 1) & 0xFFFF, pc[-1], cyc, p->irq_lines, GETP(), S); } while (0)
#else
#define COUNT() do { } while (0)
#endif

#if defined(PCE_STEP)
#define IDLE_ZERO() (*chkp |= 2)      /* the caller zeroes its own budget */
#else
#define IDLE_ZERO() (cyc = 0)
#endif
#if defined(PCE_STEP)
/* one instruction; every instruction boundary is a check point */
#define NEXT do { *chkp |= 1; goto out; } while (0)
#define CHECK do { *chkp |= 1; goto out; } while (0)
#elif defined(PCE_SINGLE_STEP)
/* testing: check the budget and interrupts after every instruction */
#define NEXT goto check
#define CHECK do { } while (0)
#else
/* when the budget ends at a VDC event (or an interrupt that became pending
   cut it), it is tested after every instruction, as the HuC6280 samples its
   interrupt lines after every instruction; for the timer alone only at the
   check points (jumps, taken branches, ...) */
#define NEXT do { if (UNLIKELY(cyc <= 0) && p->ev_exact) goto check; goto next; } while (0)
#define CHECK do { if (UNLIKELY(cyc <= 0)) goto check; } while (0)
#endif

/* taken branch to pc+o; short backward ones are checked for idle loops */
#define TAKE(o, len) do { \
        cyc -= 2; \
        if (UNLIKELY((o) < 0) && (o) >= -10 && p->idle_skip) { \
            unsigned _t = (LPC() + (o)) & 0xFFFF; \
            if (_t == p->idle_yes) IDLE_ZERO(); \
            else if (p->idle_last != _t) { \
                EMUX_BEGIN(PS_IDLE); int _il = idle_loop(p, _t, LPC() - (len)); EMUX_END(PS_IDLE); \
                if (_il) { IDLE_ZERO(); p->idle_yes = _t; } else p->idle_last = _t; } } \
        const uint8_t *_np = pc + (o); \
        if (LIKELY(_np >= pstart && _np < pend)) pc = _np; else JUMP(LPC() + (o)); \
        CHECK; } while (0)

#ifdef PCE_STEP
int cpu_step(pce_t *p, int *chkp)
{
    int cycles = 1000;
    *chkp = 0;
#elif defined(PCE_RAMRUN)
/* recompiler fallback: interpret from p->PC (code the recompiler does not
 * translate: RAM pages, rare instructions) until control passes to a ROM
 * page or the budget runs out at a check point. Starts without a check
 * (the caller may be between check points); p->jchk = 1 when it stopped at
 * a check point, 0 when it stopped after a jump into ROM. */
int cpu_ramrun(pce_t *p, int cycles)
{
    uint32_t leave = 0;
    p->jchk = 0;
#else
int cpu_run(pce_t *p, int cycles)
{
#endif
    static const void *const disp[256] = DISPATCH_TABLE;
    unsigned A = p->A, X = p->X, Y = p->Y, S = p->S;
    unsigned fn, fz, fv, fd, fi, fc;
    SETP(p->P);
#if !defined(PCE_STEP) && !defined(PCE_RAMRUN)
    int pre = 0;
    if (UNLIKELY(p->bx_n)) {
        p->cyc_left = cycles;
        cpu_bx_continue(p);
        if (p->bx_n || p->cyc_left <= 0) {
            int u = cycles - p->cyc_left + p->cycles_extra;
            p->cycles_extra = 0;
            return u;
        }
        pre = cycles - p->cyc_left; cycles = p->cyc_left;
    }
#endif
    uint8_t *const *rmap = p->rmap, *const *wmap = p->wmap;
    uint8_t *ram = p->ram;
    int sh = p->speed ? 0 : 2;
    int cyc = cycles >> sh;
    int adj = 0;
    int tm = 0;
    unsigned ea, v;
    const uint8_t *pc, *pbase, *pstart, *pend;
    uint8_t tmp[8];
    JUMP(p->PC);
#if defined(PCE_STEP) || defined(PCE_RAMRUN)
#ifdef PCE_RAMRUN
    leave = 0;
#endif
    if (UNLIKELY(pc >= pend)) goto slowfetch;
    pc++; goto *disp[pc[-1]];
#else
    goto check;
#endif

#if !defined(PCE_SINGLE_STEP) && !defined(PCE_STEP)
next:
#ifdef PCE_RAMRUN
    if (UNLIKELY(leave)) goto out;
#endif
    /* running on into the next 8 KB page is a check point (straight-line
       code through unmapped memory would otherwise never stop) */
    if (UNLIKELY(pc >= pend)) { CHECK; goto slowfetch; }
    pc++; COUNT(); goto *disp[pc[-1]];
#endif
take2: { int o = (int8_t)*pc++; TAKE(o, 2); NEXT; }
take3: { int o = (int8_t)pc[1]; pc += 2; TAKE(o, 3); NEXT; }

#include "cpu_ops.inc"

    /* ---- the less common instructions ---- */
x_brk: {
        unsigned r = (LPC() + 1) & 0xFFFF; PUSH(r >> 8); PUSH(r & 0xFF); PUSH(GETP() | FB);
        fi = 1; fd = 0; JUMP(RD(0xFFF6) | RD(0xFFF7) << 8); cyc -= 8; CHECK; NEXT; }
x_bsr: {
        int o = (int8_t)*pc++; unsigned r = (LPC() - 1) & 0xFFFF; PUSH(r >> 8); PUSH(r & 0xFF);
        JUMP(LPC() + o); cyc -= 8; CHECK; NEXT; }
x_jmp: {
        unsigned t = pc[0] | pc[1] << 8; pc += 2;
        if (UNLIKELY(t == ((LPC() - 3) & 0xFFFF)) && p->idle_skip) IDLE_ZERO();
        JUMP(t); cyc -= 4; CHECK; NEXT; }
x_jmpi: { unsigned a = pc[0] | pc[1] << 8; cyc -= 7; JUMP(RD(a) | RD((a + 1) & 0xFFFF) << 8); CHECK; NEXT; }
x_jmpx: { unsigned a = ((pc[0] | pc[1] << 8) + X) & 0xFFFF; cyc -= 7; JUMP(RD(a) | RD((a + 1) & 0xFFFF) << 8); CHECK; NEXT; }
x_jsr: {
        unsigned t = pc[0] | pc[1] << 8; pc += 2;
        unsigned r = (LPC() - 1) & 0xFFFF; PUSH(r >> 8); PUSH(r & 0xFF);
        JUMP(t); cyc -= 7; CHECK; NEXT; }
x_rts: { unsigned l = PULL(); unsigned h = PULL(); JUMP((l | h << 8) + 1); cyc -= 7; CHECK; NEXT; }
x_rti: { unsigned pp = PULL(); SETP(pp); unsigned l = PULL(); unsigned h = PULL(); JUMP(l | h << 8); cyc -= 7; }
#if defined(PCE_STEP)
    CHECK;
#else
    goto check;
#endif
x_cli: fi = 0; cyc -= 2;
    /* the I flag change takes effect after the next instruction: a pending
       interrupt is taken once that has run (handlers that CLI before reading
       the VDC status rely on it). CLI SEI still lets a pending one in. */
    if (*pc != 0x78) {
        if (UNLIKELY(p->irq_lines)) { IOPRE(); pce_irq_soon(p); IOPOST(); }
        NEXT;
    }
#if defined(PCE_STEP)
    CHECK;
#else
    goto check;
#endif
x_plp: { unsigned pp = PULL(); SETP(pp); cyc -= 4; }
#if defined(PCE_STEP)
    CHECK;
#else
    goto check;
#endif
x_set: {
        /* T flag: only the next ORA/AND/EOR/ADC is affected */
        unsigned k = op_kind[*pc];
        tm = (k == O_ORA || k == O_AND || k == O_EOR || k == O_ADC);
        cyc -= 2;
        if (UNLIKELY(pc >= pend)) goto slowfetch;
        pc++; COUNT(); goto *disp[pc[-1]]; }
x_csl: x_csh: {
        unsigned op = pc[-1];
        cyc -= 3;
        int nsh = (op == 0xD4) ? 0 : 2;
        if (nsh != sh) {
            int ncyc = (cyc << sh) >> nsh;
            adj += (ncyc << nsh) - (cyc << sh);     /* keep the count exact */
            cyc = ncyc; sh = nsh; p->speed = (op == 0xD4);
#ifdef PCE_RAMRUN
            leave = 1;          /* the recompiler runs at high speed only */
#endif
        }
        CHECK; NEXT; }
x_tam: {
        unsigned m = *pc++;
        for (int i = 0; i < 8; i++) if (m & (1u << i)) { p->mpr[i] = A; pce_map(p, i); }
        p->idle_yes = 0xFFFFFFFF;
        JUMP(LPC());            /* the current page may have been remapped */
        cyc -= 5; NEXT; }
x_tma: {
        unsigned m = *pc++;
        for (int i = 0; i < 8; i++) if (m & (1u << i)) { A = p->mpr[i]; break; }
        cyc -= 4; NEXT; }
x_st0: { unsigned v = *pc++; cyc -= 5; IOPRE(); pce_io_write(p, 0x1FE000, v); IOPOST(); NEXT; }
x_st1: { unsigned v = *pc++; cyc -= 5; IOPRE(); pce_io_write(p, 0x1FE002, v); IOPOST(); NEXT; }
x_st2: { unsigned v = *pc++; cyc -= 5; IOPRE(); pce_io_write(p, 0x1FE003, v); IOPOST(); NEXT; }
x_blk: {
        unsigned op = pc[-1];
        unsigned src = pc[0] | pc[1] << 8, dst = pc[2] | pc[3] << 8, len = pc[4] | pc[5] << 8;
        pc += 6;
        unsigned here = LPC();
        PUSH(Y); PUSH(A); PUSH(X);
        uint32_t n = len ? len : 0x10000, k = n;
        int room = cyc - 17;
        if ((int)(BX_COST * n) > room) {
            /* the rest in later runs: this one takes the whole budget */
            k = room <= 0 ? 1 : (uint32_t)(room + BX_COST - 1) / BX_COST;
            if (k > n) k = n;
        }
        cyc -= 17 + BX_COST * (int)k;
        p->bx_op = op; p->bx_n = n - k;
        IOPRE();
        block_xfer(p, op, src, dst, k, 0);
        IOPOST();
        X = PULL(); A = PULL(); Y = PULL();
        JUMP(here);
        CHECK; NEXT; }

    /* ---- budget and interrupts ---- */
check: __attribute__((unused));
#ifdef PCE_STEP
    goto out;
#endif
#ifdef PCE_RAMRUN
    if (cyc <= 0) { p->jchk = 1; goto out; }
#else
    if (cyc <= 0) goto out;
#endif
    if (UNLIKELY(p->irq_lines) && !fi) {
        unsigned l = p->irq_lines;
        unsigned vec = (l & 4) ? 0xFFFA : (l & 2) ? 0xFFF8 : 0xFFF6;
        unsigned r = LPC();
#if defined(SCROLL_LOG) && !defined(PCE_STEP) && !defined(PCE_RAMRUN)
        if (p->frame >= SCROLL_LOG && p->frame < SCROLL_LOG + 3) {
            int now = p->chunk_end - (cyc << sh);
            fprintf(stderr, "IRQ%u f%lu line %d +%d pc %04x st %02x mpr2 %02x\n", l, (unsigned long)p->frame, now / CYCLES_PER_LINE, now % CYCLES_PER_LINE, r, p->vdc.status, p->mpr[2]);
        }
#endif
        PUSH(r >> 8); PUSH(r & 0xFF);
        PUSH(GETP() & ~FB);
        fi = 1; fd = 0;
        JUMP(RD(vec) | RD(vec + 1) << 8);
        cyc -= 8;
    }
    if (UNLIKELY(pc >= pend)) goto slowfetch;
    pc++; COUNT(); goto *disp[pc[-1]];

slowfetch: {
        unsigned lpc = LPC();
        if ((lpc & 0x1FFF) < 0x1FF9) {
            JUMP(lpc);
        } else {
            /* may straddle two pages: fetch the bytes through the map */
            for (int i = 0; i < 8; i++) tmp[i] = RD((lpc + i) & 0xFFFF);
            pbase = tmp - lpc; pc = tmp; pstart = pend = tmp;
        }
        pc++; COUNT(); goto *disp[pc[-1]];
    }

out:
    p->A = A; p->X = X; p->Y = Y; p->S = S; p->PC = LPC();
    p->P = GETP();
    {
#if defined(PCE_STEP) || defined(PCE_RAMRUN)
        return cycles - (cyc << sh) + adj;     /* I/O penalties stay for the caller */
#else
        int used = pre + cycles - (cyc << sh) + adj + p->cycles_extra;
        p->cycles_extra = 0;
        return used;
#endif
    }
}
