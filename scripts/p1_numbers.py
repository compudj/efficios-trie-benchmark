#!/usr/bin/env python3
"""Every measured number P1 sec. 7 quotes, recomputed from the bench CSVs.

  p1_numbers.py old [sections]   from bench HEAD (git show): the take the paper
                                 was last committed with -- run this FIRST when
                                 re-taking; it must reproduce the paper's text
  p1_numbers.py new [sections]   from the bench working tree

sections (comma-separated; all if omitted): readcost, stall, readclass,
existence_layout, placement, writecost, profile, writerscale.  The profile
section reads the retirement-sampled csv P1 quotes (p1_writer_profile_ret.csv,
written by scripts/run_p1_writer_profile.sh with EVENT=cycles TAG=_ret), in
cycles per update; P1_PROFILE=p1_writer_profile.csv reads the precise-event one.
The writer-scaling batch is P1_WS (default p1_writer_scaling_h.csv).

Written 2026-10-03 for the re-pin at engine 2793224e: on the 18809ea8 take it
reproduced every figure P1 quoted except the traversal-order "about 15%", which
no take on the guard-free harness supports.  Companion: p1_figdata.py, which
regenerates articles' p1-sw-flip-latch/data/fig-*.csv (its `check` mode
reproduced the seven committed files byte for byte from the 18809ea8 data).
"""
import csv, io, os, statistics, subprocess, sys
from collections import defaultdict
BENCH = '/mnt/data/efficios/git/efficios-trie-benchmark'
mode = sys.argv[1]
only = sys.argv[2].split(',') if len(sys.argv) > 2 else None
# The writer-scaling batch: _h is the 2793224e take (_f was 18809ea8's).
WSNAME = os.environ.get('P1_WS', 'p1_writer_scaling_h.csv')
PROFILE = os.environ.get('P1_PROFILE', 'p1_writer_profile_ret.csv')
med = statistics.median

def rows(name):
    try:
        if mode == 'old':
            txt = subprocess.run(['git', '-C', BENCH, 'show', 'HEAD:scripts/' + name],
                                 capture_output=True, text=True, check=True).stdout
        else:
            txt = open(f'{BENCH}/scripts/{name}').read()
    except Exception as e:
        print(f'   [{name}: unavailable: {type(e).__name__}]'); return []
    return list(csv.DictReader(io.StringIO(txt)))

def rng(v, fmt='{:.3f}'):
    return f"{fmt.format(min(v))}..{fmt.format(max(v))}"

def sec(name):
    if only and name not in only: return False
    print(f'\n===== {name}'); return True

# ---------------------------------------------------------------- read cost
if sec('readcost'):
    for lay, src in (('scattered', 'p1_resolve_control_shuffled.csv'), ('walk', 'p1_resolve_control_noguard.csv')):
        r = rows(src)
        if not r: continue
        v = {(x['engine'], int(x['run']), int(x['x'])): float(x['read_mvisits']) for x in r}
        xs = sorted({k[2] for k in v}); runs = sorted({k[1] for k in v})
        print(f'-- {lay}: viol total {sum(int(x["viol"]) for x in r)}; readers {xs}; runs {runs}')
        ratio = {}
        for e in ('rculist_load', 'rculist_resolve', 'txn_sw_fwd', 'rculist_loadbr'):
            ratio[e] = [med([v[(e, rr, n)] / v[('rculist', rr, n)] for rr in runs]) for n in xs]
            print(f'   {e:16s} ratio to rculist {rng(ratio[e], "{:.4f}")}   below rculist {100*(1-max(ratio[e])):.1f}..{100*(1-min(ratio[e])):.1f}%')
        d = [abs(a / b - 1) * 100 for a, b in zip(ratio['txn_sw_fwd'], ratio['rculist_resolve'])]
        print(f'   txn vs +tag test: differ by up to {max(d):.2f}% (min {min(d):.2f}%)')
        for e in ('rculist', 'rculist_load', 'rculist_resolve', 'txn_sw_fwd', 'rculist_loadbr'):
            m1 = med([v[(e, rr, xs[0])] for rr in runs]); mN = med([v[(e, rr, xs[-1])] for rr in runs])
            print(f'   {e:16s} {m1:8.1f} @1 -> {mN:9.1f} @{xs[-1]}: scaling {100*mN/(xs[-1]*m1):.1f}% of ideal')
    r = rows('p1_resolve_deref_noguard.csv')
    if r:
        print('-- per-node counters (8 s - 2 s), walk order:')
        c = defaultdict(dict)
        for x in r:
            c[(x['engine'], x['run'])][x['seconds']] = x
        per = defaultdict(list)
        for (e, run), d in c.items():
            a, b = d['2'], d['8']
            visits = (float(b['mvisits_per_s']) * 8 - float(a['mvisits_per_s']) * 2) * 1e6
            per[e].append(tuple((float(b[k]) - float(a[k])) / visits for k in ('instructions', 'branches', 'loads')))
        base = per.get('rculist')
        for e, l in per.items():
            m = [med([t[i] for t in l]) for i in range(3)]
            extra = '' if not base else '   extra: ' + ' '.join(f'{m[i]-med([t[i] for t in base]):+.2f}' for i in range(3))
            print(f'   {e:16s} instr {m[0]:.2f} br {m[1]:.2f} ld {m[2]:.2f}{extra}')

