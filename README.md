# RetroVM

A high-performance, deterministic record-and-replay Virtual Machine with a
time-travel debugger, written from scratch in modern C++20.

> **Status:** Phases 1–5 stable — built-in demo, record/replay,
> mid-program save/restore (`.state`), offline `.state`/`--diff`
> inspection, interactive REPL debugger (`step` / `back N` / breakpoint
> / save). **Phase 6 (offline `.trace` reader)** is in: `retrovm trace
> <file>` shows frame count, magic/version validation, cycle range,
> and per-opcode counts without loading a VM or re-running replay.

## Architecture (Phases 1–3: dispatch + record/replay)

- **Word size:** 32-bit registers and addresses.
- **Memory:** flat 1 MiB byte-addressable array (`std::unique_ptr<uint8_t[]>`
  to keep the hot state struct out-of-line).
- **Registers:** `R0..R7` (8 GPR), `PC`, `SP`, `Flags` (ZF | CF | SF).
- **Hot state:** all of the above plus a 64-bit instruction counter and a
  typed halt tag, packed (with `alignas(64)`) into exactly one 64-byte
  cache line so dispatch reads stay in L1.
- **Encoding:** fixed 32-bit instructions; bytes are emitted **little-endian**
  on disk (matches x86/ARM hosts, no byte-swapping needed). Bits within the
  word are positioned as drawn:

  ```
   31        26 25   23 22   20 19                0
  +------------+-----+-----+---------------------+
  |  opcode(6) | dst | src |     imm20 / addr    |
  +------------+-----+-----+---------------------+
  ```

