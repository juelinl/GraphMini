#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  BUILD_DIR=build ./scripts/compare_schedulers.sh <graph_name> <path_to_graph> <query_name> <query_adjmat> <query_type> <pruning_type> <parallel_type> [exp_base]

Runs the same query with graphpi, graphmini, and graphzero schedulers,
prints runtime/result for each, and exits with non-zero status if the
final RESULT differs across schedulers.
EOF
}

if [[ $# -lt 7 || $# -gt 8 ]]; then
  usage
  exit 1
fi

BUILD_DIR="${BUILD_DIR:-build}"
RUN_BIN="${RUN_BIN:-${BUILD_DIR}/bin/run}"

GRAPH_NAME="$1"
GRAPH_DIR="$2"
QUERY_NAME="$3"
QUERY_ADJMAT="$4"
QUERY_TYPE="$5"
PRUNING_TYPE="$6"
PARALLEL_TYPE="$7"
EXP_BASE="${8:-$(( $(date +%s) % 1000000 ))}"

cmake --build "${BUILD_DIR}" --target run >/dev/null

declare -A RESULTS
declare -A TIMES
schedulers=(graphpi graphmini graphzero)

extract_last_match() {
  local pattern="$1"
  local text="$2"
  printf '%s\n' "${text}" | sed -n "s/.*${pattern}\\(.*\\)\$/\\1/p" | tail -n 1 | tr -d '[:space:]'
}

for i in "${!schedulers[@]}"; do
  scheduler="${schedulers[$i]}"
  exp_id=$((EXP_BASE + i))
  echo "=== ${scheduler} ==="
  output="$("${RUN_BIN}" \
    "--graph_name=${GRAPH_NAME}" \
    "--path_to_graph=${GRAPH_DIR}" \
    "--query_name=${QUERY_NAME}" \
    "--query_adjmat=${QUERY_ADJMAT}" \
    "--query_type=${QUERY_TYPE}" \
    "--pruning_type=${PRUNING_TYPE}" \
    "--parallel_type=${PARALLEL_TYPE}" \
    "--scheduler=${scheduler}" \
    "--exp_id=${exp_id}" 2>&1)"
  printf '%s\n' "${output}"

  result="$(extract_last_match 'Result: ' "${output}")"
  runtime="$(extract_last_match 'Execution Time: ' "${output}")"

  if [[ -z "${result}" || -z "${runtime}" ]]; then
    echo "Failed to parse RESULT or CODE_EXECUTION_TIME(s) for ${scheduler}" >&2
    exit 1
  fi

  RESULTS["${scheduler}"]="${result}"
  TIMES["${scheduler}"]="${runtime}"
done

echo
printf '%-12s %-16s %-20s\n' "scheduler" "result" "time_s"
for scheduler in "${schedulers[@]}"; do
  printf '%-12s %-16s %-20s\n' "${scheduler}" "${RESULTS[${scheduler}]}" "${TIMES[${scheduler}]}"
done

baseline="${RESULTS[graphpi]}"
for scheduler in "${schedulers[@]}"; do
  if [[ "${RESULTS[${scheduler}]}" != "${baseline}" ]]; then
    echo "Result mismatch detected across schedulers." >&2
    exit 1
  fi
done

fastest=""
fastest_time=""
for scheduler in "${schedulers[@]}"; do
  current_time="${TIMES[${scheduler}]}"
  if [[ ! "${current_time}" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
    continue
  fi
  if [[ -z "${fastest}" ]] || awk "BEGIN { exit !(${current_time} < ${fastest_time}) }"; then
    fastest="${scheduler}"
    fastest_time="${current_time}"
  fi
done

echo "All schedulers produced the same RESULT=${baseline}."
if [[ -n "${fastest}" ]]; then
  echo "Fastest scheduler on this run: ${fastest} (${fastest_time}s)"
fi
