#!/usr/bin/env python3
"""Checks on the MSL that gcn_dis generates for the shaders in tools/msl_fixtures.

usage: tools/msl_check.py [check ...]   (all checks when none is named)
Run from Driveclub-Port after `cmake --build build --target gcn_dis`.
"""
import os
import re
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GCN_DIS = os.path.join(ROOT, 'build', 'gcn_dis')
FIXTURES = os.path.join(ROOT, 'tools', 'msl_fixtures')


def fixture_bin(name):
    words = [int(w, 16) for w in open(os.path.join(FIXTURES, name + '.hex')).read().split()]
    f = tempfile.NamedTemporaryFile(suffix='.bin', delete=False)
    f.write(struct.pack('<%dI' % len(words), *words))
    f.close()
    return f.name


def gen(name, stage='ps', **opts):
    """Returns (msl text, compiled ok, unsupported list) for a fixture."""
    path = fixture_bin(name)
    try:
        args = [GCN_DIS, path, stage, '--msl'] + ['%s=%s' % kv for kv in opts.items()]
        out = subprocess.run(args, capture_output=True, text=True).stdout
    finally:
        os.unlink(path)
    start = out.find('#include <metal_stdlib>')
    end = out.rfind('MSL compiled OK')
    msl = out[start:end if end > 0 else len(out)]
    unsupported = re.findall(r'^unsupported: (.*)$', out, re.M)
    return msl, 'MSL compiled OK' in out, unsupported


def body(msl):
    """The entry point's code (after the prelude helpers)."""
    i = msl.find(' main0(')
    return msl[i:] if i >= 0 else msl


def fixtures():
    return sorted(f[:-4] for f in os.listdir(FIXTURES) if f.endswith('.hex'))


def check_compiles():
    for name in fixtures():
        msl, ok, unsupported = gen(name)
        assert ok, '%s: MSL does not compile' % name
        assert not unsupported, '%s: unsupported %s' % (name, unsupported)
        assert msl.count('{') == msl.count('}'), '%s: unbalanced braces' % name


CHECKS = {
    'compiles': check_compiles,
}


def main(names):
    failed = 0
    for name in names or CHECKS:
        try:
            CHECKS[name]()
            print('PASS', name)
        except AssertionError as e:
            failed += 1
            print('FAIL', name, '-', e)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
