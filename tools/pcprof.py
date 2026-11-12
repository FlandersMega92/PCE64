# pcprof.py HIST SYMS FRAMES : cycles per frame by function (P lines from XVSF_PCPROF)
import bisect,collections,sys
syms=[]
for l in open(sys.argv[2]):
    p=l.split()
    if len(p)==4: syms.append((int(p[0],16),int(p[1],16),p[3]))
    elif len(p)==3: syms.append((int(p[0],16),0,p[2]))
syms.sort(); addrs=[s[0] for s in syms]
def name(a):
    i=bisect.bisect_right(addrs,a)-1
    if i<0: return '?'
    s=syms[i]
    return s[2] if (s[1]==0 or a<s[0]+s[1]+32) else 'JIT/?'+s[2]
P=collections.Counter(); PA=collections.Counter(); tot=0
for l in open(sys.argv[1]):
    p=l.split()
    if len(p)==3 and p[0]=='P':
        a=int(p[1],16); n=int(p[2]); P[name(a)]+=n; PA[a]+=n; tot+=n
fr=int(sys.argv[3])
print('total clock/frame', tot//fr)
for f,n in P.most_common(int(sys.argv[4]) if len(sys.argv)>4 else 30): print(f'  {n//fr:9d} {100*n/tot:5.1f}% {f}')
if len(sys.argv)>5:
    fn=sys.argv[5]
    print('hot PCs in',fn)
    for a,n in sorted(PA.items(), key=lambda x:-x[1]):
        if name(a)==fn: print(f'   {a:08x} {n//fr}')
        if n < tot/2000: break
