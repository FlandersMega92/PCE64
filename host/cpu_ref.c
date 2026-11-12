/* HuC6280 interpreter.
 * Registers are kept in locals for the duration of cpu_run so the compiler
 * can hold them in machine registers; memory goes through the 8-slot map. */
#include "../core/pce.h"
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

void cpu_reset_ref(pce_t *p)
{
    p->A = p->X = p->Y = 0;
    p->S = 0xFF;
    p->P = FI | FZ;
    p->speed = 0;
    p->mpr[7] = 0x00;
    for (int i = 0; i < 7; i++) p->mpr[i] = 0xFF;  /* hardware leaves these undefined */
    for (int i = 0; i < 8; i++) pce_map(p, i);
    p->PC = p->rmap[7][0x1FFE] | (p->rmap[7][0x1FFF] << 8);
    p->halted = 0;
}

/* ------------------------------------------------------------------------ */

#define RD(a)  ({ uint16_t _a = (a); uint8_t *_m = rmap[_a >> 13]; _m ? _m[_a & 0x1FFF] : pce_slow_read(p, _a); })
#define WR(a, v) do { uint16_t _a = (a); uint8_t *_m = wmap[_a >> 13]; if (_m) _m[_a & 0x1FFF] = (v); else pce_slow_write(p, _a, (v)); } while (0)
/* zero page and stack live at logical $2000/$2100 (slot 1) */
#define ZR(z)  RD(0x2000 | (uint8_t)(z))
#define ZW(z, v) WR(0x2000 | (uint8_t)(z), v)
#define PUSH(v) do { WR(0x2100 | S, v); S--; } while (0)
#define PULL()  (S++, RD(0x2100 | S))

#define FETCH()  RD(PC++)
#define FETCHW() ({ uint8_t _l = RD(PC); uint8_t _h = RD((uint16_t)(PC + 1)); PC += 2; (uint16_t)(_l | _h << 8); })
#define ZPW(z)   ({ uint8_t _z = (z); (uint16_t)(ZR(_z) | ZR((uint8_t)(_z + 1)) << 8); })

/* flags: N and Z are kept as values (nzv: N=bit7 of fn, Z=(fz==0)) */
#define SETNZ(v) do { fn = fz = (v); } while (0)

#define GETP() ((fn & 0x80) | (fv ? FV : 0) | (fd ? FD : 0) | (fi ? FI : 0) | (fz ? 0 : FZ) | (fc ? FC : 0))
#define SETP(v) do { uint8_t _p = (v); fn = _p & 0x80; fv = _p & FV; fd = _p & FD; fi = _p & FI; fz = !(_p & FZ); fc = _p & FC; } while (0)

/* commit/restore registers around calls that may look at CPU state */
#define SYNC_OUT() do { p->A = A; p->X = X; p->Y = Y; p->S = S; p->PC = PC; p->P = GETP(); } while (0)
#define SYNC_IN()  do { /* slow writes may remap (TAM via io? no) or change irq lines */ } while (0)

#define CYC(n) (cyc -= (n) << spdsh)

/* addressing modes: produce an effective address */
#define EA_ZP()   ((uint16_t)(0x2000 | FETCH()))
#define EA_ZPX()  ((uint16_t)(0x2000 | (uint8_t)(FETCH() + X)))
#define EA_ZPY()  ((uint16_t)(0x2000 | (uint8_t)(FETCH() + Y)))
#define EA_ABS()  (FETCHW())
#define EA_ABX()  ((uint16_t)(FETCHW() + X))
#define EA_ABY()  ((uint16_t)(FETCHW() + Y))
#define EA_IZX()  (ZPW((uint8_t)(FETCH() + X)))
#define EA_IZY()  ((uint16_t)(ZPW(FETCH()) + Y))
#define EA_IZP()  (ZPW(FETCH()))

/* T-flag variants: ALU result goes to (zp,X) instead of A */
#define ALU_T(op, MV, extra) do { \
        uint16_t _ta = 0x2000 | X; uint8_t _tv = RD(_ta); \
        op(_tv, MV); WR(_ta, _tv); extra; } while (0)

