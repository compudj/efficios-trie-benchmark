#!/bin/bash
# check_dcache_figures.sh -- fail if any dcache figure shows data that the
# current code did not produce.
#
# A figure is FRESH only if
#   1. every row of every CSV it is plotted from carries the CURRENT provenance
#      id (scripts/dcache_src_id.sh: a hash of the dcache engines + bench
#      harnesses, and the liburcu commit) -- so a partial re-run (PANELS=...)
#      cannot pass for a full one, and an unstamped (pre-provenance) row is
#      stale; and
#   2. the figure was drawn after that data (re-plotting is the last step).
# Comparing figure timestamps alone let a re-plot of old data pass as fresh.
#
# The slab-route figure compares four liburcu builds: its four CSVs must carry
# the current SOURCE hash and one common liburcu commit.
#
# Run it after any dcache change and before quoting or committing a figure; a
# re-sweep is complete only when this prints nothing and exits 0.
set -u
REPO=$(git -C "$(dirname "$0")" rev-parse --show-toplevel) || exit 2
cd "$REPO" || exit 2
S=scripts
CUR=$("$S/dcache_src_id.sh")
CUR_SRC=${CUR%%-*}

# figure -> the CSVs it is plotted from
declare -A INPUTS=(
	[dcache_s3]="dcache_sweep"
	[dcache_readdir]="dcache_sweep"
	[dcache_dpath]="dcache_sweep"
	[dcache_hit]="dcache_sweep"
	[dcache_idle]="dcache_sweep"
	[dcache_sat]="dcache_sweep"
	[dcache_height]="dcache_height"
	[dcache_churn]="dcache_churn"
	[dcache_churn_scaling]="dcache_churn_scaling"
	[dcache_namewidth]="dcache_namewidth"
	[dcache_optaxonomy]="dcache_optaxonomy"
	[dcache_optype]="dcache_optype"
	[dcache_readdir_churn]="dcache_readdir_churn"
	[dcache_slabroute]="dcache_churn dcache_churn_rseq dcache_churn_batch dcache_churn_batch_rseq"
)

# When was @f last changed: commit time if clean, else mtime.
when() {
	if [[ -n "$(git status --porcelain -- "$1")" ]]; then
		stat -c %Y "$1"
	else
		git log -1 --format=%ct -- "$1"
	fi
}

# The provenance ids in @csv's src column, one per distinct value ("" for an
# unstamped row or a file without the column).
ids_of() {
	awk -F, 'NR == 1 { for (i = 1; i <= NF; i++) if ($i == "src") c = i; next }
		 { print (c ? $c : "") }' "$1" | sort -u
}

stale=0
report() { printf 'STALE  %-34s %s\n' "$1" "$2"; stale=$((stale + 1)); }

for fig in figures/dcache_*.png; do
	name=$(basename "$fig" .png)
	inputs=${INPUTS[$name]:-}
	if [[ -z "$inputs" ]]; then
		report "$fig" "(no known input CSV: add it to check_dcache_figures.sh)"
		continue
	fi
	why=""
	commits=""
	for c in $inputs; do
		csv=$S/$c.csv
		if [[ ! -f "$csv" ]]; then
			why="$why $c.csv missing;"
			continue
		fi
		for id in $(ids_of "$csv" | sed 's/^$/<unstamped>/'); do
			if [[ "$name" == dcache_slabroute ]]; then
				[[ "${id%%-*}" == "$CUR_SRC" ]] ||
					why="$why $c.csv has rows from $id;"
				commits="$commits ${id#*-}"
			elif [[ "$id" != "$CUR" ]]; then
				why="$why $c.csv has rows from $id;"
			fi
		done
		[[ $(when "$fig") -ge $(when "$csv") ]] ||
			why="$why drawn before $c.csv was last written;"
	done
	if [[ -n "$commits" ]] &&
	   [[ $(tr ' ' '\n' <<< "$commits" | sed '/^$/d' | sort -u | wc -l) -gt 1 ]]; then
		why="$why routes mix liburcu commits ($(tr ' ' '\n' <<< "$commits" | sed '/^$/d' | sort -u | tr '\n' ' '));"
	fi
	[[ -n "$why" ]] && report "$fig" "$why"
done

if [[ $stale -gt 0 ]]; then
	echo "$stale stale dcache figure(s) (current id $CUR): re-sweep and re-plot before quoting them." >&2
	exit 1
fi
echo "all dcache figures show data from the current code ($CUR)"
