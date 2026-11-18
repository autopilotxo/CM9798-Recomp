"""Recursive-descent disassembly of CM2E16 object 1 using objdump as decoder.
Output: insns.json {addr: [len, mnem, ops]} and funcs.json {start: [addrs...]}"""
import subprocess,re,json,sys,collections
import glob
W=sys.argv[1]; CODE=sorted(glob.glob(W+'/obj1_*.bin'))[0]; BASE=int(CODE.rsplit('_',1)[1][:8],16); SHIFT=BASE-0x10000
code=open(CODE,'rb').read(); END=BASE+len(code)
RX=re.compile(r'^\s*([0-9a-f]+):\t((?:[0-9a-f]{2} )+)\s*\t?(.*)$')
def objdump(start,stop):
    out=subprocess.run(['objdump','-D','-w','-b','binary','-mi386',f'--adjust-vma={BASE:#x}',
        f'--start-address={start:#x}',f'--stop-address={stop:#x}',CODE],capture_output=True,text=True).stdout
    res={}
    for l in out.splitlines():
        m=RX.match(l)
        if not m: continue
        a=int(m.group(1),16); n=len(m.group(2).split()); t=m.group(3).strip()
        t=re.sub(r'\s+#.*$','',t); t=re.sub(r'\s+<[^>]*>','',t)
        parts=t.split(None,1)
        if not parts: continue
        mn=parts[0]; ops=parts[1] if len(parts)>1 else ''
        # prefixes like rep/repnz/lock/data16/addr16/cs/es segment
        while mn in('rep','repz','repe','repnz','repne','lock','data16','addr16','cs','ds','es','ss','fs','gs') and ops:
            p2=ops.split(None,1); mn=mn+' '+p2[0]; ops=p2[1] if len(p2)>1 else ''
        res[a]=[n,mn,ops]
    return res
lin=objdump(BASE,END)
ins=dict(lin)
def get(a):
    if a in ins: return ins[a]
    ins.update({k:v for k,v in objdump(a,min(a+0x200,END)).items() if k>=a and k not in ins or k==a})
    # ensure the window decode from a wins over linear-sweep misalignment
    w=objdump(a,min(a+0x200,END))
    for k,v in w.items(): ins.setdefault(k,v)
    ins[a]=w.get(a,ins.get(a))
    return ins.get(a)
fix=[tuple(int(x,16) for x in l.split(' -> ')) for l in open(W+'/fixups.txt')]
code_ptr_targets={t for s,t in fix if BASE<=t<END}
fix_by_src={s:t for s,t in fix}
seeds={0xbdc64+SHIFT}
for a,(n,mn,ops) in lin.items():
    if mn=='call' and re.fullmatch(r'0x[0-9a-f]+',ops):
        t=int(ops,16)
        if BASE<=t<END: seeds.add(t)
# code pointers stored in DATA object (function pointer tables) are function seeds
for s,t in fix:
    if BASE<=t<END and not (BASE<=s<END): seeds.add(t)
COND=re.compile(r'^j(n?[eolsgazcbp]|ae|be|ge|le|na|nb|nbe|nae|nl|nle|ng|nge|ecxz|cxz)$')
def succ(a,i):
    n,mn,ops=i; nxt=a+n
    if mn in('ret','iret','hlt','ud2','lret') or mn.startswith('ret') or mn.startswith('repz ret'): return [],None
    if mn=='jmp':
        if re.fullmatch(r'0x[0-9a-f]+',ops): return [int(ops,16)],None
        # indirect jmp: jump table? find fixups in table
        m=re.fullmatch(r'\*(?:%cs:)?0x([0-9a-f]+)\(,%e(..),4\)',ops)
        if m:
            tb=int(m.group(1),16); tg=[]; k=tb
            while k in fix_by_src and BASE<=fix_by_src[k]<END: tg.append(fix_by_src[k]); k+=4
            return tg,('jtab',tb,len(tg))
        return [],('ijmp',None,0)
    if COND.match(mn) or mn.startswith('loop'):
        return [int(ops,16),nxt],None
    if mn=='call': return [nxt],None
    return [nxt],None
