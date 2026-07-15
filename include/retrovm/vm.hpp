// RetroVM — VM class & supporting types.
//
// Hot architectural state is packed into a single 64-byte cache line. The
// 1 MB data memory is allocated separately so that normal registers (R0..R7,
// PC, SP, flags, cycle_count, halt_reason) stay cache-resident regardless
// of how messy the working data set gets.

#pragma once

#include "retrovm/opcodes.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace retrovm {

// Hot VM state — 64 bytes used within a 64-byte cache line (no tail pad).
//
// Layout order: `cycle_count` first (forces 8-byte alignment at offset 0
// so the rest packs with no internal holes), then the GPR file, then PC,
// SP, flags, the typed halt tag, and finally the Phase-4 snapshot timer.
// Total used = 64 B; `alignas(64)` fits the cache line exactly.
//
// `snap_interval` and `snap_timer` live here (not on the VM class) so the
// dispatch hot path's `if (--snap_timer == 0) make_snapshot()` is a
// single cache-line-resident decrement — no second line miss per cycle.
struct alignas(64) VMState {
    uint64_t cycle_count  = 0;            //  8 B — instructions retired
    uint32_t regs[8]      = {};           // 32 B — R0..R7
    uint32_t pc           = 0;            //  4 B
    uint32_t sp           = 0;            //  4 B — full-descending stack top
    uint32_t flags        = 0;            //  4 B — ZF|CF|SF bitmask
    HaltReason halted     = HALT_RUNNING; //  4 B — typed halt tag
    uint32_t snap_interval = 0;           //  4 B — 0 = checkpointing off
    uint32_t snap_timer    = 0;           //  4 B — reloads from snap_interval
};

// Non-deterministic input hooks. Phase 1 binds the producers (`in`,
// `rand`) to live host I/O. Phase 3 replaces them with trace-driven
// replay closures that validate `(cycle, opcode)` against each frame.
//
// `on_nondeterministic` is an observer that fires on every IN/RAND with
// `(cycle, opcode, value)` — exactly the 64+8+32 trace frame the spec
// asks for. A trace logger attaches here without modifying the dispatch
// loop. Leave any field unset to no-op.
struct IOHooks {
    std::function<void(uint64_t cycle, uint8_t opcode, uint32_t value)> on_nondeterministic;
    std::function<uint32_t(uint64_t cycle)> in;    // IN   : read 32-bit int from "the world"
    std::function<uint32_t(uint64_t cycle)> rand;  // RAND : pull 32 bits from a PRNG
};

// =========================================================================
// Phase 4: delta checkpointing
// =========================================================================
//
// A MemDelta records one STORE's effect: the address that was written, and
// the value that was in memory *before* the write. Run-length fold: if two
// consecutive STOREs target the same (page, offset) the second is merged
// into the first (run_count incremented) so a single triple describes a
// burst of overwrites. On restore, applying the triple once is enough.
struct MemDelta {
    std::uint8_t  run_count;   // 1 = single write, 2..255 = RLE burst
    std::uint8_t  page;        // 0..255 — MEM_SIZE/4096 = 256 pages
    std::uint16_t offset;      // 0..4095 — byte offset within the page
    std::uint32_t old_value;   // 4 B — value at the cell BEFORE the burst
};
static_assert(sizeof(MemDelta) == 8, "MemDelta must remain 8 B");

// Architectural-state snapshot + the memory deltas since the last snapshot.
// `deltas` is drained on take_snapshot() so subsequent STOREs start fresh.
struct Snapshot {
    std::uint64_t    cycle_count = 0;
    std::uint32_t    regs[8]     = {};
    std::uint32_t    pc          = 0;
    std::uint32_t    sp          = 0;
    std::uint32_t    flags       = 0;
    HaltReason       halted      = HALT_RUNNING;
    std::vector<MemDelta> deltas;
};

// On-disk snapshot file format (little-endian byte order):
//
//   SnapshotFileHeader (32 B):
//     char     magic[8];     // "RVMSNAP\0" (8 bytes, no null terminator)
//     uint32_t version;      // currently 1
//     uint32_t flags;        // 0; reserved
//     uint64_t cycle_count;  // cycle on which snapshot fired
//     uint32_t halted;       // HaltReason at snapshot time
//     uint32_t num_deltas;   // count of MemDelta triples that follow
//
//   SnapshotFileRegs (40 B):
//     uint32_t regs[8];      // R0..R7
//     uint32_t pc;
//     uint32_t sp;
//
//   RLE delta list (8 B * num_deltas):
//     MemDelta triples, fixed-width, in dispatch order.
//
// File size = 32 + 40 + 8 * num_deltas.
struct SnapshotFileHeader {
    char          magic[8];
    std::uint32_t version;
    std::uint32_t flags;
    std::uint64_t cycle_count;
    std::uint32_t halted;
    std::uint32_t num_deltas;
};
static_assert(sizeof(SnapshotFileHeader) == 32, "SnapshotFileHeader must be 32 B");

