# NOTES.md — maintainer-facing notes

This file is for things that should NOT live in the user-facing
README:

  * **Negative findings** from scoped experiments that didn't ship
    — what was tried, what was found, and what would have to change
    for a future attempt to clear the bar.
  * **Hidden-bug postmortems** that aren't visible from reading the
    shipped code, because the code was reverted before it landed.
    Future maintainers who re-attempt the same area deserve to see
    the trap before they walk into it.
  * **Deferred design choices** — work that was explicitly scoped
    out of a phase, with the rationale so the next phase's author
    doesn't have to re-derive it from first principles.

User-facing documentation (build instructions, usage, architecture
overview of the shipped phases) remains in `README.md`. Everything
here is OPTIONAL context for maintainers.

---

## Phase 8 — hint-dispatch pre-decode experiment (REVERTED)

### Status

Not in tree. The hint path was added as a scoped experiment, A/B'd
against the baseline dispatch, and reverted after the A/B failed to
clear the bar. The repository state at commit `62a03cf` is the
post-revert baseline; the experiment's source code is gone.

### Hypothesis

Pre-decoding the next instruction via a sidecar precomputed in
`VM::load_program()` would let `TAIL_DISPATCH()` skip the second
`mem_load_u32` + `decode_*` step on a steady-state computed-goto
loop — saving one cache-line-resident fetch and four bitfield
extractions per retired instruction.

### Implementation (now reverted)

* **New VM fields** in `include/retrovm/vm.hpp`:

  * `bool hint_dispatch_enabled_` — a runtime toggle, default `false`,
    set via `VM::set_hint_dispatch_enabled(bool)`.

  * `next_handler_v_` — a heap-allocated sidecar of pre-encoded
    `(op << 2) | dir` for every `pc >> 2` slot covered by the
    loaded program.

  * `build_next_handler_table()` — `VM::load_program()` walked the
    loaded bytecode once and populated the sidecar when
    `hint_dispatch_enabled_` was true.

* **`TAIL_DISPATCH()` macro body** in `src/vm.cpp` grew an early
  `if (hint_dispatch_enabled_) { ... goto *dispatch[label]; }`
  branch — the label was derived from the sidecar at
  `(nh >> 2) & (OP_COUNT - 1u)`. The default `false` kept every
  existing --bench-* / REPL / record / replay path byte-equal to
  the pre-experiment behaviour.

* **New CLI:** `retrovm --bench-back-sweep-hint N` — a mirror of
  the existing `--bench-back-sweep` with hint enabled, sharing
  the 3×5 grid (snap_intervals {1, 10, 100} × depths
  {1, 50, 257, 512, 1024}) so the two CLIs were directly
  comparable.

### A/B result

Same 3×5 grid `--bench-back-sweep` runs, 50 timed iterations per
cell.

| Outcome                              | Cells   |
|--------------------------------------|---------|
| hint slower than baseline by 2–12%   | 14 of 15 |
| hint indistinguishable (<2% delta)   | 1 of 15 |
| hint faster than baseline by ≥5%     | 0 of 15 |

The minimum bar to keep the experiment was **a ≥5% gain in any
cell**. No cell cleared it. **Decision: revert.**

### The hidden bug that almost escaped

The label-derivation mask was `& (OP_COUNT - 1u)`. With
`OP_COUNT == 12`, the mask is `11 = 0b1011` — a 4-bit mask in
which the `0b0100` and `0b1000` bits are *not* set. This
silently excluded opcodes 4–7 (SUB=4, MUL=5, DIV=6, JUMP=7):
every label derivation for those opcodes bit-shifted to
`label & 11 = 0`, which dispatched to `label_HALT`.

**Symptom:** `hint=on` halted at cycle 5 with `HALT_NORMAL`,
because the first SUB (at pc=16, cycle 5) dispatched straight to
HALT.

**How it surfaced:** the A/B sanity check (`vm.state().cycle_count
!= (2u + 9u * kBenchmarkLoopCount + 1u)`) catches divergent
cycle counts before printing a benchmark. Without that sanity
check the bug would have escaped as a "slightly different timing
number for one of the modes". The sanity check is the reason
this experiment did not silently ship a correctness regression.

**Fix:** `& 0xFu` (an unconditional 4-bit mask covering opcodes
0–15). After the fix the hint path produced correct cycle counts
but *still lost the perf comparison* — the bug was a true-positive
in the A/B but a red herring for the perf verdict.

### Why the hint path didn't help even when correct

