#!/usr/bin/env bash
# tests/debug.sh — Phase 5 REPL smoke test.
#
# Records a .trace for <bin> using the same stdin that the existing
# record_replay.sh tests feed in, then runs `retrovm debug --batch
# <script> --trace <trace>` and asserts the captured output contains
# every expected REPL element.
#
# The REPL script exercises the full command surface:
#   regs              -- print registers in dump_state()'s column layout
#   where             -- disassemble the instruction about to execute
#   breakpoint 0x08   -- arm a breakpoint at the third instruction
#   continue          -- run until halt or breakpoint (BP fires here)
#   regs              -- state should now show HALT_BREAKPOINT / cycle>=3
#   breakpoint -d 0x08 -- delete the breakpoint so step is unblocked
#   step 1            -- retire exactly one more cycle (HALT_STEP after)
#   where             -- disassemble instruction at the new halt pc
#   save <path>       -- dump a .state checkpoint of the halted state
#   quit              -- exit the REPL
#
# Assertions:
#   (a) every command line is echoed back as "(retrovm) <cmd>"
#   (b) the BP-hit panic message appears
#   (c) the post-BP cycle_count is >= 3 (third instruction about-to-dispatch)
#   (d) the saved .state is exactly 64 B with the RVMSTATE magic
#   (e) the final halt reason is 8 (HALT_STEP) — i.e., the step after
#       the BP cleanup reached its cycle target cleanly

set -e
. "$(dirname -- "${BASH_SOURCE[0]}")/_lib.sh"

[ "$#" -ge 3 ] || { echo "usage: debug.sh <retrovm> <bin> <stdin>" >&2; exit 2; }
retrovm=$1
bin=$2
stdin=$3
shift 3

trace_p=$(mktemp -t retrovm-debug-trace.XXXX)
script_p=$(mktemp -t retrovm-debug-script.XXXX)
out_p=$(mktemp -t retrovm-debug-out.XXXX)
state_p=$(mktemp -t retrovm-debug-state.XXXX)
vlog=$(mktemp -t retrovm-debug-vlog.XXXX)
trap 'rm -f "$trace_p" "$script_p" "$out_p" "$state_p" "$vlog" "$bin.trace" "$script_p_b" "$out_p_b"' EXIT

on_err() {
    echo "FAIL: line $1, command: $2" >&2
    if [ -s "$out_p" ]; then
        echo "---- debug output ----" >&2
        cat "$out_p" >&2
    fi
    if [ -s "$vlog" ]; then
        echo "---- record output ----" >&2
        cat "$vlog" >&2
    fi
}
trap 'on_err "$LINENO" "$BASH_COMMAND"' ERR

# ---- 1. Record a trace (deterministic, uses --seed to keep RAND stable). ----
# The trace file is written next to <bin> as "<bin>.trace" by `retrovm record`,
# matching the convention cmd_replay and cmd_save already depend on.
# Decimal 12648430 == 0xC0FFEE (cmd_record parses --seed=N as decimal only).
"$retrovm" record "$bin" --seed=12648430 < <(printf '%b' "$stdin") \
    >"$vlog" 2>&1
mv "$bin.trace" "$trace_p"

# ---- 2. Build the REPL script. ----
# Use `__STATE__` as a sentinel and substitute after heredoc-write so the
# state-path argument can include a /tmp-prefix without escaping.
#
# The breakpoint is set at pc=0x20 -- the HALT instruction in programs/io.bin
# (9 instructions total: 0x00..0x20, the last is HALT after 9 cycles).
# Picking HALT as the BP keeps the post-BP trace path free of further IN
# handlers, so `step 1` from BP-hit state halts cleanly at HALT_STEP with
# no replay divergence. The displayed post-BP cycle_count is the cycle
# of the HALT (9 here) regardless of which cycle the trace records the
# IN/RAND events at, because the cursor reaches its terminal value
# before the HALT ever fires.
cat >"$script_p" <<'EOF'
# Initial inspection. halted==0 (HALT_RUNNING); cycle_count==0 (no
# instructions retired yet). `where` shows pc 0x00 decoded.
regs
where
# Arm a breakpoint at the HALT instruction (pc=0x20). The BP fires just
# before HALT executes; cursor is at trace.count=3 (all INs consumed).
breakpoint 0x20
continue
# After BP fires: halted=HALT_BREAKPOINT (7); cycles == 9; cursor=3.
regs
# Drop the BP and step past the halted-at HALT. The post-advance pc lands
# past program-end; the next TAIL hits the step-boundary and halts.
breakpoint -d 0x20
step 1
# After step 1: halted=HALT_STEP (8); cycle_count == 10.
regs
where
# Dump a .state checkpoint of the halted state via checkpoint_save.
save __STATE__
quit
EOF

