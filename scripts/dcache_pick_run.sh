# dcache_pick_run.sh -- sourced by the dcache sweeps: which of a point's RUNS
# repetitions to report.
#
# pick_run <target ops/s, 0 = unpaced>: stdin has one line per conserved run,
# "<reader rate> <writer rate in M/s> [extra fields...]"; prints the ONE line
# best-of-RUNS keeps, with a pacing verdict appended:
#   - the best reader run among those whose writers sustained >= 95% of the
#     target ("OK"; "-" when unpaced, where every run qualifies);
#   - if none did, the run whose writers came CLOSEST to the target ("SHORT").
#     Picking the best reader among short runs would favour the run whose
#     writers fell furthest behind, i.e. the quietest machine.
# The reader and writer numbers always come from the SAME run: taking each
# column's maximum separately paired numbers no run produced together.
pick_run() {
  awk -v tgt="$1" '
    NF >= 2 { ok = (tgt == 0 || $2 * 1e6 >= 0.95 * tgt)
              if (ok) { if (!hok || $1 > olk) { olk = $1; oline = $0 }; hok = 1 }
              else if (!hs || $2 > srn) { srn = $2; sline = $0; hs = 1 } }
    END { if (hok) print oline, (tgt == 0 ? "-" : "OK")
          else if (hs) print sline, "SHORT"
          else print 0, 0, "-" }'
}
