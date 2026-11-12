/* runs a game with the reference CPU or the new one, idle skip optional; prints hashes */
#include "../core/pce.h"
#include <stdio.h>
#include <stdlib.h>
#ifdef USE_REF
int cpu_run_ref(pce_t *p, int cycles);
#endif