if sec('stall'):
    r = rows('p1_stall.csv')
    v = defaultdict(list)
    for x in r: v[(x['layout'], x['engine'])].append(float(x['read_mvisits']))
    for lay in ('seq10k', 'shuf10k', 'shuf1m', 'shuf4m'):
        if (lay, 'rculist') not in v: continue
        b = v[(lay, 'rculist')]; mb = med(b)
        print(f'-- {lay}: rculist {mb:.1f} Mvisits/s = {1000/mb:.2f} ns/hop; run-to-run range/median {100*(max(b)-min(b))/mb:.1f}%')
        for e in ('rculist_load', 'rculist_resolve', 'txn_sw_fwd', 'rculist_loadbr'):
            a = v[(lay, e)]
            pr = [x / y for x, y in zip(a, b)]
            print(f'   {e:16s} median/median {med(a)/mb:.3f}   paired-by-run median {med(pr):.3f} [{min(pr):.3f}..{max(pr):.3f}]')

# ---------------------------------------------------------------- read class
if sec('readclass'):
    r = rows('p1_readclass.csv')
    if r:
        print(f'-- viol total {sum(int(x["viol"]) for x in r)}')
        for lay in ('shuffled', 'walk'):
            v = defaultdict(list)
            for x in r:
                if x['layout'] == lay: v[(x['engine'], int(x['x']))].append(float(x['read_mvisits']))
            xs = sorted({k[1] for k in v})
            m = {k: med(l) for k, l in v.items()}
            print(f'-- {lay}: readers {xs}')
            for e in ('existence', 'rlu', 'mvrlu'):
                lead = [m[('txn_sw_list', n)] / m[(e, n)] for n in xs]
                trail = [100 * (1 - m[(e, n)] / m[('txn_sw_list', n)]) for n in xs]
                print(f'   txn leads {e:9s} {rng(lead, "{:.2f}")}  @192 {lead[-1]:.2f}; trails by {rng(trail, "{:.1f}")}%')
                print('      per reader count trail%: ' + ' '.join(f'{n}:{t:.1f}' for n, t in zip(xs, trail)))
            for e in ('txn_sw_list', 'existence', 'rlu', 'mvrlu'):
                print(f'   {e:11s} {m[(e, xs[0])]:8.1f} @1 -> {m[(e, xs[-1])]:9.1f} @{xs[-1]}: scaling {100*m[(e, xs[-1])]/(xs[-1]*m[(e, xs[0])]):.1f}% of ideal')
    rc = rows('p1_resolve_control_noguard.csv'); rs = rows('p1_resolve_control_shuffled.csv')
    if r and rc:
        for lay, src in (('walk', rc), ('shuffled', rs)):
            f = defaultdict(list)
            for x in src:
                if x['engine'] == 'txn_sw_fwd': f[int(x['x'])].append(float(x['read_mvisits']))
            b = defaultdict(list)
            for x in r:
                if x['layout'] == lay and x['engine'] == 'txn_sw_list': b[int(x['x'])].append(float(x['read_mvisits']))
            o = [med(b[n]) / med(f[n]) for n in sorted(f) if n in b]
            print(f'-- traversal order ({lay}): txn fwd+back / fwd+fwd = {rng(o, "{:.3f}")}')
    r = rows('p1_readclass_deref.csv')
    if r:
        print('-- per-node counters, readclass arms:')
        c = defaultdict(dict)
        for x in r: c[(x['engine'], x['run'])][x['seconds']] = x
        per = defaultdict(list)
        for (e, run), d in c.items():
            a, b = d['2'], d['8']
            visits = (float(b['mvisits_per_s']) * 8 - float(a['mvisits_per_s']) * 2) * 1e6
            per[e].append(tuple((float(b[k]) - float(a[k])) / visits for k in ('instructions', 'branches', 'loads')))
        base = per.get('rculist')
        for e, l in per.items():
            m = [med([t[i] for t in l]) for i in range(3)]
            print(f'   {e:12s} instr {m[0]:.2f} br {m[1]:.2f} ld {m[2]:.2f}   extra: ' + ' '.join(f'{m[i]-med([t[i] for t in base]):+.2f}' for i in range(3)))
    r = rows('p1_rlu_cmp.csv')
    if r:
        print('-- RLU compare A/B:')
        c = defaultdict(dict)
        for x in r: c[(x['build'], x['engine'], x['run'])][x['seconds']] = x
        per = defaultdict(list)
        for (bld, e, run), d in c.items():
            a, b = d['2'], d['8']
            visits = (float(b['mvisits_per_s']) * 8 - float(a['mvisits_per_s']) * 2) * 1e6
            per[(bld, e)].append(tuple((float(b[k]) - float(a[k])) / visits for k in ('instructions', 'branches', 'loads')))
        mm = {k: [med([t[i] for t in l]) for i in range(3)] for k, l in per.items()}
        for k, m in mm.items(): print(f'   {k[0]:8s} {k[1]:9s} instr {m[0]:.2f} br {m[1]:.2f} ld {m[2]:.2f}')
        if ('samecmp', 'rlu_list') in mm and ('rawcmp', 'rlu_list') in mm:
            d = [mm[('samecmp', 'rlu_list')][i] - mm[('rawcmp', 'rlu_list')][i] for i in range(3)]
            print(f'   compare costs: instr {d[0]:.2f} br {d[1]:.2f} ld {d[2]:.2f}')
            d = [mm[('rawcmp', 'rlu_list')][i] - mm[('rawcmp', 'rculist')][i] for i in range(3)]
            print(f'   RLU deref alone (rawcmp - rculist): instr {d[0]:+.2f} br {d[1]:+.2f} ld {d[2]:+.2f}')

