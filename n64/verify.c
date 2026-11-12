/* N64 test: recompiled and interpreted machines side by side, frame by frame.
 * On the first frame whose state differs, both rerun that frame logging every
 * CPU run, and the first differing run is printed. */
#include <libdragon.h>
#include <malloc.h>
#include "pce.h"
#include "script_input.h"
static pce_t P, Q, SP, SQ;
#define LST (MAX_WIDTH + 32)
static uint8_t lb[(SCREEN_LINES + 1) * LST], ls[(SCREEN_LINES + 1) * LST]; static uint16_t se[2 * SCREEN_LINES], se2[2 * SCREEN_LINES];
static struct vlog_s la[4096], lq[4096];
static int vwa[8192][4], vwq[8192][4];
static void fix(pce_t *p) { pce_relocate(p); }
int main(void)
{
    debug_init_isviewer(); jit_init();
    uint32_t off = 0x110000; while (io_read(0x10000000 + off) != 0x50434536) off += 0x10000;
    uint32_t rs = io_read(0x10000000 + off + 12), total = (rs + 64 + 15) & ~15;
    uint8_t *blob = memalign(16, total); data_cache_hit_writeback_invalidate(blob, total); dma_read(blob, off, total); data_cache_hit_invalidate(blob, total);
    static int16_t ab[4096], ab2[4096];
    pce_load(&P, blob, total); psg_init(&P, 32000, ab, 4096);
    P.layer_bg = lb + 16; P.layer_spr = ls + 16; P.layer_stride = LST; P.spr_ext = se; pce_reset(&P);
    memcpy(&Q, &P, sizeof P); fix(&Q); Q.psg.buf = ab2; Q.spr_ext = se2; Q.force_interp = 1;
    int si = 0; uint8_t pad = 0;
    P.vwlog = vwa; Q.vwlog = vwq;
#ifdef JIT_WATCH
    { extern pce_t *jw_p; jw_p = &P; }
#endif
    for (int fr = 0; fr < 3000; fr++) {
        while (si < (int)(sizeof script / sizeof script[0]) && script[si].f <= (uint32_t)fr) pad = script[si++].p;
        P.pad[0] = Q.pad[0] = pad;
        memcpy(&SP, &P, sizeof P); memcpy(&SQ, &Q, sizeof Q);
        P.psg.bufpos = Q.psg.bufpos = 0;
        if (fr < 4) { P.vlog = la; P.vn = 0; Q.vlog = lq; Q.vn = 0; P.vwn = Q.vwn = 0; P.rn = Q.rn = 0; }
        pce_run_frame(&P); pce_run_frame(&Q);
        if (fr < 4) {
            for (int i = 0; i < P.vn && i < Q.vn && i < 4096; i++)
                if (memcmp(&la[i], &lq[i], sizeof la[i])) {
                    debugf("VERIFY live run %d of frame %d: jit fc=%d end=%d used=%d pc=%04x regs=%08lx h=%08lx | int fc=%d end=%d used=%d pc=%04x regs=%08lx h=%08lx\n", i, fr,
                        la[i].fc, la[i].end, la[i].used, la[i].pc, (unsigned long)la[i].a, (unsigned long)la[i].ram,
                        lq[i].fc, lq[i].end, lq[i].used, lq[i].pc, (unsigned long)lq[i].a, (unsigned long)lq[i].ram);
                    if (i) debugf("VERIFY   previous run %d: fc=%d end=%d used=%d pc=%04x\n", i - 1, la[i-1].fc, la[i-1].end, la[i-1].used, la[i-1].pc);
                    break;
                }
            for (int i = 0; i < P.vwn || i < Q.vwn; i++) {
                if (i < P.vwn && i < Q.vwn && !memcmp(P.vwlog[i], Q.vwlog[i], 12)) continue;
                debugf("VERIFY live vram write %d: jit %04x=%04x @%d | int %04x=%04x @%d\n", i, P.vwlog[i][0], P.vwlog[i][1], P.vwlog[i][2], Q.vwlog[i][0], Q.vwlog[i][1], Q.vwlog[i][2]);
                break;
            }
            P.vlog = Q.vlog = NULL;
        }
        if (pce_hash(&P) != pce_hash(&Q) || P.PC != Q.PC) {
            debugf("VERIFY first differing frame %d\n", fr);
#ifdef JIT_WATCH
            { extern void jit_dump(pce_t *, uint32_t); jit_dump(&P, 0xe07d); jit_dump(&P, 0xe084); jit_dump(&P, 0xe06e); }
#endif
            { int d = -1; for (int i = 0; i < 0x2000; i++) if (P.ram[i] != Q.ram[i]) { d = i; break; }
              int e = -1; for (int i = 0; i < 0x8000; i++) if (P.vdc.vram[i] != Q.vdc.vram[i]) { e = i; break; }
              int r = -1; for (int i = 0; i < 20; i++) if (P.vdc.reg[i] != Q.vdc.reg[i]) { r = i; break; }
              debugf("VERIFY ramdiff@%x (%02x/%02x) vramdiff@%x (%04x/%04x) regdiff %d | P %02x/%02x tm %d/%d irq %x/%x cx %d/%d st %x/%x pal %d\n",
                     d, d >= 0 ? P.ram[d] : 0, d >= 0 ? Q.ram[d] : 0, e, e >= 0 ? P.vdc.vram[e] : 0, e >= 0 ? Q.vdc.vram[e] : 0, r,
                     P.P, Q.P, P.tm_count, Q.tm_count, P.irq_pend, Q.irq_pend, P.cycles_extra, Q.cycles_extra, P.vdc.status, Q.vdc.status,
                     memcmp(P.vce.pal, Q.vce.pal, 1024) != 0); }
            memcpy(&P, &SP, sizeof P); fix(&P); memcpy(&Q, &SQ, sizeof Q); fix(&Q); P.vwlog = vwa; Q.vwlog = vwq;
            P.vlog = la; P.vn = 0; Q.vlog = lq; Q.vn = 0;
            P.rn = Q.rn = 0; P.vwn = Q.vwn = 0;
#ifdef PCE_VTRACE
            /* trace the first differing run (the interpreter instruction by instruction) */
            Q.vtrace_run1 = VTRACE_RUN + 1; P.vtrace_run1 = VTRACE_RUN + 1;
#endif
            pce_run_frame(&P);
            pce_run_frame(&Q);
            for (int i = 0; i < P.vwn || i < Q.vwn; i++) {
                if (i < P.vwn && i < Q.vwn && !memcmp(P.vwlog[i], Q.vwlog[i], 12)) continue;
                debugf("VERIFY vram write %d: jit %04x=%04x @%d pc %x | int %04x=%04x @%d pc %x\n", i,
                       P.vwlog[i][0], P.vwlog[i][1], P.vwlog[i][2], P.vwlog[i][3], Q.vwlog[i][0], Q.vwlog[i][1], Q.vwlog[i][2], Q.vwlog[i][3]);
                break;
            }
            for (int i = 0; i < P.rn || i < Q.rn; i++) debugf("VERIFY io %d: jit pos=%d io=%x pc=%x | int pos=%d io=%x pc=%x\n", i, P.rlog[i][0], P.rlog[i][1], P.rlog[i][2], Q.rlog[i][0], Q.rlog[i][1], Q.rlog[i][2]);
            for (int i = 0; i < P.vn && i < Q.vn && i < 4096; i++) {
                if (memcmp(&la[i], &lq[i], sizeof la[i])) {
                    debugf("VERIFY run %d: jit fc=%d end=%d used=%d pc=%04x regs=%08lx h=%08lx | int fc=%d end=%d used=%d pc=%04x regs=%08lx h=%08lx\n", i,
                        la[i].fc, la[i].end, la[i].used, la[i].pc, (unsigned long)la[i].a, (unsigned long)la[i].ram,
                        lq[i].fc, lq[i].end, lq[i].used, lq[i].pc, (unsigned long)lq[i].a, (unsigned long)lq[i].ram);
                    if (i) debugf("VERIFY previous run %d: fc=%d end=%d used=%d pc=%04x\n", i - 1, la[i-1].fc, la[i-1].end, la[i-1].used, la[i-1].pc);
                    break;
                }
            }
            break;
        }
        if (fr % 100 == 0) debugf("VERIFY frame %d ok\n", fr);
    }
    debugf("VERIFY DONE\n");
    for (;;) {}
}