#define DO_ORA(r, m) do { r |= (m); SETNZ(r); } while (0)
#define DO_AND(r, m) do { r &= (m); SETNZ(r); } while (0)
#define DO_EOR(r, m) do { r ^= (m); SETNZ(r); } while (0)
#define DO_ADC(r, m) do { \
        unsigned _m = (m); \
        if (!fd) { unsigned _s = r + _m + (fc ? 1 : 0); \
            fv = (~(r ^ _m) & (r ^ _s) & 0x80); fc = _s > 0xFF; r = (uint8_t)_s; SETNZ(r); } \
        else { unsigned _lo = (r & 0x0F) + (_m & 0x0F) + (fc ? 1 : 0); \
            unsigned _hi = (r & 0xF0) + (_m & 0xF0); \
            if (_lo > 9) { _lo += 6; } \
            _hi += (_lo > 0x0F) ? 0x10 : 0; \
            if (_hi > 0x90) _hi += 0x60; \
            fc = _hi > 0xFF; r = (uint8_t)((_lo & 0x0F) | (_hi & 0xF0)); SETNZ(r); CYC(1); } \
    } while (0)
#define DO_SBC(r, m) do { \
        unsigned _m = (m); \
        if (!fd) { unsigned _s = r - _m - (fc ? 0 : 1); \
            fv = ((r ^ _m) & (r ^ _s) & 0x80); fc = _s < 0x100; r = (uint8_t)_s; SETNZ(r); } \
        else { int _lo = (r & 0x0F) - (_m & 0x0F) - (fc ? 0 : 1); \
            int _hi = (r & 0xF0) - (_m & 0xF0); \
            if (_lo < 0) { _lo -= 6; _hi -= 0x10; } \
            if (_hi < 0) _hi -= 0x60; \
            fc = !(((int)r - (int)_m - (fc ? 0 : 1)) < 0); \
            r = (uint8_t)((_lo & 0x0F) | (_hi & 0xF0)); SETNZ(r); CYC(1); } \
    } while (0)

#define ORA(MV) do { uint8_t m_ = (MV); if (tmode) { ALU_T(DO_ORA, m_, CYC(3)); } else DO_ORA(A, m_); } while (0)
#define AND(MV) do { uint8_t m_ = (MV); if (tmode) { ALU_T(DO_AND, m_, CYC(3)); } else DO_AND(A, m_); } while (0)
#define EOR(MV) do { uint8_t m_ = (MV); if (tmode) { ALU_T(DO_EOR, m_, CYC(3)); } else DO_EOR(A, m_); } while (0)
#define ADC(MV) do { uint8_t m_ = (MV); if (tmode) { ALU_T(DO_ADC, m_, CYC(3)); } else DO_ADC(A, m_); } while (0)
#define SBC(MV) do { uint8_t m_ = (MV); DO_SBC(A, m_); } while (0)
#define CMP(r, m) do { unsigned _t = (r) - (uint8_t)(m); fc = _t < 0x100; SETNZ((uint8_t)_t); } while (0)
#define BIT(m) do { uint8_t _b = (m); fn = _b; fv = _b & 0x40; fz = A & _b; } while (0)

#define ASL(v) ({ uint8_t _v = (v); fc = _v & 0x80; _v <<= 1; SETNZ(_v); _v; })
#define LSR(v) ({ uint8_t _v = (v); fc = _v & 1; _v >>= 1; SETNZ(_v); _v; })
#define ROL(v) ({ uint8_t _v = (v); int _c = fc ? 1 : 0; fc = _v & 0x80; _v = (_v << 1) | _c; SETNZ(_v); _v; })
#define ROR(v) ({ uint8_t _v = (v); int _c = fc ? 0x80 : 0; fc = _v & 1; _v = (_v >> 1) | _c; SETNZ(_v); _v; })
#define INCV(v) ({ uint8_t _v = (v) + 1; SETNZ(_v); _v; })
#define DECV(v) ({ uint8_t _v = (v) - 1; SETNZ(_v); _v; })

#define RMW(ea, fnc, cy) do { uint16_t _e = (ea); uint8_t _r = RD(_e); _r = fnc(_r); WR(_e, _r); CYC(cy); } while (0)

#define BRANCH(cond) do { int8_t _o = (int8_t)FETCH(); if (cond) { PC += _o; CYC(4); } else CYC(2); } while (0)

