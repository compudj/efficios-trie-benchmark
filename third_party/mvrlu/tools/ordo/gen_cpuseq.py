#!/usr/bin/env python3
"""Python-3 port of mv-rlu tools/ordo/gen_cpuseq.py (Apache-2.0, Virginia Tech).

Same algorithm, verbatim semantics:
  - parse /proc/cpuinfo into per-cpu dicts
  - keep only PRIMARY hyperthreads (processor == min(thread_siblings_list))
  - order: every cpu of cpu0's package first, then the remaining packages
Emits cpuseq.h: `int online_cpus` + `int cpuseq[]`.
"""
import errno
import sys

with open("/proc/cpuinfo") as f:
    blocks = f.read().split("\n\n")

cpuinfo = [dict((k.strip(), v.strip())
                for k, v in (line.split(":", 1) for line in block.splitlines()
                             if ":" in line))
           for block in blocks if block.strip()]

primaries = set()
for cpu in cpuinfo:
    processor = cpu["processor"]
    try:
        with open("/sys/devices/system/cpu/cpu%s/topology/thread_siblings_list"
                  % processor) as f:
            s = f.read()
    except EnvironmentError as e:
        if e.errno == errno.ENOENT:
            primaries.add(processor)
            continue
        raise
    try:
        ss = set(map(int, s.split("-")))
    except ValueError:
        ss = set(map(int, s.split(",")))
    if int(processor) == min(ss):
        primaries.add(processor)
cpuinfo = [cpu for cpu in cpuinfo if cpu["processor"] in primaries]


def seq(info):
    packages = {}
    package_ids = set()
    cpu0_package_id = None
    for cpu in info:
        if "physical id" in cpu:
            package_id = int(cpu["physical id"])
            packages.setdefault(package_id, []).append(cpu)
            if cpu["processor"] == "0":
                cpu0_package_id = package_id
            package_ids.add(package_id)
        else:
            yield cpu
    if cpu0_package_id is None:
        return
    for cpu in packages[cpu0_package_id]:
        yield cpu
    package_ids.discard(cpu0_package_id)
    for package_id in sorted(package_ids):
        for cpu in packages[package_id]:
            yield cpu


def order():
    for cpu in seq(cpuinfo):
        yield cpu["processor"]


if __name__ == "__main__":
    cpus = list(order())
    out = sys.argv[1] if len(sys.argv) > 1 else "cpuseq.h"
    with open(out, "w") as f:
        f.write("int online_cpus = %d;\n" % len(cpus))
        f.write("int cpuseq[] = { %s };\n" % ", ".join(cpus))
    print("%d primary cpus -> %s" % (len(cpus), out), file=sys.stderr)
