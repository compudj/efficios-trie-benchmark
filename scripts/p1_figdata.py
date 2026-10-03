#!/usr/bin/env python3
"""Regenerate p1-sw-flip-latch/data/fig-*.csv from the benchmark tree's CSVs.

  p1_figdata.py check   reproduce the committed fig-*.csv from the bench CSVs
                        at bench HEAD (git show), and diff
  p1_figdata.py write [WRITER_SCALING_CSV [FIGURES]]
                        write fig-*.csv from the bench working tree; the
                        writer-scaling batch defaults to p1_writer_scaling_h.csv
                        (the 2793224e take; _f was 18809ea8's),
                        FIGURES is a comma list of writecost, readcost,
                        readclass, writerscale

Medians over runs; fig-readcost is the median over runs of the ratio to rculist
in the same run; fig-writerscale carries max-median and median-min for its bars.
See p1_numbers.py.
"""
import csv, io, statistics, subprocess, sys
from collections import defaultdict
BENCH = '/mnt/data/efficios/git/efficios-trie-benchmark'
DATA = '/mnt/data/efficios/git/articles/p1-sw-flip-latch/data'
mode = sys.argv[1]
WS = sys.argv[2] if len(sys.argv) > 2 else None   # writer-scaling csv name override

def rows(name):
    if mode == 'check':
        txt = subprocess.run(['git', '-C', BENCH, 'show', 'HEAD:scripts/' + name],
                             capture_output=True, text=True, check=True).stdout
    else:
        txt = open(f'{BENCH}/scripts/{name}').read()
    return list(csv.DictReader(io.StringIO(txt)))

med = statistics.median
def emit(name, header, lines):
    txt = ','.join(header) + '\n' + ''.join(','.join(l) + '\n' for l in lines)
    path = f'{DATA}/{name}'
    if mode == 'check':
        old = open(path).read()
        print(f'{name}: {"IDENTICAL" if old == txt else "DIFFERS"}')
        if old != txt:
            for a, b in zip(old.splitlines(), txt.splitlines()):
                if a != b: print('   committed:', a, '\n   generated:', b)
    else:
        open(path, 'w').write(txt); print('wrote', path)

def writecost():
    r = rows('p1_writecost.csv')
    for lay, out in (('walk', 'fig-writecost.csv'), ('scattered', 'fig-writecost-scattered.csv')):
        v = defaultdict(list)
        for x in r:
            if x['layout'] == lay and x['mode'] == 'mixed':
                v[(int(x['x']), x['engine'])].append(float(x['write_mops']))
        xs = sorted({k[0] for k in v})
        emit(out, ['readers', 'rcu', 'txn'],
             [[str(n), f"{med(v[(n,'rculist_mutex')]):.2f}", f"{med(v[(n,'txn_mutex')]):.2f}"] for n in xs])

def readcost():
    for src, out in (('p1_resolve_control_noguard.csv', 'fig-readcost.csv'),
                     ('p1_resolve_control_shuffled.csv', 'fig-readcost-shuffled.csv')):
        v = {}
        for x in rows(src):
            v[(x['engine'], int(x['run']), int(x['x']))] = float(x['read_mvisits'])
        xs = sorted({k[2] for k in v}); runs = sorted({k[1] for k in v})
        arms = [('load', 'rculist_load'), ('resolve', 'rculist_resolve'), ('txn', 'txn_sw_fwd'), ('loadbr', 'rculist_loadbr')]
        emit(out, ['readers'] + [a for a, _ in arms],
             [[str(n)] + [f"{med([v[(e, r, n)] / v[('rculist', r, n)] for r in runs]):.4f}" for _, e in arms] for n in xs])

def readclass():
    r = rows('p1_readclass.csv')
    for lay, out in (('walk', 'fig-readclass.csv'), ('shuffled', 'fig-readclass-shuffled.csv')):
        v = defaultdict(list)
        for x in r:
            if x['layout'] == lay:
                v[(int(x['x']), x['engine'])].append(float(x['read_mvisits']))
        xs = sorted({k[0] for k in v})
        arms = [('txn', 'txn_sw_list'), ('existence', 'existence'), ('rlu', 'rlu'), ('mvrlu', 'mvrlu')]
        emit(out, ['readers'] + [a for a, _ in arms],
             [[str(n)] + [f"{med(v[(n, e)]):.1f}" for _, e in arms] for n in xs])

def writerscale():
    v = defaultdict(list)
    for x in rows(WS or 'p1_writer_scaling_h.csv'):
        if x['write_mops']:
            v[(int(x['writers']), x['arm'])].append(float(x['write_mops']))
    xs = sorted({k[0] for k in v})
    arms = [('bit', 'txn_sw_bitlock'), ('stripe', 'txn_sw_nodelock'), ('rcu', 'rculist_nodelock'), ('mutex', 'txn_sw_mutex')]
    hdr = ['writers'] + [f'{a}{s}' for a, _ in arms for s in ('', '_ep', '_em')]
    lines = []
    for n in xs:
        l = [str(n)]
        for _, e in arms:
            m = med(v[(n, e)])
            l += [f"{m:.2f}", f"{max(v[(n, e)]) - m:.2f}", f"{m - min(v[(n, e)]):.2f}"]
        lines.append(l)
    emit('fig-writerscale.csv', hdr, lines)

which = sys.argv[3].split(',') if len(sys.argv) > 3 else ['writecost', 'readcost', 'readclass', 'writerscale']
for w in which: globals()[w]()
