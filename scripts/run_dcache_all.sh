#!/bin/bash
# run_dcache_all.sh -- re-run EVERY dcache sweep on the default liburcu route,
# re-plot every figure that route feeds, then check that no dcache figure
# predates the code it measures.
#
# The sweeps run strictly one after another: each one pins threads across the
# whole machine, so two at once would measure each other.  A sweep that exits
# non-zero is recorded and the driver carries on; its figure is then NOT
# re-plotted, and check_dcache_figures.sh reports it as stale.
#
# The slab-route figure (dcache_slabroute.png) compares four liburcu builds
# (default, rseq, batch, batch+rseq) built from ONE commit with the same CFLAGS;
# after the default route, the churn sweep re-runs against each route build
# (see ROUTES below), and check_dcache_figures.sh verifies the four CSVs share a
# liburcu commit and the current source hash.
#
# Usage: scripts/run_dcache_all.sh            (logs in $LOGDIR, default /tmp)
#        ONLY="run_dcache_optype.sh ..." scripts/run_dcache_all.sh   (a subset)
set -u
REPO=$(git -C "$(dirname "$0")" rev-parse --show-toplevel) || exit 2
S=$REPO/scripts
LOGDIR=${LOGDIR:-/tmp/dcache_resweep.$(date +%Y%m%d-%H%M%S)}
mkdir -p "$LOGDIR"

# sweep script -> the plot scripts its CSV feeds
declare -a SWEEPS=(
	"run_dcache.sh:plot_dcache.py plot_dcache_sat.py plot_dcache_readdir.py plot_dcache_dpath.py plot_dcache_hit.py plot_dcache_idle.py"
	"run_dcache_height.sh:plot_dcache_height.py"
	"run_dcache_churn.sh:plot_dcache_churn.py"
	"run_dcache_churn_scaling.sh:plot_dcache_churn_scaling.py"
	"run_dcache_namewidth.sh:plot_dcache_namewidth.py"
	"run_dcache_optaxonomy.sh:plot_dcache_optaxonomy.py"
	"run_dcache_optype.sh:plot_dcache_optype.py"
	"run_dcache_readdir_churn.sh:plot_dcache_readdir_churn.py"
)

echo ">> logs: $LOGDIR" >&2
echo ">> building the in-tree bench binaries" >&2
make -C "$REPO/experiments/dcache" -j32 bench height churn \
	> "$LOGDIR/build.log" 2>&1 ||
	{ echo "!! build failed, see $LOGDIR/build.log" >&2; exit 1; }

failed=""
for entry in "${SWEEPS[@]}"; do
	sweep=${entry%%:*}
	plots=${entry#*:}
	[[ -n "${ONLY:-}" && " $ONLY " != *" $sweep "* ]] && continue
	log=$LOGDIR/${sweep%.sh}.log
	start=$(date +%s)
	echo ">> $(date +%T) $sweep" >&2
	if bash "$S/$sweep" > "$log" 2>&1; then
		for p in $plots; do
			python3 "$S/$p" >> "$LOGDIR/plots.log" 2>&1 ||
				{ echo "!! $p failed" >&2; failed="$failed $p"; }
		done
	else
		echo "!! $sweep exited non-zero, see $log" >&2
		failed="$failed $sweep"
	fi
	echo ">> $(date +%T) $sweep done in $(( $(date +%s) - start ))s," \
	     "conservation failures: $(grep -c 'CONSERVATION FAILED' "$log")" >&2
done

# ---- slab routes: the churn sweep against each route's liburcu -------------
# dcache_slabroute.png compares the descriptor slab's four routes; each needs
# the in-tree churn binaries rebuilt against that route's library (-B: the
# Makefile's library dependency is by timestamp and would not notice a
# different build), then the default binaries restored.  ROUTES="" skips it.
# Only churn_w, allocating toggles, runs there: the one panel and mode the
# slab-route figure reads (in-place toggles commit no descriptor on the lock
# engines).
ROUTES=${ROUTES-"rseq:urcu-txn-build-rseq-a69be31e batch:urcu-txn-build-batch-a69be31e batch_rseq:urcu-txn-build-batch-rseq-a69be31e"}
if [[ -n "$ROUTES" && -z "${ONLY:-}" ]]; then
	for r in $ROUTES; do
		name=${r%%:*}
		lib=$REPO/${r#*:}
		log=$LOGDIR/run_dcache_churn_$name.log
		echo ">> $(date +%T) run_dcache_churn.sh on route $name ($lib)" >&2
		if make -B -C "$REPO/experiments/dcache" -j32 URCU_TXN_BUILD="$lib" churn \
			> "$LOGDIR/build_$name.log" 2>&1 &&
		   URCU_TXN_BUILD="$lib" CSV="$S/dcache_churn_$name.csv" PANELS=churn_w MODES=alloc \
			bash "$S/run_dcache_churn.sh" > "$log" 2>&1; then
			:
		else
			echo "!! route $name failed, see $log" >&2
			failed="$failed route:$name"
		fi
		echo ">> $(date +%T) route $name done, conservation failures:" \
		     "$(grep -c 'CONSERVATION FAILED' "$log" 2>/dev/null)" >&2
	done
	make -B -C "$REPO/experiments/dcache" -j32 churn > "$LOGDIR/build_restore.log" 2>&1 ||
		{ echo "!! restoring default churn binaries failed" >&2; failed="$failed restore"; }
	python3 "$S/plot_dcache_slabroute.py" >> "$LOGDIR/plots.log" 2>&1 ||
		{ echo "!! plot_dcache_slabroute.py failed" >&2; failed="$failed slabroute"; }
fi

echo ">> figure freshness:" >&2
"$S/check_dcache_figures.sh" 2>&1 | tee "$LOGDIR/freshness.log" >&2
[[ -n "$failed" ]] && echo "!! failed:$failed" >&2
echo ">> DONE (logs in $LOGDIR)" >&2
