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
}

static COLD uint8_t rd_slow(pce_t *p, unsigned a) { return pce_slow_read(p, (uint16_t)a); }
static COLD void wr_slow(pce_t *p, unsigned a, uint8_t v) { pce_slow_write(p, (uint16_t)a, v); }

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
static COLD int block_xfer(pce_t *p, unsigned op, unsigned src, unsigned dst, unsigned len)
{
    uint32_t n = len ? len : 0x10000;
    int alt = 0;
    uint8_t *const *rmap = p->rmap, *const *wmap = p->wmap;
    for (uint32_t i = 0; i < n; i++) {
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
    return 17 + 6 * (int)n;
}

/* Is the code from `target` up to `br` (the branch) a pure wait on RAM?
 * Allowed: loads/compares/tests of RAM, ROM or immediates. */
static COLD int idle_loop(pce_t *p, unsigned target, unsigned br)
{
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

#define FETCH()  (pcb[PC++])
#define FETCHW() ({ unsigned _l = pcb[PC]; unsigned _h = pcb[PC + 1]; PC += 2; _l | _h << 8; })
#define RD(a)    rd(p, rmap, (a))
#define WR(a, v) wr(p, wmap, (a), (v))
#define ZR(z)    (ram[(uint8_t)(z)])
#define ZW(z, v) (ram[(uint8_t)(z)] = (v))
#define ZPW(z)   ({ unsigned _z = (uint8_t)(z); ram[_z] | ram[(uint8_t)(_z + 1)] << 8; })
#define PUSH(v)  do { ram[0x100 | S] = (v); S = (uint8_t)(S - 1); } while (0)
#define PULL()   (S = (uint8_t)(S + 1), ram[0x100 | S])

#define SETNZ(v) do { fn = fz = (v); } while (0)
#define GETP() ((fn & 0x80) | (fv ? FV : 0) | (fd ? FD : 0) | (fi ? FI : 0) | ((fz & 0xFF) ? 0 : FZ) | (fc ? FC : 0))
#define SETP(v) do { unsigned _p = (v); fn = _p & 0x80; fv = _p & FV; fd = _p & FD; fi = _p & FI; fz = !(_p & FZ); fc = _p & FC; } while (0)

/* effective addresses */
#define EA_ZPX() ((uint8_t)(FETCH() + X))
#define EA_ZPY() ((uint8_t)(FETCH() + Y))
#define EA_ABS() (FETCHW())
#define EA_ABX() ((uint16_t)(FETCHW() + X))
#define EA_ABY() ((uint16_t)(FETCHW() + Y))
#define EA_IZX() (ZPW(FETCH() + X))
#define EA_IZY() ((uint16_t)(ZPW(FETCH()) + Y))
#define EA_IZP() (ZPW(FETCH()))

#define ADC_(m) do { unsigned _m = (m); \
        if (LIKELY(!fd)) { unsigned _s = A + _m + (fc ? 1 : 0); \
            fv = (~(A ^ _m) & (A ^ _s) & 0x80); fc = _s > 0xFF; A = (uint8_t)_s; } \
        else { A = adc_dec(A, _m, &fc); cyc--; } SETNZ(A); } while (0)
#define SBC_(m) do { unsigned _m = (m); \
        if (LIKELY(!fd)) { unsigned _s = A - _m - (fc ? 0 : 1); \
            fv = ((A ^ _m) & (A ^ _s) & 0x80); fc = _s < 0x100; A = (uint8_t)_s; } \
        else { A = sbc_dec(A, _m, &fc); cyc--; } SETNZ(A); } while (0)