PCE_HOT int cpu_run_ref(pce_t *p, int cycles)
{
    uint8_t A = p->A, X = p->X, Y = p->Y, S = p->S;
    uint16_t PC = p->PC;
    unsigned fn, fz, fv, fd, fi, fc;
    int tmode = 0;
    SETP(p->P);
    uint8_t **rmap = p->rmap, **wmap = p->wmap;
    int spdsh = p->speed ? 0 : 2;
    int cyc = cycles;

    while (cyc > 0) {
        if (p->irq_lines && !fi) {
            uint16_t vec;
            uint8_t l = p->irq_lines;
            if (l & 4) vec = 0xFFFA;       /* timer */
            else if (l & 2) vec = 0xFFF8;  /* VDC */
            else vec = 0xFFF6;             /* IRQ2 */
            PUSH(PC >> 8); PUSH(PC & 0xFF);
            PUSH(GETP() & ~FB);
            fi = 1; fd = 0;
            PC = RD(vec) | RD(vec + 1) << 8;
            CYC(8);
            p->halted = 0;
        }
        tmode = 0;
again:;
#ifdef PCE_TRACE
        { extern int pce_trace; if (pce_trace) { pce_trace--; fprintf(stderr, "%04x: %02x %02x %02x A=%02x X=%02x Y=%02x S=%02x I=%d line=%d st=%02x\n", PC, RD(PC), RD((uint16_t)(PC+1)), RD((uint16_t)(PC+2)), A, X, Y, S, fi?1:0, p->line, p->vdc.status); } }
#endif
        uint8_t op = FETCH();
#ifdef PCE_COUNT
        { extern uint64_t cnt_ins, cnt_pc[65536]; cnt_ins++; cnt_pc[(uint16_t)(PC-1)]++; }
#endif
        switch (op) {
        /* ---- loads ---- */
        case 0xA9: A = FETCH(); SETNZ(A); CYC(2); break;
        case 0xA5: A = RD(EA_ZP()); SETNZ(A); CYC(4); break;
        case 0xB5: A = RD(EA_ZPX()); SETNZ(A); CYC(4); break;
        case 0xAD: A = RD(EA_ABS()); SETNZ(A); CYC(5); break;
        case 0xBD: A = RD(EA_ABX()); SETNZ(A); CYC(5); break;
        case 0xB9: A = RD(EA_ABY()); SETNZ(A); CYC(5); break;
        case 0xA1: A = RD(EA_IZX()); SETNZ(A); CYC(7); break;
        case 0xB1: A = RD(EA_IZY()); SETNZ(A); CYC(7); break;
        case 0xB2: A = RD(EA_IZP()); SETNZ(A); CYC(7); break;
        case 0xA2: X = FETCH(); SETNZ(X); CYC(2); break;
        case 0xA6: X = RD(EA_ZP()); SETNZ(X); CYC(4); break;
        case 0xB6: X = RD(EA_ZPY()); SETNZ(X); CYC(4); break;
        case 0xAE: X = RD(EA_ABS()); SETNZ(X); CYC(5); break;
        case 0xBE: X = RD(EA_ABY()); SETNZ(X); CYC(5); break;
        case 0xA0: Y = FETCH(); SETNZ(Y); CYC(2); break;
        case 0xA4: Y = RD(EA_ZP()); SETNZ(Y); CYC(4); break;
        case 0xB4: Y = RD(EA_ZPX()); SETNZ(Y); CYC(4); break;
        case 0xAC: Y = RD(EA_ABS()); SETNZ(Y); CYC(5); break;
        case 0xBC: Y = RD(EA_ABX()); SETNZ(Y); CYC(5); break;
        /* ---- stores ---- */
        case 0x85: WR(EA_ZP(), A); CYC(4); break;
        case 0x95: WR(EA_ZPX(), A); CYC(4); break;
        case 0x8D: WR(EA_ABS(), A); CYC(5); break;
        case 0x9D: WR(EA_ABX(), A); CYC(5); break;
        case 0x99: WR(EA_ABY(), A); CYC(5); break;
        case 0x81: WR(EA_IZX(), A); CYC(7); break;
        case 0x91: WR(EA_IZY(), A); CYC(7); break;
        case 0x92: WR(EA_IZP(), A); CYC(7); break;
        case 0x86: WR(EA_ZP(), X); CYC(4); break;
        case 0x96: WR(EA_ZPY(), X); CYC(4); break;
        case 0x8E: WR(EA_ABS(), X); CYC(5); break;
        case 0x84: WR(EA_ZP(), Y); CYC(4); break;
        case 0x94: WR(EA_ZPX(), Y); CYC(4); break;
        case 0x8C: WR(EA_ABS(), Y); CYC(5); break;
        case 0x64: WR(EA_ZP(), 0); CYC(4); break;
        case 0x74: WR(EA_ZPX(), 0); CYC(4); break;
        case 0x9C: WR(EA_ABS(), 0); CYC(5); break;
        case 0x9E: WR(EA_ABX(), 0); CYC(5); break;
        /* ---- ALU ---- */
#define ALUGROUP(base, OPM) \
        case base + 0x09: OPM(FETCH()); CYC(2); break; \
        case base + 0x05: OPM(RD(EA_ZP())); CYC(4); break; \
        case base + 0x15: OPM(RD(EA_ZPX())); CYC(4); break; \
        case base + 0x0D: OPM(RD(EA_ABS())); CYC(5); break; \
        case base + 0x1D: OPM(RD(EA_ABX())); CYC(5); break; \
        case base + 0x19: OPM(RD(EA_ABY())); CYC(5); break; \
        case base + 0x01: OPM(RD(EA_IZX())); CYC(7); break; \
        case base + 0x11: OPM(RD(EA_IZY())); CYC(7); break; \
        case base + 0x12: OPM(RD(EA_IZP())); CYC(7); break;
        ALUGROUP(0x00, ORA)
        ALUGROUP(0x20, AND)
        ALUGROUP(0x40, EOR)
        ALUGROUP(0x60, ADC)
        ALUGROUP(0xE0, SBC)
#define CMPA(m) CMP(A, m)
        ALUGROUP(0xC0, CMPA)
        case 0xE0: CMP(X, FETCH()); CYC(2); break;
        case 0xE4: CMP(X, RD(EA_ZP())); CYC(4); break;
        case 0xEC: CMP(X, RD(EA_ABS())); CYC(5); break;
        case 0xC0: CMP(Y, FETCH()); CYC(2); break;
        case 0xC4: CMP(Y, RD(EA_ZP())); CYC(4); break;
        case 0xCC: CMP(Y, RD(EA_ABS())); CYC(5); break;
        case 0x89: { uint8_t m = FETCH(); fn = m; fv = m & 0x40; fz = A & m; CYC(2); break; }
        case 0x24: BIT(RD(EA_ZP())); CYC(4); break;
        case 0x34: BIT(RD(EA_ZPX())); CYC(4); break;
        case 0x2C: BIT(RD(EA_ABS())); CYC(5); break;
        case 0x3C: BIT(RD(EA_ABX())); CYC(5); break;
        /* ---- shifts / inc / dec ---- */
        case 0x0A: A = ASL(A); CYC(2); break;
        case 0x4A: A = LSR(A); CYC(2); break;
        case 0x2A: A = ROL(A); CYC(2); break;
        case 0x6A: A = ROR(A); CYC(2); break;
        case 0x1A: A = INCV(A); CYC(2); break;
        case 0x3A: A = DECV(A); CYC(2); break;
#define RMWGROUP(base, F) \
        case base + 0x06: RMW(EA_ZP(), F, 6); break; \
        case base + 0x16: RMW(EA_ZPX(), F, 6); break; \
        case base + 0x0E: RMW(EA_ABS(), F, 7); break; \
        case base + 0x1E: RMW(EA_ABX(), F, 7); break;
        RMWGROUP(0x00, ASL)
        RMWGROUP(0x20, ROL)
        RMWGROUP(0x40, LSR)
        RMWGROUP(0x60, ROR)
        RMWGROUP(0xC0, DECV)
        RMWGROUP(0xE0, INCV)
        case 0xE8: X++; SETNZ(X); CYC(2); break;
        case 0xC8: Y++; SETNZ(Y); CYC(2); break;
        case 0xCA: X--; SETNZ(X); CYC(2); break;
        case 0x88: Y--; SETNZ(Y); CYC(2); break;
        /* ---- TSB / TRB ---- */
        case 0x04: { uint16_t e = EA_ZP(); uint8_t m = RD(e); fz = A & m; fn = m; fv = m & 0x40; WR(e, m | A); CYC(6); break; }
        case 0x0C: { uint16_t e = EA_ABS(); uint8_t m = RD(e); fz = A & m; fn = m; fv = m & 0x40; WR(e, m | A); CYC(7); break; }
        case 0x14: { uint16_t e = EA_ZP(); uint8_t m = RD(e); fz = A & m; fn = m; fv = m & 0x40; WR(e, m & ~A); CYC(6); break; }
        case 0x1C: { uint16_t e = EA_ABS(); uint8_t m = RD(e); fz = A & m; fn = m; fv = m & 0x40; WR(e, m & ~A); CYC(7); break; }
        /* ---- TST ---- */
        case 0x83: { uint8_t i = FETCH(); uint8_t m = RD(EA_ZP()); fn = m; fv = m & 0x40; fz = i & m; CYC(7); break; }
        case 0xA3: { uint8_t i = FETCH(); uint8_t m = RD(EA_ZPX()); fn = m; fv = m & 0x40; fz = i & m; CYC(7); break; }
        case 0x93: { uint8_t i = FETCH(); uint8_t m = RD(EA_ABS()); fn = m; fv = m & 0x40; fz = i & m; CYC(8); break; }
        case 0xB3: { uint8_t i = FETCH(); uint8_t m = RD(EA_ABX()); fn = m; fv = m & 0x40; fz = i & m; CYC(8); break; }
        /* ---- RMB / SMB / BBR / BBS ---- */
#define BITOPS(n) \
        case 0x07 + (n << 4): { uint16_t e = EA_ZP(); WR(e, RD(e) & ~(1 << n)); CYC(7); break; } \
        case 0x87 + (n << 4): { uint16_t e = EA_ZP(); WR(e, RD(e) | (1 << n)); CYC(7); break; } \
        case 0x0F + (n << 4): { uint8_t m = RD(EA_ZP()); int8_t o = (int8_t)FETCH(); if (!(m & (1 << n))) { PC += o; CYC(8); } else CYC(6); break; } \
        case 0x8F + (n << 4): { uint8_t m = RD(EA_ZP()); int8_t o = (int8_t)FETCH(); if (m & (1 << n)) { PC += o; CYC(8); } else CYC(6); break; }
        BITOPS(0) BITOPS(1) BITOPS(2) BITOPS(3) BITOPS(4) BITOPS(5) BITOPS(6) BITOPS(7)
        /* ---- branches ---- */
        case 0x10: BRANCH(!(fn & 0x80)); break;
        case 0x30: BRANCH(fn & 0x80); break;
        case 0x50: BRANCH(!fv); break;
        case 0x70: BRANCH(fv); break;
        case 0x90: BRANCH(!fc); break;
        case 0xB0: BRANCH(fc); break;
        case 0xD0: BRANCH(fz); break;
        case 0xF0: BRANCH(!fz); break;
        case 0x80: { int8_t o = (int8_t)FETCH(); PC += o; CYC(4); break; }
        /* ---- jumps / calls ---- */
        case 0x4C: PC = FETCHW(); CYC(4); break;
        case 0x6C: { uint16_t a = FETCHW(); PC = RD(a) | RD((uint16_t)(a + 1)) << 8; CYC(7); break; }
        case 0x7C: { uint16_t a = FETCHW() + X; PC = RD(a) | RD((uint16_t)(a + 1)) << 8; CYC(7); break; }
        case 0x20: { uint16_t a = FETCHW(); uint16_t r = PC - 1; PUSH(r >> 8); PUSH(r & 0xFF); PC = a; CYC(7); break; }
        case 0x44: { int8_t o = (int8_t)FETCH(); uint16_t r = PC - 1; PUSH(r >> 8); PUSH(r & 0xFF); PC += o; CYC(8); break; }
        case 0x60: { uint8_t l = PULL(); uint8_t h = PULL(); PC = (l | h << 8) + 1; CYC(7); break; }
        case 0x40: { uint8_t pp = PULL(); SETP(pp); uint8_t l = PULL(); uint8_t h = PULL(); PC = l | h << 8; CYC(7); break; }
        case 0x00: { /* BRK */
            uint16_t r = PC + 1; PUSH(r >> 8); PUSH(r & 0xFF); PUSH(GETP() | FB);
            fi = 1; fd = 0; PC = RD(0xFFF6) | RD(0xFFF7) << 8; CYC(8); break; }
        /* ---- stack ---- */
        case 0x48: PUSH(A); CYC(3); break;
        case 0xDA: PUSH(X); CYC(3); break;
        case 0x5A: PUSH(Y); CYC(3); break;
        case 0x08: PUSH(GETP() | FB); CYC(3); break;
        case 0x68: A = PULL(); SETNZ(A); CYC(4); break;
        case 0xFA: X = PULL(); SETNZ(X); CYC(4); break;
        case 0x7A: Y = PULL(); SETNZ(Y); CYC(4); break;
        case 0x28: { uint8_t pp = PULL(); SETP(pp); CYC(4); break; }
        /* ---- transfers / flags ---- */
        case 0xAA: X = A; SETNZ(X); CYC(2); break;
        case 0xA8: Y = A; SETNZ(Y); CYC(2); break;
        case 0x8A: A = X; SETNZ(A); CYC(2); break;
        case 0x98: A = Y; SETNZ(A); CYC(2); break;
        case 0xBA: X = S; SETNZ(X); CYC(2); break;
        case 0x9A: S = X; CYC(2); break;
        case 0x22: { uint8_t t = A; A = X; X = t; CYC(3); break; }
        case 0x42: { uint8_t t = A; A = Y; Y = t; CYC(3); break; }
        case 0x02: { uint8_t t = X; X = Y; Y = t; CYC(3); break; }
        case 0x62: A = 0; CYC(2); break;
        case 0x82: X = 0; CYC(2); break;
        case 0xC2: Y = 0; CYC(2); break;
        case 0x18: fc = 0; CYC(2); break;
        case 0x38: fc = 1; CYC(2); break;
        case 0x58: fi = 0; CYC(2); break;
        case 0x78: fi = 1; CYC(2); break;
        case 0xB8: fv = 0; CYC(2); break;
        case 0xD8: fd = 0; CYC(2); break;
        case 0xF8: fd = 1; CYC(2); break;
        case 0xF4: CYC(2); tmode = 1; goto again;  /* SET */
        case 0xEA: CYC(2); break;
        /* ---- HuC6280 specials ---- */
        case 0x54: CYC(3); p->speed = 0; spdsh = 2; break;   /* CSL */
        case 0xD4: CYC(3); p->speed = 1; spdsh = 0; break;   /* CSH */
        case 0x53: { /* TAM */
            uint8_t m = FETCH();
            for (int i = 0; i < 8; i++) if (m & (1 << i)) { p->mpr[i] = A; pce_map(p, i); }
            CYC(5); break; }
        case 0x43: { /* TMA */
            uint8_t m = FETCH();
            for (int i = 0; i < 8; i++) if (m & (1 << i)) { A = p->mpr[i]; break; }
            CYC(4); break; }
        case 0x03: { uint8_t v = FETCH(); pce_io_write(p, 0x1FE000, v); CYC(5); break; } /* ST0 */
        case 0x13: { uint8_t v = FETCH(); pce_io_write(p, 0x1FE002, v); CYC(5); break; } /* ST1 */
        case 0x23: { uint8_t v = FETCH(); pce_io_write(p, 0x1FE003, v); CYC(5); break; } /* ST2 */
        /* ---- block transfers ---- */
        case 0x73: case 0xC3: case 0xD3: case 0xE3: case 0xF3: {
            uint16_t src = FETCHW(), dst = FETCHW(), len = FETCHW();
            uint32_t n = len ? len : 0x10000;
            PUSH(Y); PUSH(A); PUSH(X);
            int alt = 0;
            for (uint32_t i = 0; i < n; i++) {
                uint8_t v = RD(src);
                WR(dst, v);
                switch (op) {
                case 0x73: src++; dst++; break;                    /* TII */
                case 0xC3: src--; dst--; break;                    /* TDD */
                case 0xD3: src++; break;                           /* TIN */
                case 0xE3: src++; dst += alt ? -1 : 1; break;      /* TIA */
                case 0xF3: src += alt ? -1 : 1; dst++; break;      /* TAI */
                }
                alt ^= 1;
            }
            X = PULL(); A = PULL(); Y = PULL();
            cyc -= (int)((17 + 6 * n) << spdsh);
            break; }
        default:
            /* undefined opcodes act as NOP on the HuC6280 */
            CYC(2); break;
        }
    }

    p->A = A; p->X = X; p->Y = Y; p->S = S; p->PC = PC;
    p->P = GETP();
    int used = cycles - cyc + p->cycles_extra;
    p->cycles_extra = 0;
    return used;
}
