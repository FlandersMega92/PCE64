#include "../core/pce.h"
#include <stdio.h>
#include <stdlib.h>
int cpu_run_ref(pce_t *p, int cycles);
static pce_t P, A, B; 
static void clonep(pce_t *d, pce_t *s){ *d=*s; d->layer_bg=NULL; for(int i=0;i<8;i++) pce_map(d,i); }
int main(int argc,char**argv){
  FILE*f=fopen(argv[1],"rb");fseek(f,0,2);long sz=ftell(f);fseek(f,0,0);uint8_t*b=malloc(sz);fread(b,1,sz,f);fclose(f);
  pce_load(&P,b,sz); static int16_t ab[4096]; psg_init(&P,32000,ab,4096); pce_reset(&P);
  FILE*inf=fopen(argv[2],"r"); long ef[20000]; char es[20000][32]; int ne=0; char line[128];
  while(inf&&fgets(line,128,inf)){ long fr; char rest[64]=""; if(sscanf(line,"%ld %63[^\n]",&fr,rest)>=1){ef[ne]=fr; strcpy(es[ne],rest); ne++;}}
  int ei=0, bad=0; long total=0;
  for(long fr=0; fr<6000 && !bad; fr++){
    while(ei<ne&&ef[ei]<=fr){ uint8_t pad=0; char*s=es[ei]; if(strstr(s,"RUN"))pad|=PAD_RUN; if(strstr(s,"R"))pad|=PAD_RIGHT; if(strstr(s,"I"))pad|=PAD_I; P.pad[0]=pad; ei++;}
    P.psg.bufpos=0; pce_run_frame(&P);
    if(fr%97==0){
      clonep(&A,&P); clonep(&B,&P); A.idle_skip=B.idle_skip=0; A.psg.buf=B.psg.buf=NULL;
      for(int k=0;k<3000;k++){
        uint16_t pc=A.PC;
        int ca=cpu_run_ref(&A, A.speed?1:4); int cb=cpu_run(&B, B.speed?1:4); total++;
        if(A.A!=B.A||A.X!=B.X||A.Y!=B.Y||A.S!=B.S||A.P!=B.P||A.PC!=B.PC||memcmp(A.ram,B.ram,0x2000)||memcmp(A.mpr,B.mpr,8)||A.speed!=B.speed||memcmp(A.vdc.vram,B.vdc.vram,sizeof A.vdc.vram)){
          uint8_t *m=P.rmap[pc>>13];
          printf("MISMATCH frame %ld step %d at %04x op %02x: ref A=%02x X=%02x Y=%02x S=%02x P=%02x PC=%04x | new A=%02x X=%02x Y=%02x S=%02x P=%02x PC=%04x ram=%d\n",
            fr,k,pc,A.rmap[pc>>13]?A.rmap[pc>>13][pc&0x1fff]:0,A.A,A.X,A.Y,A.S,A.P,A.PC,B.A,B.X,B.Y,B.S,B.P,B.PC,memcmp(A.ram,B.ram,0x2000)!=0);
          bad=1; break; }
        if(ca!=cb && !(A.speed==0)) { /* cycle counts can differ only via rounding in slow mode */
          uint8_t op=A.rmap[pc>>13]?A.rmap[pc>>13][pc&0x1fff]:0;
          static int warned[256]; if(!warned[op]++) printf("cycles differ op %02x at %04x: ref %d new %d\n",op,pc,ca,cb); }
      }
    }
  }
  printf("%s: %ld lockstep instructions, %s\n", argv[1], total, bad?"MISMATCH":"all match");
}
