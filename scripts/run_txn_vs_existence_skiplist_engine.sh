#!/bin/bash
# Focused skiplist TXN-vs-existence sweep with the CURRENT engine.
# Three engines: existence (flip), txn-default (funnel-fix + bloom + full help/steal),
# txn-spin (+ NO_HELP + NO_STEAL). Width matched at 3 key-moves. jemalloc.
set -u
REPO=/mnt/data/efficios/git/efficios-trie-benchmark
JE=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2
EX=$REPO/perfbook/datastruct/existence/existence_3skiplist_uperf
TD=/tmp/sl_txn_default
TS=/tmp/sl_txn_spin
CSV=$REPO/../../../tmp/claude-1000/-mnt-data-efficios-git-efficios-trie-benchmark/04356a38-6e28-4dfa-a3bc-6ae835aa8222/scratchpad/engine_cmp.csv
CSV=/tmp/claude-1000/-mnt-data-efficios-git-efficios-trie-benchmark/04356a38-6e28-4dfa-a3bc-6ae835aa8222/scratchpad/engine_cmp.csv
DUR=1000
RUNS=3
sp() { echo $((16 + 3 * $1)); }
field() { awk -v L="$2" '{for(i=1;i<=NF;i++) if(tolower($i)==tolower(L)){print $(i+1);exit}}' <<< "$1"; }

# run engine cores b -> best-of-RUNS ns/key-move + Mmoves/s ; appends to CSV
run() {
  local panel=$1 eng=$2 cores=$3 b=$4
  local bin extra kps=$((b*cores))
  case $eng in
    existence) bin=$EX; extra="--groupobjs 3";;
    txn-default) bin=$TD; extra="--movesper 3 --ryw 1";;
    txn-spin) bin=$TS; extra="--movesper 3 --ryw 1";;
  esac
  local r bestmm=0 bestns=0
  for r in $(seq 1 $RUNS); do
    out=$(LD_PRELOAD=$JE timeout 600 "$bin" --nupdaters "$cores" --nreaders 0 --cpustride 1 \
          --updatespacing "$(sp "$b")" --duration "$DUR" $extra 2>/dev/null)
    [[ "$out" == *"CONSERVATION FAILED"* ]] && echo "!! $panel/$eng c=$cores b=$b CONSERVATION FAILED" >&2
    mm=$(field "$out" "Mmoves/s:"); ns=$(field "$out" "ns/key-move:")
    awk -v v="${mm:-0}" -v best="$bestmm" 'BEGIN{exit !(v>best)}' && { bestmm=$mm; bestns=$ns; }
  done
  echo "$panel,$eng,$cores,$b,$kps,${bestmm:-0},${bestns:-0}" >> "$CSV"
  printf "  %-6s %-12s cores=%-3s n=%-6s  %8s Mmoves/s  %9s ns/km\n" "$panel" "$eng" "$cores" "$kps" "$bestmm" "$bestns" >&2
}

echo "panel,engine,cores,b,keys_per_sl,mmoves_s,ns_per_keymove" > "$CSV"

echo ">> FIXED panel: n=3840 keys/skiplist at every core count, width 3" >&2
for c in 1 2 4 8 16 32 64 96 128 192; do
  b=$((3840 / c))
  for e in existence txn-default txn-spin; do run fixed "$e" "$c" "$b"; done
done

echo ">> SIZE panel: 192 cores, sweep keys/skiplist, width 3" >&2
for b in 5 10 20 40 80; do
  for e in existence txn-default txn-spin; do run size "$e" 192 "$b"; done
done

echo ">> DONE: $(($(wc -l < "$CSV") - 1)) rows -> $CSV" >&2
