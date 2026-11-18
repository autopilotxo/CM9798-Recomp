"""lift.py - x86-32 (objdump AT&T text) -> C lifter for CM2E16.EXE.

Each guest function becomes `void f_XXXXXX(void)`. Registers are globals,
flags (CF/ZF/SF/OF) are locals spilled around calls, the guest stack is real
(call pushes the return address, ret pops it) and memory is the flat 32-bit
guest space in M. See rt/rt.h.
"""
import re, collections

R32 = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
R16 = {'ax': 'eax', 'cx': 'ecx', 'dx': 'edx', 'bx': 'ebx', 'sp': 'esp', 'bp': 'ebp', 'si': 'esi', 'di': 'edi'}
R8L = {'al': 'eax', 'cl': 'ecx', 'dl': 'edx', 'bl': 'ebx'}
R8H = {'ah': 'eax', 'ch': 'ecx', 'dh': 'edx', 'bh': 'ebx'}
SEGS = ('es', 'cs', 'ss', 'ds', 'fs', 'gs')
SEGREGS = tuple('%' + s for s in SEGS)


class Unsup(Exception):
    pass


def split_ops(s):
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch == '(':
            depth += 1
        if ch == ')':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur.strip()); cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def regsize(o):
    r = o[1:]
    if r in R32: return 32
    if r in R16: return 16
    if r in R8L or r in R8H: return 8
    return None


def is_reg(o): return o.startswith('%') and ':' not in o and regsize(o) is not None
def is_imm(o): return o.startswith('$')


def imm(o):
    t = o[1:]
    v = -int(t[1:], 16) if t.startswith('-') else int(t, 16)
    return v & 0xFFFFFFFF


SEGIDX = {'es': 0, 'cs': 1, 'ss': 2, 'ds': 3, 'fs': 4, 'gs': 5}
CTX = {'segaware': False}


def ea(o, lea=False):
    seg = None
    m0 = re.match(r'^%(' + '|'.join(SEGS) + r'):', o)
    if m0:
        seg = m0.group(1); o = o[m0.end():]
    m = re.fullmatch(r'(-?0x[0-9a-f]+)?(?:\((%\w+)?(?:,(%\w+)(?:,(\d))?)?\))?', o)
    if not m or (m.group(1) is None and m.group(2) is None and m.group(3) is None):
        raise Unsup('ea ' + o)
    parts = []
    if m.group(1):
        d = int(m.group(1), 16) & 0xFFFFFFFF
        parts.append(f'0x{d:x}u')
    if m.group(2) and m.group(2) != '%eiz':
        parts.append(m.group(2)[1:])
    if m.group(3) and m.group(3) != '%eiz':
        s = int(m.group(4) or 1)
        parts.append(m.group(3)[1:] + ('' if s == 1 else f'*{s}u'))
    e = '(uint32_t)(' + ('+'.join(parts) or '0u') + ')'
    if lea:
        return e
    if seg is None and CTX['segaware'] and m.group(2) not in ('%esp', '%ebp'):
        seg = 'ds'
    if seg in ('es', 'ds', 'fs', 'gs'):
        return f'(uint32_t)(segbase_[{SEGIDX[seg]}]+{e})'
    return e


def rd(o, w):
    if is_imm(o): return f'0x{imm(o) & ((1 << w) - 1):x}u'
    if is_reg(o):
        r = o[1:]
        if r in R32: return r
        if r in R16: return f'({R16[r]}&0xFFFFu)'
        if r in R8L: return f'({R8L[r]}&0xFFu)'
        if r in R8H: return f'(({R8H[r]}>>8)&0xFFu)'
    return {8: 'RD8', 16: 'RD16', 32: 'RD32'}[w] + '(' + ea(o) + ')'


def wr(o, w, v):
    if is_reg(o):
        r = o[1:]
        if r in R32: return f'{r}=(uint32_t)({v});'
        if r in R16: return f'SET16({R16[r]},{v});'
        if r in R8L: return f'SET8L({R8L[r]},{v});'
        if r in R8H: return f'SET8H({R8H[r]},{v});'
    if is_imm(o): raise Unsup('write to immediate')
    return {8: 'WR8', 16: 'WR16', 32: 'WR32'}[w] + '(' + ea(o) + ',' + v + ');'


