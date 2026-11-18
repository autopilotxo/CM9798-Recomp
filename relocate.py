"""relocate.py - apply relocations.txt to generated C (the gen/ files, or gen.py's output).

usage: python3 relocate.py [gendir]      (default: gen next to this file)

relocations.txt:
  move <from> <to> <new>            addresses in [from, to) move to new + (address - from)
  blocks <from> <to> <base> <old end> <new end> <new>
                                    the same for data in blocks that grow: an address base + off
                                    (off as one block sees it) goes to new + off, or, for off past
                                    the block's old end (a footer), to new + new end + (off - old end);
                                    the instructions that step from block to block need imm too
  imm <instruction> <old> <new>     a constant in one instruction

Only constants that also appear in the instruction's own disassembly (the /* addr: asm */ comment
at the start of each line) are changed, so nothing else can be caught by accident. Running it
again changes nothing: a moved address is no longer in its old range."""
import os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
LINE = re.compile(r'^(\s*/\* ([0-9a-f]+): (.*?) \*/)(.*)$')
HEX = re.compile(r'0x([0-9a-f]+)u')

def load(path=os.path.join(HERE, 'relocations.txt')):
    moves, imms = [], {}
    for l in open(path):
        l = l.split('#')[0].split()
        if not l: continue
        if l[0] == 'move': moves.append(tuple(int(x, 16) for x in l[1:4]) + (None,))
        elif l[0] == 'blocks':     # blocks <from> <to> <base> <old part end> <new part end> <new base>
            lo, hi, base, old_end, new_end, nb = (int(x, 16) for x in l[1:7])
            moves.append((lo, hi, nb, (base, old_end, new_end)))
        elif l[0] == 'imm': imms[int(l[1], 16)] = (int(l[2], 16), int(l[3], 16))
        else: raise SystemExit('relocations.txt: ' + ' '.join(l))
    return moves, imms

def relocate_code(code, moves, imms, counts=None):
    out = []
    for ln in code.split('\n'):
        m = LINE.match(ln)
        if m:
            head, addr, asm, body = m.group(1), int(m.group(2), 16), m.group(3), m.group(4)
            asm_vals = {int(h, 16) for h in re.findall(r'0x([0-9a-f]+)', asm)}
            def sub(h):
                v = int(h.group(1), 16)
                if v in asm_vals:
                    for lo, hi, nb, blk in moves:
                        if lo <= v < hi:
                            if counts is not None: counts['move'] = counts.get('move', 0) + 1
                            if blk:                 # within a block: the part after old_end moves to new_end
                                base, old_end, new_end = blk; off = v - base
                                return f'0x{nb + (off if off < old_end else new_end + off - old_end):x}u'
                            return f'0x{nb + (v - lo):x}u'
                return h.group(0)
            body = HEX.sub(sub, body)
            if addr in imms:
                old, new = imms[addr]
                body2 = re.sub(rf'\b0x{old:x}u\b', f'0x{new:x}u', body)
                if body2 != body and counts is not None: counts['imm'] = counts.get('imm', 0) + 1
                body = body2
            ln = head + body
        out.append(ln)
    return '\n'.join(out)

if __name__ == '__main__':
    d = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, 'gen')
    moves, imms = load(); counts = {}
    for f in sorted(os.listdir(d)):
        if not (f.startswith('game_') and f.endswith('.c')): continue
        p = os.path.join(d, f); s = open(p).read(); t = relocate_code(s, moves, imms, counts)
        if t != s: open(p, 'w').write(t)
    print('relocated:', counts.get('move', 0), 'addresses,', counts.get('imm', 0), 'constants')