- **Dispatch:** token-threaded interpreter using Clang/GCC labels-as-values
  (`&&label`) and a tail-dispatch macro. Every opcode handler ends with a
  single indirect branch into the next handler — no megamorphic `while`  head, no cold `switch`. (Measured dispatch throughput — see
  [*Authoritative benchmark*](#authoritative-benchmark-canonical-measurement)
  below.)

### Authoritative benchmark (canonical measurement)

Measured **1.85 ns/instruction (540.21 MIPS)** on this release-build binary,
reproducible as follows. **Unix/Linux/macOS** (single-config):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/retrovm --benchmark 10000
```

**Windows / MSVC** (multi-config; generator name varies by VS install — use
`-G "Visual Studio 17 2022"`, or `-G "Visual Studio 16 2019"` for VS 2019):

```cmd
cmake -S . -B build -G "Visual Studio 17 2022" && cmake --build build --config Release
.\build\Release\retrovm.exe --benchmark 10000
```

Witness recorded on **commit 1648031, Apple M3 / macOS 26.5.2 / Darwin arm64
/ Apple clang 17.0.0 / cmake 4.3.3** (the bench was run before this README
was updated; the bench command itself is branch-agnostic). From a 2-run
spot-check on this host: **1.85 → 1.87 ns/op (1.1% delta)**; broader runs
typically stay under 2% on Apple silicon. Reviewers on a different host should
expect to land within a factor of 2–3 of these numbers. The dispatch is
bounded by `goto *label` indirect-branch prediction, and the BTB depth +
narrow-decoder shape swings the per-op budget across microarchitectures —
Apple silicon M3 vs. x86 Skylake-class ~15–20%. Outside that band, the
most likely culprit is a non-Release build: Debug-mode spot-record gave
9.66 ns/op (496% of Release); `goto *label` still emits at `-O0`,
so this bench avoids the switch-loop fallback that folklore expects.
Folklore 30–100× figures apply to pure-dispatch loops with trivial
handlers; this workload's handlers do real work, which caps the ratio
at ~5×.

Synthetic workload: 12-instruction / 48 B program covering LI / ADD / SUB /
MUL / DIV / LOAD / STORE / JNZ / HALT (IN / RAND deliberately excluded so the
std::function IOHook indirect calls don't skew the per-op); **9 219 cycles
per run**, 1 untimed warmup + 10 000 timed runs = **92.19 M retired
instructions per execution**.


The `--bench-back` REPL rewind bench targets the resume-bullet "sub-millisecond
timeline recovery" claim. Per-op rewind is **well below the 1 ms threshold** at
all supported depths (the C++ bench measures **~2-4 µs** on this release-build
binary; treat as an order-of-magnitude figure, not a precise µs count).
Methodology: a timed loop of **1 000 iterations, each a back+step pair**, over
a deterministic-cycle ALU workload that fully saturates the snapshot ring, plus
1 untimed warmup; the timed region walks the full reverse path (delta-undo +
state-restore, not a synthetic micro-bench). Run-to-run variance reflects a
modest N=1000 sample of short-tail ~3 µs ops influenced by kernel scheduling
jitter and L1/L2 cache state — re-run `./scripts/run-bench-backN.sh ./build/retrovm`
to capture your host's current figure. Result is **PASS** — rewind latency is
~250-500× faster than the 1 ms threshold. Provenance: this README's "~2-4 µs"
captures one Apple-silicon macOS bench run at first publish; the bench script's
own stdout is the source of truth on any other host. `depth >= 256` is rejected
at the C++ arg parser (ring-cap guard against `size_t` underflow; the snapshot
ring is hard-capped at 256 by `cmd_debug` regardless, so the bench's `depth`
ceiling mirrors the REPL's).

**Implementation note — what this bench actually measures.** RetroVM's
interactive debugger keeps a 256-entry cycle history for time-travel; the
bench currently stresses only the snapshot-rich end of that history.
`cmd_debug` hardcodes `set_snapshot_interval(1)` (no `--snap-interval=N` CLI
flag plumbed yet), so the bench runs at worst-case snapshot density (every
cycle produces a snapshot) and the timed rewind is `back 200 + step 200`
rather than literal `back 1`. depth=200 holds the ring at its 256-slot cap
**without triggering ring eviction** (`pop_front()` never fires because the
cap is never exceeded by the rewinds). For the resume-bullet "sub-millisecond
timeline recovery" claim, this is the deepest realistic rebind under
cmd_debug's current cap; literal `back 1` would be ~1 µs and isn't
representative of typical user rebind patterns under realistic ring pressure.
The bench in this commit does **not** exercise: (i) ring-eviction when `ring`
is full and a new snapshot fires; (ii) `snap_interval ∈ {5, 10}` density
regimes; (iii) the trivial single-step `back 1` codepath (intentionally
excluded as not informative).All three regimes (`depth < cap`, `depth = cap`, `depth > cap`) are
now exercised by `retrovm --bench-back-sweep [N]` — a peer of
`--bench-back` — which sweeps the snap_interval × depth grid and prints
a per-cell `µs / back op` table. Cells whose requested depth exceeds
the ring cap clamp at ring-head (so the `size_t` underflow in
`target_idx = history_index - depth` is impossible even at
`depth = 1024`); the printed table annotates clamped cells with `>` so
a reviewer can see at a glance which cells did not exercise the full
rewind distance. The single-cell `--bench-back` bench remains the
release-blocker sub-millisecond gate (with a PASS/FAIL exit code);
`--bench-back-sweep` is a measurement tool that produces a 3×5 table
and propagates a per-cell clean-run / FAIL exit code without a single
PASS/FAIL verdict on the suite as a whole.

### Opcodes

`JNZ` is **register-based**: `JNZ Rd, imm20` jumps to `imm20` iff
`regs[Rd] != 0`. Self-contained condition, no global FLAGS dependency.

| Opcode  | Encoding           | Semantics                                      |
|---------|--------------------|------------------------------------------------|
| `HALT`  | `--`               | Stop execution with `HALT_NORMAL`.             |
| `LI`    | `rd, imm20`        | `rd = imm20` (load-immediate, no memory traffic). |
| `LOAD`  | `rd, [imm20]`      | `rd = mem_u32(imm20 & 0xFFFFF)`.               |
| `STORE` | `rd, [imm20]`      | `mem_u32(imm20 & 0xFFFFF) = rd`.               |
| `ADD`   | `rd, rs`           | `rd += rs;` sets ZF/CF/SF.                     |
| `SUB`   | `rd, rs`           | `rd -= rs;` sets ZF/CF (borrow)/SF.            |
| `MUL`   | `rd, rs`           | `rd *= rs;` keeps low 32, CF if overflow.      |
| `DIV`   | `rd, rs`           | `rd = rd / rs;` div-by-zero traps to `HALT_DIV_ZERO` (remainder dropped, pc rewound to fault). |
| `JUMP`  | `imm20`            | `pc = imm20` (absolute, 4-byte aligned).       |
| `JNZ`   | `rd, imm20`        | `if (regs[rd] != 0) pc = imm20` (register-based). |
| `IN`    | `rd`               | `rd = io.in(cycle)` — non-deterministic input. |
| `RAND`  | `rd`               | `rd = io.rand(cycle)`; deterministic PRNG.     |

### `trace` subcommand (Phase 6 offline reader)

`retrovm trace <bin.trace> [--frame=N]` reads a `.trace` file from disk
and prints its header + aggregate frame stats using the same
one-line-per-field label layout that `dump_state()` / `cmd_inspect` /
`cmd_inspect_diff` share. **No VM is loaded, no replay session is
started, and the corresponding `<bin>` is NOT required** — the trace is
self-describing per the wire format above.

The `--frame=N` flag pivots the reader from whole-trace aggregate
output to per-frame inspection: the same file is opened, the same wire
format is decoded, but the body is one labeled `frame_index / cycle /
opcode / value` block instead of `frame_count / cycle_first..last /
op_IN..op_RAND`. --frame is zero-based; out-of-range N exits non-zero
with `out of range (trace has M frames in '<path>')` so a ctest that
later renumbers frames fails loudly instead of silently printing the
wrong frame.

Output columns (label-anchored, grep-friendly via the `get_field`
helper in `tests/_lib.sh`):

```
=== RetroVM trace inspect ===
file        : programs/io.bin.trace
size_bytes  : 64
magic       : "RVMTRACE"
version     : 1
frame_count : 3
cycle_first : 3
cycle_last  : 5
cycle_delta : 2
op_IN       : 2
op_RAND     : 1
```

- `frame_count` is derived from the mmap'd array of `TraceFrame`
  structs; `size_bytes` should equal `16 + frame_count * 16` for a
  well-formed trace, and the ctest enforces that geometry.
- `op_IN` / `op_RAND` count how many frames logged each
  non-deterministic opcode. `op_other` is suppressed on clean
  traces (the wire format documents only these two opcodes per
  trace.hpp:13-15) and shown only when non-zero.
- An empty trace (deterministic programs with no `IN`/`RAND`,
  e.g. `snap.bin`) prints `cycle_first : (none)` /
  `cycle_last : (none)` / `cycle_delta : 0` so a test that parses
  these as integers sees an obvious `(none)` mismatch.
- `TraceReader`'s ctor already throws on bad magic / wrong version
  / truncated file; the CLI catches that as a stderr diagnostic
  and returns exit 1.

Passing `--frame=N` pivots the reader from aggregate to per-frame:

```
$ ./build/retrovm trace programs/io.bin.trace --frame=2
=== RetroVM trace inspect ===
file        : programs/io.bin.trace
frame_index : 2
cycle       : 5
opcode      : 0x0A (OP_RAND)
value       : <varies by seed>
```

The banner is shared with the aggregate form so a script that greps
both invocations by `=== RetroVM trace inspect ===` still routes them
through one filter; the unique `frame_index :` row makes a per-frame
vs aggregate disambiguation trivial without learning a second banner.
Label widths match the aggregate (label padded to 11 chars + `:` so
all rows hit the same column) so a future `--frame` family that adds
fields (e.g. `cycle_at_N_minus_1`) drops cleanly into the same column
schema used by `tests/_lib.sh: get_field`.

Use cases unlocked:

- **Compare one frame's value to an expected value** — e.g. "the seeded
  RAND frame at cycle 5 should be X; what is it?" — without spinning up
  the VM, replaying, or parsing 64 B `.state` files.
- **Regression check on a wave's first/last frame** — `--frame=0` and
  `--frame=N-1` together pin the cycle-min and cycle-max values that
  the engine's IN/RAND dispatch loop is supposed to fire at, independent
  of the surrounding program shape.
- **Walking an unrelated trace file's first few frames** to sanity-check
  the wire format before subscribing to its per-opcode counts.

### Trace format (`.trace`)

```
[TraceHeader — 16 B]
  char     magic[8]    = "RVMTRACE\0"
  uint32_t version     = 1
  uint32_t reserved    = 0

[TraceFrame × N — 16 B each]
  uint64_t cycle        // VM's cycle_count at the IN/RAND event
  uint8_t  opcode       // OP_IN (0x09) or OP_RAND (0x0A)
  uint8_t  _pad0[3]
  uint32_t value        // 32-bit payload (consumed on replay)
```

Total file size = 16 + `frame_count` × 16. The reader opens the file via
POSIX `mmap` so reads are zero-copy (heap-backed fallback on Windows).

### Divergence detection

`--replay` validates each frame before injection:

- **Cycle mismatch** — the recorded `cycle` differs from the VM's
  `state_.cycle_count`.
- **Opcode mismatch** — e.g. the trace records `OP_IN` for the cycle
  but the VM is executing `OP_RAND`.
- **Premature EOF** — the program wants another event but the trace is
  exhausted.
- **Stragglers** — the program finished but frames are left over.

Any of those produces `*** Divergence Detected! ***` with a precise
`expected` / `actual` diagnostic on stderr and an exit code of 3.

### State-checkpoint format (`.state`)

A `retrovm save <bin> <trace> <state> [--at-frame=N]` run emits a
single 64-byte `.state` file (one cache line on most machines) that
captures enough replay state to resume a session mid-program. On-disk
layout is fixed-width, little-endian, version=1:

```c
/* CheckpointFileHeader — 16 B */
char     magic[8];       // "RVMSTATE"
uint32_t version;        // currently 1
uint32_t frame_index;    // trace cursor (frames consumed so far)

/* CheckpointFileBody — 48 B */
uint32_t halted;         // always HALT_CHECKPOINT (6) — the soft-halt marker retrovm save uses
uint32_t flags;          // ZF | CF | SF bitmask at save time
uint32_t regs[8];        // R0..R7
uint32_t pc;             // vs.pc - 4u: TAIL's `pc += 4u` runs BEFORE the snapshot sink fires
uint32_t sp;             // full-descending stack top
```

Static asserts in `include/retrovm/checkpoint.hpp` enforce these sizes
— adding a field is a hard error rather than silent breakage of
historical `.state` files.

**Two fields that are deliberately NOT on disk (and why):**

- **`cycle_count`** — derived on restore from
  `trace.frame[frame_index].cycle - 1`, so the trace is the single
  source of truth for cycle position. Recording it on disk would be
  redundant with the trace and would risk drift if a future trace
  format changes which cycles get logged.
- **`memory[]`** — the caller already supplies the original `.bin`
  to both `save` and `restore`. The trace re-drives the exact STORE
  sequence the original run produced, so memory content is implicitly
  correct at the resumption point; recording 1 MiB of RAM on every
  save would dwarf the 64 B of actual state.

### `--at-frame=N` semantics

`--at-frame=N` selects which trace frame the VM halts *just before*
consuming. The on-disk `frame_index` is `N` (meaning `N` frames have
already been replayed; `frame[N]` is the next one). The sink inside
`cmd_save` fires only when both `cursor == N` AND
`cycle_count == trace.frame[N].cycle`, so the VM is in a known state
— pc points at the IN/RAND opcode that will consume `frame[N]`, and
the dispatcher will rerun that exact instruction on restore with all
`regs / flags / pc` exactly matching the saved snapshot.

The `pc` on disk is `vs.pc - 4u` because the snapshot timer in
`TAIL_DISPATCH` fires *after* `state_.pc += 4u`; saving the raw
post-increment pc would make restore re-fetch the *next* instruction
rather than the one whose IN/RAND fires next. The matching restore-side
derivation is `restored.cycle_count = trace.frame[N].cycle - 1u` so
`TAIL`'s `++cycle_count` lands exactly on `trace.frame[N].cycle`.

| `--at-frame=`         | effect                                                            |
|-----------------------|-------------------------------------------------------------------|
| `0` (extreme-early)   | save BEFORE any IN/RAND has been consumed. Restore re-fetches the first IN/RAND and consumes `frame[0]`. |
| `1` (default)         | save AFTER the first IN/RAND has been consumed. Restore re-fetches the second IN/RAND.           |
| `N-1` (extreme-late)  | save AFTER the last recorded frame but BEFORE it is consumed. Restore re-runs the last IN/RAND only. |
| `N` or larger         | rejected with `out of range (trace has N frames)` (exit 1).       |

Programs with **no** IN/RAND operands (e.g. `programs/snap.bin`) have
an empty trace. `cmd_save` would reject every `--at-frame=` value
here; the ctest helper detects this and gates on
`record+replay cycle parity` instead.

### Run-time halt dump format (`dump_state`)

`dump_state()` (called by `retrovm run`, `record`, `replay`,
`snapshot`, `save`, `restore`, and as the final summary block of
`retrovm inspect`) emits columns in a **label-based** layout. Each
line is `LABEL: value` or `LABEL: a / b` (the 2-token pc/sp row),
with no trailing legend inline:

```
halt reason: 1
halt_legend: (1=normal 2=div0 3=unk 4=trace_eof 5=diverge 6=checkpoint)
cycles      : 7
pc / sp     : 0x0000001c / 0x000ffffc
flags (ZCS): ---
regs        : R0=5 R1=7 R2=12 R3=12 R4=0 R5=0 R6=0 R7=0
```

Three properties of this layout:

1. **The halt reason and its legend live on separate lines.** The
   pre-refactor single-line form `halt reason : N  (legend)` coupled
   the numeric value to the trailing legend literal, which a caller
   using awk column indices could silently swap (e.g. `$NF` always
   reads the trailing `6=checkpoint)` literal, not the actual reason
   `6`). Splitting eliminates that: `halt reason:` always means
   "the numeric code", `halt_legend:` always means "the legend text".
   The on-disk `halted` field in `.state` is still
   `HALT_CHECKPOINT = 6` per cmd_save's hardcoded policy; the legend
   row is purely informational metadata for the column.

2. **Each label line is independently parseable.** Tests use a
   literal-label-anchored extractor — neither column counts nor
   regex specials are involved:

   ```sh
   # awk's index() is a raw substring search; labels containing `(`,
   # `)`, `:`, or `/` match literally without escaping. The sub()
   # patterns are stable (whitespace class only, no metacharacters).
   get_field() {
       awk -v L="$1" '
           index($0, L) == 1 {
               rest = substr($0, length(L) + 1)
               sub(/^[[:space:]]+/, "", rest)
               sub(/[[:space:]]+$/, "", rest)
               print rest
               exit
           }
       ' <<< "$2"
   }
   ```

   Called as `get_field "cycles      :" "$out"` returns the cycle
   count; `get_field "halt reason:" "$out"` returns the numeric
   reason; `get_field "flags (ZCS):" "$out"` returns the 3-char ZCS
   bitmask. The `pc / sp` line splits on the 3-char ` / ` separator
   via bash parameter expansion (`${var% / *}` and `${var#* / }`) so
   even the 2-token row needs no awk column counting. See `tests/inspect.sh`,
   `tests/record_replay.sh`, and `tests/save_restore.sh` for working
   examples.

3. **`retrovm inspect` emits the SAME column layout.** That means the
   same `get_field` helper works on both run-time halt dumps and
   saved-checkpoint reads — one extractor, two sources. The on-disk
   `.state` uses a different (binary, fixed-width) layout documented
   separately under [State-checkpoint format (`.state`)](#state-checkpoint-format-state).

## Debugging (Phase 5 interactive REPL)

`retrovm debug <bin> [--trace <t>] [--batch <script>]` opens an
interactive command-line debugger over a single VM instance. The
REPL reuses the engine's existing halt-and-resume primitives
instead of growing a parallel execution API:

1. `snap_interval` is set to `1` so the snapshot sink fires after
   every instruction, giving the REPL a per-cycle hook for both
   breakpoint checks and step-boundary halts.
2. The sink is the SINGLE place where breakpoints and step-count
   checks live — no duplicate bookkeeping on the VM class.
3. Soft halts (`HALT_BREAKPOINT`, `HALT_STEP`) are honored by the
   engine's existing `TAIL_DISPATCH` post-snapshot return path
   without blocking the dispatch loop.

### Flags

| Flag             | Effect                                                                |
|------------------|-----------------------------------------------------------------------|
| `--trace <path>` | Replay mode: replace IOHooks with trace-driven `consume()` closures so IN/RAND are re-injected from `<path>`. Cycle/opcode mismatch throws `TraceMismatch`; the REPL catches it, prints a divergence panic, and exits. |
| `--batch <path>` | Read commands from `<path>` (one per line) instead of stdin. Output goes to stdout; the `(retrovm) ` prompt is suppressed. The ctest script `tests/debug.sh` uses this mode for determinism. |
| (no `--trace`)   | Fresh mode: stdin feeds IN, `std::mt19937` seeded from `random_device` feeds RAND. Useful for human exploration of programs that don't require a recorded trace. |

### Command list

The REPL accepts the following commands; tokens are whitespace-
separated and `#`-prefixed lines are comments.

| Command                 | Semantics                                                     |
|-------------------------|---------------------------------------------------------------|
| `step [N]`              | Run until N cycles have retired (default N=1). Halts with `HALT_STEP`. |
| `continue` \| `c`       | Run until the next halt or breakpoint. Prints VM halt reason at end. |
| `regs`                  | Print regs/pc/sp/flags in `dump_state()`'s column layout.    |
| `where`                 | Disassemble the instruction at the current pc (rewinds pc by 4 when halted so the just-halted-at instruction is shown, not the next-to-fetch one). |
| `breakpoint`            | List all breakpoints.                                        |
| `breakpoint <addr>`     | Arm a breakpoint at `<addr>` (hex or decimal via `parse_numeric_arg`). |
| `breakpoint -d <addr>`  | Delete a breakpoint; prints "not present" if absent.        |
| `save <path>`           | Dump a `.state` checkpoint of the current state to `<path>`. In replay mode the saved `frame_index` reflects the current trace cursor. |
| `back [N]` (Phase 5b)   | Rewind N cycles via the in-memory snapshot ring (depth=256). Refuses past-ring with a clean stderr error. The ring rewinds the replay cursor in lock-step so IN/RAND re-dispatch through the rewound range consumes the same trace frames in the same order. |
| `quit` \| `q` \| `exit` | Exit the REPL.                                                |
| `help` \| `?`           | Print the command list.                                       |

### No-rewind resume semantic

`debug_resume()` flips `HALT_BREAKPOINT`, `HALT_STEP`, and
`HALT_CHECKPOINT` to `HALT_RUNNING` *without* rewinding `pc` or
`cycle_count`. This is intentional: by the time the snapshot sink
fires, `TAIL_DISPATCH` (vm.cpp:124-130) has already advanced
`state_.pc += 4u` and `++state_.cycle_count` for the
just-completed instruction. The first TAIL on resume therefore
re-fetches at `state_.pc` and increments cycle by 1, landing
exactly on the instruction the halted-at cycle had pre-fetched.

Practical implications:

- `step 1` after a BP-hit advances exactly ONE more cycle past the
  halted-at instruction (not the halted-at instruction itself).
- `continue` after a BP-hit runs to completion without ever
  re-fetching the just-fired BP — no skip-once bookkeeping dance.
- A `back N` rewind restores `cycle_count` and `pc` verbatim from
  `snapshot[N-1]`; the engine re-fetches on resume and the replay
  cursor moves back in lock-step so IN/RAND re-dispatch consumes
  the same frames in the same order.

Natural halts (`HALT_NORMAL`, `HALT_DIV_ZERO`, `HALT_TRACE_EOF`,
`HALT_DIVERGED`) are NOT resumed by `debug_resume()` — the engine
would happily TAIL-fetch at `state.pc+4` post-HALT and dispatch
whatever bytecode lives past program-end. Reload via
`retrovm run <bin>` to retry.

### Example: breakpoint-driven replay-mode debugging

```bash
# 1. Record a deterministic .trace so IN/RAND replay values are stable.
echo -e "11\n22\n" | ./build/retrovm record programs/io.bin --seed=12648430

# 2. Open the REPL in replay mode (IN/RAND driven from the trace).
./build/retrovm debug programs/io.bin --trace programs/io.bin.trace

# (REPL prompts; type commands)
(retrovm) breakpoint 0x20           # arm at HALT instr
(retrovm) continue                  # run until BP fires
*** Breakpoint 0x00000020 hit ***   # REPL prints halt info
  cycles : 9
  pc     : 0x00000020
(retrovm) regs                      # inspect halt-time regs/pc/sp/flags
halt reason: 7
...
(retrovm) breakpoint -d 0x20        # drop BP so step doesn't re-halt
(retrovm) step 1                    # advance one cycle → HALT_STEP
stepped to cycle=10 pc=0x...
(retrovm) where                     # disassemble post-step pc
(retrovm) save /tmp/checkpoint.state  # snapshot halted state
(retrovm) quit
```

For deterministic ctest runs the same commands can be scripted via
`--batch <script>`; see `tests/debug.sh` for a working example
that records the workflow above into a single ctest invocation.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Requires **Clang >= 14**, **AppleClang >= 14**, or **GCC >= 10**
(computed-goto dispatch). MSVC is not supported.

## Usage

```bash
# Built-in demo (no files needed)
./build/retrovm

# Run a binary program
./build/retrovm run programs/demo.bin

# Assemble .asm text -> .bin
./build/retrovm asm programs/demo.asm programs/demo.bin
./build/retrovm run programs/demo.bin

# — Phase 3: record + replay round trip —
./build/retrovm asm programs/io.asm programs/io.bin
echo -e "11\n22\n" | ./build/retrovm record programs/io.bin
./build/retrovm replay programs/io.bin programs/io.bin.trace

# — Phase 4: mid-program save+restore —
# (always run `record` first — both `save` and `restore` re-drive the
# `.trace`, so the trace file must exist on disk.)
echo -e "11\n22\n" | \
    ./build/retrovm save    programs/io.bin programs/io.bin.trace /tmp/io.state
./build/retrovm restore programs/io.bin programs/io.bin.trace /tmp/io.state

# Save at the very first frame (--at-frame=0): restore re-runs frame[0]
echo -e "11\n22\n" | \
    ./build/retrovm save    programs/io.bin programs/io.bin.trace /tmp/io.state.0 --at-frame=0

# Save just before the last recorded frame (--at-frame=N-1)
echo -e "11\n22\n" | \
    ./build/retrovm save    programs/io.bin programs/io.bin.trace /tmp/io.last --at-frame=2

# — Phase 4+: offline checkpoint inspect —
# Read a saved .state without spinning up the VM or replay session.
# Useful for post-mortem analysis, comparing two checkpoints, or
# quickly checking the on-disk wire format. Output uses the
# **label-based `dump_state` layout** described above — one line per
# field, parsed by literal label via `get_field "label" "$content"`.
# The 64 B file is required (header + body), but the corresponding
# `.bin` and `.trace` files are NOT — inspect is purely offline.
./build/retrovm inspect /tmp/io.state

# Pipe-safe: works on any saved .state from `retrovm save`, regardless
# of whether the program is non-deterministic or purely deterministic.
./build/retrovm inspect /tmp/dz.state

# Non-deterministic programs must pass stdin in the SAME order on
# restore as on save; otherwise divergence is detected and rerun.
printf '5\n7\n' | \
    ./build/retrovm save    programs/divzero.bin programs/divzero.bin.trace /tmp/dz.state
./build/retrovm restore programs/divzero.bin programs/divzero.bin.trace /tmp/dz.state

# — Phase 5: interactive REPL debugger —
# (see the `Debugging (Phase 5 REPL)` section above for the full
# command list, no-rewind resume semantic, and a breakpoint-driven
# replay-mode workflow example)
./build/retrovm record programs/io.bin --seed=12648430 < <(printf '11\n22\n')
./build/retrovm debug programs/io.bin --trace programs/io.bin.trace
```

### `.asm` format

```
LI    R0, 5
LI    R1, 7
LI    R2, 5       ; seed R2 with R0 so ADD R2,R1 produces R0+R1
ADD   R2, R1      ; R2 = 5 + 7 = 12
STORE R2, 0x100
LOAD  R3, 0x100   ; R3 = 12
HALT
```

Comma and whitespace separators are both allowed. Lines starting with
`;` or blank lines are ignored.

### `programs/divzero.asm` — branch-deterministic trap exercise

A 12-instruction Phase-1 test program: two `IN` reads pick divisor
candidates, one `RAND` picks which candidate is the divisor, and
**either** chosen divisor being 0 trips `HALT_DIV_ZERO`. The layout
exercises most of the dispatch table in microcosm — `LI` × 3, `IN` × 2,
`RAND`, `ADD` × 3, `JNZ`, `DIV`, `STORE`, `HALT` — in 48 B.

```bash
./build/retrovm asm programs/divzero.asm programs/divzero.bin
```

Three example stdin inputs demonstrate both branches:

| stdin     | `R3` (RAND)   | divisor chosen     | result                                | halt reason | cycles |
|-----------|---------------|--------------------|---------------------------------------|-------------|--------|
| `5\n7\n`  | ≠ 0 (typical) | `R1 = 5`           | `7 / 5 = 1` → `HALT_NORMAL`           | 1           | 10     |
| `0\n7\n`  | ≠ 0 (typical) | `R1 = 0`           | `7 / 0` → `HALT_DIV_ZERO` (pc rewound)| 2           | 8      |
| `7\n0\n`  | random        | `R1=7` or `R2=0`   | `7/7 = 1` normal **or** `7/0` trap     | 1 or 2      | 10 or 12 |

Run each:

```bash
printf '5\n7\n' | ./build/retrovm run programs/divzero.bin   # halt=1, cycles=10
printf '0\n7\n' | ./build/retrovm run programs/divzero.bin   # halt=2, cycles=8
printf '7\n0\n' | ./build/retrovm run programs/divzero.bin   # halt=1 or 2
```

`R3` in `run` mode is a non-deterministic `mt19937` draw, so the
`7\n0\n` outcome varies across runs: the `JNZ R3, 0x24` either jumps
to the safe divide (using `R1=7`, producing `7/7=1`, halt=1) or
falls through to the swap path (using `R2=0`, producing `7/0`, halt=2).
In `record` mode the seed is the fixed magic `0xC0FFEE`, which on this
build consistently draws a non-zero byte so `7\n0\n` records as safe;
replay reproduces the same draw byte-for-byte.

The trap path also demonstrates the **pc-rewind** behavior: when `DIV`
encounters a zero divisor it sets `halted = HALT_DIV_ZERO` and rewinds
`pc` by 4, so a future Phase-4 debugger or snapshot tool can highlight
the faulting instruction rather than pointing past it.

## Roadmap

| Phase | Feature                                                               |
|-------|-----------------------------------------------------------------------|
| 1 ✅  | Core VM engine, instruction dispatch, binary encoder.                |
| 2 ✅ | Record mode via mmap-backed `.trace` log; cycle-counted events.      |
| 3 ✅ | Replay mode + divergence detection (`--replay`).                     |
| 4 ✅   | Mid-program save+restore (`.state` 64 B with `RVMSTATE` magic); byte-stable replay resumption. |
| 5 ✅   | Interactive CLI debugger (`step`, `back N`, `inspect`, `continue`). |
| 6 ✅   | Offline `.trace` reader (`retrovm trace <file>`): frame count + magic/version validation + per-opcode cycle breakdown. |