COND = {'e': 'zf', 'z': 'zf', 'ne': '!zf', 'nz': '!zf', 'l': '(sf!=of)', 'nge': '(sf!=of)', 'ge': '(sf==of)',
        'nl': '(sf==of)', 'le': '(zf||sf!=of)', 'ng': '(zf||sf!=of)', 'g': '(!zf&&sf==of)', 'nle': '(!zf&&sf==of)',
        'b': 'cf', 'c': 'cf', 'nae': 'cf', 'ae': '!cf', 'nb': '!cf', 'nc': '!cf', 'be': '(cf||zf)', 'na': '(cf||zf)',
        'a': '(!cf&&!zf)', 'nbe': '(!cf&&!zf)', 's': 'sf', 'ns': '!sf', 'o': 'of', 'no': '!of'}
SUF = {'b': 8, 'w': 16, 'l': 32}
ALU = {'add', 'adc', 'sub', 'sbb', 'cmp', 'and', 'or', 'xor', 'test', 'inc', 'dec', 'neg', 'not', 'mov', 'shl', 'sal',
       'shr', 'sar', 'rol', 'ror', 'rcl', 'rcr', 'imul', 'mul', 'idiv', 'div', 'push', 'pop', 'lea', 'xchg'}


def base_size(mn):
    if mn in ALU: return mn, None
    if mn[-1:] in SUF and mn[:-1] in ALU: return mn[:-1], SUF[mn[-1]]
    return mn, None


def opsize(ops, forced):
    # operand size comes from the destination (last operand) first: for `shl %cl,%edx`
    # the 8-bit count register must not decide the width
    if forced: return forced
    for o in reversed(ops):
        if is_reg(o): return regsize(o)
    return 32


def MSK(w): return {8: '0xFFu', 16: '0xFFFFu', 32: '0xFFFFFFFFu'}[w]