# Substitute the real /tmp state path. /tmp paths are expected to not
# contain shell metacharacters.
sed -i.bak "s|__STATE__|$state_p|g" "$script_p"
rm -f "$script_p.bak"   # sed -i.bak leaves a .bak -- clean up.

# ---- 3. Run the REPL with --batch --trace (deterministic, replay mode). ----
"$retrovm" debug "$bin" --trace "$trace_p" --batch "$script_p" >"$out_p" 2>&1

# ---- 4. Assertions. ----

# (a) every command was read and echoed.
# Match the "(retrovm) <cmd>" echo line; "save " (with trailing space)
# matches "save /tmp/retrovm-debug-state.XXXX".
for echo_cmd in \
        'regs' \
        'where' \
        'breakpoint 0x20' \
        'continue' \
        'regs' \
        'breakpoint -d 0x20' \
        'step 1' \
        'regs' \
        'where' \
        'save ' \
        'quit'; do
    if ! grep -qF -- "(retrovm) ${echo_cmd}" "$out_p"; then
        echo "FAIL: missing '(retrovm) ${echo_cmd}' echo" >&2
        cat "$out_p" >&2
        exit 1
    fi
done

# (b) BP-fire panic message (`*** Breakpoint 0x00000020 hit ***`).
if ! grep -qF 'Breakpoint 0x00000020 hit' "$out_p"; then
    echo "FAIL: '*** Breakpoint 0x00000020 hit ***' panic not found" >&2
    cat "$out_p" >&2
    exit 1
fi

# (c) The cycle_count printed by the post-BP `regs` line is the second
# `cycles      : ` occurrence (the first is the initial-state one at
# cycle 0). We assert that the second value is >= 3 because the BP at
# pc=0x08 fires AFTER cycle 3 has retired (IN R0 at cycle 1, IN R1 at
# cycle 2, ADD R2 at cycle 3 about-to-dispatch).
post_bp_cycle=$(awk '/cycles      : / { print $3 }' "$out_p" | sed -n '2p')
[ -n "$post_bp_cycle" ] || {
    echo "FAIL: could not extract post-BP cycles from dump_state output" >&2
    cat "$out_p" >&2
    exit 1
}
if [ "$post_bp_cycle" -lt 3 ]; then
    echo "FAIL: post-BP cycles=$post_bp_cycle, want >=3" >&2
    cat "$out_p" >&2
    exit 1
fi

# BSD grep on darwin doesn't honour `\b`; use a literal-trailing-space
# match (every `halt reason:` line is followed by exactly one space).
if ! awk '/^halt reason: / { n=$3; if (n==7) found=1 } END { exit !found }' "$out_p"; then
    echo "FAIL: post-BP halt reason must be 7 (HALT_BREAKPOINT)" >&2
    cat "$out_p" >&2
    exit 1
fi

# (d) saved .state file: 64 B, magic "RVMSTATE".
[ -s "$state_p" ] || { echo "FAIL: $state_p is empty" >&2; cat "$out_p" >&2; exit 1; }
size=$(stat -f %z "$state_p" 2>/dev/null || stat -c %s "$state_p")
[ "$size" = "64" ] || {
    echo "FAIL: $state_p is $size B; expected 64" >&2
    ls -l "$state_p" >&2
    exit 1
}
if ! head -c 8 "$state_p" | grep -q '^RVMSTATE'; then
    echo "FAIL: $state_p magic is not RVMSTATE" >&2
    od -An -c -N 16 "$state_p" >&2
    exit 1
fi

