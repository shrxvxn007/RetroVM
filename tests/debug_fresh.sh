#!/usr/bin/env bash
# tests/debug_fresh.sh — Phase 5 fresh-mode REPL smoke test.
#
# Mirrors tests/debug.sh in shape (label-anchored `get_field` extraction
# via tests/_lib.sh, exit-0 iff every assertion holds) but inverts the
# IOHook path: rather than replaying from a recorded `.trace` this test
# runs in fresh mode, where:
#   * `IN` reads from stdin via the VM's default `scanf("%u")` hook.
#   * `RAND` draws from a thread-local `std::mt19937` seeded from
#     `random_device` (per VM ctor in src/vm.cpp).
#   * `on_nondeterministic` is the default (an empty std::function),
#     so the replay-mode `[trace]` lines do NOT appear.
#
# Why a separate ctest? tests/debug.sh covers the replay-mode IOHook
# path (TraceReader → consume closures); this test covers the parallel
# fresh-mode path that cmd_debug() takes when `--trace` is absent.
# Together they pin Phase 5's two IOHook code paths in main.cpp.
#
# Args:
#   $1 = path to retrovm binary
#   $2 = path to .bin program. IN-only is preferred (no RAND) so the
#        test isn't vulnerable to non-deterministic mt19937 draws; if
#        the test uses io.bin (which has IN IN RAND), the RAND step is
#        tolerated by the STEP-boundary halt before any deeper assertion.
#   $3 = stdin payload (literal \n-separated integers, fed to the
#        process via a tempfile so the REPL's --batch script and the
#        VM's IN handler don't compete for the same stdin).
#
# Exit 0 iff retrovm debug exits 0 AND the echo-back of every command
# line is present AND no `[trace]` line appears AND cycle progression
# 0 → step 5 → 5 lands at HALT_STEP=8.

set -e
. "$(dirname -- "${BASH_SOURCE[0]}")/_lib.sh"

RETROVM="$1"
BIN="$2"
STDIN="$3"

SCRIPT_P=$(mktemp -t retrovm-debug-fresh-script.XXXX)
OUT_P=$(mktemp -t retrovm-debug-fresh-out.XXXX)
STDIN_P=$(mktemp -t retrovm-debug-fresh-stdin.XXXX)
trap 'rm -f "$SCRIPT_P" "$OUT_P" "$STDIN_P"' EXIT

# Tiny script: regs / where (initial), step 5, regs / where (post),
# quit. With five REPL commands we exercise every output format
# (regs dump, where disassembly, step-boundary halt, two more regs /
# where pairs, and the quit gate) while keeping the script short.
cat >"$SCRIPT_P" <<'EOF'
regs
where
step 5
regs
where
quit
EOF

# Write the IN values to a tempfile. In fresh mode the REPL doesn't
# read commands from stdin (it reads from $SCRIPT_P), so stdin is
# left for the VM's IN handler. Process substitution would also work
# but a tempfile survives across the catch-all ERR trap if anything
# earlier in the script fails (process substitution lives in a fd
# that disappears when the parent shell exits).
printf '%s' "$STDIN" >"$STDIN_P"

# Run the REPL in fresh mode. `--batch` makes the script the command
# source (so ctest is deterministic); `<"$STDIN_P"` feeds the VM's
# IN handler through stdin WITHOUT competing with the REPL.
"$RETROVM" debug "$BIN" --batch "$SCRIPT_P" <"$STDIN_P" >"$OUT_P" 2>&1

# 1. Every command line must be echoed back as `(retrovm) <cmd>`.
for echo_cmd in 'regs' 'where' 'step 5' 'regs' 'where' 'quit'; do
    if ! grep -qF -- "(retrovm) ${echo_cmd}" "$OUT_P"; then
        echo "FAIL: missing '(retrovm) ${echo_cmd}' echo" >&2
        cat "$OUT_P" >&2
        exit 1
    fi
done

# 2. `where` after each step must emit a pc line and a decoded
# instruction (the disassemble_one output is multi-line, but the
# `dec : ` and `pc  : ` markers are stable across programs — using
# them keeps the test free of brittle opcode-specific expectations).
if ! grep -qE '^  pc  : 0x[0-9a-fA-F]{8}$' "$OUT_P"; then
    echo "FAIL: 'where' did not emit a pc line" >&2
    cat "$OUT_P" >&2
    exit 1
fi
if ! grep -qE '^  dec : ' "$OUT_P"; then
    echo "FAIL: 'where' did not emit a dec line" >&2
    cat "$OUT_P" >&2
    exit 1
fi

# 3. CRITICAL: fresh mode must NOT emit any `[trace]` replay line.
# The on_nondeterministic hook is the default (empty std::function)
# in fresh mode; replay mode's consume-closure path is what prints
# them. This assertion is the load-bearing signal that we exercised
# the fresh-mode IOHook path, not replay mode.
if grep -qF '[trace]' "$OUT_P"; then
    echo "FAIL: fresh mode should not emit [trace] lines (replayed one?)" >&2
    cat "$OUT_P" >&2
    exit 1
fi

# 4. Step 5 actually advanced and the engine halted cleanly. Two
# complementary signals — `get_field` for first-occurrence values
# (the only values it can resolve) plus literal grep for the
# post-step-5 emissions that only appear after the engine commits
# the halt. The combination is grep-friendly without dragging in
# a per-field awk pipeline.
#
#   * `initial_cycle` (via `get_field`) — REPL starts in HALT_RUNNING
#     state with no instructions retired; cycle MUST be 0.
#   * `stepped to cycle=5` literal grep — engine's step-boundary
#     handler prints exactly this line when target_cycle=5 is
#     reached. A regression that swaps the bp/stop-bound checks or
#     stops the sink firing would lose this line.
initial_cycle=$(get_field "cycles      :" "$OUT_P")
if [ "$initial_cycle" != "0" ]; then
    echo "FAIL: initial cycles=$initial_cycle; want 0 (HALT_RUNNING baseline)" >&2
    cat "$OUT_P" >&2
    exit 1
fi
if ! grep -qF "stepped to cycle=5" "$OUT_P"; then
    echo "FAIL: 'stepped to cycle=5' not in output (step-boundary sink did not fire)" >&2
    cat "$OUT_P" >&2
    exit 1
fi

# 5. Final halt was HALT_STEP=8. `get_field` returns the FIRST
# `halt reason:` (initial state, RUNNING=0); a literal grep for
# "halt reason: 8" confirms the post-step-5 state printed by
# the second regs call. Together they pin the no-rewind semantic:
# initial halted=RUNNING, post-step-5 halted=HALT_STEP.
initial_halt=$(get_field "halt reason:" "$OUT_P")
if [ "$initial_halt" != "0" ]; then
    echo "FAIL: initial halt_reason=$initial_halt; want 0 (HALT_RUNNING)" >&2
    cat "$OUT_P" >&2
    exit 1
fi
if ! grep -qF "halt reason: 8" "$OUT_P"; then
    echo "FAIL: 'halt reason: 8' (HALT_STEP) not in output" >&2
    cat "$OUT_P" >&2
    exit 1
fi

echo "PASS: debug fresh mode — echo-backs OK; no [trace] line; cycle 0→5; final halt_reason=HALT_STEP"
exit 0