/* T flag: the ALU op targets (zp,X) instead of A */
#define TOP(OPX, m) do { unsigned _sa = A; A = ram[X]; OPX(m); ram[X] = A; A = _sa; cyc -= 3; } while (0)
#define ORA_(m) do { A |= (m); SETNZ(A); } while (0)
#define AND_(m) do { A &= (m); SETNZ(A); } while (0)
#define EOR_(m) do { A ^= (m); SETNZ(A); } while (0)
#define ORA(m) do { unsigned _v = (m); if (UNLIKELY(tm)) TOP(ORA_, _v); else ORA_(_v); } while (0)
#define AND(m) do { unsigned _v = (m); if (UNLIKELY(tm)) TOP(AND_, _v); else AND_(_v); } while (0)
#define EOR(m) do { unsigned _v = (m); if (UNLIKELY(tm)) TOP(EOR_, _v); else EOR_(_v); } while (0)
#define ADC(m) do { unsigned _v = (m); if (UNLIKELY(tm)) TOP(ADC_, _v); else ADC_(_v); } while (0)
#define SBC(m) SBC_(m)
#define CMP(r, m) do { unsigned _t = (r) - (uint8_t)(m); fc = _t < 0x100; SETNZ((uint8_t)_t); } while (0)
#define CMPA(m) CMP(A, m)
#define BIT(m) do { unsigned _b = (m); fn = _b; fv = _b & 0x40; fz = A & _b; } while (0)

#define ASL(v) ({ unsigned _v = (v); fc = _v & 0x80; _v = (uint8_t)(_v << 1); SETNZ(_v); _v; })
#define LSR(v) ({ unsigned _v = (v); fc = _v & 1; _v >>= 1; SETNZ(_v); _v; })
#define ROL(v) ({ unsigned _v = (v); unsigned _c = fc ? 1 : 0; fc = _v & 0x80; _v = (uint8_t)((_v << 1) | _c); SETNZ(_v); _v; })
#define ROR(v) ({ unsigned _v = (v); unsigned _c = fc ? 0x80 : 0; fc = _v & 1; _v = (_v >> 1) | _c; SETNZ(_v); _v; })
#define INCV(v) ({ unsigned _v = (uint8_t)((v) + 1); SETNZ(_v); _v; })
#define DECV(v) ({ unsigned _v = (uint8_t)((v) - 1); SETNZ(_v); _v; })

#define RMWZ(eaz, F, cy) do { unsigned _e = (uint8_t)(eaz); ram[_e] = F(ram[_e]); cyc -= cy; } while (0)
#define RMWM(ea, F, cy) do { unsigned _e = (ea); unsigned _r = RD(_e); _r = F(_r); WR(_e, _r); cyc -= cy; } while (0)

/* taken branch; short backward ones are checked for idle loops.
 * `len` is the branch instruction's length (2, or 3 for BBR/BBS). */
#define TAKE(o, len) do { unsigned _end = PC; PC = (uint16_t)(PC + (o)); cyc -= 2; \
        if (UNLIKELY((int8_t)(o) < 0) && (int8_t)(o) >= -10 && p->idle_skip) { \
            if (p->idle_last != PC) { \
                if (idle_loop(p, PC, _end - (len))) cyc = 0; else p->idle_last = PC; } } } while (0)
#define BRANCH(cond) do { int8_t _o = (int8_t)FETCH(); cyc -= 2; if (cond) TAKE(_o, 2); } while (0)

