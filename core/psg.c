/* HuC6280 PSG: 6 wavetable channels, DDA, noise on channels 4-5.
 * Samples are produced per scanline into a stereo int16 buffer. */
#include "pce.h"
#include <math.h>

#define PSG_CLOCK 3579545u

static int32_t amp_tab[96];
static int amp_ready = 0;
static void amp_init(void);

void psg_init(pce_t *p, int rate, int16_t *buf, int cap)
{
    psg_t *s = &p->psg;
    memset(s, 0, sizeof(*s));
    s->rate = rate;
    s->buf = buf ? buf : s->outbuf;
    s->bufcap = buf ? cap : (int)(sizeof s->outbuf / sizeof s->outbuf[0]);
    /* output samples per scanline, 16.16 */
    double lines_per_sec = (double)PSG_CLOCK * 6.0 / 1365.0;   /* 21.477 MHz / 1365 */
    s->step = (uint32_t)((double)rate / lines_per_sec * 65536.0);
    /* attenuation in 1.5 dB units -> linear amplitude (Q12); one channel at
       full volume peaks around 16*amp, six channels must fit in int16 */
    for (int ch = 0; ch < 6; ch++) { s->ch[ch].lfsr = 1; s->ch[ch].bal = 0xFF; s->ch[ch].dirty = 1; }
    s->mainbal = 0xFF;
    s->anydirty = 1;
    s->kstep = (uint32_t)(((uint64_t)PSG_CLOCK << 16) / (uint64_t)rate);
    if (!amp_ready) amp_init();
}


static void amp_init(void)
{
    for (int u = 0; u < 96; u++) {
        double db = -1.5 * u;
        amp_tab[u] = (u >= 90) ? 0 : (int32_t)(330.0 * pow(10.0, db / 20.0));
    }
    amp_ready = 1;
}

static inline __attribute__((always_inline)) void psg_apply(psg_t *s, int a, uint8_t v)
{
    psg_ch_t *c = &s->ch[s->sel < 6 ? s->sel : 0];
    /* wave/DDA data (6) is read directly by the mixer and needs no recompute */
    if (a >= 2 && a <= 7 && a != 6 && s->sel < 6) { c->dirty = 1; s->anydirty = 1; }
    switch (a) {
    case 0: s->sel = v & 7; break;
    case 1: s->mainbal = v; for (int i = 0; i < 6; i++) s->ch[i].dirty = 1; s->anydirty = 1; break;
    case 2: if (s->sel < 6) c->freq = (c->freq & 0xF00) | v; break;
    case 3: if (s->sel < 6) c->freq = (c->freq & 0x0FF) | ((v & 0x0F) << 8); break;
    case 4:
        if (s->sel >= 6) break;
        if ((c->ctrl & 0x40) && !(v & 0x40)) c->wpos = 0;
        if ((v & 0xC0) == 0x40) c->wpos = 0;
        c->ctrl = v;
        break;
    case 5: if (s->sel < 6) c->bal = v; break;
    case 6:
        if (s->sel >= 6) break;
        if (c->ctrl & 0x40) c->dda = v & 0x1F;
        else { c->wave[c->wpos] = v & 0x1F; c->wpos = (c->wpos + 1) & 31; c->tdirty = 1; }
        break;
    case 7: if (s->sel >= 4 && s->sel < 6) c->noise = v; break;
    case 8: s->lfofreq = v; break;
    case 9: s->lfoctrl = v; break;
    }
}

static inline int units(int vol, int lb, int mb)
{
    if (!lb || !mb || !vol) return 95;
    return (31 - vol) + 2 * (15 - lb) + 2 * (15 - mb);
}

PCE_WARM static void psg_recompute(psg_t *s)
{
    s->active = 0;
    for (int i = 0; i < 6; i++) {
        psg_ch_t *c = &s->ch[i];
        if (c->dirty) {
            int32_t al0 = c->al, ar0 = c->ar;
            c->dirty = 0;
            c->al = c->ar = 0; c->step = 0;
            if (c->ctrl & 0x80) {
                int vol = c->ctrl & 0x1F;
                c->al = amp_tab[units(vol, c->bal >> 4, s->mainbal >> 4)];
                c->ar = amp_tab[units(vol, c->bal & 15, s->mainbal & 15)];
                if (!(c->ctrl & 0x40)) {
                    if (i >= 4 && (c->noise & 0x80)) {
                        int nf = (~c->noise) & 0x1F;
                        if (!nf) nf = 1;
                        c->step = s->kstep / (64u * nf);
                    } else {
                        uint32_t per = c->freq ? c->freq : 0x1000;
                        if (per >= 8) c->step = s->kstep / per;
                        else c->al = c->ar = 0;   /* above hearing: silent */
                    }
                }
            }
            if (c->al != al0 || c->ar != ar0) {
                for (int v = 0; v < 32; v++) { c->lutl[v] = (v - 16) * c->al; c->lutr[v] = (v - 16) * c->ar; }
                c->tdirty = 1;
            }
        }
        if (c->al || c->ar) s->active |= 1 << i;
    }
    s->anydirty = 0;
}

