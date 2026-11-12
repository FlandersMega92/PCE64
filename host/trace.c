#include "../core/pce.h"
#include <stdio.h>
#include <stdlib.h>
int pce_trace = 0;
static pce_t P; 
int main(int argc,char**argv){
  FILE*f=fopen(argv[1],"rb");fseek(f,0,2);long sz=ftell(f);fseek(f,0,0);uint8_t*b=malloc(sz);fread(b,1,sz,f);fclose(f);
  pce_load(&P,b,sz); static int16_t ab[4096]; psg_init(&P,32000,ab,4096); pce_reset(&P);
  long fr=atol(argv[2]); for(long i=0;i<fr;i++){P.psg.bufpos=0;pce_run_frame(&P);}
  pce_trace=atoi(argv[3]); P.psg.bufpos=0; pce_run_frame(&P);
}