The synthetic `--bench` workload is `kBenchmarkLoopCount == 1024`
cycles of 9 ALU ops + 1 STORE + 1 LOAD + 1 JNZ + 1 SUB inside the
inner loop, totaling 9 219 cycles. `TAIL_DISPATCH()` already does:

    instr = mem_load_u32(mem_.get(), state_.pc);   // 4 B, L1-resident
    op  = decode_op(instr);                        // (instr >> 28) & 0xF
    dst = decode_dst(instr);                       // (instr >> 24) & 0xF
    src = decode_src(instr);                       // (instr >> 20) & 0xF
    imm = decode_imm(instr);                       //  instr        & 0xFFFFF
    goto *dispatch[op];                            // the indirect branch

The sidecar saved the `mem_load_u32` and four mask+shift decodes
— but on Apple Clang's computed-goto path the dominant tax is
the indirect branch itself (~2 ns/op floor), and the L1-resident
`mem_load_u32` is cheaper than the indirect jump it precedes.
The net was negative because the sidecar added a second
cache-line miss (the table is not in the same footprint as the
instruction bytes being executed) to a path with no spare memory
bandwidth budget.

**Conclusion:** pre-decode has no headroom on a dispatch loop
already at ~2 ns/op. A different strategy is required for any
≥5% gain — see the recommendation below.

### Recommendation for future attempts

1. **Do not reattempt sidecar pre-decoding on this workload.**
   The dispatch path is at the perf floor and any save from
   removing the inner decode is dominated by the indirect-branch
   tax plus the sidecar's extra cache pressure.

2. **A different strategy is required for a ≥5% gain.** Candidates
   worth scoping as their own phases:

   * **Superblock dispatch.** Compile consecutive independent ops
     into one straight-line block with a single indirect-jump
     tail. Most register VMs (Lua 5, Android Dalvik interpreter
     baseline) reach 1.5–2× over plain computed-goto this way.
     Requires a bytecode-side basic-block analysis pass and a
     second jump-table key.

   * **BTB-aligned opcode layout.** Sequence the 12 opcodes
     so the indirect branch hits the same BTB entry across a
     loop iteration — the inner loop's hotspot opcodes
     (SUB → JNZ → SUB → JNZ …) land in the same BTB row today
     by accident; locking them in via opcode reordering could
     lift the floor a few percent. Doesn't change the bytecode
     wire format, just internal labels.

   * **Fused decode + tail-jump.** Build the dispatch label at
     STORE time (when opcodes are loaded) rather than at fetch
     time, so TAIL_DISPATCH jumps directly into the entry. The
     Engine code already has the right hooks in
     `build_next_handler_table()`; the missing piece is moving
     the label-resolution work into `load_program()`.

   All three of these are scoped projects, not single-flag
   toggles — budget each at a half-day to a day to land cleanly.

3. **Pin baseline numbers first.** Before any future dispatch
   experiment, capture:

       ./build/retrovm --bench-back-sweep 50 > sweep_baseline.txt
       ./build/retrovm --bench-dispatch-cmp 1000 > dcmp_baseline.txt

   and commit the two `.txt` files next to the NOTES.md entry.
   That gives any future `--bench-back-sweep-*` variant a
   known-good reference row.

4. **Keep the sanity-check contract.** The
   `cycles_per_run != (2u + 9u * kBenchmarkLoopCount + 1u)`
   gate is what surfaced the mask bug. Don't drop it in any
   future dispatch-mode experiment — divergent cycle counts
   are a leading indicator of a correctness regression even
   when the timings "look reasonable".

5. **Don't mask by `OP_COUNT - 1` again.** The trap is generic:
   `OP_COUNT` rounded up to the next power of two minus one
   *looks* like a complete mask for opcodes 0..OP_COUNT-1, but
   for any `OP_COUNT` that is NOT a power of two it isn't.

   * If OP_COUNT == 12 (current): the proper mask is `0xFu`
     (covers 0..15 — opcodes 12..15 are reserved).
   * If OP_COUNT grows to 16 or beyond: use the actual bit width
     required by the new opcode range, and verify it
     round-trips through `decode_op()` in a unit test.

   A future hint path (or any future pre-decode that produces
   a label index) should encode the opcode in a fixed-width
   field — e.g. `op | (dir << 4)` — and assert in tests that
   `decode_op(sidecar[i]) == i` for every populated sidecar slot.
   A one-line static assertion that fails CI is much cheaper than
   a benchmarking A/B that quietly disagrees.

---

(Add new top-level `##` sections below for future maintainer notes.
Cross-link from a PR description or a `CHANGELOG` entry whenever a
new section lands so future maintainers know to read it.)