struct SnapshotFileRegs {
    std::uint32_t regs[8];
    std::uint32_t pc;
    std::uint32_t sp;
};
static_assert(sizeof(SnapshotFileRegs) == 40, "SnapshotFileRegs must be 40 B");

// =========================================================================
//   DispatchMode — VM::run() control-flow selector (retrovm namespace)
// =========================================================================
//
//   Lives at retrovm namespace scope (NOT nested inside class VM) so
//   external callers — e.g. main.cpp's cmd_benchmark_dispatch_cmp —
//   can write `using retrovm::DispatchMode;` without qualifying through
//   VM::. Nested types in class VM do not promote to the enclosing
//   namespace in C++, so the setter's full type (`VM::DispatchMode`)
//   would have leaked into main.cpp. Putting the enum at namespace
//   scope keeps the call sites clean and makes the type reusable by
//   any future bench that wants to flip dispatch control flow without
//   becoming a member of VM.
//
//   Two members:
//     ComputedGoto : default production dispatch (`&&label` indirect
//                    jumps; ~1.87 ns/op on Apple Clang).
//     NaiveSwitch  : cascade ladder (`while + switch(op)`); the apples-
//                    to-apples baseline for --bench-dispatch-cmp.
//
//   Maintenance: adding a new opcode requires updating BOTH dispatch
//   paths in vm.cpp's VM::run() (the &&label labels AND the
//   corresponding case in the switch-case ladder). See the
//   MAINTENANCE block at the top of VM::run() in vm.cpp.
enum class DispatchMode : std::uint8_t {
    ComputedGoto = 0,  // default: &&label + goto *dispatch[op]
    NaiveSwitch  = 1,  // while + switch(op) cascade ladder
};

class VM {
public:
    static constexpr std::size_t MEM_SIZE = 1024u * 1024u;     // 1 MiB
    static constexpr uint32_t    MEM_MASK = 0xFFFFFu;          // 20-bit addr mask

    VM();
    ~VM();

    VM(const VM&)            = delete;
    VM& operator=(const VM&) = delete;

    // Reset registers / pc / sp / flags to power-on defaults. Memory is zeroed.
    void reset_state();

    // Fast reset: zero regs/pc/sp/flags/cycle_count WITHOUT touching memory.
    // Used by the benchmark to amortise a 1 MiB memset across thousands of
    // dispatch runs, and by any future replay-style workload that wants the
    // CPU to restart from PC=0/cycle=0 while keeping program + data live.
    void reset_cpu_for_reuse() noexcept;

    // Copy `size` bytes of a `.bin` program into the VM at address 0.
    // Returns false if the program exceeds MEM_SIZE.
    bool load_program(const uint8_t* bytes, std::size_t size);

    // Execute until HALT or another HaltReason. Returns the final reason.
    HaltReason run();

    // Inspection (read-only) for tests & tools.
    const VMState& state()  const noexcept { return state_; }
    const uint8_t*  memory() const noexcept { return mem_.get(); }
    std::size_t     mem_size() const noexcept { return MEM_SIZE; }

    // Apply a MemDelta's old_value back to memory. The LE byte order
    // and MEM_MASK wrap live here so callers don't have to know them.
    // (Current consumer: cmd_debug's snapshot ring rolls memory back
    // when the user issues `back N`; any future tool that needs to undo
    // a STORE can route through the same primitive.)
    void apply_old_value(const MemDelta& d) noexcept {
        // page is uint8 (promotes to int via integer promotion — no
        // explicit cast); offset is uint16 (cast needed because
        // `uint16_t << 12` triggers `-Wsign-conversion` on clang).
        const uint32_t addr = (uint32_t(d.page) << 12)
                            | static_cast<uint32_t>(d.offset);
        mem_[addr                    & MEM_MASK] = static_cast<uint8_t>(d.old_value        );
        mem_[(addr + 1u)             & MEM_MASK] = static_cast<uint8_t>(d.old_value >>  8u);
        mem_[(addr + 2u)             & MEM_MASK] = static_cast<uint8_t>(d.old_value >> 16u);
        mem_[(addr + 3u)             & MEM_MASK] = static_cast<uint8_t>(d.old_value >> 24u);
    }

