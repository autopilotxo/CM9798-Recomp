"""make_gen.py - make gen/ (the C translation of the game's code) from your own copy of the game.

    python3 make_gen.py <path to CM2E16.EXE> [gen]

The game's code isn't part of this source: every player makes it from their own CM2E16.EXE
(Championship Manager 97/98, v2.93). This
  1. checks the EXE is that version (size and CRC-32, as the program itself does),
  2. loads it the way DOS/4GW would (tools/gen/img.py: both objects, all fixups, 0x1000000 higher),
  3. decodes the instructions listed in functions.json (which instructions make up each function:
     addresses only) with objdump, giving the same disassembly the recompiler was developed on,
  4. runs gen.py, which writes gen/ and applies patches.txt and relocations.txt.
Needs Python 3 and GNU objdump (binutils; on a Mac "brew install binutils"; or set OBJDUMP).
Takes a few seconds."""
import json, os, re, subprocess, sys, tempfile, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
SIZE, CRC = 1407137, 0x359E908E          # rt/main.c: SUPPORTED_SIZE, SUPPORTED_CRC32
SHIFT = 0x1000000

def die(m): print(m); sys.exit(1)

if len(sys.argv) < 2: die(__doc__)
exe, out = sys.argv[1], (sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, 'gen'))
data = open(exe, 'rb').read()
if len(data) != SIZE or (zlib.crc32(data) & 0xFFFFFFFF) != CRC:
    die(f'{exe} is not CM2E16.EXE v2.93 (size {len(data)}, CRC-32 {zlib.crc32(data) & 0xFFFFFFFF:08X}; '
        f'expected {SIZE}, {CRC:08X}).')
def gnu_objdump():                      # (macOS's own objdump is LLVM's, whose output differs)
    for c in [os.environ.get('OBJDUMP'), 'objdump', 'gobjdump', '/opt/homebrew/opt/binutils/bin/objdump',
              '/usr/local/opt/binutils/bin/objdump']:
        if not c: continue
        try:
            v = subprocess.run([c, '--version'], capture_output=True, text=True).stdout
            if 'GNU' in v: return c
        except Exception: pass
    die('GNU objdump is needed: install binutils (Linux: binutils; Mac: brew install binutils; '
        'Windows/MSYS2: mingw-w64-ucrt-x86_64-binutils), or set OBJDUMP to its path.')
OBJDUMP = gnu_objdump()

W = tempfile.mkdtemp(prefix='cm9798gen_')
print('loading the EXE...')
subprocess.run([sys.executable, os.path.join(HERE, 'tools', 'gen', 'img.py'), exe, W, hex(SHIFT)], check=True)
code_file = [f for f in os.listdir(W) if f.startswith('obj1_')][0]
BASE = int(code_file[5:13], 16)
CODE = os.path.join(W, code_file); END = BASE + os.path.getsize(CODE)

# the same decoding as disasm.py (objdump text, prefixes joined to the mnemonic)
RX = re.compile(r'^\s*([0-9a-f]+):\t((?:[0-9a-f]{2} )+)\s*\t?(.*)$')
def objdump(start, stop):
    o = subprocess.run([OBJDUMP, '-D', '-w', '-b', 'binary', '-mi386', f'--adjust-vma={BASE:#x}',
                        f'--start-address={start:#x}', f'--stop-address={stop:#x}', CODE],
                       capture_output=True, text=True).stdout
    res = {}
    for l in o.splitlines():
        m = RX.match(l)
        if not m: continue
        a = int(m.group(1), 16); n = len(m.group(2).split()); t = m.group(3).strip()
        t = re.sub(r'\s+#.*$', '', t); t = re.sub(r'\s+<[^>]*>', '', t)
        parts = t.split(None, 1)
        if not parts: continue
        mn = parts[0]; ops = parts[1] if len(parts) > 1 else ''
        while mn in ('rep', 'repz', 'repe', 'repnz', 'repne', 'lock', 'data16', 'addr16', 'cs', 'ds', 'es', 'ss', 'fs', 'gs') and ops:
            p2 = ops.split(None, 1); mn = mn + ' ' + p2[0]; ops = p2[1] if len(p2) > 1 else ''
        res[a] = [n, mn, ops]
    return res

man = json.load(open(os.path.join(HERE, 'functions.json')))
funcs = {}
for k, deltas in man['funcs'].items():
    a = int(k, 16) + SHIFT; body = []
    for d in deltas: a += d; body.append(a)
    funcs[int(k, 16) + SHIFT] = body
need = sorted({a for b in funcs.values() for a in b})
print(f'decoding {len(need)} instructions in {len(funcs)} functions...')
lin = objdump(BASE, END)
ins = {}
for a in need:
    if a in lin: ins[a] = lin[a]; continue
    w = objdump(a, min(a + 0x40, END))                   # (after data in the code: decode from here)
    if a not in w: die(f'can\'t decode the instruction at {a - SHIFT:#x}')
    ins[a] = w[a]
# jump tables: the indirect jumps listed in functions.json; the table is the jump's operand, its length
# the run of consecutive fixups into the code from there (as disasm.py finds them)
fix = {}
for l in open(os.path.join(W, 'fixups.txt')):
    s_, t_ = l.split(' -> '); fix[int(s_, 16)] = int(t_, 16)
jtabs = {}
for k in man['jtabs']:
    a = int(k, 16) + SHIFT
    m = re.fullmatch(r'\*(?:%cs:)?0x([0-9a-f]+)\(,%e..,4\)', ins[a][2])
    if not m: die(f'no jump table at {a - SHIFT:#x}')
    tb = int(m.group(1), 16); n = 0
    while tb + 4 * n in fix and BASE <= fix[tb + 4 * n] < END: n += 1
    jtabs[a] = ['jtab', tb, n]
json.dump({'ins': {str(a): v for a, v in ins.items()}, 'funcs': {str(k): v for k, v in funcs.items()},
           'jtabs': {str(k): v for k, v in jtabs.items()}, 'bad': []}, open(os.path.join(W, 'disasm.json'), 'w'))
lab = os.path.join(HERE, 'labels.csv')
if os.path.exists(lab):
    with open(lab) as s, open(os.path.join(W, 'labels.csv'), 'w') as d: d.write(s.read())
print('generating C...')
subprocess.run([sys.executable, os.path.join(HERE, 'gen.py'), W, out, hex(SHIFT)], check=True)
print(f'done: {out}')
