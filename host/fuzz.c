/* random-instruction differential test: reference core vs table-driven core */
#include "../core/pce.h"
#include <stdio.h>
#include <stdlib.h>
int cpu_run_ref(pce_t *p, int cycles);
static pce_t A, B;
static uint8_t rom[0x10000 + 64];
static uint32_t rs = 12345;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static void remap(pce_t *p) { for (int i = 0; i < 8; i++) pce_map(p, i); }
int main(void)
{
    memcpy(rom, "PCE64GM1", 8);
    rom[12] = 0; rom[13] = 1; rom[14] = 0; rom[15] = 0;   /* 64 KB rom */
    pce_load(&A, rom, sizeof rom);
    pce_reset(&A);
    A.idle_skip = 0;
    long bad = 0, n = 0;
    for (long t = 0; t < 1500000 && bad < 10; t++) {
        /* random machine state: RAM, registers, mapping (RAM at slots 1-6, ROM at 0 and 7) */
        if ((t & 255) == 0) for (int i = 0; i < 0x2000; i++) A.ram[i] = rnd();
        for (int i = 0; i < 0x200; i++) A.ram[i] = rnd();   /* zero page + stack */
        A.A = rnd(); A.X = rnd(); A.Y = rnd(); A.S = rnd(); A.P = rnd() & ~0x30;
        A.speed = rnd() & 1;
        A.mpr[0] = 0; A.mpr[7] = 1;
        for (int i = 1; i < 7; i++) A.mpr[i] = 0xF8;
        remap(&A);
        A.PC = 0x2000 + (rnd() & 0x1FFF);   /* code in RAM */
        if (A.PC & 0x1000) A.PC = 0x3FF8 + (rnd() & 7);  /* straddling into slot 2 (also RAM) */
        for (int i = 0; i < 8; i++) A.ram[(A.PC + i) & 0x1FFF] = rnd();
        uint8_t op = rnd();
        if (op == 0x73 || op == 0xC3 || op == 0xD3 || op == 0xE3 || op == 0xF3) {
            /* keep block transfers short and inside RAM */
            A.ram[(A.PC + 5) & 0x1FFF] = rnd() & 0x1F; A.ram[(A.PC + 6) & 0x1FFF] = 0;
            A.ram[(A.PC + 2) & 0x1FFF] = 0x20 + (rnd() & 0x1F); A.ram[(A.PC + 4) & 0x1FFF] = 0x20 + (rnd() & 0x1F);
        }
        if (op == 0x53) A.ram[(A.PC + 1) & 0x1FFF] &= 0x7C; /* TAM: keep slots 0/1/7 */
        A.ram[A.PC & 0x1FFF] = op;
        if ((op == 0x6C || op == 0x7C || op == 0x8D || op == 0x9D || op == 0x99 || op == 0x8E || op == 0x8C || op == 0x9C || op == 0x9E) && 0) {}
        A.irq_lines = 0; A.irq_pend = 0;
        B = A; remap(&B);
        int budget = A.speed ? 1 : 4;
        int ca = cpu_run_ref(&A, budget);
        int cb = cpu_run(&B, budget);
        n++;
        if (A.A != B.A || A.X != B.X || A.Y != B.Y || A.S != B.S || A.P != B.P || A.PC != B.PC ||
            memcmp(A.ram, B.ram, 0x2000) || memcmp(A.mpr, B.mpr, 8) || ca != cb) {
            printf("op %02x: ref A=%02x X=%02x Y=%02x S=%02x P=%02x PC=%04x cyc=%d | new A=%02x X=%02x Y=%02x S=%02x P=%02x PC=%04x cyc=%d ram=%s\n",
                   op, A.A, A.X, A.Y, A.S, A.P, A.PC, ca, B.A, B.X, B.Y, B.S, B.P, B.PC, cb, memcmp(A.ram, B.ram, 0x2000) ? "DIFF" : "same");
            bad++;
        }
    }
    printf("%ld random instructions, %ld mismatches\n", n, bad);
    return bad != 0;
}