if sec('existence_layout'):
    if mode == 'old':
        try: txt = subprocess.run(['git', '-C', BENCH, 'show', 'HEAD:scripts/existence_layout.csv'], capture_output=True, text=True, check=True).stdout
        except Exception: txt = ''
    else:
        try: txt = open(f'{BENCH}/scripts/existence_layout.csv').read()
        except Exception: txt = ''
    r = list(csv.DictReader(io.StringIO(txt)))
    v = defaultdict(list)
    for x in r: v[(x['layout'], x['engine'], int(x['readers']))].append(float(x['read_mvisits']))
    xs = sorted({k[2] for k in v})
    mean = lambda l: sum(l) / len(l)
    if r:
        for n in (1, 32, 192):
            print(f'   {n:3d} rdr: ' + '  '.join(f'{lay} {mean(v[(lay, "existence_list", n)]):.0f}' for lay in ('perfbook', 'packed', 'split')) + f'  txn {mean(v[("split", "txn_sw_list", n)]):.0f}')
        for a, b in (('packed', 'perfbook'), ('split', 'perfbook')):
            q = [mean(v[(a, 'existence_list', n)]) / mean(v[(b, 'existence_list', n)]) for n in xs]
            print(f'   {a}/{b}: {rng(q, "{:.2f}")}')
        for lay in ('perfbook', 'packed', 'split'):
            q = [mean(v[(lay, 'txn_sw_list', n)]) / mean(v[(lay, 'existence_list', n)]) for n in xs]
            print(f'   txn/{lay}: {rng(q, "{:.2f}")}')

if sec('placement'):
    r = rows('p1_readside_placement.csv')
    v = defaultdict(list)
    for x in r: v[(x['cond'], x['layout'], x['arm'], int(x['x']))].append(float(x['read_mvisits']))
    mean = lambda l: sum(l) / len(l)
    worst = []
    for (c, lay, arm, n), l in v.items():
        if c != 'socket0': continue
        a = v.get(('all', lay, arm, n))
        if a: worst.append((abs(mean(l) / mean(a) - 1) * 100, mean(l) / mean(a), lay, arm, n))
    worst.sort(reverse=True)
    for w in worst[:6]: print(f'   socket0/all {w[1]:.3f} ({w[0]:.1f}%)  {w[2]} {w[3]} x={w[4]}')
    if r: print(f'   viol total {sum(int(x["viol"]) for x in r)}; points {len(worst)}')

# ---------------------------------------------------------------- write cost
def writecost(name, label):
    r = rows(name)
    if not r: return
    print(f'-- {label}: viol total {sum(int(x["viol"]) for x in r)}')
    for lay in ('scattered', 'walk'):
        v = defaultdict(list)
        for x in r:
            if x['layout'] == lay: v[(x['mode'], x['engine'], int(x['x']))].append(float(x['write_mops']))
        xs = sorted({k[2] for k in v if k[0] == 'mixed'})
        m = {k: med(l) for k, l in v.items()}
        q = [m[('mixed', 'rculist_mutex', n)] / m[('mixed', 'txn_mutex', n)] for n in xs]
        print(f'   {lay}: ratio rcu/txn {rng(q, "{:.2f}")}')
        print('      ' + ' '.join(f'{n}:{a:.2f}' for n, a in zip(xs, q)))
        print('      rcu  ' + ' '.join(f'{n}:{m[("mixed","rculist_mutex",n)]:.2f}' for n in xs))
        print('      txn  ' + ' '.join(f'{n}:{m[("mixed","txn_mutex",n)]:.2f}' for n in xs))
        a, b = m[('noread', 'rculist_mutex', 0)], m[('noread', 'txn_mutex', 0)]
        pr = [x / y for x, y in zip(v[('noread', 'rculist_mutex', 0)], v[('noread', 'txn_mutex', 0)])]
        print(f'      no reader: rcu {a:.2f} txn {b:.2f} ratio {a/b:.2f} (per-run {rng(pr, "{:.2f}")})')
    return r

