import struct,sys,re,collections
p=sys.argv[1]; out=sys.argv[2]; SHIFT=int(sys.argv[3],16) if len(sys.argv)>3 else 0
d=open(p,'rb').read()
mz=0x26654; le=0x290a4
H=lambda off,fmt='<I':struct.unpack_from(fmt,d,le+off)[0]
npages=H(0x14); ps=H(0x28); last=H(0x2c); dp=mz+H(0x80)
objs=[struct.unpack_from('<6I',d,le+H(0x40)+24*n) for n in range(H(0x44))]
# page map (LE: 4 bytes each, page number in bytes 1..2 big-endian-ish)
pm=[]
for k in range(npages):
    b=d[le+H(0x48)+4*k:le+H(0x48)+4*k+4]; pm.append((b[0]<<16)|(b[1]<<8)|b[2])
mem={}
for oi,(vs,base,fl,pmi,pmn,_) in enumerate(objs):
    buf=bytearray(vs)
    for j in range(pmn):
        pg=pmi+j; num=pm[pg-1]
        sz= last if pg==npages else ps
        off=dp+(num-1)*ps
        chunk=d[off:off+sz]; buf[j*ps:j*ps+len(chunk)]=chunk[:max(0,min(len(chunk),vs-j*ps))]
    mem[oi+1]=(base+SHIFT,buf)
fp=[struct.unpack_from('<I',d,le+H(0x68)+4*k)[0] for k in range(npages+1)]
# map global page -> (obj, offset in obj)
pg2=[None]*(npages+1)
for oi,(vs,base,fl,pmi,pmn,_) in enumerate(objs):
    for j in range(pmn): pg2[pmi+j]=(oi+1,j*ps)
fixtargets=[]
for p_ in range(1,npages+1):
    a=le+H(0x6c)+fp[p_-1]; b=le+H(0x6c)+fp[p_]
    so,po=pg2[p_]; base,buf=mem[so]
    while a<b:
        src=d[a]; flg=d[a+1]; a+=2
        lst= src&0x20
        if lst: cnt=d[a]; a+=1
        else: soff=struct.unpack_from('<h',d,a)[0]; a+=2
        tobj=d[a]; a+=1
        if flg&0x10: toff=struct.unpack_from('<I',d,a)[0]; a+=4
        else: toff=struct.unpack_from('<H',d,a)[0]; a+=2
        offs=[struct.unpack_from('<h',d,a+2*k)[0] for k in range(cnt)] if lst else [soff]
        if lst: a+=2*cnt
        val=mem[tobj][0]+toff
        for s in offs:
            o=po+s
            if 0<=o and o+4<=len(buf): struct.pack_into('<I',buf,o,val)
            elif 0<=o<len(buf):  # straddles page; write partial
                bb=struct.pack('<I',val)
                for k in range(4):
                    if o+k<len(buf): buf[o+k]=bb[k]
            fixtargets.append((mem[so][0]+o,val))
for k,(base,buf) in mem.items():
    open(f'{out}/obj{k}_{base:08x}.bin','wb').write(buf)
open(f'{out}/fixups.txt','w').write('\n'.join(f'{a:08x} -> {v:08x}' for a,v in sorted(set(fixtargets))))
print('written',[ (hex(b),len(x)) for b,x in mem.values()])