# code pointers held in the CODE object (mov $fn,reg / tables in TEXT) are also entries,
# except jump-table slots, which are discovered below and stay intra-function labels
jt_slots=set()
for a,(n,mn,ops) in lin.items():
    m=re.fullmatch(r'\*(?:%cs:)?0x([0-9a-f]+)\(,%e(..),4\)',ops) if mn.endswith('jmp') else None
    if m:
        k=int(m.group(1),16)
        while k in fix_by_src and BASE<=fix_by_src[k]<END: jt_slots.add(k); k+=4
extra_seeds={t for s_,t in fix if BASE<=t<END and BASE<=s_<END and s_ not in jt_slots}-seeds
import os as _os
_pf=W+'/pruned.json'
if _os.path.exists(_pf): extra_seeds-=set(json.load(open(_pf)))
seeds|=extra_seeds
funcs={}; owner={}; jtabs={}; bad=set(); funcbad={}
seeds_strong=set(seeds)-extra_seeds
work=sorted(seeds); allseeds=set(seeds)
while work:
    f=work.pop()
    if f in funcs: continue
    body=set(); st=[f]; hit_bad=False
    while st:
        a=st.pop()
        if a in body: continue
        if a!=f and a in allseeds: continue      # tail jump into another function
        if not(BASE<=a<END): bad.add(a); continue
        i=get(a)
        if i is None or i[1]=='(bad)': bad.add(a); hit_bad=True; continue
        body.add(a)
        nx,extra=succ(a,i)
        if extra and extra[0]=='jtab': jtabs[a]=extra
        if i[1]=='call' and re.fullmatch(r'0x[0-9a-f]+',i[2]):
            t=int(i[2],16)
            if t not in allseeds and BASE<=t<END: allseeds.add(t); work.append(t)
        st.extend(nx)
    funcs[f]=sorted(body)
    funcbad[f]=hit_bad
# prune entries that were only seeded from code-object pointers and decode like data
core=collections.Counter()
for f,b in funcs.items():
    if f not in extra_seeds or f in seeds_strong:
        for a in b: core[ins[a][1]]+=1
vocab={m for m,c in core.items() if c>=3}|{'lret','lretl','iret','iretl','pusha','popa','pushal','popal','pushf','popf','pushfl','popfl','cli','sti','cld','std','lahf','sahf'}
# instructions a 1997 Watcom/DOS4GW program never contains: decoding them means we are in data
JUNK=re.compile(r'^(p[a-z]+|f(u?comi|icom|disi|eni|setpm|bstp|bld|xtract|prem|scale|cos|sin|ptan|patan|2xm1)|bound|into|arpl|aa[adms]|da[as]|xlat|ins[bwl]?|outs[bwl]?|icebp|ud[0-9]?|cpuid|rdtsc|ljmp|lcall|ss|addr16|lock|.*\(bad\).*|movq|emms|wbinvd|invd|rsm|sal[cl]$)')
changed=True
while changed:
    changed=False
    for f in list(funcs):
        if f in extra_seeds and f not in seeds_strong:
            if funcbad.get(f) or any(ins[a][1] not in vocab and JUNK.search(ins[a][1]) for a in funcs[f]):
                del funcs[f]; changed=True
pruned=len([f for f in extra_seeds if f not in funcs])
json.dump(sorted(set(json.load(open(_pf)) if _os.path.exists(_pf) else [])|{f for f in extra_seeds if f not in funcs}),open(_pf,'w'))
print('pointer-only entries kept',len([f for f in extra_seeds if f in funcs]),'pruned',pruned)
# also: jmp targets that land on other seeds are tail calls (fine)
n_ins=sum(len(v) for v in funcs.values())
cov=sum(ins[a][0] for v in funcs.values() for a in v)
print('functions',len(funcs),'instructions',n_ins,'bytes covered',cov,'of',len(code),f'({cov/len(code):.1%})','jump tables',len(jtabs),'bad',len(bad))
json.dump({'ins':{str(a):ins[a] for v in funcs.values() for a in v},'funcs':{str(k):v for k,v in funcs.items()},'jtabs':{str(k):v for k,v in jtabs.items()},'bad':sorted(bad)},open(W+'/disasm.json','w'))
mn=collections.Counter(ins[a][1] for v in funcs.values() for a in v)
open(W+'/mnem_reached.txt','w').write('\n'.join(f'{c} {m}' for m,c in mn.most_common()))
print('distinct mnemonics',len(mn))
