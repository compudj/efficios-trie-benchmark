#!/usr/bin/env python3
"""Categorize a perf profile of the P1 single-writer run, per thread.

Usage: p1_writer_profile_categorize.py ENGINE PERF_DATA UPDATES_PER_S SECONDS

Prints CSV rows: engine,thread,category,pct_of_process,units_per_update.
Units are the sampled cycle counts (perf's "Event count", cycles:pp, which on
this machine is IBS op sampling) divided by updates; compare them across
engines and categories, not against the clock.  The writer thread is the one
running the engine's write function; the call_rcu worker, on its own core, is
the one running reclaim callbacks.
"""
import re, subprocess, sys
from collections import defaultdict

eng, data, rate, secs = sys.argv[1], sys.argv[2], float(sys.argv[3]), float(sys.argv[4])
WSYM = {'txn_sw_list': 'su_write', 'rculist': 'rl_write'}[eng]

def cat(dso, sym):
    if 'commit_flavor' in sym: return 'commit'
    if sym in ('su_write', 'rl_write', 'writer_thread'): return 'list_op_and_staging'
    # pthread_mutex_* on these threads is the allocator's own arena locks:
    # under gdb, every writer-thread hit after thread registration is
    # pthread_mutex_trylock called from libjemalloc (calloc <- su_write).
    if ('jemalloc' in dso or sym in ('urcu_slab_alloc', 'urcu_txn_sw_free_rcu')
            or 'memset' in sym or 'pthread_mutex' in sym):
        return 'allocation_and_free'
    if 'kernel' in dso: return 'kernel'
    if (sym.startswith('urcu_qsbr') or 'call_rcu' in sym or sym.startswith('sched_getcpu')
            or sym == '__tls_get_addr' or sym == 'seg_reclaim_cb'):
        return 'call_rcu_plumbing'
    return 'other'

def report(extra):
    return subprocess.run(['perf', 'report', '-i', data, '--stdio', '--no-children'] + extra,
                          capture_output=True, text=True).stdout

tids = [m.group(1) for m in re.finditer(r'^\s+[\d.]+%\s+(\d+):', report(['--sort', 'pid']), re.M)][:3]
upd = rate * 1e6 * secs
for t in tids:
    txt = report(['--sort', 'dso,sym', '--percent-limit', '0', '--tid', t])
    ev = int(re.search(r'Event count \(approx\.\): (\d+)', txt).group(1))
    c, syms = defaultdict(float), []
    for m in re.finditer(r'^\s+([\d.]+)%\s+(\S+)\s+\[[.k]\]\s+(.*)$', txt, re.M):
        c[cat(m.group(2), m.group(3).strip())] += float(m.group(1))
        syms.append(m.group(3).strip())
    top = syms[:15]
    role = ('writer' if WSYM in top else
            'worker' if any(s in ('call_rcu_thread', 'urcu_txn_sw_free_rcu', 'seg_reclaim_cb') for s in top)
            else 'main')
    for k, v in sorted(c.items()):
        print(f"{eng},{role},{k},{v:.2f},{ev * v / 100 / upd:.1f}")