/* Register writes are logged with their scanline and replayed when the frame
 * is synthesized, so the channel state is touched once per frame instead of
 * once per line. Samples come out exactly as if generated line by line. */
void psg_write(pce_t *p, int a, uint8_t v)
{
    psg_t *s = &p->psg;
    if (s->nlog < PSG_LOG_MAX) {
        int ln = p->in_cpu ? pce_now(p) / CYCLES_PER_LINE : p->line;
        if (ln >= LINES_PER_FRAME) ln = LINES_PER_FRAME - 1;
        s->log[s->nlog].line = (uint16_t)ln;
        s->log[s->nlog].reg = (uint8_t)a;
        s->log[s->nlog].val = v;
        s->nlog++;
    } else {
        psg_apply(s, a, v);      /* log full: apply now (slightly early) */
    }
}

/* n samples with the current channel state.
 * Each waveform channel has a table tt[] giving, per wave position, its left
 * and right contribution packed in one 64-bit word (each lane offset by PB so
 * it stays positive): a sample is one table load and add per channel, and the
 * lanes are separated once at the end. With all of a channel's state in
 * registers this is a handful of instructions per channel and sample. */
#define PB (1 << 20)
static void build_tt(psg_ch_t *c)
{
    for (int i = 0; i < 32; i++) {
        int w = c->wave[i];
        c->tt[i] = ((uint64_t)(uint32_t)(c->lutl[w] + PB) << 32) | (uint32_t)(c->lutr[w] + PB);
    }
    c->tdirty = 0;
}
static inline void put(int16_t *o, int32_t l, int32_t r)
{
    if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
    if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    *(uint32_t *)o = (uint32_t)l << 16 | (uint16_t)r;
#else
    o[0] = (int16_t)l; o[1] = (int16_t)r;
#endif
}
#define CH(j)   uint32_t p##j = ph[j], s##j = st[j]; const uint64_t *t##j = tt[j];
#define STEP(j) a += t##j[(p##j >> 16) & 31]; p##j += s##j;
#define SAVE(j) wc[j]->phase = p##j;
#define OUT()   put(out + 2 * k, (int32_t)(a >> 32) - bias, (int32_t)(uint32_t)a - bias)
#define PACK(l, r) (((uint64_t)(uint32_t)(l) << 32) | (uint32_t)(r))
/* DDA value changes inside a run (sample offset, channel, new value) */
typedef struct { int16_t off; uint8_t ch, val; } dda_ev_t;
/* The run in segments between DDA changes (one segment without them); the
 * DDA channels' sum is the base of each sample in the segment: b_ */
#define SEGS(BODY) \
    for (int k0 = 0, e_ = 0; k0 < n; ) { \
        int k1 = n; uint64_t b_ = base; \
        if (use_dd) { \
            for (; e_ < nev && ev[e_].off <= k0; e_++) { \
                unsigned i_ = ev[e_].ch, v_ = ev[e_].val; \
                if (dmask & (1u << i_)) { psg_ch_t *c_ = &s->ch[i_]; L += c_->lutl[v_] - c_->lutl[dcur[i_]]; R += c_->lutr[v_] - c_->lutr[dcur[i_]]; } \
                dcur[i_] = v_; } \
            b_ = PACK(L + PB, R + PB); \
            if (e_ < nev && ev[e_].off < k1) k1 = ev[e_].off; \
        } \
        for (int k = k0; k < k1; k++) { BODY } \
        k0 = k1; }
