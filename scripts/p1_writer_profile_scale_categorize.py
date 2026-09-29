#!/usr/bin/env python3
"""Categorize a whole-process perf profile of a multi-writer point.

Usage: p1_writer_profile_scale_categorize.py ENGINE WRITERS PERF_DATA MOPS SECONDS

The scale counterpart of p1_writer_profile_categorize.py: that one reads the
single writer and its worker thread by thread; at 192 writers every thread is
read, each is given a role -- writer, worker (a call_rcu worker, pinned to its
writer's PU), or main -- and the samples are summed by role and category.

Prints CSV rows to stdout:
  engine,writers,role,category,pct_of_process,units_per_update
and each role's top symbols to stderr (for the log).  Units are the sampled
event counts (cycles:pp, IBS op sampling on this machine) divided by the
updates made while perf sampled (MOPS x 1e6 x SECONDS); compare them across
engines and categories.  perf stat's cycles per update, taken on a twin
process, gives the scale.

Categories are p1_writer_profile_categorize.py's, with two changes:
  write_fn     the write function (su_write / rl_write).  The node-lock write
               path is inlined into it: planning, the stripe spinlocks, the
               validation and, for rculist, the list operation.  Lock spinning
               is split out by annotation, not here.
  txn_staging  the engine's out-of-line staging and list calls
               (urcu_txn_sw_record, __find_ryw, list_del_rcu, ...); the
               single-writer categorizer counted the inlined part only, inside
               the write function.
The same two thread-dependent attributions apply: pthread_mutex_* on a
txn_sw_list writer is the descriptor slab's free-list pop lock, jemalloc's
elsewhere; sched_getcpu on a txn_sw_list writer is 40% slab, 60% call_rcu
(one slab allocation and 1.5 call_rcu per update).
"""
import re, subprocess, sys
from collections import defaultdict

eng, nw, data, mops, secs = (sys.argv[1], int(sys.argv[2]), sys.argv[3],
                             float(sys.argv[4]), float(sys.argv[5]))
upd = mops * 1e6 * secs
WSYM = {'txn_sw_list': 'su_write', 'rculist': 'rl_write'}[eng]
SLAB_GETCPU_SHARE = 1.0 / 2.5
WRITER_SYMS = {WSYM, WSYM + '.cold', 'writer_thread'}
WORKER_SYMS = {'call_rcu_thread', 'seg_reclaim_cb', 'urcu_txn_sw_free_rcu'}

def cat(dso, sym, role):
    if sym in WRITER_SYMS:
        return [('write_fn', 1.0)]
    if sym.startswith('urcu_txn_sw_commit_flavor'):
        return [('commit', 1.0)]
    if sym.startswith('urcu_slab_') or sym == 'urcu_txn_sw_free_rcu':
        return [('descriptor_slab', 1.0)]
    if sym.startswith('urcu_txn_sw'):
        return [('txn_staging', 1.0)]
    if 'pthread_mutex' in sym:
        if eng == 'txn_sw_list' and role == 'writer' and 'trylock' not in sym:
            return [('descriptor_slab', 1.0)]
        return [('node_allocation', 1.0)]
    if 'jemalloc' in dso or 'memset' in sym:
        return [('node_allocation', 1.0)]
    if 'kernel' in dso:
        return [('kernel', 1.0)]
    if sym.startswith('sched_getcpu'):
        if eng == 'txn_sw_list' and role == 'writer':
            return [('descriptor_slab', SLAB_GETCPU_SHARE),
                    ('call_rcu_plumbing', 1.0 - SLAB_GETCPU_SHARE)]
        return [('call_rcu_plumbing', 1.0)]
    if (sym.startswith('urcu_qsbr') or 'call_rcu' in sym or 'rcu' in dso
            or sym == '__tls_get_addr' or sym == 'seg_reclaim_cb'):
        return [('call_rcu_plumbing', 1.0)]
    return [('other', 1.0)]

LINE = re.compile(r'^\s*(\d+)\s+(\d+)\s+[0-9a-f]+\s+(.*?)\s+\((.*)\)\s*$')
per_tid = defaultdict(lambda: defaultdict(int))        # tid -> (dso, sym) -> period
p = subprocess.Popen(['perf', 'script', '-i', data, '-F', 'tid,period,ip,sym,dso'],
                     stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
for line in p.stdout:
    m = LINE.match(line)
    if m:
        per_tid[int(m.group(1))][(m.group(4), m.group(3))] += int(m.group(2))
p.wait()

roles = defaultdict(lambda: defaultdict(int))           # role -> (dso, sym) -> period
nroles = defaultdict(int)
for tid, syms in per_tid.items():
    names = {s for _, s in syms}
    w, k = bool(names & (WRITER_SYMS | {'urcu_txn_sw_commit_flavor.constprop.0'})), bool(names & WORKER_SYMS)
    role = 'MIXED' if (w and k) else 'writer' if w else 'worker' if k else 'main'
    nroles[role] += 1
    for key, v in syms.items():
        roles[role][key] += v

total = sum(v for r in roles.values() for v in r.values())
print(f"## {eng} writers={nw}: {mops} Mops/s, {total} units, threads by role "
      + ", ".join(f"{r}={n}" for r, n in sorted(nroles.items())), file=sys.stderr)
for role, syms in sorted(roles.items()):
    c = defaultdict(float)
    for (dso, sym), v in syms.items():
        for k, wgt in cat(dso, sym, role):
            c[k] += v * wgt
    for k, v in sorted(c.items()):
        print(f"{eng},{nw},{role},{k},{100 * v / total:.2f},{v / upd:.1f}")
    rt = sum(syms.values())
    print(f"#   {role}: {100 * rt / total:.1f}% of process; top symbols:", file=sys.stderr)
    for (dso, sym), v in sorted(syms.items(), key=lambda kv: -kv[1])[:14]:
        print(f"#     {100 * v / rt:6.2f}%  {sym}  [{dso.rsplit('/', 1)[-1]}]", file=sys.stderr)
