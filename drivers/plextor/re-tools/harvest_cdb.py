import re, collections
ASM="/tmp/claude-1000/-home-kgr-Git-accudisc/8314fd99-7d58-4b49-a17f-580f122705e6/scratchpad/re/ptpxl.asm"
lines=open(ASM).read().splitlines()
rx=re.compile(r'^\s+([0-9a-f]{6}):\t(.*)$')
insns=[]
for ln in lines:
    m=rx.match(ln)
    if m: insns.append((int(m.group(1),16), m.group(2).strip()))
func_start=[False]*len(insns)
for i,(a,t) in enumerate(insns):
    if i==0 or (insns[i-1][1].startswith('int3') and not t.startswith('int3')):
        func_start[i]=True
def frange(i):
    s=i
    while s>0 and not func_start[s]: s-=1
    return s
def emulate(s, call_i):
    esp=0; mem={}; regptr={}
    for i in range(s, call_i):
        a,t=insns[i]
        m=re.match(r'sub    esp,0x([0-9a-f]+)',t)
        if m: esp-=int(m.group(1),16); continue
        m=re.match(r'add    esp,0x([0-9a-f]+)',t)
        if m: esp+=int(m.group(1),16); continue
        if t.startswith('push'): esp-=4
        if t.startswith('pop'): esp+=4
        m=re.match(r'lea    (\w+),\[esp(?:\+0x([0-9a-f]+))?\]',t)
        if m:
            regptr[m.group(1)]=esp+(int(m.group(2),16) if m.group(2) else 0); continue
        m=re.match(r'mov    BYTE PTR \[esp(?:\+0x([0-9a-f]+))?\],0x([0-9a-f]+)',t)
        if m:
            mem[esp+(int(m.group(1),16) if m.group(1) else 0)]=int(m.group(2),16); continue
        m=re.match(r'mov    BYTE PTR \[esp(?:\+0x([0-9a-f]+))?\],(\w+)',t)
        if m:
            mem[esp+(int(m.group(1),16) if m.group(1) else 0)]='rr'; continue
    return mem,regptr
def cdb_template(mem, base):
    out=[]
    for k in range(0,12):
        v=mem.get(base+k)
        out.append('??' if v is None else ('rr' if v=='rr' else '%02x'%v))
    return out
# collect vendor sites
sites=[]
for i,(a,t) in enumerate(insns):
    if t.startswith('call') and '0x47b240' in t:
        s=frange(i); mem,regptr=emulate(s,i)
        best=None
        for reg,base in regptr.items():
            if base in mem and isinstance(mem[base],int) and mem[base]>=0xd0:
                best=base
        if best is not None:
            sites.append((insns[s][0], a, mem[best], cdb_template(mem,best)))
sites.sort(key=lambda x:(x[2],x[0]))
print("op  fn        call      CDB[0..11] template  (rr=from register/arg)")
for fn,call,op,tpl in sites:
    print("%02x  %06x  %06x  %s"%(op,fn,call,' '.join(tpl)))