if sec('writecost'):
    writecost('p1_writecost.csv', 'pin build')
    writecost('p1_writecost_nobatch.csv', 'NO_BATCH control')
    writecost('p1_writecost_rseq.csv', 'rseq slab')

def profile(name, label):
    r = rows(name)
    if not r: return
    print(f'-- {label}')
    u = defaultdict(dict)
    for x in r: u[(x['engine'], x['thread'])][x['category']] = (float(x['units_per_update']), float(x['pct_of_process']))
    tw, rw = u[('txn_sw_list', 'writer')], u[('rculist', 'writer')]
    tot_t, tot_r = sum(a for a, _ in tw.values()), sum(a for a, _ in rw.values())
    gap = tot_t - tot_r
    print(f'   writer thread units/update: txn {tot_t:.1f} rculist {tot_r:.1f} ratio {tot_t/tot_r:.2f} gap {gap:.1f}')
    for c in sorted(set(tw) | set(rw)):
        a, b = tw.get(c, (0, 0))[0], rw.get(c, (0, 0))[0]
        print(f'   {c:20s} txn {a:6.1f}  rculist {b:6.1f}  diff {a-b:+7.1f}  share of gap {100*(a-b)/gap:5.1f}%')
    for e in ('txn_sw_list', 'rculist'):
        p = {t: sum(pp for _, pp in u[(e, t)].values()) for t in ('writer', 'worker', 'main') if (e, t) in u}
        print(f'   {e}: % of process by thread ' + ' '.join(f'{t} {q:.1f}' for t, q in p.items()) + (f'   worker/writer {p["worker"]/p["writer"]:.2f}' if 'worker' in p else ''))

if sec('profile'):
    profile(PROFILE, 'pin build')
    profile(PROFILE.replace('.csv', '_nobatch.csv'), 'NO_BATCH control')

if sec('writerscale'):
    r = rows(WSNAME)
    v = defaultdict(list)
    for x in r:
        if x['write_mops']: v[(x['arm'], int(x['writers']))].append(float(x['write_mops']))
    xs = sorted({k[1] for k in v})
    m = {k: med(l) for k, l in v.items()}
    if r:
        print(f'-- {WSNAME}: violations {sum(int(x["violations"] or 0) for x in r)} retries {sum(int(x["retries"] or 0) for x in r)} missing {sum(1 for x in r if not x["write_mops"])}')
        for arm in ('txn_sw_bitlock', 'txn_sw_nodelock', 'rculist_nodelock', 'txn_sw_mutex', 'txn_list'):
            print(f'   {arm:17s} ' + ' '.join(f'{n}:{m[(arm, n)]:.1f}' for n in xs))
        b = 'txn_sw_bitlock'
        print(f'   bit: {m[(b,1)]:.2f} -> {m[(b,192)]:.1f} = {m[(b,192)]/m[(b,1)]:.1f}x; vs one lock at 192: {m[(b,192)]/m[("txn_sw_mutex",192)]:.0f}x (one lock {m[("txn_sw_mutex",1)]:.2f} -> {m[("txn_sw_mutex",192)]:.2f})')
        print('   rcu/stripe: ' + ' '.join(f'{n}:{m[("rculist_nodelock", n)]/m[("txn_sw_nodelock", n)]:.2f}' for n in xs))
        print('   rcu/bit:    ' + ' '.join(f'{n}:{m[("rculist_nodelock", n)]/m[(b, n)]:.2f}' for n in xs))
        print('   spread (max-min)/max per arm: ')
        for arm in ('txn_sw_bitlock', 'txn_sw_nodelock', 'rculist_nodelock'):
            print(f'      {arm:17s} ' + ' '.join(f'{n}:{100*(max(v[(arm,n)])-min(v[(arm,n)]))/max(v[(arm,n)]):.0f}%' for n in xs))
        print('   192 over 191: ' + ' '.join(f'{arm} {100*(m[(arm,192)]/m[(arm,191)]-1):+.0f}%' for arm in ('txn_sw_bitlock', 'txn_sw_nodelock', 'rculist_nodelock')))
