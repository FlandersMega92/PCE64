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
    return s[2] if (s[1]==0 or a<s[0]+s[1]+32) else '?'+s[2]
I=collections.Counter(); D=collections.Counter(); A=collections.Counter()
tot={'I':0,'D':0,'A':0}
for l in open(sys.argv[1]):
    p=l.split()
    if len(p)!=3: continue
    k,a,n=p[0],int(p[1],16),int(p[2])
    if k in ('I','D','A'):
        {'I':I,'D':D,'A':A}[k][name(a)]+=n; tot[k]+=n
fr=int(sys.argv[3]) if len(sys.argv)>3 else 120
print('per frame: I misses',tot['I']//fr,' D misses',tot['D']//fr)
print('I-miss by function:'); [print(f'  {n//fr:8d} {f}') for f,n in I.most_common(12)]
print('D-miss by code:'); [print(f'  {n//fr:8d} {f}') for f,n in D.most_common(8)]
print('D-miss by data:'); [print(f'  {n//fr:8d} {f}') for f,n in A.most_common(6)]
