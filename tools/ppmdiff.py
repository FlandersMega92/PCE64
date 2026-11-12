import sys
def rd(p):
    d=open(p,'rb').read()
    parts=d.split(b'\n',3)
    w,h=map(int,parts[1].split()); return w,h,parts[3]
tot=0
for a,b in zip(sys.argv[1::2],sys.argv[2::2]):
    wa,ha,da=rd(a); wb,hb,db=rd(b)
    if (wa,ha)!=(wb,hb): print(a,'size differs'); continue
    n=sum(1 for i in range(0,len(da),3) if da[i:i+3]!=db[i:i+3])
    rows=sorted(set((i//3)//wa for i in range(0,len(da),3) if da[i:i+3]!=db[i:i+3]))
    print(f'{a.split("/")[-2]}/{a.split("/")[-1]}: {n} px differ' + (f' rows {rows[0]}-{rows[-1]} ({len(rows)})' if rows else ''))
