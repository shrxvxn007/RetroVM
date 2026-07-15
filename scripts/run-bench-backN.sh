#!/usr/bin/env bash
# scripts/run-bench-backN.sh — wrapper around `retrovm --bench-back`.
#
# Substantiates (or refutes) the resume-bullet "sub-millisecond timeline
# recovery" claim with a reproducible, std::chrono-timed measurement of
# the `back N` REPL rewind path. The bench itself lives in
# src/main.cpp:cmd_benchmark_backn (parallel to cmd_benchmark, the
# existing dispatcher benchmark). This wrapper:
#
#   1. Confirms the binary is built (out-of-tree; uses CMake build dir).
#   2. Invokes `retrovm --bench-back [N [depth]]` with the caller-supplied
#      arguments (defaults mirror the C++ defaults: N=1000, depth=200).
#   3. Propagates the bench's exit code as its own: 0 = sub-millisecond
#      claim substantiated (`µs / back op < 1000µs`), 1 = refuted.
#
# Usage:
#   scripts/run-bench-backN.sh <retrovm-binary> [N] [depth]
#
# Examples:
#   scripts/run-bench-backN.sh ./build/retrovm
#   scripts/run-bench-backN.sh ./build/retrovm 1000 200
#   scripts/run-bench-backN.sh ./build/retrovm 5000 100       # larger ring window
#
# Exit codes:
#   0 — bench ran cleanly AND sub-millisecond claim substantiated
#   1 — bench ran cleanly but claim refuted (>=1000 µs / back op)
#   2 — bench could not run (binary missing or unwritable, USAGE error)
#
# Bash 3.2 compatible (default macOS shell).

set -e

[ "$#" -ge 1 ] || {
    cat >&2 <<'USAGE'
usage: scripts/run-bench-backN.sh <retrovm-binary> [N] [depth]
  retrovm-binary  path to the retrovm binary (required)
  N               timed iters (default: 1000; max 100000000)
  depth           snapshots per back op (default: 200; max ring cap)
USAGE
    exit 2
}

RETROVM="$1"
N="${2:-1000}"
DEPTH="${3:-200}"

if [ ! -x "$RETROVM" ]; then
    echo "ERROR: $RETROVM is missing or not executable (build first: cmake -S . -B build && cmake --build build -j)" >&2
    exit 2
fi

# Run the bench. The bench's stdout is the measurement report; its
# exit code IS the sub-millisecond assertion (0=PASS, 1=FAIL).
"$RETROVM" --bench-back "$N" "$DEPTH"
rc=$?

# Echo a one-line summary so a ctest/script wrapper can grep a single
# `bench-backN: PASS|FAIL|ERROR` line without parsing the full report.
# `FAIL` is reserved for the bench's assertion verdict (rc=1); the
# binary-missing USAGE path above is `ERROR` so a `grep FAIL` over
# wrapper output picks up ONLY the bench's verdict, not the wrapper's
# own absence-of-input signal.
if [ "$rc" -eq 0 ]; then
    echo "bench-backN: PASS (sub-millisecond claim substantiated on $(uname -s) $(uname -m))"
elif [ "$rc" -eq 1 ]; then
    echo "bench-backN: FAIL (sub-millisecond claim refuted on $(uname -s) $(uname -m))"
else
    echo "bench-backN: ERROR (bench exited rc=$rc)" >&2
fi

exit $rc