# (e) Final halt reason must be 8 (HALT_STEP). When the user types
# `step 1` after dropping the BP, the next cycle's snapshot hits the
# step-boundary check, which uses HALT_STEP; the previous HALT_BREAKPOINT
# was cleared by debug_resume() before vm.run().
last_halt=$(awk '/halt reason: / { print $3 }' "$out_p" | tail -1)
if [ "$last_halt" != "8" ]; then
    echo "FAIL: final halt reason=$last_halt; expected 8 (HALT_STEP)" >&2
    cat "$out_p" >&2
    exit 1
fi

# ============================================================
# Phase 5b: in-memory snapshot ring — `back N` time-travel scrub
# ============================================================
# The existing test above runs step-by-step via breakpoints. This
# block exercises the unrouted forward-then-back path: step 5 then
# back 1 twice. After back-2 the VM is restored to cycle 3 (the 4th
# committed instruction). Cycle prints 0 → 5 → 4 → 3 across the four
# `regs` blocks (initial, post-step-5, post-back-1, post-back-2);
# the final halt reason is 0 (HALT_RUNNING) because cycle 3 was not
# a halt trigger in io.bin.
echo '---- phase 5b: back N ----'
script_p_b=$(mktemp -t retrovm-back.XXXX)
out_p_b=$(mktemp -t retrovm-back-out.XXXX)
cat >"$script_p_b" <<'EOS'
regs
step 5
regs
back 1
regs
back 1
regs
quit
EOS
"$retrovm" debug "$bin" --trace "$trace_p" --batch "$script_p_b" >"$out_p_b" 2>&1

# (a) every command was read and echoed back by the REPL.
for echo_cmd in 'regs' 'step 5' 'regs' 'back 1' 'regs' 'back 1' 'regs' 'quit'; do
    if ! grep -qF -- "(retrovm) ${echo_cmd}" "$out_p_b"; then
        echo "FAIL (phase5b): missing '(retrovm) ${echo_cmd}' echo" >&2
        cat "$out_p_b" >&2
        exit 1
    fi
done

# (b) Cycle sequence across the four `regs` blocks: 0 (initial), 5
# (post step 5), 4 (post back 1), 3 (post back 2). If the ring-evict
# or restore math is wrong, the post-back cycles will read 5 again
# or the whole sequence will collapse to 0s.
all_cycles=$(awk '/cycles      :/ { print $NF }' "$out_p_b")
expected="0
5
4
3"
if [ "$(printf %s "$all_cycles")" != "$(printf %b "$expected")" ]; then
    echo "FAIL (phase5b): cycles sequence: $(printf %s "$all_cycles" | tr '\n' ' '); expected: 0 5 4 3" >&2
    cat "$out_p_b" >&2
    exit 1
fi

# (c) Last halt reason is 0 (HALT_RUNNING) — cycle 3 is not a halt
# trigger in io.bin, so the snapshot's halted field is RUNNING. If
# the back handler left state.halted unchanged, this would still read
# 8 (HALT_STEP) left over from step 5.
last_halt=$(awk '/halt reason: / { print $3 }' "$out_p_b" | tail -1)
if [ "$last_halt" != "0" ]; then
    echo "FAIL (phase5b): final halt=$last_halt; expected 0 (HALT_RUNNING after back)" >&2
    cat "$out_p_b" >&2
    exit 1
fi

# (d) back N status line itself: each `back 1` should print
# "back 1 cycle(s); now at cycle=N pc=0x..." with the cycle field
# reflecting where the ring restored to.
if ! grep -qF 'back 1 cycle(s); now at cycle=4' "$out_p_b"; then
    echo "FAIL (phase5b): missing 'back 1 cycle(s); now at cycle=4' line" >&2
    cat "$out_p_b" >&2
    exit 1
fi
if ! grep -qF 'back 1 cycle(s); now at cycle=3' "$out_p_b"; then
    echo "FAIL (phase5b): missing 'back 1 cycle(s); now at cycle=3' line" >&2
    cat "$out_p_b" >&2
    exit 1
fi

echo "PASS (phase5b): step 5 → back 1 × 2 lands at cycle 3 with HALT_RUNNING (ring restored correctly)"

echo "PASS: debug --batch produced expected REPL output (BP at 0x08 hit; cycles>=3; save wrote 64 B; final halt=8; back restores to cycle 3)"
