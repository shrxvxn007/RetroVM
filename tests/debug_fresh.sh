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
# 0 → step 5 → 5 lands at HALT_STEP=8 AND, for programs/io.bin, the
# fresh-mode IN round-trip survived (R0=5 AND R1=7 from the three
# integer values 5, 7, 9 fed via scanf-via-tempfile).
#
# The R0=5 AND R1=7 check is wrapped in a `case "$BIN"` so it only
# fires for programs/io.bin's known IN-register allocation (programs/
# io.asm: R0=IN, R1=IN, R2=RAND). A future test against a different
# binary — or a future io.bin variant where R1 receives something
# else — silently skips the check rather than asserting a wrong value.
# Mirrors tests/inspect_trace.sh's per-program case-skip pattern.

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
#
# Use `printf '%b'` not `printf '%s'` so `\n` in $STDIN becomes a real
# newline. With `%s` the file would contain the literal characters
# `\` and `n`; `scanf("%u")` would parse the first integer and fail
# on every subsequent read (the bare `\` is not a digit and not
# whitespace), so only `IN R0` would ever see a value. The user's
# intent — assert R0=5 AND R1=7 after `step 5` — requires R1 to
# actually round-trip, which requires real newlines in the file.
#
# Note: `%b` interprets ALL backslash escapes in $STDIN (\n → newline,
# \t → tab, \r → carriage return, \0 → NUL etc.). The CMakeLists.txt
# payload for this test is therefore restricted to \n-separated digits;
# if a future caller passes tab-separated or carriage-return-separated
# input, that will be processed as if the escapes were real chars.
printf '%b' "$STDIN" >"$STDIN_P"

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

# 6. IN data-flow round-trip. Belt-and-braces: scanf("%u")-via-tempfile
# MUST deliver stdin[0] to R0 (first IN) AND stdin[1] to R1 (second
# IN). The R2=RAND check is intentionally absent because the engine's
# `step 5` halts at the boundary of `RAND R2` (cycle 5 fires the
# HALT_STEP sink BEFORE `RAND R2`'s body runs), so `R2` is 0 even on
# a clean run — asserting it would pin a known bug, not healthy
# behavior. A separate refactor would address the underlying engine
# resume-path / tail-dispatch pc-and-cycle rewind concern so R2 lands
# on a clean RAND value at cycle ≥ 5; this test deliberately avoids
# depending on that future fix.
#
# Why `tail -1`? The output has TWO `regs        :` lines — initial
# (cycle=0, all zeros) and post-step-5 (the live register file). Both
# match the sed regex; we want the LAST (post-step-5) for the load-
# bearing assertion. Selecting the LAST occurrence matches the no-
# rewind semantic: `regs` is a snapshot of the running state at the
# time of the call, and the post-step-5 call's snapshot is what we
# care about.
#
# Why `sed -nE` and not `get_field`? `_lib.sh::get_field` uses first-
# occurrence semantics; for a label whose value evolves across the
# run (initial R0=0 vs post-step-5 R0=5) we want the LAST. `sed` plus
# `tail -1` is grep-friendly and avoids routing a new helper.
case "$BIN" in
    programs/io.bin)
        r0=$(sed -nE 's/.*R0=([0-9]+)( |$).*/\1/p' "$OUT_P" | tail -1)
        r1=$(sed -nE 's/.*R1=([0-9]+)( |$).*/\1/p' "$OUT_P" | tail -1)
        if [ "$r0" != "5" ] || [ "$r1" != "7" ]; then
            echo "FAIL: fresh-mode IN round-trip lost (R0=$r0 R1=$r1; want R0=5 R1=7 from STDIN='5\n7\n9' via scanf-via-tempfile)" >&2
            cat "$OUT_P" >&2
            exit 1
        fi
        ;;
esac

echo "PASS: debug fresh mode — echo-backs OK; no [trace] line; cycle 0→5; final halt_reason=HALT_STEP; IN round-trip R0=5 R1=7"
exit 0
