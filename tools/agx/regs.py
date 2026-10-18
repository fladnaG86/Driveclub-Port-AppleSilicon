# regs.py <agx bin>...: max GPR index and instruction count (applegpu)
import sys, io, re, contextlib
import os; sys.path.insert(0, os.environ.get('APPLEGPU', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'applegpu')))
import disassemble as D
D.STOP_ON_STOP = False
for f in sys.argv[1:]:
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        D.disassemble(open(f, 'rb').read())
    t = buf.getvalue()
    regs = [int(x) for x in re.findall(r'\br(\d+)[lh]?\b', t)]
    n = sum(1 for l in t.split('\n') if re.match(r'\s*[0-9a-f]+:', l))
    fails = t.count('disassembly failed')
    print(f, 'max_r', max(regs) if regs else -1, 'insts', n, 'fail', fails)
