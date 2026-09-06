#!/usr/bin/env python3
"""Field-level diff for two peers' canonical battle-state dumps.

On a desync both peers write sqtext_p{0,1}_f{frame}.txt: the same frame's state
in squiroll's canonical text form. They are ONE long line, so `diff` only ever
says "line 1 differs". This finds the first differing byte, then walks back to
the enclosing `key;` so the output names the field that split rather than an
offset, and prints the surrounding context from both peers.

    sqtext_diff.py sqtext_p0_f1560.txt sqtext_p1_f1560.txt [context_chars]
"""
import sys, re

def enclosing_key(s, pos):
    """Nearest `sN:name;` label at or before pos — the field the divergence is in."""
    best = None
    for m in re.finditer(r's(\d+):([A-Za-z_][A-Za-z0-9_]*);', s[:pos]):
        best = m
    return best.group(2) if best else '<root>'

def path_to(s, pos):
    """Container path: every unclosed `key;{` or `key;a` scope above pos."""
    stack, i = [], 0
    key = '<root>'
    while i < pos:
        m = re.compile(r's(\d+):([A-Za-z_][A-Za-z0-9_]*);').match(s, i)
        if m:
            key = m.group(2); i = m.end(); continue
        if s[i] == '{' or s[i] == '[':
            stack.append(key); key = '?'
        elif s[i] == '}' or s[i] == ']':
            if stack: key = stack.pop()
        i += 1
    return '.'.join(x for x in stack if x != '?')

def main():
    if len(sys.argv) < 3:
        print(__doc__); return 1
    a = open(sys.argv[1], 'r', errors='replace').read()
    b = open(sys.argv[2], 'r', errors='replace').read()
    ctx = int(sys.argv[3]) if len(sys.argv) > 3 else 90
    if a == b:
        print('identical'); return 0
    n = min(len(a), len(b))
    i = next((k for k in range(n) if a[k] != b[k]), n)
    print(f'lengths: {len(a)} vs {len(b)}')
    print(f'first difference at byte {i}')
    print(f'field   : {enclosing_key(a, i)}')
    print(f'path    : {path_to(a, i)}')
    lo = max(0, i - ctx)
    print(f'\np0: ...{a[lo:i]}>>>{a[i:i+ctx]}...')
    print(f'\np1: ...{b[lo:i]}>>>{b[i:i+ctx]}...')
    # How many more differences, coarsely — one desync is usually one root cause.
    diffs = sum(1 for k in range(n) if a[k] != b[k])
    print(f'\ndiffering bytes in the common prefix region: {diffs} of {n}')
    return 0

sys.exit(main())