static inline __attribute__((always_inline)) void gen_noise_t(psg_t *s, int16_t *out, int n, const dda_ev_t *ev, int nev, const int use_dd,
    unsigned dmask, uint8_t *dcur, int32_t L, int32_t R, const uint64_t base, int32_t bias, const int nw, const int nn,
    uint32_t *ph, uint32_t *st, const uint64_t **tt, psg_ch_t **wc, psg_ch_t **noise)
{
    uint32_t np[2], ns[2], lf[2]; int no[2]; uint64_t lo[2], hi[2], cur[2];
    uint32_t P_[5], S_[5]; const uint64_t *T_[5];
    for (int j = 0; j < nw; j++) { P_[j] = ph[j]; S_[j] = st[j]; T_[j] = tt[j]; }
    for (int j = 0; j < nn; j++) {
        psg_ch_t *c = noise[j];
        np[j] = c->nphase; ns[j] = c->step; lf[j] = c->lfsr; no[j] = c->noise_out;
        lo[j] = PACK(c->lutl[0] + PB, c->lutr[0] + PB);
        hi[j] = PACK(c->lutl[31] + PB, c->lutr[31] + PB);
        cur[j] = PACK(c->lutl[no[j] + 16] + PB, c->lutr[no[j] + 16] + PB);
    }
    int32_t nbias = bias + nn * PB;
    SEGS(
        uint64_t a = b_;
        for (int j = 0; j < nw; j++) { a += T_[j][(P_[j] >> 16) & 31]; P_[j] += S_[j]; }
        for (int j = 0; j < nn; j++) {
            np[j] += ns[j];
            if (np[j] >= 0x10000) {
                uint32_t l = lf[j];
                do {
                    np[j] -= 0x10000;
                    uint32_t bit = ((l >> 0) ^ (l >> 1) ^ (l >> 11) ^ (l >> 12) ^ (l >> 17)) & 1;
                    l = (l >> 1) | (bit << 17);
                } while (np[j] >= 0x10000);
                lf[j] = l;
                no[j] = (l & 1) ? 15 : -16;
                cur[j] = (l & 1) ? hi[j] : lo[j];
            }
            a += cur[j];
        }
        put(out + 2 * k, (int32_t)(a >> 32) - nbias, (int32_t)(uint32_t)a - nbias);
    )
    for (int j = 0; j < nn; j++) { psg_ch_t *c = noise[j]; c->nphase = np[j]; c->lfsr = lf[j]; c->noise_out = no[j]; }
    for (int j = 0; j < nw; j++) wc[j]->phase = P_[j];
}
static inline __attribute__((always_inline)) void gen_t(psg_t *s, int16_t *out, int n, const uint8_t *d0, const dda_ev_t *ev, int nev, const int use_dd)
{
    if (s->anydirty) psg_recompute(s);
    unsigned act = s->active;
    if (!act) { memset(out, 0, 4 * n); return; }
    /* with DDA changes: the DDA channels' sum from the values at the run's start */
    int32_t L = 0, R = 0;
    uint8_t dcur[6];
    unsigned dmask = 0;
    if (use_dd)
        for (int i = 0; i < 6; i++) {
            dcur[i] = d0[i];
            psg_ch_t *c = &s->ch[i];
            if ((act & (1u << i)) && (c->ctrl & 0x40)) { dmask |= 1u << i; L += c->lutl[dcur[i]]; R += c->lutr[dcur[i]]; }
        }
    int nw = 0;
    int32_t fl = 0, fr = 0;
    uint32_t ph[6], st[6];
    const uint64_t *tt[6];
    psg_ch_t *wc[6];
    psg_ch_t *noise[2]; int nn = 0;
    for (int i = 0; i < 6; i++) {
        if (!(act & (1u << i))) continue;
        psg_ch_t *c = &s->ch[i];
        if (c->ctrl & 0x40) { if (!use_dd) { fl += c->lutl[c->dda]; fr += c->lutr[c->dda]; } }
        else if (i >= 4 && (c->noise & 0x80)) noise[nn++] = c;
        else {
            if (c->tdirty) build_tt(c);
            tt[nw] = c->tt; ph[nw] = c->phase; st[nw] = c->step; wc[nw] = c; nw++;
        }
    }
    /* base: the DDA channels, offset like a table entry; every lane stays positive */
    int32_t bias = (nw + 1) * PB;
    const uint64_t base = PACK(fl + PB, fr + PB);
    if (nn) {
        /* noise: per-sample shift register (its state in registers, its
           contribution packed like a table entry); one specialised loop per
           channel count, so the compiler keeps everything in registers */
#define NZ(NW, NN) case (NW) * 2 + (NN) - 1: gen_noise_t(s, out, n, ev, nev, use_dd, dmask, dcur, L, R, base, bias, NW, NN, ph, st, tt, wc, noise); return;
        switch (nw * 2 + nn - 1) {
        NZ(0, 1) NZ(0, 2) NZ(1, 1) NZ(1, 2) NZ(2, 1) NZ(2, 2) NZ(3, 1) NZ(3, 2) NZ(4, 1) NZ(4, 2) NZ(5, 1)
        }
#undef NZ
        return;
    }
    switch (nw) {
    case 0: SEGS( uint64_t a = b_; OUT(); ) break;
    case 1: { CH(0) SEGS( uint64_t a = b_; STEP(0) OUT(); ) SAVE(0) break; }
    case 2: { CH(0) CH(1) SEGS( uint64_t a = b_; STEP(0) STEP(1) OUT(); ) SAVE(0) SAVE(1) break; }
    case 3: { CH(0) CH(1) CH(2) SEGS( uint64_t a = b_; STEP(0) STEP(1) STEP(2) OUT(); ) SAVE(0) SAVE(1) SAVE(2) break; }
    case 4: { CH(0) CH(1) CH(2) CH(3) SEGS( uint64_t a = b_; STEP(0) STEP(1) STEP(2) STEP(3) OUT(); )
              SAVE(0) SAVE(1) SAVE(2) SAVE(3) break; }
    case 5: { CH(0) CH(1) CH(2) CH(3) CH(4) SEGS( uint64_t a = b_; STEP(0) STEP(1) STEP(2) STEP(3) STEP(4) OUT(); )
              SAVE(0) SAVE(1) SAVE(2) SAVE(3) SAVE(4) break; }
    default: { CH(0) CH(1) CH(2) CH(3) CH(4) CH(5) SEGS( uint64_t a = b_; STEP(0) STEP(1) STEP(2) STEP(3) STEP(4) STEP(5) OUT(); )
              SAVE(0) SAVE(1) SAVE(2) SAVE(3) SAVE(4) SAVE(5) break; }
    }
}
PCE_WARM static void __attribute__((noinline)) gen(psg_t *s, int16_t *out, int n) { gen_t(s, out, n, NULL, NULL, 0, 0); }
PCE_WARM static void __attribute__((noinline)) gen_dd(psg_t *s, int16_t *out, int n, const uint8_t *d0, const dda_ev_t *ev, int nev)
{
    gen_t(s, out, n, d0, ev, nev, 1);
}

