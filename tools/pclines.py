# pclines.py HIST ELF FRAMES FUNC [N]: hottest source lines inside FUNC (needs syms file HIST.syms)
import bisect,collections,sys,subprocess
hist,elf,fr,fn=sys.argv[1],sys.argv[2],int(sys.argv[3]),sys.argv[4]
N=int(sys.argv[5]) if len(sys.argv)>5 else 25
syms=[]
for l in open(hist+'.syms'):
    p=l.split()
    if len(p)==4: syms.append((int(p[0],16),int(p[1],16),p[3]))
syms.sort(); addrs=[s[0] for s in syms]
def name(a):
    i=bisect.bisect_right(addrs,a)-1
    return syms[i][2] if i>=0 and a<syms[i][0]+syms[i][1] else '?'
PA=collections.Counter()
for l in open(hist):
    p=l.split()
    if len(p)==3 and p[0]=='P':
        a=int(p[1],16)
        if name(a)==fn: PA[a]+=int(p[2])
addrs2=list(PA)
out=subprocess.run(['/opt/libdragon/bin/mips64-elf-addr2line','-e',elf,'-i','-f','-C']+[hex(a) for a in addrs2],capture_output=True,text=True).stdout
# with -i, variable lines per address; use separate calls instead for robustness
L=collections.Counter()
res=subprocess.run(['/opt/libdragon/bin/mips64-elf-addr2line','-e',elf]+[hex(a) for a in addrs2],capture_output=True,text=True).stdout.split('\n')
for a,line in zip(addrs2,res): L[line.split('/')[-1].split(' ')[0]]+=PA[a]
tot=sum(PA.values())
print(fn,'clock/frame',tot//fr)
for k,v in L.most_common(N): print(f'  {v//fr:8d} {k}')