    // Phase 4 restore: overwrite the full VMState (regs/pc/sp/flags/cycle/
    // halted). Memory is NOT touched — the loader is expected to have
    // already populated the .bin into mem_, and any program writes between
    // record and restore are expected to be re-driven by the trace replay
    // path. Pass a snapshot taken at cycle N to resume mid-program.
    void set_state(const VMState& s) noexcept { state_ = s; }

    // Phase 4 save: one-shot halt. The next time the dispatch loop's
    // snapshot timer fires (i.e. the very next cycle when snap_interval
    // is 1) the VM will return from run() with `state_.halted == r`
    // instead of executing another opcode. Used by `cmd_save` to stop at
    // a specific trace frame index. No-op when snap_interval == 0.
    //
    // Resuming-via-vm.run() callers must NOT manually re-initialise
    // `state_.snap_timer` after a halt: `TAIL_DISPATCH` reloads the
    // timer on every snap fire (before honoring this halt), so once
    // run() returns the timer is already consistent. Treating the
    // timer as caller-managed will break Phase 4 checkpointing.
    void request_halt(HaltReason r) noexcept { state_.halted = r; }

    // Replace I/O hooks. Used by Phase 2 record / Phase 3 replay to wire
    // trace-driven sources without touching the dispatch loop.
    void set_io_hooks(IOHooks hooks) noexcept { io_ = std::move(hooks); }

    // ----- Phase 4: delta checkpointing --------------------------------
    //
    // The dispatch loop calls `make_snapshot()` (private) every
    // `snap_interval` cycles. By default no snapshotting happens.
    // When enabled, the snapshot is fired to the registered `SnapshotSink`
    // (set via set_snapshot_sink) — the VM itself never opens files.

    // Cycle interval between snapshots. 0 = disabled (default).
    //
    // Snapshot invariant: after every sink fire (`make_snapshot()`
    // returns), `TAIL_DISPATCH` reloads `state_.snap_timer =
    // state_.snap_interval` *before* honoring the sink's soft-halt
    // request. A caller resuming via vm.run() therefore does NOT
    // need to manually re-initialise `snap_timer` — the engine's
    // macro keeps it healthy across halt/resume cycles.
    void set_snapshot_interval(std::uint32_t cycles) noexcept {
        state_.snap_interval = cycles;
        state_.snap_timer    = cycles;  // first snapshot fires after `cycles`
    }
    std::uint32_t snapshot_interval() const noexcept {
        return state_.snap_interval;
    }

    // Sink receives every Snapshot fired by the dispatch loop. Pass an
    // empty std::function to detach.
    using SnapshotSink = std::function<void(const Snapshot&)>;
    void set_snapshot_sink(SnapshotSink sink) noexcept {
        snap_sink_ = std::move(sink);
    }

    // Capture the current architectural state + drain pending deltas.
    // Useful for tests and for manual snapshotting from outside `run()`.
    Snapshot take_snapshot() noexcept;

    // Number of memory-delta entries accumulated since the last snapshot.
    std::size_t pending_deltas() const noexcept { return deltas_.size(); }

    // Dispatch control-flow setter / getter. The enum itself
    // (DispatchMode) lives at retrovm namespace scope below so
    // external callers — including main.cpp's cmd_benchmark_dispatch_cmp —
    // can write `using retrovm::DispatchMode;` without needing to
    // qualify through VM::. The enum's contract (ComputedGoto vs
    // NaiveSwitch) and the maintenance rule (new opcode requires
    // updating BOTH dispatch paths in vm.cpp) are documented adjacent
    // to the enum's declaration.
    void set_dispatch_mode(DispatchMode m) noexcept { dispatch_mode_ = m; }
    DispatchMode dispatch_mode() const noexcept { return dispatch_mode_; }

private:
    // Internal: build a Snapshot, drain the delta log, dispatch to sink.
    // Called from TAIL_DISPATCH when snap_timer counts down to zero.
    void make_snapshot() noexcept;

    // 1 MiB heap-backed memory — owned via unique_ptr so VMState stays tiny.
    std::unique_ptr<uint8_t[]> mem_;

    VMState      state_{};
    IOHooks      io_{};
    SnapshotSink snap_sink_;
    std::vector<MemDelta> deltas_;
    // Dispatch hairpin selector; see the public DispatchMode getter
    // above. Default ComputedGoto preserves the historical dispatch
    // behaviour for every existing --bench-* benchmark and the REPL.
    DispatchMode dispatch_mode_ = DispatchMode::ComputedGoto;
};

} // namespace retrovm