/* What applying this line's writes does to the samples generated before them:
   0 nothing (selects, LFO, writes to channels that are off), 1 only new DDA
   values (handled inside the run), 2 more than that (end the run first). */
static int audible(psg_t *s, int li)
{
    int kind = 0;
    unsigned sel = s->sel;
    uint8_t ctrl[6];
    for (int i = 0; i < 6; i++) ctrl[i] = s->ch[i].ctrl;
    int line = s->log[li].line;
    for (; li < s->nlog && s->log[li].line == line; li++) {
        unsigned a = s->log[li].reg, v = s->log[li].val;
        switch (a) {
        case 0: sel = v & 7; break;
        case 8: case 9: break;
        case 1: return 2;
        case 4:
            if (sel < 6) { if ((ctrl[sel] | v) & 0x80) return 2; ctrl[sel] = v; }
            break;
        case 6:
            if (sel < 6 && (ctrl[sel] & 0x80)) {
#ifdef PSG_NODD
                return 2;
#endif
                if (ctrl[sel] & 0x40) kind = 1; else return 2; }
            break;
        case 2: case 3: case 5: case 7:
            if (sel < 6 && (ctrl[sel] & 0x80)) return 2;
            break;
        default: return 2;
        }
    }
    return kind;
}

long psg_stat_log, psg_stat_runs;
PCE_WARM void psg_frame(pce_t *p)
{
    psg_t *s = &p->psg;
    psg_stat_log += s->nlog;
#ifdef PSG_LOGHASH
    { extern uint32_t psg_log_hash;
      for (int i = 0; i < s->nlog; i++) { psg_log_hash = (psg_log_hash ^ ((uint32_t)s->log[i].line << 16 | s->log[i].reg << 8 | s->log[i].val)) * 16777619u; } }
#endif
    int li = 0;
    int run = 0;                       /* samples pending with the current state */
    static dda_ev_t ev[PSG_LOG_MAX];
    int nev = 0;
    uint8_t d0[6];                     /* DDA values at the start of the run */
    for (int i = 0; i < 6; i++) d0[i] = s->ch[i].dda;
#define FLUSH() do { if (s->buf && s->bufpos + 2 * run <= s->bufcap) { \
            if (nev) gen_dd(s, s->buf + s->bufpos, run, d0, ev, nev); else gen(s, s->buf + s->bufpos, run); \
            s->bufpos += 2 * run; } \
        run = 0; nev = 0; psg_stat_runs++; } while (0)
    for (int line = 0; line < LINES_PER_FRAME; line++) {
        if (li < s->nlog && s->log[li].line == line) {
            int kind = audible(s, li);
            if (run && kind == 2) FLUSH();
            for (; li < s->nlog && s->log[li].line == line; li++) {
                unsigned a = s->log[li].reg, v = s->log[li].val;
                if (a == 6 && run && s->sel < 6 && (s->ch[s->sel].ctrl & 0x40)) {
                    ev[nev].off = run; ev[nev].ch = s->sel; ev[nev].val = v & 0x1F; nev++;
                }
                psg_apply(s, a, v);
            }
            if (!run) for (int i = 0; i < 6; i++) d0[i] = s->ch[i].dda;
        }
        s->frac += s->step;
        run += s->frac >> 16;
        s->frac &= 0xFFFF;
    }
    if (run) FLUSH();
#undef FLUSH
    for (; li < s->nlog; li++) psg_apply(s, s->log[li].reg, s->log[li].val);
    s->nlog = 0;
}