PCE_HOT int cpu_run(pce_t *p, int cycles)
{
    unsigned A = p->A, X = p->X, Y = p->Y, S = p->S;
    unsigned PC = p->PC;
    unsigned fn, fz, fv, fd, fi, fc;
    SETP(p->P);
    uint8_t *const *rmap = p->rmap, *const *wmap = p->wmap;
    uint8_t *ram = p->ram;
    int sh = p->speed ? 0 : 2;
    int cyc = cycles >> sh;
    const uint8_t *pcb = NULL;
    unsigned pcpage = 1;   /* invalid: forces a lookup */
    uint8_t tmp[8];

    while (cyc > 0) {
        if (UNLIKELY(p->irq_lines) && !fi) {
            unsigned l = p->irq_lines;
            unsigned vec = (l & 4) ? 0xFFFA : (l & 2) ? 0xFFF8 : 0xFFF6;
            PUSH(PC >> 8); PUSH(PC & 0xFF);
            PUSH(GETP() & ~FB);
            fi = 1; fd = 0;
            PC = RD(vec) | RD(vec + 1) << 8;
            cyc -= 8;
        }
        PC &= 0xFFFF;
        if (UNLIKELY((PC & 0xE000) != pcpage)) {
            pcpage = PC & 0xE000;
            const uint8_t *m = rmap[PC >> 13];
            if (!m) m = p->ffpage;
            pcb = m - pcpage;
        }
        if (UNLIKELY((PC & 0x1FFF) >= 0x1FF9)) {
            /* the instruction may straddle two pages: fetch through the map */
            for (int i = 0; i < 8; i++) tmp[i] = RD((PC + i) & 0xFFFF);
            pcb = tmp - PC;
            pcpage = 1;
        }
        int tm = 0;
again:;
#ifdef PCE_TRACE
        { extern int pce_trace; if (pce_trace) { pce_trace--; fprintf(stderr, "%04x: %02x %02x %02x A=%02x X=%02x Y=%02x S=%02x\n", PC, pcb[PC], pcb[PC+1], pcb[PC+2], A, X, Y, S); } }
#endif
        unsigned op = FETCH();
#ifdef PCE_COUNT
        { extern uint64_t cnt_ins, cnt_op[256]; cnt_ins++; cnt_op[op]++; }
#endif
        switch (op) {
        /* ---- loads ---- */
        case 0xA9: A = FETCH(); SETNZ(A); cyc -= 2; break;
        case 0xA5: A = ZR(FETCH()); SETNZ(A); cyc -= 4; break;
        case 0xB5: A = ZR(EA_ZPX()); SETNZ(A); cyc -= 4; break;
        case 0xAD: A = RD(EA_ABS()); SETNZ(A); cyc -= 5; break;
        case 0xBD: A = RD(EA_ABX()); SETNZ(A); cyc -= 5; break;
        case 0xB9: A = RD(EA_ABY()); SETNZ(A); cyc -= 5; break;
        case 0xA1: A = RD(EA_IZX()); SETNZ(A); cyc -= 7; break;
        case 0xB1: A = RD(EA_IZY()); SETNZ(A); cyc -= 7; break;
        case 0xB2: A = RD(EA_IZP()); SETNZ(A); cyc -= 7; break;
        case 0xA2: X = FETCH(); SETNZ(X); cyc -= 2; break;
        case 0xA6: X = ZR(FETCH()); SETNZ(X); cyc -= 4; break;
        case 0xB6: X = ZR(EA_ZPY()); SETNZ(X); cyc -= 4; break;
        case 0xAE: X = RD(EA_ABS()); SETNZ(X); cyc -= 5; break;
        case 0xBE: X = RD(EA_ABY()); SETNZ(X); cyc -= 5; break;
        case 0xA0: Y = FETCH(); SETNZ(Y); cyc -= 2; break;
        case 0xA4: Y = ZR(FETCH()); SETNZ(Y); cyc -= 4; break;
        case 0xB4: Y = ZR(EA_ZPX()); SETNZ(Y); cyc -= 4; break;
        case 0xAC: Y = RD(EA_ABS()); SETNZ(Y); cyc -= 5; break;
        case 0xBC: Y = RD(EA_ABX()); SETNZ(Y); cyc -= 5; break;
        /* ---- stores ---- */
        case 0x85: ZW(FETCH(), A); cyc -= 4; break;
        case 0x95: ZW(EA_ZPX(), A); cyc -= 4; break;
        case 0x8D: WR(EA_ABS(), A); cyc -= 5; break;
        case 0x9D: WR(EA_ABX(), A); cyc -= 5; break;
        case 0x99: WR(EA_ABY(), A); cyc -= 5; break;
        case 0x81: WR(EA_IZX(), A); cyc -= 7; break;
        case 0x91: WR(EA_IZY(), A); cyc -= 7; break;
        case 0x92: WR(EA_IZP(), A); cyc -= 7; break;
        case 0x86: ZW(FETCH(), X); cyc -= 4; break;
        case 0x96: ZW(EA_ZPY(), X); cyc -= 4; break;
        case 0x8E: WR(EA_ABS(), X); cyc -= 5; break;
        case 0x84: ZW(FETCH(), Y); cyc -= 4; break;
        case 0x94: ZW(EA_ZPX(), Y); cyc -= 4; break;
        case 0x8C: WR(EA_ABS(), Y); cyc -= 5; break;
        case 0x64: ZW(FETCH(), 0); cyc -= 4; break;
        case 0x74: ZW(EA_ZPX(), 0); cyc -= 4; break;
        case 0x9C: WR(EA_ABS(), 0); cyc -= 5; break;
        case 0x9E: WR(EA_ABX(), 0); cyc -= 5; break;
        /* ---- ALU ---- */
#define ALUGROUP(base, OPM) \
        case base + 0x09: OPM(FETCH()); cyc -= 2; break; \
        case base + 0x05: OPM(ZR(FETCH())); cyc -= 4; break; \
        case base + 0x15: OPM(ZR(EA_ZPX())); cyc -= 4; break; \
        case base + 0x0D: OPM(RD(EA_ABS())); cyc -= 5; break; \
        case base + 0x1D: OPM(RD(EA_ABX())); cyc -= 5; break; \
        case base + 0x19: OPM(RD(EA_ABY())); cyc -= 5; break; \
        case base + 0x01: OPM(RD(EA_IZX())); cyc -= 7; break; \
        case base + 0x11: OPM(RD(EA_IZY())); cyc -= 7; break; \
        case base + 0x12: OPM(RD(EA_IZP())); cyc -= 7; break;
        ALUGROUP(0x00, ORA)
        ALUGROUP(0x20, AND)
        ALUGROUP(0x40, EOR)
        ALUGROUP(0x60, ADC)
        ALUGROUP(0xE0, SBC)
        ALUGROUP(0xC0, CMPA)
        case 0xE0: CMP(X, FETCH()); cyc -= 2; break;
        case 0xE4: CMP(X, ZR(FETCH())); cyc -= 4; break;
        case 0xEC: CMP(X, RD(EA_ABS())); cyc -= 5; break;
        case 0xC0: CMP(Y, FETCH()); cyc -= 2; break;
        case 0xC4: CMP(Y, ZR(FETCH())); cyc -= 4; break;
        case 0xCC: CMP(Y, RD(EA_ABS())); cyc -= 5; break;
        case 0x89: { unsigned m = FETCH(); fn = m; fv = m & 0x40; fz = A & m; cyc -= 2; break; }
        case 0x24: BIT(ZR(FETCH())); cyc -= 4; break;
        case 0x34: BIT(ZR(EA_ZPX())); cyc -= 4; break;
        case 0x2C: BIT(RD(EA_ABS())); cyc -= 5; break;
        case 0x3C: BIT(RD(EA_ABX())); cyc -= 5; break;
        /* ---- shifts / inc / dec ---- */
        case 0x0A: A = ASL(A); cyc -= 2; break;
        case 0x4A: A = LSR(A); cyc -= 2; break;
        case 0x2A: A = ROL(A); cyc -= 2; break;
        case 0x6A: A = ROR(A); cyc -= 2; break;
        case 0x1A: A = INCV(A); cyc -= 2; break;
        case 0x3A: A = DECV(A); cyc -= 2; break;
#define RMWGROUP(base, F) \
        case base + 0x06: RMWZ(FETCH(), F, 6); break; \
        case base + 0x16: RMWZ(EA_ZPX(), F, 6); break; \
        case base + 0x0E: RMWM(EA_ABS(), F, 7); break; \
        case base + 0x1E: RMWM(EA_ABX(), F, 7); break;
        RMWGROUP(0x00, ASL)
        RMWGROUP(0x20, ROL)
        RMWGROUP(0x40, LSR)
        RMWGROUP(0x60, ROR)
        RMWGROUP(0xC0, DECV)
        RMWGROUP(0xE0, INCV)
        case 0xE8: X = (uint8_t)(X + 1); SETNZ(X); cyc -= 2; break;
        case 0xC8: Y = (uint8_t)(Y + 1); SETNZ(Y); cyc -= 2; break;
        case 0xCA: X = (uint8_t)(X - 1); SETNZ(X); cyc -= 2; break;
        case 0x88: Y = (uint8_t)(Y - 1); SETNZ(Y); cyc -= 2; break;
        /* ---- TSB / TRB ---- */
        case 0x04: { unsigned e = FETCH(); unsigned m = ram[e]; fz = A & m; fn = m; fv = m & 0x40; ram[e] = m | A; cyc -= 6; break; }
        case 0x0C: { unsigned e = EA_ABS(); unsigned m = RD(e); fz = A & m; fn = m; fv = m & 0x40; WR(e, m | A); cyc -= 7; break; }
        case 0x14: { unsigned e = FETCH(); unsigned m = ram[e]; fz = A & m; fn = m; fv = m & 0x40; ram[e] = m & ~A; cyc -= 6; break; }
        case 0x1C: { unsigned e = EA_ABS(); unsigned m = RD(e); fz = A & m; fn = m; fv = m & 0x40; WR(e, m & ~A); cyc -= 7; break; }
        /* ---- TST ---- */
        case 0x83: { unsigned i = FETCH(); unsigned m = ZR(FETCH()); fn = m; fv = m & 0x40; fz = i & m; cyc -= 7; break; }
        case 0xA3: { unsigned i = FETCH(); unsigned m = ZR(EA_ZPX()); fn = m; fv = m & 0x40; fz = i & m; cyc -= 7; break; }
        case 0x93: { unsigned i = FETCH(); unsigned m = RD(EA_ABS()); fn = m; fv = m & 0x40; fz = i & m; cyc -= 8; break; }
        case 0xB3: { unsigned i = FETCH(); unsigned m = RD(EA_ABX()); fn = m; fv = m & 0x40; fz = i & m; cyc -= 8; break; }
        /* ---- RMB / SMB / BBR / BBS ---- */
        case 0x07: case 0x17: case 0x27: case 0x37: case 0x47: case 0x57: case 0x67: case 0x77:
            { unsigned e = FETCH(); ram[e] &= ~(1u << (op >> 4)); cyc -= 7; break; }
        case 0x87: case 0x97: case 0xA7: case 0xB7: case 0xC7: case 0xD7: case 0xE7: case 0xF7:
            { unsigned e = FETCH(); ram[e] |= 1u << ((op >> 4) & 7); cyc -= 7; break; }
        case 0x0F: case 0x1F: case 0x2F: case 0x3F: case 0x4F: case 0x5F: case 0x6F: case 0x7F:
            { unsigned m = ZR(FETCH()); int8_t o = (int8_t)FETCH(); cyc -= 6;
              if (!(m & (1u << (op >> 4)))) { TAKE(o, 3); }
              break; }
        case 0x8F: case 0x9F: case 0xAF: case 0xBF: case 0xCF: case 0xDF: case 0xEF: case 0xFF:
            { unsigned m = ZR(FETCH()); int8_t o = (int8_t)FETCH(); cyc -= 6;
              if (m & (1u << ((op >> 4) & 7))) { TAKE(o, 3); }
              break; }
        /* ---- branches ---- */
        case 0x10: BRANCH(!(fn & 0x80)); break;
        case 0x30: BRANCH(fn & 0x80); break;
        case 0x50: BRANCH(!fv); break;
        case 0x70: BRANCH(fv); break;
        case 0x90: BRANCH(!fc); break;
        case 0xB0: BRANCH(fc); break;
        case 0xD0: BRANCH(fz & 0xFF); break;
        case 0xF0: BRANCH(!(fz & 0xFF)); break;
        case 0x80: { int8_t o = (int8_t)FETCH(); cyc -= 2; TAKE(o, 2); break; }
        /* ---- jumps / calls ---- */
        case 0x4C: { unsigned t = FETCHW(); if (UNLIKELY(t == ((PC - 3) & 0xFFFF)) && p->idle_skip) cyc = 0; PC = t; cyc -= 4; break; }
        case 0x6C: { unsigned a = FETCHW(); PC = RD(a) | RD((a + 1) & 0xFFFF) << 8; cyc -= 7; break; }
        case 0x7C: { unsigned a = (uint16_t)(FETCHW() + X); PC = RD(a) | RD((a + 1) & 0xFFFF) << 8; cyc -= 7; break; }
        case 0x20: { unsigned a = FETCHW(); unsigned r = (PC - 1) & 0xFFFF; PUSH(r >> 8); PUSH(r & 0xFF); PC = a; cyc -= 7; break; }
        case 0x44: { int8_t o = (int8_t)FETCH(); unsigned r = (PC - 1) & 0xFFFF; PUSH(r >> 8); PUSH(r & 0xFF); PC = (uint16_t)(PC + o); cyc -= 8; break; }
        case 0x60: { unsigned l = PULL(); unsigned h = PULL(); PC = (uint16_t)((l | h << 8) + 1); cyc -= 7; break; }
        case 0x40: { unsigned pp = PULL(); SETP(pp); unsigned l = PULL(); unsigned h = PULL(); PC = l | h << 8; cyc -= 7; break; }
        case 0x00: {
            unsigned r = (PC + 1) & 0xFFFF; PUSH(r >> 8); PUSH(r & 0xFF); PUSH(GETP() | FB);
            fi = 1; fd = 0; PC = RD(0xFFF6) | RD(0xFFF7) << 8; cyc -= 8; break; }
        /* ---- stack ---- */
        case 0x48: PUSH(A); cyc -= 3; break;
        case 0xDA: PUSH(X); cyc -= 3; break;
        case 0x5A: PUSH(Y); cyc -= 3; break;
        case 0x08: PUSH(GETP() | FB); cyc -= 3; break;
        case 0x68: A = PULL(); SETNZ(A); cyc -= 4; break;
        case 0xFA: X = PULL(); SETNZ(X); cyc -= 4; break;
        case 0x7A: Y = PULL(); SETNZ(Y); cyc -= 4; break;
        case 0x28: { unsigned pp = PULL(); SETP(pp); cyc -= 4; break; }
        /* ---- transfers / flags ---- */
        case 0xAA: X = A; SETNZ(X); cyc -= 2; break;
        case 0xA8: Y = A; SETNZ(Y); cyc -= 2; break;
        case 0x8A: A = X; SETNZ(A); cyc -= 2; break;
        case 0x98: A = Y; SETNZ(A); cyc -= 2; break;
        case 0xBA: X = S; SETNZ(X); cyc -= 2; break;
        case 0x9A: S = X; cyc -= 2; break;
        case 0x22: { unsigned t = A; A = X; X = t; cyc -= 3; break; }
        case 0x42: { unsigned t = A; A = Y; Y = t; cyc -= 3; break; }
        case 0x02: { unsigned t = X; X = Y; Y = t; cyc -= 3; break; }
        case 0x62: A = 0; cyc -= 2; break;
        case 0x82: X = 0; cyc -= 2; break;
        case 0xC2: Y = 0; cyc -= 2; break;
        case 0x18: fc = 0; cyc -= 2; break;
        case 0x38: fc = 1; cyc -= 2; break;
        case 0x58: fi = 0; cyc -= 2; break;
        case 0x78: fi = 1; cyc -= 2; break;
        case 0xB8: fv = 0; cyc -= 2; break;
        case 0xD8: fd = 0; cyc -= 2; break;
        case 0xF8: fd = 1; cyc -= 2; break;
        case 0xF4: cyc -= 2; tm = 1; goto again;  /* SET */
        case 0xEA: cyc -= 2; break;
        /* ---- HuC6280 specials ---- */
        case 0x54: case 0xD4: {          /* CSL / CSH: rescale the remaining budget */
            cyc -= 3;
            int nsh = (op == 0xD4) ? 0 : 2;
            if (nsh != sh) { cyc = (cyc << sh) >> nsh; sh = nsh; p->speed = (op == 0xD4); }
            break; }
        case 0x53: {
            unsigned m = FETCH();
            for (int i = 0; i < 8; i++) if (m & (1u << i)) { p->mpr[i] = A; pce_map(p, i); }
            pcpage = 1; cyc -= 5; break; }
        case 0x43: {
            unsigned m = FETCH();
            for (int i = 0; i < 8; i++) if (m & (1u << i)) { A = p->mpr[i]; break; }
            cyc -= 4; break; }
        case 0x03: { unsigned v = FETCH(); pce_io_write(p, 0x1FE000, v); cyc -= 5; break; }
        case 0x13: { unsigned v = FETCH(); pce_io_write(p, 0x1FE002, v); cyc -= 5; break; }
        case 0x23: { unsigned v = FETCH(); pce_io_write(p, 0x1FE003, v); cyc -= 5; break; }
        case 0x73: case 0xC3: case 0xD3: case 0xE3: case 0xF3: {
            unsigned src = FETCHW(), dst = FETCHW(), len = FETCHW();
            PUSH(Y); PUSH(A); PUSH(X);
            cyc -= block_xfer(p, op, src, dst, len);
            X = PULL(); A = PULL(); Y = PULL();
            pcpage = 1;
            break; }
        default: cyc -= 2; break;
        }
    }

    p->A = A; p->X = X; p->Y = Y; p->S = S; p->PC = PC;
    p->P = GETP();
    int used = cycles - (cyc << sh) + p->cycles_extra;
    p->cycles_extra = 0;
    return used;
}