class Fn:
    def __init__(s, start, addrs, ins, funcset, jtabs, fix):
        s.start, s.addrs, s.ins, s.funcset, s.jtabs, s.fix = start, addrs, ins, funcset, jtabs, fix
        s.body = set(addrs); s.labels = set(); s.unsup = collections.Counter(); s.cur = start

    # ---- control transfer helpers
    def target(s, t):
        if t in s.body:
            s.labels.add(t)
            # backward jump = loop: give the runtime a chance to deliver "interrupts"
            # (mouse/keyboard), like a real IRQ arriving in the middle of a busy-wait
            if t <= s.cur: return f'BACKEDGE; goto L_{t:x};'
            return f'goto L_{t:x};'
        if t in s.funcset: return f'SPILL; f_{t:06x}(); return;'
        return f'SPILL; rt_call(0x{t:x}u); return;'

    # after a call, g_ret holds the address the guest code actually returned to; if it is not
    # our continuation (get-PC tricks, returns that skip a frame), unwind this C frame too
    def call(s, t, nxt):
        fn = f'f_{t:06x}()' if t in s.funcset else f'rt_call(0x{t:x}u)'
        return f'PUSH32(0x{nxt:x}u); SPILL; {fn}; RELOAD; if(g_ret!=0x{nxt:x}u) return;'

    @staticmethod
    def shift_count(op):
        return '(ecx&0x1Fu)' if op == '%cl' else f'0x{imm(op) & 31:x}u'

    # ---- one instruction
    def lift1(s, a):
        n, mn, opstr = s.ins[a]; nxt = a + n; s.cur = a
        PFX = set(SEGS) | {'notrack', 'bnd', 'data16', 'addr16', 'rep', 'repz', 'repe', 'repnz', 'repne', 'lock'}
        allt = (mn + ' ' + opstr).split()
        k_ = 0
        while k_ < len(allt) and allt[k_] in PFX: k_ += 1
        if k_ < len(allt):
            mn = ' '.join(allt[:k_ + 1]); opstr = ' '.join(allt[k_ + 1:])
        ops = split_ops(opstr)
        toks = mn.split(); rep = 0; force16 = False
        while toks and (toks[0] in SEGS or toks[0] in ('notrack', 'bnd')): toks = toks[1:]
        while toks and toks[0] in ('data16', 'addr16'):
            force16 = force16 or toks[0] == 'data16'; toks = toks[1:]
        if toks and toks[0] in ('rep', 'repz', 'repe'): rep = 1; toks = toks[1:]
        elif toks and toks[0] in ('repnz', 'repne'): rep = 2; toks = toks[1:]
        mn = toks[0] if toks else ''

        sm = re.fullmatch(r'(movs|stos|lods|scas|cmps)([bwl]?)', mn)
        if sm:
            k = sm.group(1); w = SUF.get(sm.group(2))
            if not w:
                regs = [regsize(o) for o in ops if is_reg(o)]
                w = regs[0] if regs else 32
            if k in ('scas', 'cmps'): return f'SPILL; rt_{k}({w // 8},{rep}); RELOAD;'
            return f'rt_{k}({w // 8},{rep});'
        if mn in ('ret', 'retl'):
            extra = imm(ops[0]) if ops else 0
            # push addr / ret is used as an indirect jump (e.g. Watcom int386x thunks):
            # if the popped address is not a known return site, run the code there instead.
            return (f'{{ uint32_t ra_=POP32(); esp+={extra}u; SPILL; '
                    f'if(!rt_is_retaddr(ra_)) rt_call(ra_); else g_ret=ra_; return; }}')
        if mn == 'lretw':
            extra = imm(ops[0]) if ops else 0
            return f'g_ret=POP16(); esp+={2 + extra}u; SPILL; return;'
        if mn in ('lret', 'lretl'):
            extra = imm(ops[0]) if ops else 0
            return f'g_ret=POP32(); esp+={4 + extra}u; SPILL; return;'  # far return pops EIP + CS
        if mn in ('iret', 'iretl'): return 'g_ret=POP32(); esp+=8u; SPILL; return;'
        if mn == 'jmp':
            o = ops[0]
            if re.fullmatch(r'0x[0-9a-f]+', o): return s.target(int(o, 16))
            t = rd(o[1:], 32)
            if a in s.jtabs:
                _, tb, cnt = s.jtabs[a]; cases = set()
                for k in range(cnt):
                    tt = s.fix[tb + 4 * k]
                    if tt in s.body:
                        s.labels.add(tt); cases.add(f'case 0x{tt:x}u: goto L_{tt:x};')
                return f'{{ uint32_t _t={t}; switch(_t){{ {" ".join(sorted(cases))} default: SPILL; rt_call(_t); return; }} }}'
            return f'{{ uint32_t _t={t}; SPILL; rt_call(_t); return; }}'
        if mn == 'call':
            o = ops[0]
            if re.fullmatch(r'0x[0-9a-f]+', o): return s.call(int(o, 16), nxt)
            return f'{{ uint32_t _t={rd(o[1:], 32)}; PUSH32(0x{nxt:x}u); SPILL; rt_call(_t); RELOAD; if(g_ret!=0x{nxt:x}u) return; }}'
        m = re.fullmatch(r'j(.+)', mn)
        if m and m.group(1) in COND: return f'if({COND[m.group(1)]}) {{ {s.target(int(ops[0], 16))} }}'
        if mn in ('jecxz', 'jcxz'): return f'if(ecx==0) {{ {s.target(int(ops[0], 16))} }}'
        if mn == 'loop': return f'if(--ecx) {{ {s.target(int(ops[0], 16))} }}'
        if mn in ('loope', 'loopz'): return f'if(--ecx && zf) {{ {s.target(int(ops[0], 16))} }}'
        if mn in ('loopne', 'loopnz'): return f'if(--ecx && !zf) {{ {s.target(int(ops[0], 16))} }}'
        m = re.fullmatch(r'set(.+)', mn)
        if m and m.group(1) in COND: return wr(ops[0], 8, f'({COND[m.group(1)]})?1u:0u')
        if mn.startswith('f'): return s.fpu(mn, ops)
        simple = {'nop': ';', 'fwait': ';', 'cli': ';', 'sti': ';', 'cld': 'g_df=0;', 'std': 'g_df=1;', 'clc': 'cf=0;',
                  'stc': 'cf=1;', 'cmc': 'cf^=1;', 'hlt': f'rt_unimpl(0x{a:x}u,"hlt");',
                  'int3': f'rt_unimpl(0x{a:x}u,"int3");'}
        if mn in simple: return simple[mn]
        if mn == 'int': return f'SPILL; rt_int(0x{imm(ops[0]):x}u); RELOAD;'
        if mn in ('in', 'inb', 'inw', 'inl'):
            port = '(edx&0xFFFFu)' if ops[0] == '(%dx)' else f'0x{imm(ops[0]):x}u'
            w = regsize(ops[1]); return wr(ops[1], w, f'rt_in({port},{w})')
        if mn in ('out', 'outb', 'outw', 'outl'):
            port = '(edx&0xFFFFu)' if ops[1] == '(%dx)' else f'0x{imm(ops[1]):x}u'
            w = regsize(ops[0]); return f'rt_out({port},{rd(ops[0], w)},{w});'
        if mn in ('lar', 'lsl', 'verr', 'verw', 'sidtl', 'sgdtl', 'sidt', 'sgdt', 'str', 'sldt', 'arpl', 'smsw',
                  'lidtl', 'lgdtl', 'lidt', 'lgdt', 'lmsw', 'invlpg', 'lldt', 'ltr', 'clts', 'wbinvd') or '%cr' in opstr or '%dr' in opstr:
            return f'SPILL; rt_sys(0x{a:x}u,"{mn}"); RELOAD;'
        if mn in ('cwtl', 'cwde'): return 'eax=(uint32_t)(int32_t)(int16_t)(eax&0xFFFF);'
        if mn in ('cbtw', 'cbw'): return 'SET16(eax,(uint32_t)(int16_t)(int8_t)(eax&0xFF));'
        if mn in ('cltd', 'cdq'): return 'edx=((int32_t)eax<0)?0xFFFFFFFFu:0u;'
        if mn in ('cwtd', 'cwd'): return 'SET16(edx,((int16_t)(eax&0xFFFF)<0)?0xFFFFu:0u);'
        if mn == 'sahf': return 'sf=(eax>>15)&1; zf=(eax>>14)&1; cf=(eax>>8)&1;'
        if mn == 'lahf': return 'SET8H(eax,(sf<<7)|(zf<<6)|2u|cf);'
        if mn in ('pushf', 'pushfl'): return 'SPILL; PUSH32(rt_eflags());'
        if mn == 'pushfw': return 'SPILL; PUSH16(rt_eflags());'
        if mn in ('popf', 'popfl'): return 'rt_set_eflags(POP32()); RELOAD;'
        if mn == 'leave': return 'esp=ebp; ebp=POP32();'
        if mn in ('pusha', 'pushal'):
            return '{ uint32_t s_=esp; PUSH32(eax); PUSH32(ecx); PUSH32(edx); PUSH32(ebx); PUSH32(s_); PUSH32(ebp); PUSH32(esi); PUSH32(edi); }'
        if mn in ('popa', 'popal'):
            return 'edi=POP32(); esi=POP32(); ebp=POP32(); esp+=4; ebx=POP32(); edx=POP32(); ecx=POP32(); eax=POP32();'
        if mn == 'enter':
            lvl = imm(ops[1]) & 31
            if lvl == 0: return f'PUSH32(ebp); ebp=esp; esp-=0x{imm(ops[0]):x}u;'
            return (f'{{ PUSH32(ebp); uint32_t fr_=esp; for(int i_=1;i_<{lvl};i_++){{ ebp-=4; PUSH32(RD32(ebp)); }} '
                    f'PUSH32(fr_); ebp=fr_; esp-=0x{imm(ops[0]):x}u; }}')
        if mn == 'aam':
            b = imm(ops[0]) if ops else 10
            return f'{{ uint32_t al_=eax&0xFFu; SET8H(eax,al_/{b}u); SET8L(eax,al_%{b}u); SZF(8,eax&0xFFu); }}'
        if mn in ('les', 'lds', 'lfs', 'lgs', 'lss'):
            e = ea(ops[0])
            return f'{{ uint32_t a_={e}; uint32_t o_=RD32(a_), s_=RD16(a_+4); SPILL; rt_set_seg({SEGIDX[mn[1:]]},s_); RELOAD; {wr(ops[1], 32, "o_")} }}'
        if mn in ('movsbl', 'movsbw', 'movswl', 'movzbl', 'movzbw', 'movzwl'):
            sw = {'b': 8, 'w': 16}[mn[4]]; dw = {'w': 16, 'l': 32}[mn[5]]
            v = rd(ops[0], sw)
            if mn[3] == 's': v = f'(uint32_t)(int32_t)(int{sw}_t){v}'
            return wr(ops[1], dw, v)
        if mn in ('bsr', 'bsf', 'bt', 'btl', 'shld', 'shrd', 'shldl', 'shrdl'):
            return s.bitops(mn, ops)

        base, forced = base_size(mn)
        if force16 and not forced: forced = 16
        w = opsize(ops, forced); M_ = MSK(w)
        if base == 'mov':
            if ops[0] in SEGREGS:
                return wr(ops[1], 16 if not is_reg(ops[1]) else w, f'sel_[{SEGIDX[ops[0][1:]]}]')
            if ops[1] in SEGREGS:
                return f'SPILL; rt_set_seg({SEGIDX[ops[1][1:]]},{rd(ops[0], 16 if not is_reg(ops[0]) else regsize(ops[0]))}&0xFFFFu); RELOAD;'
            return wr(ops[1], w, rd(ops[0], w))
        if base == 'lea': return wr(ops[1], 32, ea(ops[0], lea=True))
        if base == 'push':
            if ops[0] in SEGREGS: return f'PUSH32(sel_[{SEGIDX[ops[0][1:]]}]);'
            if is_imm(ops[0]): return f'PUSH32(0x{imm(ops[0]):x}u);'
            return f'PUSH16({rd(ops[0], 16)});' if w == 16 else f'PUSH32({rd(ops[0], 32)});'
        if base == 'pop':
            if ops[0] in SEGREGS: return f'SPILL; rt_set_seg({SEGIDX[ops[0][1:]]},POP32()&0xFFFFu); RELOAD;'
            return wr(ops[0], w, 'POP16()' if w == 16 else 'POP32()')
        if base == 'xchg':
            return f'{{ uint32_t _a={rd(ops[0], w)},_b={rd(ops[1], w)}; {wr(ops[0], w, "_b")} {wr(ops[1], w, "_a")} }}'
        if base in ('add', 'adc', 'sub', 'sbb', 'cmp', 'and', 'or', 'xor', 'test'):
            src, dst = ops
            hdr = f'{{ uint32_t a_={rd(dst, w)},b_={rd(src, w)},r_; '
            if base == 'add':
                body = f'r_=(a_+b_)&{M_}; cf=r_<a_; of=MSB({w},(~(a_^b_))&(a_^r_)); '
            elif base == 'adc':
                body = f'uint64_t t_=(uint64_t)a_+b_+cf; r_=(uint32_t)t_&{M_}; cf=(uint32_t)(t_>>{w})&1u; of=MSB({w},(~(a_^b_))&(a_^r_)); '
            elif base in ('sub', 'cmp'):
                body = f'r_=(a_-b_)&{M_}; cf=a_<b_; of=MSB({w},(a_^b_)&(a_^r_)); '
            elif base == 'sbb':
                body = f'uint64_t t_=(uint64_t)b_+cf; r_=(uint32_t)((uint64_t)a_-t_)&{M_}; cf=(uint64_t)a_<t_; of=MSB({w},(a_^b_)&(a_^r_)); '
            else:
                op = {'and': '&', 'or': '|', 'xor': '^', 'test': '&'}[base]
                body = f'r_=(a_{op}b_)&{M_}; cf=0; of=0; '
            body += f'SZF({w},r_); '
            if base not in ('cmp', 'test'): body += wr(dst, w, 'r_')
            return hdr + body + ' }'
        if base in ('inc', 'dec'):
            o = ops[0]; op = '+' if base == 'inc' else '-'
            ofc = f'(r_==(1u<<{w - 1}))' if base == 'inc' else f'(a_==(1u<<{w - 1}))'
            return f'{{ uint32_t a_={rd(o, w)},r_=(a_{op}1u)&{M_}; of={ofc}; SZF({w},r_); {wr(o, w, "r_")} }}'
        if base == 'neg':
            o = ops[0]
            return f'{{ uint32_t a_={rd(o, w)},r_=(0u-a_)&{M_}; cf=(a_!=0); of=(a_==(1u<<{w - 1})); SZF({w},r_); {wr(o, w, "r_")} }}'
        if base == 'not':
            o = ops[0]; return wr(o, w, f'(~{rd(o, w)})&{M_}')
        if base in ('shl', 'sal', 'shr', 'sar'):
            if len(ops) == 1: cnt, o = '1u', ops[0]
            else: cnt, o = s.shift_count(ops[0]), ops[1]
            if base in ('shl', 'sal'):
                core = f'uint64_t t_=(uint64_t)a_<<n_; r_=(uint32_t)t_&{M_}; cf=(uint32_t)(t_>>{w})&1u; of=MSB({w},r_)^cf;'
            elif base == 'shr':
                core = f'r_=a_>>n_; cf=(a_>>(n_-1))&1u; of=MSB({w},a_);'
            else:
                core = (f'int32_t s_=(int32_t)(a_<<(32-{w}))>>(32-{w}); uint32_t k_=n_>31?31:n_; '
                        f'r_=(uint32_t)(s_>>k_)&{M_}; cf=(uint32_t)(s_>>(k_-1))&1u; of=0;')
            return f'{{ uint32_t a_={rd(o, w)},n_={cnt},r_; if(n_){{ {core} SZF({w},r_); {wr(o, w, "r_")} }} }}'
        if base in ('rol', 'ror', 'rcl', 'rcr'):
            if len(ops) == 1: cnt, o = '1u', ops[0]
            else: cnt, o = s.shift_count(ops[0]), ops[1]
            k = {'rol': 0, 'ror': 1, 'rcl': 2, 'rcr': 3}[base]
            return f'{{ SPILL; uint32_t r_=rt_rot({k},{w},{rd(o, w)},{cnt}); RELOAD; {wr(o, w, "r_")} }}'
        if base == 'imul':
            if len(ops) == 1:
                o = ops[0]
                if w == 32: return f'{{ int64_t p_=(int64_t)(int32_t)eax*(int32_t){rd(o, 32)}; eax=(uint32_t)p_; edx=(uint32_t)((uint64_t)p_>>32); cf=of=(p_!=(int32_t)p_); }}'
                if w == 16: return f'{{ int32_t p_=(int16_t)(eax&0xFFFF)*(int16_t){rd(o, 16)}; SET16(eax,(uint32_t)p_); SET16(edx,(uint32_t)p_>>16); cf=of=(p_!=(int16_t)p_); }}'
                return f'{{ int32_t p_=(int8_t)(eax&0xFF)*(int8_t){rd(o, 8)}; SET16(eax,(uint32_t)p_); cf=of=(p_!=(int8_t)p_); }}'
            if len(ops) == 2: src, dst = ops; a_, b_ = rd(dst, w), rd(src, w)
            else: src, mid, dst = ops; a_, b_ = rd(mid, w), rd(src, w)
            st = f'int{w}_t'
            return f'{{ int64_t p_=(int64_t)({st}){a_}*(int64_t)({st}){b_}; cf=of=(p_!=({st})p_); {wr(dst, w, "(uint32_t)p_")} }}'
        if base == 'mul':
            o = ops[0]
            if w == 32: return f'{{ uint64_t p_=(uint64_t)eax*{rd(o, 32)}; eax=(uint32_t)p_; edx=(uint32_t)(p_>>32); cf=of=(edx!=0); }}'
            if w == 16: return f'{{ uint32_t p_=(eax&0xFFFFu)*{rd(o, 16)}; SET16(eax,p_); SET16(edx,p_>>16); cf=of=((p_>>16)!=0); }}'
            return f'{{ uint32_t p_=(eax&0xFFu)*{rd(o, 8)}; SET16(eax,p_); cf=of=((p_>>8)!=0); }}'
        if base in ('idiv', 'div'):
            o = ops[0]; de = f'rt_divide_error(0x{a:x}u)'
            if w == 32 and base == 'idiv':
                return f'{{ int64_t n_=(int64_t)(((uint64_t)edx<<32)|eax); int32_t d_=(int32_t){rd(o, 32)}; if(!d_) {de}; else {{ eax=(uint32_t)(n_/d_); edx=(uint32_t)(n_%d_); }} }}'
            if w == 32:
                return f'{{ uint64_t n_=((uint64_t)edx<<32)|eax; uint32_t d_={rd(o, 32)}; if(!d_) {de}; else {{ eax=(uint32_t)(n_/d_); edx=(uint32_t)(n_%d_); }} }}'
            if w == 16 and base == 'idiv':
                return f'{{ int32_t n_=(int32_t)(((edx&0xFFFFu)<<16)|(eax&0xFFFFu)); int32_t d_=(int16_t){rd(o, 16)}; if(!d_) {de}; else {{ SET16(eax,(uint32_t)(n_/d_)); SET16(edx,(uint32_t)(n_%d_)); }} }}'
            if w == 16:
                return f'{{ uint32_t n_=((edx&0xFFFFu)<<16)|(eax&0xFFFFu); uint32_t d_={rd(o, 16)}; if(!d_) {de}; else {{ SET16(eax,n_/d_); SET16(edx,n_%d_); }} }}'
            if base == 'idiv':
                return f'{{ int32_t n_=(int16_t)(eax&0xFFFFu); int32_t d_=(int8_t){rd(o, 8)}; if(!d_) {de}; else {{ SET8L(eax,(uint32_t)(n_/d_)); SET8H(eax,(uint32_t)(n_%d_)); }} }}'
            return f'{{ uint32_t n_=eax&0xFFFFu; uint32_t d_={rd(o, 8)}; if(!d_) {de}; else {{ SET8L(eax,n_/d_); SET8H(eax,n_%d_); }} }}'
        raise Unsup(mn)

    def bitops(s, mn, ops):
        b = mn if mn in ('bsr', 'bsf') else mn.rstrip('l')
        if b == 'bt':
            w = opsize(ops, 32 if mn == 'btl' else None)
            return f'cf=({rd(ops[1], w)}>>({rd(ops[0], w)}&{w - 1}u))&1u;'
        if b in ('bsr', 'bsf'):
            w = opsize(ops, None); f = '31-__builtin_clz(v_)' if b == 'bsr' else '__builtin_ctz(v_)'
            return f'{{ uint32_t v_={rd(ops[0], w)}; zf=(v_==0); if(v_) {{ {wr(ops[1], w, f)} }} }}'
        cnt = s.shift_count(ops[0]); src, dst = ops[1], ops[2]
        if b == 'shld': core = 'r_=(d_<<n_)|(s_>>(32-n_)); cf=(d_>>(32-n_))&1u;'
        else: core = 'r_=(d_>>n_)|(s_<<(32-n_)); cf=(d_>>(n_-1))&1u;'
        return f'{{ uint32_t d_={rd(dst, 32)},s_={rd(src, 32)},n_={cnt},r_; if(n_){{ {core} SZF(32,r_); {wr(dst, 32, "r_")} }} }}'

    def fpu(s, mn, ops):
        def st(o):
            m = re.fullmatch(r'%st(?:\((\d)\))?', o); return int(m.group(1) or 0) if m else None
        memw = {'s': 'F32', 'l': 'F64'}
        if mn in ('flds', 'fldl'): return f'FPUSH((long double)RD{memw[mn[-1]]}({ea(ops[0])}));'
        if mn == 'fldt': return f'FPUSH(fpu_ld80({ea(ops[0])}));'
        if mn == 'fld': return f'{{ long double v_=ST({st(ops[0])}); FPUSH(v_); }}'
        if mn == 'fildll': return f'{{ uint64_t v_; memcpy(&v_,M+{ea(ops[0])},8); FPUSH((long double)(int64_t)v_); }}'
        if mn in ('filds', 'fildl'):
            t = '(int16_t)RD16' if mn == 'filds' else '(int32_t)RD32'
            return f'FPUSH((long double)({t}({ea(ops[0])})));'
        consts = {'fldz': '0.0L', 'fld1': '1.0L', 'fldlg2': '0.301029995663981195214L', 'fldpi': '3.14159265358979323846L',
                  'fldln2': '0.693147180559945309417L', 'fldl2e': '1.44269504088896340736L'}
        if mn in consts: return f'FPUSH({consts[mn]});'
        if mn in ('fsts', 'fstl', 'fstps', 'fstpl'):
            w = memw[mn[-1]]; ty = 'float' if w == 'F32' else 'double'
            return f'WR{w}({ea(ops[0])},({ty})ST(0));' + (' FPOP();' if 'p' in mn else '')
        if mn == 'fstpt': return f'fpu_st80({ea(ops[0])},ST(0)); FPOP();'
        if mn in ('fst', 'fstp'): return f'ST({st(ops[0])})=ST(0);' + (' FPOP();' if mn == 'fstp' else '')
        if mn == 'fistpll': return f'{{ int64_t v_=fpu_round(ST(0)); memcpy(M+{ea(ops[0])},&v_,8); FPOP(); }}'
        if mn in ('fists', 'fistl', 'fistps', 'fistpl'):
            w = {'s': 16, 'l': 32}[mn[-1]]
            return f'{{ int64_t v_=fpu_round(ST(0)); WR{w}({ea(ops[0])},(uint32_t)(int{w}_t)v_); }}' + (' FPOP();' if 'p' in mn else '')
        m = re.fullmatch(r'f(add|sub|subr|mul|div|divr)(p?)([sl]?)', mn)
        if m:
            op, p, sz = m.groups()
            fwd = {'add': '{d}={d}+{s};', 'sub': '{d}={d}-{s};', 'subr': '{d}={s}-{d};',
                   'mul': '{d}={d}*{s};', 'div': '{d}={d}/{s};', 'divr': '{d}={s}/{d};'}
            if sz: return fwd[op].format(d='ST(0)', s=f'(long double)RD{memw[sz]}({ea(ops[0])})')
            if len(ops) == 1 and not p: return fwd[op].format(d='ST(0)', s=f'ST({st(ops[0])})')
            if len(ops) == 2 and st(ops[1]) == 0 and not p:
                return fwd[op].format(d='ST(0)', s=f'ST({st(ops[0])})')
            # destination is st(j), j!=0 (incl. operand-less pop forms = st(1)).
            # GNU AT&T syntax swaps sub/subr and div/divr for these forms.
            j = st(ops[1]) if len(ops) == 2 else (st(ops[0]) if len(ops) == 1 else 1)
            rev = {'sub': 'subr', 'subr': 'sub', 'div': 'divr', 'divr': 'div'}.get(op, op)
            return fwd[rev].format(d=f'ST({j})', s='ST(0)') + (' FPOP();' if p else '')
        m = re.fullmatch(r'fi(add|sub|subr|mul|div|divr)([sl])', mn)
        if m:
            op, sz = m.groups(); t = '(int16_t)RD16' if sz == 's' else '(int32_t)RD32'
            src = f'(long double)({t}({ea(ops[0])}))'
            return {'add': f'ST(0)+={src};', 'sub': f'ST(0)-={src};', 'subr': f'ST(0)={src}-ST(0);',
                    'mul': f'ST(0)*={src};', 'div': f'ST(0)/={src};', 'divr': f'ST(0)={src}/ST(0);'}[op]
        m = re.fullmatch(r'f(u?)com(p{0,2})([sl]?)', mn)
        if m:
            _, p, sz = m.groups()
            if sz: src = f'(long double)RD{memw[sz]}({ea(ops[0])})'
            elif ops: src = f'ST({st(ops[0])})'
            else: src = 'ST(1)'
            return f'fpu_compare(ST(0),{src});' + {'': '', 'p': ' FPOP();', 'pp': ' FPOP(); FPOP();'}[p]
        m = re.fullmatch(r'ficom(p?)([sl])', mn)
        if m:
            p, sz = m.groups(); t = '(int16_t)RD16' if sz == 's' else '(int32_t)RD32'
            return f'fpu_compare(ST(0),(long double)({t}({ea(ops[0])})));' + (' FPOP();' if p else '')
        if mn == 'ftst': return 'fpu_compare(ST(0),0.0L);'
        if mn in ('fnstsw', 'fstsw'):
            if not ops or ops[0] == '%ax': return 'SET16(eax,fpu_status());'
            return f'WR16({ea(ops[0])},fpu_status());'
        if mn in ('fnstcw', 'fstcw'): return f'WR16({ea(ops[0])},fcw);'
        if mn == 'fldcw': return f'fcw=RD16({ea(ops[0])}); fpu_setcw();'
        if mn in ('finit', 'fninit'): return 'ftop=0; fsw=0; fcw=0x37Fu; fpu_setcw();'
        if mn in ('fnclex', 'fclex'): return 'fsw&=0x7F00u;'
        if mn in ('fsave', 'fnsave'): return f'fpu_save({ea(ops[0])});'
        if mn == 'frstor': return f'fpu_restore({ea(ops[0])});'
        simple = {'fchs': 'ST(0)=-ST(0);', 'fabs': 'ST(0)=fabsl(ST(0));', 'fsqrt': 'ST(0)=sqrtl(ST(0));',
                  'frndint': 'ST(0)=(long double)fpu_round(ST(0));',
                  'fyl2x': '{ long double x_=ST(0); FPOP(); ST(0)=ST(0)*log2l(x_); }',
                  'fincstp': 'ftop=(ftop+1)&7;', 'fdecstp': 'ftop=(ftop-1)&7;', 'fwait': ';', 'fnop': ';'}
        if mn in simple: return simple[mn]
        if mn == 'fxch':
            i = st(ops[0]) if ops else 1
            return f'{{ long double t_=ST(0); ST(0)=ST({i}); ST({i})=t_; }}'
        raise Unsup(mn)

    # ---- whole function
    def emit(s):
        CTX['segaware'] = any(re.search(r',%ds$', s.ins[a][2]) or s.ins[a][2] == '%ds' and 'pop' in s.ins[a][1]
                              or s.ins[a][1] in ('lds',) for a in s.addrs)
        s.segaware = CTX['segaware']
        lines = []
        for a in s.addrs:
            try:
                c = s.lift1(a)
            except Unsup as e:
                s.unsup[str(e)] += 1; c = f'rt_unimpl(0x{a:x}u,"unsupported");'
            except Exception as e:
                s.unsup['ERR ' + type(e).__name__ + ' ' + s.ins[a][1]] += 1; c = f'rt_unimpl(0x{a:x}u,"lift error");'
            lines.append((a, c))
        # fall-through gotos (may add labels)
        tails = []
        for idx, (a, c) in enumerate(lines):
            n, mn, _ = s.ins[a]
            nxt = a + n
            following = lines[idx + 1][0] if idx + 1 < len(lines) else None
            ends = re.match(r'(?:\S+\s)*(jmp|ret|retl|lret|iret)$', mn) is not None
            s.cur = a
            tails.append(s.target(nxt) if (not ends and following != nxt) else '')
        out = [f'void f_{s.start:06x}(void) {{', '  FLAGS_LOCAL;', f'  TRACE_FN(0x{s.start:x}u);']
        if lines and lines[0][0] != s.start:
            # body contains code below the entry point (shared epilogues etc.): enter at the real start
            s.labels.add(s.start); out.append(f'  goto L_{s.start:x};')
        for idx, (a, c) in enumerate(lines):
            if a in s.labels: out.append(f' L_{a:x}:;')
            n, mn, ops = s.ins[a]
            cmt = (mn + ' ' + ops).replace('*/', '* /')
            out.append(f'  /* {a:06x}: {cmt} */ {c}')
            if tails[idx]: out.append('  ' + tails[idx])
        out.append('}')
        return '\n'.join(out)
