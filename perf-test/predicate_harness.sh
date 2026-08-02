#!/usr/bin/env bash
#
# predicate_harness.sh — runs the predicate push-down benchmark cases and
# collects wall time, matching-row count, and peak RSS per invocation.
#
# Design: each Catch2 case prints one CSV line to stdout of the form
#   BENCH,<fixture>,<mode>,<wall_ms>,<rows>
# and the wrapping `/usr/bin/time -v` reports peak RSS on stderr. We run
# each case in a fresh process so RSS reflects only that case's work.
#
# Each config runs N_RUNS times. Output goes to results.csv with columns:
#   fixture,mode,run_id,wall_ms,rows,peak_rss_kb
#
# Run inside the DAPHNE dev container on the VM, from the daphne repo root:
#
#     bash perf-test/predicate_harness.sh
#

set -euo pipefail

CASES=(
    "bench\, 1M sorted\, baseline|1M_sorted|baseline"
    "bench\, 1M sorted\, predicate|1M_sorted|predicate"
    "bench\, 1M shuffled\, baseline|1M_shuffled|baseline"
    "bench\, 1M shuffled\, predicate|1M_shuffled|predicate"
    "bench\, 10M sorted\, baseline|10M_sorted|baseline"
    "bench\, 10M sorted\, predicate|10M_sorted|predicate"
    "bench\, 10M shuffled\, baseline|10M_shuffled|baseline"
    "bench\, 10M shuffled\, predicate|10M_shuffled|predicate"
)

N_RUNS=${N_RUNS:-5}
OUT_CSV=${OUT_CSV:-/data/predicate-bench/results.csv}

mkdir -p "$(dirname "$OUT_CSV")"
echo "fixture,mode,run_id,wall_ms,rows,peak_rss_kb" > "$OUT_CSV"

# Warm the caches once for each fixture so run 1 doesn't carry the cold-cache tax.
for path in /data/predicate-bench/fixtures/*.orc; do
    cat "$path" > /dev/null
done

for spec in "${CASES[@]}"; do
    IFS='|' read -r test_name fixture mode <<<"$spec"
    for run_id in $(seq 1 "$N_RUNS"); do
        # Capture stdout (BENCH line) and stderr (/usr/bin/time -v output) separately.
        stdout_tmp=$(mktemp)
        stderr_tmp=$(mktemp)
        /usr/bin/time -v ./bin/run_tests "$test_name" >"$stdout_tmp" 2>"$stderr_tmp" || true

        bench_line=$(grep '^BENCH,' "$stdout_tmp" || true)
        rss_kb=$(grep -oE "Maximum resident set size \(kbytes\): [0-9]+" "$stderr_tmp" | awk '{print $NF}')

        if [[ -z "$bench_line" ]]; then
            echo "WARN: no BENCH line for [$test_name] run $run_id" >&2
            cat "$stderr_tmp" >&2
            rm -f "$stdout_tmp" "$stderr_tmp"
            continue
        fi

        # bench_line: BENCH,<fixture>,<mode>,<wall_ms>,<rows>
        IFS=',' read -r _ bench_fixture bench_mode wall_ms rows <<<"$bench_line"
        echo "${bench_fixture},${bench_mode},${run_id},${wall_ms},${rows},${rss_kb}" >> "$OUT_CSV"
        echo "  [$test_name] run $run_id: ${wall_ms} ms, ${rows} rows, ${rss_kb} KB"

        rm -f "$stdout_tmp" "$stderr_tmp"
    done
done

echo ""
echo "Wrote $(wc -l <"$OUT_CSV") lines to $OUT_CSV"
