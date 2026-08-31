#!/usr/bin/env python3
"""MCS-251 SOURCE-MODE disassembler for the Plextor PX-716A firmware.

Source mode is byte-identical to binary mode with A5 prepended (SLEIGH GROUP3),
so we probe the stock rz-ghidra 80251:BE:24:default module with an injected A5
and subtract the prefix byte.  GROUP1 (plain 8051, opcodes < 0x60) is the
fallback when GROUP3 does not match.

NOTE: rz-ghidra's `pdj` (JSON) output is broken for this module -- it reports
`invalid` for instructions its own `pd` text output decodes correctly.  We
therefore parse `pd` text.  Batching uses ';'-separated -c commands; the
`@@=` iterator silently caps at ~115 results and must not be used.
"""
import re,subprocess
LINE=re.compile(r'0x([0-9a-f]{8})\s+(.*?)\s*$')

def _probe(slots,chunk=600):
    """slots: list of 8-byte buffers -> list of (size, text)"""
    res=[(1,'invalid')]*len(slots)
    for base in range(0,len(slots),chunk):
        part=slots[base:base+chunk]
        open('/var/tmp/_w.bin','wb').write(b''.join(part))
        cmds=';'.join('pd 2 @ %d'%(k*8) for k in range(len(part)))
        out=subprocess.run(["rizin","-q","-e","scr.color=0","-e","asm.arch=ghidra",
            "-e","asm.cpu=80251:BE:24:default","-c",cmds,"/var/tmp/_w.bin"],
            capture_output=True,text=True,timeout=600).stdout
        rows=[]
        for line in out.splitlines():
            m=LINE.search(line.strip())
            if m: rows.append((int(m.group(1),16),m.group(2)))
        # rows come in pairs: (slot_start, txt), (next_addr, _)
        i=0
        while i+1<len(rows):
            a,txt=rows[i]; b,_=rows[i+1]
            if a%8==0 and 0<b-a<=8:
                res[base+a//8]=(b-a,txt)
                i+=2
            else:
                i+=1
    return res

class SrcDis:
    def __init__(self,path): self.d=open(path,'rb').read()
    def window(self,start,span):
        d=self.d; self.start=start; self.span=span
        self.P=_probe([b'\xa5'+d[start+k:start+k+7] for k in range(span)])
        self.Q=_probe([      d[start+k:start+k+8]   for k in range(span)])
    def at(self,i):
        """-> (size, text, is_group3)"""
        b=self.d[self.start+i]
        if b==0xA5:
            s,txt=self.Q[i+1] if i+1<self.span else (1,'invalid')
            return 1+s,"[A5] "+txt,False
        s,txt=self.P[i]
        if 'invalid' not in txt and s>=2: return s-1,txt,True
        s,txt=self.Q[i]
        if 'invalid' in txt and b>=0x60: return 1,"db 0x%02x"%b,False
        return s,txt,False
    def dump(self,count,mark=()):
        i=0;n=0
        while i<self.span-8 and n<count:
            sz,txt,g3=self.at(i); off=self.start+i
            fl="   <<<< PATCH" if any(off<=m<off+sz for m in mark) else ""
            print("  0x%06x  %-12s %s%s"%(off,self.d[off:off+sz].hex(),txt,fl))
            i+=max(1,sz); n+=1
