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
#include "cpu_tab.h"
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
                EMUX_BEGIN(PS_IDLE); int _il = idle_loop(p, PC, _end - (len)); EMUX_END(PS_IDLE); if (_il) cyc = 0; else p->idle_last = PC; } } } while (0)
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
    int adj = 0;

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
        unsigned md = op_mode[op];
        cyc -= op_cyc[op];
        unsigned ea = 0, v = 0;
        switch (md & 15) {
        case M_IMP: break;
        case M_IMM: v = FETCH(); break;
        case M_ZP:  ea = FETCH(); v = ram[ea]; ea |= 0x2000; break;
        case M_ZPX: ea = (uint8_t)(FETCH() + X); v = ram[ea]; ea |= 0x2000; break;
        case M_ZPY: ea = (uint8_t)(FETCH() + Y); v = ram[ea]; ea |= 0x2000; break;
        case M_ABS: ea = FETCHW(); goto rdchk;
        case M_ABX: ea = (FETCHW() + X) & 0xFFFF; goto rdchk;
        case M_ABY: ea = (FETCHW() + Y) & 0xFFFF; goto rdchk;
        case M_IZX: ea = ZPW(FETCH() + X); goto rdchk;
        case M_IZY: ea = (ZPW(FETCH()) + Y) & 0xFFFF; goto rdchk;
        case M_IZP: ea = ZPW(FETCH());
        rdchk:
            if (md & 16) v = RD(ea);
            break;
        }
        switch (op_kind[op]) {
        case O_LDA: A = v; SETNZ(A); break;
        case O_LDX: X = v; SETNZ(X); break;
        case O_LDY: Y = v; SETNZ(Y); break;
        case O_STA: WR(ea, A); break;
        case O_STX: WR(ea, X); break;
        case O_STY: WR(ea, Y); break;
        case O_STZ: WR(ea, 0); break;
        case O_ORA: ORA(v); break;
        case O_AND: AND(v); break;
        case O_EOR: EOR(v); break;
        case O_ADC: ADC(v); break;
        case O_SBC: SBC(v); break;
        case O_CMP: CMP(A, v); break;
        case O_CPX: CMP(X, v); break;
        case O_CPY: CMP(Y, v); break;
        case O_BIT: fn = v; fv = v & 0x40; fz = A & v; break;
        case O_ASL: v = ASL(v); WR(ea, v); break;
        case O_LSR: v = LSR(v); WR(ea, v); break;
        case O_ROL: v = ROL(v); WR(ea, v); break;
        case O_ROR: v = ROR(v); WR(ea, v); break;
        case O_INC: v = INCV(v); WR(ea, v); break;
        case O_DEC: v = DECV(v); WR(ea, v); break;
        case O_ASLA: A = ASL(A); break;
        case O_LSRA: A = LSR(A); break;
        case O_ROLA: A = ROL(A); break;
        case O_RORA: A = ROR(A); break;
        case O_INA: A = INCV(A); break;
        case O_DEA: A = DECV(A); break;
        case O_INX: X = (uint8_t)(X + 1); SETNZ(X); break;
        case O_INY: Y = (uint8_t)(Y + 1); SETNZ(Y); break;
        case O_DEX: X = (uint8_t)(X - 1); SETNZ(X); break;
        case O_DEY: Y = (uint8_t)(Y - 1); SETNZ(Y); break;
        case O_TSB: fz = A & v; fn = v; fv = v & 0x40; WR(ea, v | A); break;
        case O_TRB: fz = A & v; fn = v; fv = v & 0x40; WR(ea, v & ~A); break;
        case O_TST: {
            unsigned i = FETCH(), m;
            switch (op) {
            case 0x83: m = ram[FETCH()]; break;
            case 0xA3: m = ram[(uint8_t)(FETCH() + X)]; break;
            case 0x93: m = RD(FETCHW()); break;
            default:   m = RD((FETCHW() + X) & 0xFFFF); break;
            }
            fn = m; fv = m & 0x40; fz = i & m; break; }
        case O_RMB: WR(ea, v & ~(1u << ((op >> 4) & 7))); break;
        case O_SMB: WR(ea, v | (1u << ((op >> 4) & 7))); break;
        case O_BBR: case O_BBS: {
            unsigned m = ram[FETCH()]; int8_t o = (int8_t)FETCH();
            unsigned bit = (m >> ((op >> 4) & 7)) & 1;
            if (bit == (op >> 7)) TAKE(o, 3);
            break; }
        case O_BCC_: {
            int8_t o = (int8_t)FETCH();
            unsigned f;
            switch (op >> 6) {
            case 0: f = fn & 0x80; break;
            case 1: f = fv; break;
            case 2: f = fc; break;
            default: f = !(fz & 0xFF); break;
            }
            if ((f != 0) == ((op >> 5) & 1)) TAKE(o, 2);
            break; }
        case O_BRA: { int8_t o = (int8_t)FETCH(); TAKE(o, 2); break; }
        case O_JMP: { unsigned t = FETCHW(); if (UNLIKELY(t == ((PC - 3) & 0xFFFF)) && p->idle_skip) cyc = 0; PC = t; break; }
        case O_JMPI: { unsigned a = FETCHW(); PC = RD(a) | RD((a + 1) & 0xFFFF) << 8; break; }
        case O_JMPX: { unsigned a = (FETCHW() + X) & 0xFFFF; PC = RD(a) | RD((a + 1) & 0xFFFF) << 8; break; }
        case O_JSR: { unsigned a = FETCHW(); unsigned r = (PC - 1) & 0xFFFF; PUSH(r >> 8); PUSH(r & 0xFF); PC = a; break; }
        case O_BSR: { int8_t o = (int8_t)FETCH(); unsigned r = (PC - 1) & 0xFFFF; PUSH(r >> 8); PUSH(r & 0xFF); PC = (uint16_t)(PC + o); break; }
        case O_RTS: { unsigned l = PULL(); unsigned h = PULL(); PC = (uint16_t)((l | h << 8) + 1); break; }
        case O_RTI: { unsigned pp = PULL(); SETP(pp); unsigned l = PULL(); unsigned h = PULL(); PC = l | h << 8; break; }
        case O_BRK: {
            unsigned r = (PC + 1) & 0xFFFF; PUSH(r >> 8); PUSH(r & 0xFF); PUSH(GETP() | FB);
            fi = 1; fd = 0; PC = RD(0xFFF6) | RD(0xFFF7) << 8; break; }
        case O_PHA: PUSH(A); break;
        case O_PHX: PUSH(X); break;
        case O_PHY: PUSH(Y); break;
        case O_PHP: PUSH(GETP() | FB); break;
        case O_PLA: A = PULL(); SETNZ(A); break;
        case O_PLX: X = PULL(); SETNZ(X); break;
        case O_PLY: Y = PULL(); SETNZ(Y); break;
        case O_PLP: { unsigned pp = PULL(); SETP(pp); break; }
        case O_TAX: X = A; SETNZ(X); break;
        case O_TAY: Y = A; SETNZ(Y); break;
        case O_TXA: A = X; SETNZ(A); break;
        case O_TYA: A = Y; SETNZ(A); break;
        case O_TSX: X = S; SETNZ(X); break;
        case O_TXS: S = X; break;
        case O_SAX: { unsigned t = A; A = X; X = t; break; }
        case O_SAY: { unsigned t = A; A = Y; Y = t; break; }
        case O_SXY: { unsigned t = X; X = Y; Y = t; break; }
        case O_CLA: A = 0; break;
        case O_CLX: X = 0; break;
        case O_CLY: Y = 0; break;
        case O_CLC: fc = 0; break;
        case O_SEC: fc = 1; break;
        case O_CLI: fi = 0; break;
        case O_SEI: fi = 1; break;
        case O_CLV: fv = 0; break;
        case O_CLD: fd = 0; break;
        case O_SED: fd = 1; break;
        case O_SET: tm = 1; goto again;
        case O_CSL: case O_CSH: {
            int nsh = (op == 0xD4) ? 0 : 2;
            if (nsh != sh) {
                int ncyc = (cyc << sh) >> nsh;
                adj += (ncyc << nsh) - (cyc << sh);     /* keep the count exact */
                cyc = ncyc; sh = nsh; p->speed = (op == 0xD4);
            }
            break; }
        case O_TAM: {
            unsigned m = FETCH();
            for (int i = 0; i < 8; i++) if (m & (1u << i)) { p->mpr[i] = A; pce_map(p, i); }
            pcpage = 1; break; }
        case O_TMA: {
            unsigned m = FETCH();
            for (int i = 0; i < 8; i++) if (m & (1u << i)) { A = p->mpr[i]; break; }
            break; }
        case O_ST0: pce_io_write(p, 0x1FE000, FETCH()); break;
        case O_ST1: pce_io_write(p, 0x1FE002, FETCH()); break;
        case O_ST2: pce_io_write(p, 0x1FE003, FETCH()); break;
        case O_BLK: {
            unsigned src = FETCHW(), dst = FETCHW(), len = FETCHW();
            PUSH(Y); PUSH(A); PUSH(X);
            cyc -= block_xfer(p, op, src, dst, len);
            X = PULL(); A = PULL(); Y = PULL();
            pcpage = 1;
            break; }
        default: break;   /* NOP and undefined opcodes */
        }
    }

    p->A = A; p->X = X; p->Y = Y; p->S = S; p->PC = PC;
    p->P = GETP();
    int used = cycles - (cyc << sh) + adj + p->cycles_extra;
    p->cycles_extra = 0;
    return used;
}
