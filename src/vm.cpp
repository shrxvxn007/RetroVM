// RetroVM — execution engine.
//
// Token-threaded dispatch through computed gotos (`&&label`). Each
// opcode handler ends with TAIL_DISPATCH(): an unfold-then-goto that
// re-fetches, re-decodes, and indirect-jumps to the next instruction
// without ever falling through a `while` head or a `switch` cascade.
// This gives the BTB one entry per opcode and turns the dispatch into
// a steady chain of indirect branches.

#include "retrovm/vm.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <utility>

#if defined(__clang__) || defined(__GNUC__)
#  define RV_HAVE_COMPUTED_GOTO 1
#endif

#ifndef RV_HAVE_COMPUTED_GOTO
#  error "RetroVM requires GCC or Clang for computed-goto dispatch."
#endif

namespace retrovm {

// ----- unaligned little-endian 4-byte memory access (no aliasing UB) -----
// Address is masked so an out-of-range imm20 cannot escape the 1 MiB
// arena, and so a near-end unaligned access wraps within the arena.
static inline uint32_t mem_load_u32(const uint8_t* m, uint32_t addr) noexcept {
    addr &= VM::MEM_MASK;
    const uint32_t b0 = m[addr];
    const uint32_t b1 = m[(addr + 1u) & VM::MEM_MASK];
    const uint32_t b2 = m[(addr + 2u) & VM::MEM_MASK];
    const uint32_t b3 = m[(addr + 3u) & VM::MEM_MASK];
    return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
}
static inline void mem_store_u32(uint8_t* m, uint32_t addr, uint32_t v) noexcept {
    addr &= VM::MEM_MASK;
    m[addr]                       = uint8_t(v        );
    m[(addr + 1u) & VM::MEM_MASK] = uint8_t(v >>  8u);
    m[(addr + 2u) & VM::MEM_MASK] = uint8_t(v >> 16u);
    m[(addr + 3u) & VM::MEM_MASK] = uint8_t(v >> 24u);
}

// ----- ctor / dtor / reset / loader -----

VM::VM()
    : mem_(std::make_unique<uint8_t[]>(MEM_SIZE)) {
    reset_state();
    // Default host I/O. Phase 4 replay will replace these via set_io_hooks
    // so the dispatch loop never has to know it's replaying.
    io_.in = [](uint64_t /*cycle*/) -> uint32_t {
        uint32_t v = 0;
        if (std::scanf("%u", &v) != 1) v = 0;
        return v;
    };
    io_.rand = [](uint64_t /*cycle*/) -> uint32_t {
        // Mersenne Twister seeded once per thread — cheap, decent entropy.
        thread_local std::mt19937 rng{std::random_device{}()};
        return static_cast<uint32_t>(rng());
    };
}

VM::~VM() = default;

void VM::reset_state() {
    state_ = VMState{};
    // Full-descending stack. SP points at the next free slot at the top
    // of memory; PUSH writes at SP and decrements; POP reads and increments.
    state_.sp = static_cast<uint32_t>(MEM_SIZE - 4u);
    std::memset(mem_.get(), 0, MEM_SIZE);
    deltas_.clear();
}

void VM::reset_cpu_for_reuse() noexcept {
    // Fast reset for benchmark/replay-style reuse: zero only the CPU-visible
    // registers / pc / sp / flags / cycle_count. Memory holds the loaded
    // program + live data untouched. Skipping the 1 MiB memset means we
    // can amortise a real dispatch-throughput measurement over thousands
    // of `vm.run()` invocations without spurious noise from page-zeroing.
    state_.cycle_count  = 0;
    state_.pc           = 0;
    state_.sp           = static_cast<uint32_t>(MEM_SIZE - 4u);
    state_.flags        = 0;
    state_.halted       = HALT_RUNNING;
    // Disable snapshotting for benchmark isolation; clear any pending deltas.
    state_.snap_interval = 0;
    state_.snap_timer    = 0;
    for (auto& r : state_.regs) r = 0;
    deltas_.clear();
}

bool VM::load_program(const uint8_t* bytes, std::size_t size) {
    if (size > MEM_SIZE) return false;
    std::memcpy(mem_.get(), bytes, size);
    return true;
}

// =========================================================================
//                              SNAPSHOT (Phase 4)
// =========================================================================
Snapshot VM::take_snapshot() noexcept {
    Snapshot s;
    s.cycle_count = state_.cycle_count;
    for (int i = 0; i < 8; ++i) s.regs[i] = state_.regs[i];
    s.pc     = state_.pc;
    s.sp     = state_.sp;
    s.flags  = state_.flags;
    s.halted = state_.halted;
    // Drain the delta log: every STORE between the last snapshot and now
    // pushed a MemDelta; we move them out so subsequent STOREs start fresh.
    s.deltas = std::move(deltas_);
    deltas_  = std::vector<MemDelta>();
    return s;
}

void VM::make_snapshot() noexcept {
    if (!snap_sink_) {
        // No listener attached — still drain the log so we don't grow it
        // unbounded if the operator enabled checkpointing and forgot a sink.
        deltas_.clear();
        return;
    }
    snap_sink_(take_snapshot());
}

// =========================================================================
//                              MAIN DISPATCH LOOP
// =========================================================================
HaltReason VM::run() {
    // The dispatch table is built from `&&label` (computed goto) addresses
    // of labels declared inside this same function. `static` makes the
    // initialization happen exactly once across calls; the compiler typically
    // promotes it to .rodata so each entry is a single 8-byte pointer fetch.
    static const void* const dispatch[OP_COUNT] = {
        &&label_HALT,  &&label_LOAD,  &&label_STORE, &&label_ADD, &&label_SUB,
        &&label_MUL,   &&label_DIV,   &&label_JUMP,  &&label_JNZ, &&label_IN,
        &&label_RAND,  &&label_LI,
    };

    uint32_t instr, op, dst, src, imm;

    // Tail-dispatch macro: re-fetch-decode-dispatch in one block.
    // `op/dst/src/imm` are declared once at function scope (above); the
    // macro just reassigns them. The compiler will deduplicate the work
    // across handler bodies via CSE since the inputs are reads-of-memory
    // with no observable side effects between fetches.
    //
    // IMPORTANT: snap_timer must be RELOADED before the halted-check
    // returns. Otherwise a sink that calls request_halt() during
    // make_snapshot() will short-circuit through `return state_.halted`
    // and skip the `snap_timer = snap_interval` line below, leaving
    // snap_timer == 0 in the saved VMState. On the next vm.run() call,
    // `--snap_timer` underflows to UINT32_MAX and the snapshot sink
    // is bypassed for the next ~4B cycles, dispatching whatever
    // bytecode happens to live past the program boundary (often a
    // stray OP_HALT from zeroed memory).
#define TAIL_DISPATCH()                                                   \
    do {                                                                  \
        instr = mem_load_u32(mem_.get(), state_.pc);                      \
        state_.pc += 4u;                                                  \
        ++state_.cycle_count;                                             \
        if (__builtin_expect(state_.snap_interval > 0, 0)) {              \
            if (__builtin_expect(--state_.snap_timer == 0, 0)) {          \
                make_snapshot();                                          \
                /* ALWAYS reload the snap timer — even if the snapshot    \
                 * sink requested a halt in its callback. Otherwise the   \
                 * saved VMState has snap_timer == 0 and the next vm.run  \
                 * call's --snap_timer underflows to UINT32_MAX, masking   \
                 * every subsequent snapshot for ~4B cycles. */            \
                state_.snap_timer = state_.snap_interval;                 \
                if (__builtin_expect(state_.halted != HALT_RUNNING, 0)) { \
                    return state_.halted;                                 \
                }                                                         \
            }                                                             \
        }                                                                 \
        op  = decode_op(instr);                                           \
        dst = decode_dst(instr);                                          \
        src = decode_src(instr);                                          \
        imm = decode_imm(instr);                                          \
        if (__builtin_expect(op >= OP_COUNT, 0)) {                        \
            state_.halted = HALT_UNKNOWN_OP;                              \
            return state_.halted;                                         \
        }                                                                 \
        goto *dispatch[op];                                               \
    } while (0)

    // ----- first instruction --------------------------------------------
    TAIL_DISPATCH();

    // ----- opcode handlers ----------------------------------------------
    //
    // Each label does its work, then tail-dispatches into the next
    // opcode. We never fall through to the next label — that would be an
    // architectural bug.

label_HALT:
    state_.halted = HALT_NORMAL;
    return HALT_NORMAL;

label_LOAD:
    // LOAD Rd, [imm20] -- Rd = mem_u32(imm20)
    state_.regs[dst] = mem_load_u32(mem_.get(), imm);
    TAIL_DISPATCH();

label_STORE:
    {
        // Phase 4: delta tracking. Capture the OLD value at `imm` before
        // the write so a snapshot can roll the cell back on restore. If
        // the previous entry in `deltas_` already covers this exact
        // (page, offset) we just bump its run_count — that's the RLE
        // fold for "N consecutive STOREs to the same cell".
        const std::uint8_t  page   = static_cast<std::uint8_t>((imm >> 12) & 0xFFu);
        const std::uint16_t offset = static_cast<std::uint16_t>(imm & 0xFFFu);
        if (!deltas_.empty()
            && deltas_.back().page   == page
            && deltas_.back().offset == offset
            && deltas_.back().run_count < 255u) {
            ++deltas_.back().run_count;
        } else {
            MemDelta d;
            d.run_count = 1u;
            d.page      = page;
            d.offset    = offset;
            d.old_value = mem_load_u32(mem_.get(), imm);
            deltas_.push_back(d);
        }
    }
    // STORE Rd, [imm20] -- mem_u32(imm20) = Rd
    mem_store_u32(mem_.get(), imm, state_.regs[dst]);
    TAIL_DISPATCH();

label_ADD:
    {
        const uint32_t a = state_.regs[dst];
        const uint32_t b = state_.regs[src];
        const uint32_t r = a + b;
        state_.regs[dst] = r;
        uint32_t f = 0;
        if (r == 0u)                     f |= FLAG_ZF; // zero result
        if (r < a)                       f |= FLAG_CF; // unsigned wrap
        if (r & 0x80000000u)             f |= FLAG_SF; // sign bit
        state_.flags = f;
    }
    TAIL_DISPATCH();

label_SUB:
    {
        const uint32_t a = state_.regs[dst];
        const uint32_t b = state_.regs[src];
        const uint32_t r = a - b;
        state_.regs[dst] = r;
        uint32_t f = 0;
        if (r == 0u)                     f |= FLAG_ZF;
        if (a < b)                       f |= FLAG_CF; // borrow
        if (r & 0x80000000u)             f |= FLAG_SF;
        state_.flags = f;
    }
    TAIL_DISPATCH();

label_MUL:
    {
        const uint32_t a = state_.regs[dst];
        const uint32_t b = state_.regs[src];
        const uint64_t p = static_cast<uint64_t>(a) * static_cast<uint64_t>(b);
        const uint32_t r = static_cast<uint32_t>(p);
        state_.regs[dst] = r;
        uint32_t f = 0;
        if (r == 0u)                     f |= FLAG_ZF;
        if (p > 0xFFFFFFFFull)           f |= FLAG_CF; // dropped-bit overflow
        if (r & 0x80000000u)             f |= FLAG_SF;
        state_.flags = f;
    }
    TAIL_DISPATCH();

label_DIV:
    {
        const uint32_t a = state_.regs[dst];
        const uint32_t b = state_.regs[src];
        if (__builtin_expect(b == 0u, 0)) {
            // Trap. The TAIL_DISPATCH() above already advanced pc past
            // the DIV; rewind so a Phase-4 debugger can highlight it.
            state_.pc -= 4u;
            state_.halted = HALT_DIV_ZERO;
            return HALT_DIV_ZERO;
        }
        const uint32_t q = a / b;
        state_.regs[dst] = q;
        // Phase 1 simplification: remainder is dropped. A future MOD
        // instruction can return it without disturbing the ALU convention
        // of "read Rs, write Rd".
        uint32_t f = 0;
        if (q == 0u)                     f |= FLAG_ZF;
        if (q & 0x80000000u)             f |= FLAG_SF;
        state_.flags = f;
    }
    TAIL_DISPATCH();

label_JUMP:
    // JUMP [imm20] -- unconditional, absolute 4-byte aligned target.
    state_.pc = imm;
    TAIL_DISPATCH();

label_JNZ:
    // JNZ Rd, [imm20] -- jump iff Rd != 0.
    // "Jump-on-register-not-zero" avoids the trap of relying on a global
    // FLAGS register that some opcodes (LOAD, STORE, control flow) don't
    // update. Each JNZ self-contains its condition.
    if (state_.regs[dst] != 0u) {
        state_.pc = imm;
    }
    TAIL_DISPATCH();

label_IN:
    // IN Rd -- pull a 32-bit int from "the world". Default: stdin.
    // The hook receives the current cycle_count so a replay driver can
    // cross-check it against the recorded frame. cycle is the cycle on
    // which this IN was fetched and is now retiring.
    if (io_.in) {
        const uint32_t v = io_.in(state_.cycle_count);
        state_.regs[dst] = v;
        if (io_.on_nondeterministic)
            io_.on_nondeterministic(state_.cycle_count, OP_IN, v);
    }
    TAIL_DISPATCH();

label_RAND:
    // RAND Rd -- fill Rd from the PRNG. Same cycle-aware hook as IN.
    if (io_.rand) {
        const uint32_t v = io_.rand(state_.cycle_count);
        state_.regs[dst] = v;
        if (io_.on_nondeterministic)
            io_.on_nondeterministic(state_.cycle_count, OP_RAND, v);
    }
    TAIL_DISPATCH();

label_LI:
    // LI Rd, imm20 -- load-immediate: Rd = imm20. Zero memory traffic.
    // The asm demos and any test program need this to put a known
    // scalar into a register without first writing to memory via STORE.
    state_.regs[dst] = imm;
    TAIL_DISPATCH();

#undef TAIL_DISPATCH
}

} // namespace retrovm
