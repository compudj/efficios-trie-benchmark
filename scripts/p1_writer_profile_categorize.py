#!/usr/bin/env python3
"""Categorize a perf profile of the P1 single-writer run, per thread.

Usage: p1_writer_profile_categorize.py ENGINE PERF_DATA UPDATES_PER_S SECONDS

Prints CSV rows: engine,thread,category,pct_of_process,units_per_update.
Units are the sampled cycle counts (perf's "Event count", cycles:pp, which on
this machine is IBS op sampling) divided by updates; compare them across
engines and categories, not against the clock.  The writer thread is the one
running the engine's write function; the call_rcu worker, on its own core, is
the one running reclaim callbacks.

A reader thread, when the run has one, is reported with role `reader` and is
not part of any share: it runs on its own core.

Categories:
  commit               urcu_txn_sw_commit_flavor
  list_op_and_staging  the write function and its loop, and the engine's
                       staging and list calls (inlined into it, or out of
                       line as urcu_txn_sw_record_chain / _list_*)
  descriptor_slab      the engine's per-CPU descriptor slab (txn_sw_list only):
                       urcu_slab_alloc, its free-list pop lock, the reclaim
                       callback that returns the block (urcu_txn_sw_free_rcu),
                       and its share of sched_getcpu -- see below
  node_allocation      jemalloc: list nodes and the bench's per-node reclaim
                       wrapper, in both lists, with jemalloc's own locks
  call_rcu_plumbing    liburcu call_rcu: the per-CPU queue lookups, enqueue,
                       worker wake-up and worker loop, and their sched_getcpu
  kernel, other

Two attributions need the thread, which is why this runs per thread:

  pthread_mutex_lock/unlock on the txn_sw_list WRITER thread are the slab's
  free-list pop lock (cds_wfs_pop_lock, inlined into urcu_slab_alloc in the
  bench binary: its PLT stubs are the bench binary's, and objdump shows
  urcu_slab_alloc calling pthread_mutex_lock/unlock@plt).  Everywhere else --
  the rculist writer, and both workers, whose slab free is a lock-free push --
  pthread_mutex_* is jemalloc's.

  sched_getcpu is called once per slab allocation and once per call_rcu, and
  its self time cannot be split by caller, so on the txn_sw_list writer it is
  apportioned by call count: one slab allocation and P1_CALL_RCU_PER_UPDATE
  call_rcu per update.  That is 0.5 (the default here) on an engine that
  retires descriptors in batches, c21f5a38's default: only a delete's node is
  deferred per update -- the batch route finds its arena from the block, with
  no cpu lookup, and enters call_rcu once per batch -- so 1/1.5 = 67% goes to
  descriptor_slab.  It is 1.5 with one call_rcu per descriptor (18809ea8, or
  -DURCU_TXN_SLAB_NO_BATCH: an insert defers the descriptor; a delete defers
  the descriptor and the node), so 1/2.5 = 40%; on 18809ea8 the PLT-stub
  samples (bench:liburcu ~ 1:2) were consistent with that.
"""
import os, re, subprocess, sys
from collections import defaultdict

eng, data, rate, secs = sys.argv[1], sys.argv[2], float(sys.argv[3]), float(sys.argv[4])
WSYM = {'txn_sw_list': 'su_write', 'rculist': 'rl_write'}[eng]
# txn_sw_list writer only, see above.  P1_SLAB_GETCPU_CALLS=0 for an engine
# configured --enable-slab-rseq, whose slab reads the cpu from the rseq area.
_SLAB_CALLS = float(os.environ.get('P1_SLAB_GETCPU_CALLS', '1'))
SLAB_GETCPU_SHARE = _SLAB_CALLS / (_SLAB_CALLS + float(os.environ.get('P1_CALL_RCU_PER_UPDATE', '0.5')))

def cat(dso, sym, role):
    """Return a list of (category, weight) for one symbol."""
    if 'commit_flavor' in sym:
        return [('commit', 1.0)]
    if sym in ('su_write', 'rl_write', 'writer_thread'):
        return [('list_op_and_staging', 1.0)]
    # The engine's out-of-line staging and list calls (on 18809ea8 the edges
    # are recorded in urcu_txn_sw_record_chain, no longer inlined).
    if (sym.startswith('urcu_txn_sw_record') or sym.startswith('urcu_txn_sw__find')
            or sym.startswith('urcu_txn_sw__grow') or sym.startswith('urcu_txn_sw__chain')
            or sym.startswith('urcu_txn_sw_list_')):
        return [('list_op_and_staging', 1.0)]
    if sym.startswith('urcu_slab_') or sym == 'urcu_txn_sw_free_rcu':
        return [('descriptor_slab', 1.0)]
    if 'pthread_mutex' in sym:
        if (eng == 'txn_sw_list' and role == 'writer'
                and 'trylock' not in sym):
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
    if (sym.startswith('urcu_qsbr') or 'call_rcu' in sym
            or sym == '__tls_get_addr' or sym == 'seg_reclaim_cb'):
        return [('call_rcu_plumbing', 1.0)]
    return [('other', 1.0)]

def report(extra):
    return subprocess.run(['perf', 'report', '-i', data, '--stdio', '--no-children'] + extra,
                          capture_output=True, text=True).stdout

tids = [m.group(1) for m in re.finditer(r'^\s+[\d.]+%\s+(\d+):', report(['--sort', 'pid']), re.M)][:4]
upd = rate * 1e6 * secs
for t in tids:
    txt = report(['--sort', 'dso,sym', '--percent-limit', '0', '--tid', t])
    ev = int(re.search(r'Event count \(approx\.\): (\d+)', txt).group(1))
    rows = [(float(m.group(1)), m.group(2), m.group(3).strip())
            for m in re.finditer(r'^\s+([\d.]+)%\s+(\S+)\s+\[[.k]\]\s+(.*)$', txt, re.M)]
    top = [s for _, _, s in rows[:15]]
    role = ('writer' if WSYM in top else
            'reader' if any(s in ('su_read', 'rl_read', 'reader_thread') for s in top) else
            'worker' if any(s in ('call_rcu_thread', 'urcu_txn_sw_free_rcu', 'seg_reclaim_cb') for s in top)
            else 'main')
    c = defaultdict(float)
    for pct, dso, sym in rows:
        for k, w in cat(dso, sym, role):
            c[k] += pct * w
    for k, v in sorted(c.items()):
        print(f"{eng},{role},{k},{v:.2f},{ev * v / 100 / upd:.1f}")
