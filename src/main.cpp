// RetroVM — Phase 3 command-line entry point.
//
//   retrovm                          → run the built-in demo program
//   retrovm run      <bin>           → load and execute a binary program
//   retrovm asm      <src> <bin>     → assemble .asm text to .bin bytecode
//   retrovm record   <bin>           → run + capture non-deterministic events to <bin>.trace
//   retrovm replay   <bin> <trace>   → replay with divergence detection

#include "retrovm/vm.hpp"
#include "retrovm/opcodes.hpp"
#include "retrovm/assembler.hpp"
#include "retrovm/trace.hpp"
#include "retrovm/checkpoint.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <cctype>
#include <deque>
#include <string>
#include <vector>

using retrovm::VM;
using retrovm::VMState;
using retrovm::encode;
using retrovm::decode_op;
using retrovm::decode_dst;
using retrovm::decode_src;
using retrovm::decode_imm;
using retrovm::TraceReader;
using retrovm::TraceWriter;
using retrovm::TraceMismatch;
using retrovm::TraceFrame;
using retrovm::Snapshot;
using retrovm::MemDelta;
using retrovm::SnapshotFileHeader;
using retrovm::SnapshotFileRegs;
using retrovm::CheckpointState;
using retrovm::checkpoint_load;
using retrovm::checkpoint_save;

using retrovm::OP_HALT;
using retrovm::OP_LOAD;
using retrovm::OP_STORE;
using retrovm::OP_ADD;
using retrovm::OP_SUB;
using retrovm::OP_MUL;
using retrovm::OP_DIV;
using retrovm::OP_JUMP;
using retrovm::OP_JNZ;
using retrovm::OP_LI;
using retrovm::OP_IN;
using retrovm::OP_RAND;
using retrovm::HALT_RUNNING;
using retrovm::HALT_NORMAL;
using retrovm::HALT_DIVERGED;
using retrovm::HALT_TRACE_EOF;
using retrovm::HALT_CHECKPOINT;
using retrovm::HALT_BREAKPOINT;
using retrovm::HALT_STEP;
using retrovm::HaltReason;

namespace {

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "retrovm: cannot open '%s'\n", path.c_str());
        std::exit(1);
    }
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                                  std::istreambuf_iterator<char>());
}

void dump_state(const VM& vm, HaltReason reason) {
    const auto& s = vm.state();
    // Decoupled halt reason from its legend: emit `halt reason: <N>` on
    // its own line and `halt_legend: (...)` on a separate line. The
    // earlier single-line form (`halt reason : <N>  (legend)`) coupled
    // the actual numeric reason to the trailing legend string, which
    // forced awk-extraction paths to choose between `$4` (correct) and
    // `$NF` (always the trailing legend literal, e.g. `6=checkpoint)`).
    // Splitting the line makes the column-fragility go away: the value
    // is unambiguously "everything after `halt reason:`".
    std::printf("halt reason: %u\n", static_cast<unsigned>(reason));
    std::printf("halt_legend: (1=normal 2=div0 3=unk 4=trace_eof 5=diverge 6=checkpoint)\n");
    std::printf("cycles      : %llu\n", static_cast<unsigned long long>(s.cycle_count));
    std::printf("pc / sp     : 0x%08x / 0x%08x\n", s.pc, s.sp);
    std::printf("flags (ZCS): %c%c%c\n",
                s.flags & 0x1 ? 'Z' : '-',
                s.flags & 0x2 ? 'C' : '-',
                s.flags & 0x4 ? 'S' : '-');
    std::printf("regs        : R0=%u R1=%u R2=%u R3=%u R4=%u R5=%u R6=%u R7=%u\n",
                s.regs[0], s.regs[1], s.regs[2], s.regs[3],
                s.regs[4], s.regs[5], s.regs[6], s.regs[7]);
}

// ----- built-in demo (Phase 1, untouched) -----

void run_builtin_demo() {
    // R0=5; R1=7; R2 = R0+R1; mem[0x100] = R2; R3 = mem[0x100]; HALT.
    // ADD is "Rd += Rs" so we seed R2 with R0 first, then add R1.
    const std::vector<uint32_t> program = {
        encode(OP_LI,    0, 0, 5u    ),
        encode(OP_LI,    1, 0, 7u    ),
        encode(OP_LI,    2, 0, 5u    ),
        encode(OP_ADD,   2, 1, 0u    ),
        encode(OP_STORE, 2, 0, 0x100u),
        encode(OP_LOAD,  3, 0, 0x100u),
        encode(OP_HALT,  0, 0, 0u    ),
    };
    VM vm;
    vm.load_program(reinterpret_cast<const uint8_t*>(program.data()), program.size() * 4u);
    std::puts("=== RetroVM built-in demo ===");
    const HaltReason r = vm.run();
    dump_state(vm, r);

    const auto& s = vm.state();
    int failures = 0;
    auto check = [&](const char* what, auto got, auto want) {
        const bool ok = (got == want);
        std::printf("  %-9s got=%-10llu want=%-10llu %s\n", what,
                    static_cast<unsigned long long>(got),
                    static_cast<unsigned long long>(want),
                    ok ? "OK" : "FAIL");
        if (!ok) ++failures;
    };
    std::puts("assertions:");
    check("R0",       s.regs[0],     5u);
    check("R1",       s.regs[1],     7u);
    check("R2",       s.regs[2],    12u);
    check("R3",       s.regs[3],    12u);
    check("halt",     static_cast<uint32_t>(r), 1u);
    check("cycles",   s.cycle_count, uint64_t{7});

    const uint32_t mem100 =  uint32_t(vm.memory()[0x100])       |
                            (uint32_t(vm.memory()[0x101]) << 8) |
                            (uint32_t(vm.memory()[0x102]) << 16)|
                            (uint32_t(vm.memory()[0x103]) << 24);
    check("mem[0x100]", mem100, 12u);

    if (failures) {
        std::fprintf(stderr, "DEMO FAILED: %d assertion(s)\n", failures);
        std::exit(2);
    }
    std::puts("DEMO PASSED");
}

// ----- assemble / run -----

int cmd_assemble(const std::string& src, const std::string& dst) {
    std::vector<uint32_t> out_instrs;
    return retrovm::assemble_file(src, dst, out_instrs);
}

int cmd_run(const std::string& bin) {
    const auto bytes = read_file(bin);
    VM vm;
    if (!vm.load_program(bytes.data(), bytes.size())) {
        std::fprintf(stderr, "retrovm: program too large for 1 MiB memory\n");
        return 1;
    }
    std::printf("retrovm: running '%s' (%zu bytes)\n", bin.c_str(), bytes.size());
    const HaltReason r = vm.run();
    dump_state(vm, r);
    return (r == HALT_NORMAL) ? 0 : 2;
}

// ----- record (Phase 2 flavor): run + accumulate .trace frames -----
//
// Determinism policy:
//   * IN  reads from host stdin (scanf-style uints). EOF yields 0.
//   * RAND uses mt19937 seeded with `seed` (defaults to kRecordRandSeed
//     when the user doesn't pass --seed=N), so the same stdin + same
//     program + same seed always produces a byte-identical .trace.
//     This is the property CI replays and historical playback depend on.
//   * Every IN/RAND also writes a 16-byte TraceFrame into a TraceWriter
//     via on_nondeterministic. After HALT the writer flushes to
//     `<bin>.trace`.

// Default seed when the user doesn't pass --seed=N. Picked so the first
// mt19937 draw is non-zero on every machine, keeping the "no flags"
// invocation byte-stable across hosts.
static constexpr std::uint32_t kRecordRandSeed = 0xC0FFEEu;

int cmd_record(const std::string& bin, std::uint32_t seed) {
    const auto bytes = read_file(bin);
    VM vm;
    if (!vm.load_program(bytes.data(), bytes.size())) {
        std::fprintf(stderr, "retrovm: program too large for 1 MiB memory\n");
        return 1;
    }

    TraceWriter writer;
    auto rng_owned = std::make_shared<std::mt19937>(seed);
    retrovm::IOHooks hooks;
    hooks.in = [](std::uint64_t /*cycle*/) -> std::uint32_t {
        std::uint32_t v = 0;
        if (std::scanf("%u", &v) != 1) v = 0;
        return v;
    };
    hooks.rand = [rng_owned](std::uint64_t /*cycle*/) -> std::uint32_t {
        return static_cast<std::uint32_t>((*rng_owned)());
    };
    hooks.on_nondeterministic = [&writer](std::uint64_t cycle, std::uint8_t op,
                                          std::uint32_t value) {
        writer.append(cycle, op, value);
    };
    vm.set_io_hooks(std::move(hooks));

    std::printf("retrovm: record '%s' (%zu bytes) seed=0x%08x\n",
                bin.c_str(), bytes.size(), static_cast<unsigned>(seed));
    const HaltReason r = vm.run();

    const std::string trace_path = bin + ".trace";
    writer.flush_to_file(trace_path);
    std::printf("trace       : %zu frames -> %s\n", writer.frame_count(), trace_path.c_str());
    dump_state(vm, r);
    return (r == HALT_NORMAL) ? 0 : 2;
}

// ----- replay (Phase 3): re-inject trace frames, assert round-trip -----
//
// For each IN/RAND the VM asks for a value at cycle N. The replay hook
// validates the next frame is (cycle==N, opcode matches), then injects
// the recorded value. Any cycle/opcode mismatch OR premature EOF throws
// a TraceMismatch which the CLI translates into 'Divergence Detected!'.

int cmd_replay(const std::string& bin, const std::string& trace_path) {
    const auto bytes = read_file(bin);
    const TraceReader reader(trace_path);

    std::printf("retrovm: replay '%s' (%zu bytes) against trace '%s'\n",
                bin.c_str(), bytes.size(), trace_path.c_str());
    std::printf("trace       : %zu frames, version %u, magic \"%.8s\"\n",
                reader.frame_count(), reader.version(),
                reader.magic().data());

    VM vm;
    if (!vm.load_program(bytes.data(), bytes.size())) {
        std::fprintf(stderr, "retrovm: program too large for 1 MiB memory\n");
        return 1;
    }

    auto cursor = std::make_shared<std::size_t>(0);
    auto consume = [&reader, cursor](std::uint64_t cycle, std::uint8_t want_op)
        -> std::uint32_t {
        const std::size_t idx = *cursor;
        const TraceFrame* f = reader.frame_at(idx);
        if (!f) {
            throw TraceMismatch(cycle, want_op, /*actual_cycle*/ 0,
                                /*actual_op*/ 0, idx, /*premature_eof*/ true);
        }
        if (f->cycle != cycle || f->opcode != want_op) {
            throw TraceMismatch(cycle, want_op, f->cycle, f->opcode, idx, false);
        }
        const std::uint32_t value = f->value;
        ++(*cursor);
        return value;
    };

    retrovm::IOHooks hooks;
    hooks.in   = [consume](std::uint64_t cycle) { return consume(cycle, OP_IN);   };
    hooks.rand = [consume](std::uint64_t cycle) { return consume(cycle, OP_RAND); };
    // Mirror to stdout so users can see what got re-injected.
    hooks.on_nondeterministic = [](std::uint64_t cycle, std::uint8_t op,
                                   std::uint32_t value) {
        std::printf("  [trace] cycle=%-4llu opcode=0x%02X value=%u\n",
                    static_cast<unsigned long long>(cycle),
                    static_cast<unsigned>(op), value);
    };
    vm.set_io_hooks(std::move(hooks));

    HaltReason r;
    try {
        r = vm.run();
    } catch (const TraceMismatch& div) {
        std::fprintf(stderr,
            "\n*** Divergence Detected! ***\n"
            "  %s\n"
            "  expected at cycle=%llu  opcode=0x%02X\n",
            div.what(),
            static_cast<unsigned long long>(div.expected_cycle()),
            static_cast<unsigned>(div.expected_op()));
        if (!div.premature_eof()) {
            // Only print the bogus "frame#" line when there *is* a frame
            // to cite; on premature EOF there is no actual row to show.
            std::fprintf(stderr,
                "  trace  frame#%-4zu    opcode=0x%02X  cycle=%llu\n",
                div.frame_idx(),
                static_cast<unsigned>(div.actual_op()),
                static_cast<unsigned long long>(div.actual_cycle()));
        }
        r = div.premature_eof() ? HALT_TRACE_EOF : HALT_DIVERGED;
        dump_state(vm, r);
        std::fprintf(stderr,
            "replay aborted: %llu frame(s) consumed of %zu\n",
            static_cast<unsigned long long>(*cursor), reader.frame_count());
        return 3;
    }

    // Did we consume every frame? Stragglers = mismatched program shape.
    const std::size_t consumed = *cursor;
    if (consumed < reader.frame_count()) {
        std::fprintf(stderr,
            "\n*** Divergence Detected! ***\n"
            "  %zu unread frame(s) remain in trace (program ended early)\n",
            reader.frame_count() - consumed);
        dump_state(vm, r);
        return 3;
    }

    std::puts("replay VERIFIED");
    dump_state(vm, r);
    return (r == HALT_NORMAL) ? 0 : 2;
}

// ----- snapshot (Phase 4): dump delta-checkpointed state to <bin>.snap.NNNN
//
// Each VM snapshot is emitted as its own file, named `<bin>.snap.NNNN` with
// a zero-padded 4-digit index. Each file = 32 B header + 40 B register
// block + (8 B * num_deltas) RLE deltas. The dispatch loop fires one
// snapshot every `interval` cycles (default 10); STORE has been
// instrumented to push a MemDelta capturing the old value before each
// write, with run-length folding for consecutive same-cell writes.

int cmd_snapshot(const std::string& bin, std::uint32_t interval) {
    const auto bytes = read_file(bin);
    VM vm;
    if (!vm.load_program(bytes.data(), bytes.size())) {
        std::fprintf(stderr, "retrovm: program too large for 1 MiB memory\n");
        return 1;
    }

    std::printf("retrovm: snapshot '%s' (%zu bytes) every %u cycles\n",
                bin.c_str(), bytes.size(), interval);

    auto counter = std::make_shared<std::uint32_t>(0);
    vm.set_snapshot_interval(interval);
    vm.set_snapshot_sink([bin, counter](const Snapshot& s) {
        const std::uint32_t idx = *counter;
        char path[512];
        std::snprintf(path, sizeof(path), "%s.snap.%04u", bin.c_str(), idx);
        ++(*counter);

        std::FILE* f = std::fopen(path, "wb");
        if (!f) {
            std::fprintf(stderr, "retrovm: cannot open '%s' for writing\n", path);
            return;
        }

        // 32-byte file header.
        SnapshotFileHeader hdr;
        std::memcpy(hdr.magic, "RVMSNAP", 8);
        hdr.version     = 1u;
        hdr.flags       = 0u;
        hdr.cycle_count = s.cycle_count;
        hdr.halted      = static_cast<std::uint32_t>(s.halted);
        hdr.num_deltas  = static_cast<std::uint32_t>(s.deltas.size());

        // 40-byte register block.
        SnapshotFileRegs regs;
        for (int i = 0; i < 8; ++i) regs.regs[i] = s.regs[i];
        regs.pc = s.pc;
        regs.sp = s.sp;

        std::fwrite(&hdr,   sizeof(hdr),   1, f);
        std::fwrite(&regs,  sizeof(regs),  1, f);
        if (!s.deltas.empty()) {
            std::fwrite(s.deltas.data(), sizeof(MemDelta),
                        s.deltas.size(), f);
        }
        std::fclose(f);

        std::printf("  [%04u] %s  cycle=%-4llu  deltas=%zu  size=%zu B\n",
                    idx, path,
                    static_cast<unsigned long long>(s.cycle_count),
                    s.deltas.size(),
                    sizeof(hdr) + sizeof(regs) + s.deltas.size() * sizeof(MemDelta));
    });

    const HaltReason r = vm.run();
    std::printf("snapshots  : %u written -> %s.snap.NNNN\n",
                *counter, bin.c_str());
    dump_state(vm, r);
    return (r == HALT_NORMAL) ? 0 : 2;
}

// ----- benchmark: confirm the token-threaded dispatcher hits its
//   single-digit-ns/op target on Apple Clang.
//
// Synthetic workload is a 12-instruction program with an inner loop that
// covers the most common dispatch slots (ALU, LOAD, STORE, JNZ, HALT) but
// deliberately omits IN and RAND so the std::function IOHooks callbacks
// are never invoked during the timed phase. That keeps the measurement
// pure-dispatch (fetch + decode + indirect-jump) and avoids skewing the
// ns/op number with closure-call indirection.
//
// Layout (each instruction = 4 B):
//   0x00  LI    R0, kLoopCount          ; counter (init)
//   0x04  LI    R1, 1                   ; decrement amount
//   0x08  ADD   R2, R1                  ; \\  loop body (9 instr/iter):
//   0x0C  ADD   R3, R1                  ;  | exercises ADD, SUB, MUL, DIV,
//   0x10  SUB   R4, R0                  ;  | LOAD, STORE in the ICache,
//   0x14  MUL   R5, R1                  ;  | JNZ to keep the BTB warm.
//   0x18  DIV   R6, R1                  ;  | R1 is always 1, so DIV is safe.
//   0x1C  STORE R2, 0x100               ;  |
//   0x20  LOAD  R7, 0x100               ;  |
//   0x24  SUB   R0, R1                  ;  | counter-- + branch
//   0x28  JNZ   R0, 0x08                ; /
//   0x2C  HALT
//
// Per-run cycles = 2 (init) + 9 * kLoopCount (loop) + 1 (HALT) = 9219.

static constexpr std::uint32_t kBenchmarkLoopCount = 1024u;

int cmd_benchmark(std::size_t n_iters) {
    const std::vector<uint32_t> program = {
        encode(OP_LI,    0, 0, kBenchmarkLoopCount),
        encode(OP_LI,    1, 0, 1u                 ),
        encode(OP_ADD,   2, 1, 0u                 ),
        encode(OP_ADD,   3, 1, 0u                 ),
        encode(OP_SUB,   4, 0, 0u                 ),
        encode(OP_MUL,   5, 1, 0u                 ),
        encode(OP_DIV,   6, 1, 0u                 ),
        encode(OP_STORE, 2, 0, 0x100u             ),
        encode(OP_LOAD,  7, 0, 0x100u             ),
        encode(OP_SUB,   0, 1, 0u                 ),
        encode(OP_JNZ,   0, 0, 0x08u              ),
        encode(OP_HALT,  0, 0, 0u                 ),
    };

    VM vm;
    if (!vm.load_program(reinterpret_cast<const uint8_t*>(program.data()),
                         program.size() * 4u)) {
        std::fprintf(stderr, "retrovm: benchmark program too large\n");
        return 1;
    }

    // Sanity: one untimed run must HALT_NORMAL and retire the expected
    // cycle count. If the workload is broken, the ns/op number is
    // meaningless, so refuse to print a benchmark.
    const HaltReason sanity = vm.run();
    if (sanity != HALT_NORMAL) {
        std::fprintf(stderr, "retrovm: benchmark sanity failed (halt=%u)\n",
                     static_cast<unsigned>(sanity));
        return 1;
    }
    const std::uint64_t cycles_per_run = vm.state().cycle_count;
    const std::uint64_t expected = 2u + 9u * kBenchmarkLoopCount + 1u;
    if (cycles_per_run != expected) {
        std::fprintf(stderr,
            "retrovm: benchmark cycle count drift (got %llu, want %llu)\n",
            static_cast<unsigned long long>(cycles_per_run),
            static_cast<unsigned long long>(expected));
        return 1;
    }

    // Warmup run — populates the iCache and primes the BTB. We deliberately
    // don't time it: first-call costs (page faults on mem_, CPU ramp, branch
    // predictor cold start) would corrupt the ns/op reading.
    vm.reset_cpu_for_reuse();
    vm.run();

    // Timed phase: N back-to-back `vm.run()` calls, each preceded by the
    // fast CPU-only reset (no 1 MiB memset). Memory still holds the loaded
    // program + live data, so the dispatch loop sees a stable instruction
    // stream across iterations.
    const auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < n_iters; ++i) {
        vm.reset_cpu_for_reuse();
        vm.run();
    }
    const auto t1 = std::chrono::steady_clock::now();

    const double total_ns =
        std::chrono::duration<double, std::nano>(t1 - t0).count();
    const std::uint64_t total_ops = cycles_per_run * n_iters;
    const double ns_per_op =
        total_ns / static_cast<double>(total_ops);
    const double ops_per_sec =
        static_cast<double>(total_ops) / (total_ns * 1e-9);

    std::printf("retrovm: benchmark synthetic workload\n");
    std::printf("  program         : %zu instructions, %u B\n",
                program.size(), static_cast<unsigned>(program.size() * 4u));
    std::printf("  inner loop      : %u iterations  ->  %llu cycles/run\n",
                kBenchmarkLoopCount,
                static_cast<unsigned long long>(cycles_per_run));
    std::printf("  opcode coverage : LI ADD SUB MUL DIV LOAD STORE JNZ HALT "
                "(no IN/RAND -> pure dispatch)\n");
    std::printf("  warmup          : 1 untimed run\n");
    std::printf("  runs            : %zu iterations\n", n_iters);
    std::printf("  total ops       : %llu retired instructions\n",
                static_cast<unsigned long long>(total_ops));
    std::printf("  total time      : %.3f ms\n", total_ns / 1e6);
    std::printf("  ns / op         : %.2f ns/instruction\n", ns_per_op);
    std::printf("  instructions/s  : %.2f MIPS (millions per second)\n",
                ops_per_sec / 1e6);

    return 0;
}

// ----- save (Phase 4): replay up to a target trace frame, dump state -----
//
// `cmd_save` runs the replay driver against the .trace log, but stops the
// VM at the cycle of frame[at_frame] (i.e. just before that IN/RAND is
// re-injected). When the dispatch loop's snapshot sink fires on the
// matching cycle we serialise all 8 regs + pc + sp + flags + frame_index
// to <state_path>. The sink also calls request_halt(HALT_CHECKPOINT) so
// run() returns cleanly instead of consuming frame[at_frame].
//
// Resuming with `cmd_restore` reloads .state, initialises the VM at
// frame_index, and continues replay without restarting from PC=0.
// Memory content is implicitly correct because the trace re-drives the
// exact same STORE sequence the original run produced.

int cmd_save(const std::string& bin,
             const std::string& trace_path,
             const std::string& state_path,
             std::size_t at_frame) {
    const auto bytes = read_file(bin);
    const TraceReader reader(trace_path);

    // Look up the target cycle BEFORE running so the sink can compare.
    const TraceFrame* target = reader.frame_at(at_frame);
    if (!target) {
        std::fprintf(stderr,
            "retrovm save: --at-frame=%zu out of range "
            "(trace has %zu frames in '%s')\n",
            at_frame, reader.frame_count(), trace_path.c_str());
        return 1;
    }
    const std::uint64_t target_cycle = target->cycle;

    VM vm;
    if (!vm.load_program(bytes.data(), bytes.size())) {
        std::fprintf(stderr,
            "retrovm save: program too large for 1 MiB memory\n");
        return 1;
    }

    // Fire the snapshot sink on every cycle (snap_interval=1). The sink
    // compares both cycle_count AND the replay cursor; only when both
    // match do we write the .state file and request HALT_CHECKPOINT.
    vm.set_snapshot_interval(1);

    auto cursor = std::make_shared<std::size_t>(0);
    auto saved  = std::make_shared<bool>(false);

    vm.set_snapshot_sink(
        [&vm, cursor, saved, state_path, at_frame, target_cycle]
        (const Snapshot& /*s*/) {
            if (*saved) return;
            // Only halt on the exact cycle of frame[at_frame], before the
            // IN/RAND handler consumes it. cursor == at_frame means we
            // have already consumed at_frame frames but not yet frame
            // at_frame.
            if (*cursor != at_frame) return;
            if (vm.state().cycle_count != target_cycle) return;

            const auto& vs = vm.state();
            CheckpointState cstate{};
            cstate.frame_index = at_frame;
            cstate.halted      = HALT_CHECKPOINT;
            cstate.flags       = vs.flags;
            for (int i = 0; i < 8; ++i) cstate.regs[i] = vs.regs[i];
            // pc at sink time is post-TAIL's `state_.pc += 4u`, so it points
            // PAST the just-fetched instruction. Save pc-4 so the first
            // TAIL_DISPATCH on restore re-fetches the same instruction
            // (and the cycle_count = trace.next_frame.cycle - 1 restoration
            // makes the cycle increment land on the target cycle).
            cstate.pc          = vs.pc - 4u;
            cstate.sp          = vs.sp;

            if (!checkpoint_save(state_path, cstate)) {
                // checkpoint_save already printed a diagnostic.
                *saved = true;   // give up — don't keep retrying per cycle
                return;
            }

            // Tightened: drop the `%-2zu` / `%-4llu` field-width padding.
            // The save sink fires once per .state write so there is no
            // column to align to, and the literal "  " separators were
            // stacking with the padding to produce 3-5 space gaps that
            // varied whenever at_frame crossed 10. printf's natural field
            // width plus single-space separators reads cleanly on every
            // line length.
            std::printf("  [save] at-frame=%zu  cycle=%llu  pc=0x%08x  -> %s\n",
                        at_frame,
                        static_cast<unsigned long long>(vs.cycle_count),
                        vs.pc, state_path.c_str());

            *saved = true;
            vm.request_halt(HALT_CHECKPOINT);
        });

    // Replay driver — same shape as cmd_replay.
    auto consume = [&reader, cursor](std::uint64_t cycle, std::uint8_t want_op)
        -> std::uint32_t {
        const std::size_t idx = *cursor;
        const TraceFrame* f = reader.frame_at(idx);
        if (!f) {
            throw TraceMismatch(cycle, want_op, /*actual_cycle*/ 0,
                                /*actual_op*/ 0, idx, /*premature_eof*/ true);
        }
        if (f->cycle != cycle || f->opcode != want_op) {
            throw TraceMismatch(cycle, want_op, f->cycle, f->opcode, idx, false);
        }
        const std::uint32_t value = f->value;
        ++(*cursor);
        return value;
    };

    retrovm::IOHooks hooks;
    hooks.in   = [consume](std::uint64_t cycle) { return consume(cycle, OP_IN);   };
    hooks.rand = [consume](std::uint64_t cycle) { return consume(cycle, OP_RAND); };
    hooks.on_nondeterministic = [](std::uint64_t cycle, std::uint8_t op,
                                   std::uint32_t value) {
        std::printf("  [trace] cycle=%-4llu opcode=0x%02X value=%u\n",
                    static_cast<unsigned long long>(cycle),
                    static_cast<unsigned>(op), value);
    };
    vm.set_io_hooks(std::move(hooks));

    std::printf(
        "retrovm: save '%s' (%zu bytes) at-frame=%zu (target cycle=%llu) -> %s\n",
        bin.c_str(), bytes.size(), at_frame,
        static_cast<unsigned long long>(target_cycle),
        state_path.c_str());

    HaltReason r;
    try {
        r = vm.run();
    } catch (const TraceMismatch& div) {
        std::fprintf(stderr,
            "*** save aborted: trace mismatch ***\n"
            "  expected at cycle=%llu  opcode=0x%02X\n"
            "  trace  frame#%-4zu    opcode=0x%02X  cycle=%llu\n",
            static_cast<unsigned long long>(div.expected_cycle()),
            static_cast<unsigned>(div.expected_op()),
            div.frame_idx(),
            static_cast<unsigned>(div.actual_op()),
            static_cast<unsigned long long>(div.actual_cycle()));
        return 3;
    }

    if (r != HALT_CHECKPOINT) {
        std::fprintf(stderr,
            "retrovm save: VM halted with reason %u before reaching target "
            "(consumed %zu of %zu frames)\n",
            static_cast<unsigned>(r), *cursor, reader.frame_count());
        return 1;
    }
    // We must have consumed exactly `at_frame` frames.
    if (*cursor != at_frame) {
        std::fprintf(stderr,
            "retrovm save: cursor mismatch (got %zu, want %zu)\n",
            *cursor, at_frame);
        return 1;
    }

    std::printf("  [save] replay exhausted %zu frame(s); VM halted at "
                "frame_index=%zu\n", *cursor, at_frame);
    dump_state(vm, r);
    return 0;
}

// ----- restore (Phase 4): rewind VM to a saved .state, continue replay -----
//
// Loads <state_path>, rewinds the replay cursor to the saved
// frame_index, restores the full VMState (regs/pc/sp/flags), and
// resumes dispatch. cycle_count is recovered from `trace.frame[frame_index].cycle
// - 1` so the next TAIL_DISPATCH's increment lands on the exact cycle
// the trace expects to see at the resumption point.
//
// cycle_count is NOT serialised in the .state file (the spec only
// asks for 8 regs, pc, sp, flags, frame_index); the trace is the
// single source of truth for it.

int cmd_restore(const std::string& bin,
                const std::string& trace_path,
                const std::string& state_path) {
    const auto bytes = read_file(bin);
    const TraceReader reader(trace_path);

    CheckpointState cstate{};
    if (!checkpoint_load(state_path, cstate)) {
        // checkpoint_load already printed a diagnostic.
        return 1;
    }

    // Validate: saved frame_index must still be reachable in the trace.
    // We need at least one frame for the next IN/RAND; if frame_index is
    // already at the end of the trace, restore is pointless.
    const TraceFrame* next_frame = reader.frame_at(cstate.frame_index);
    if (!next_frame) {
        std::fprintf(stderr,
            "retrovm restore: state.frame_index=%u out of range "
            "(trace has %zu frames in '%s')\n",
            static_cast<unsigned>(cstate.frame_index),
            reader.frame_count(), trace_path.c_str());
        return 1;
    }
    if (next_frame->cycle == 0u) {
        // IN/RAND are never fetched at cycle 0 (TAIL increments to 1
        // before dispatch). Belts-and-braces guard.
        std::fprintf(stderr,
            "retrovm restore: trace.frame[%u].cycle is 0 (corrupt?)\n",
            static_cast<unsigned>(cstate.frame_index));
        return 1;
    }

    VM vm;
    if (!vm.load_program(bytes.data(), bytes.size())) {
        std::fprintf(stderr,
            "retrovm restore: program too large for 1 MiB memory\n");
        return 1;
    }

    // Rebuild a VMState from the on-disk CheckpointState. cycle_count is
    // the one field NOT on disk; derive it so the next TAIL's increment
    // lands on next_frame->cycle.
    VMState restored{};
    restored.cycle_count   = next_frame->cycle - 1u;
    for (int i = 0; i < 8; ++i) restored.regs[i] = cstate.regs[i];
    restored.pc            = cstate.pc;
    restored.sp            = cstate.sp;
    restored.flags         = cstate.flags;
    restored.halted        = HALT_RUNNING;   // always resume
    restored.snap_interval = 0;              // checkpointing off at restore
    restored.snap_timer    = 0;
    vm.set_state(restored);

    auto cursor = std::make_shared<std::size_t>(cstate.frame_index);

    auto consume = [&reader, cursor](std::uint64_t cycle, std::uint8_t want_op)
        -> std::uint32_t {
        const std::size_t idx = *cursor;
        const TraceFrame* f = reader.frame_at(idx);
        if (!f) {
            throw TraceMismatch(cycle, want_op, 0, 0, idx, true);
        }
        if (f->cycle != cycle || f->opcode != want_op) {
            throw TraceMismatch(cycle, want_op, f->cycle, f->opcode, idx, false);
        }
        const std::uint32_t value = f->value;
        ++(*cursor);
        return value;
    };

    retrovm::IOHooks hooks;
    hooks.in   = [consume](std::uint64_t cycle) { return consume(cycle, OP_IN);   };
    hooks.rand = [consume](std::uint64_t cycle) { return consume(cycle, OP_RAND); };
    hooks.on_nondeterministic = [](std::uint64_t cycle, std::uint8_t op,
                                   std::uint32_t value) {
        std::printf("  [trace] cycle=%-4llu opcode=0x%02X value=%u\n",
                    static_cast<unsigned long long>(cycle),
                    static_cast<unsigned>(op), value);
    };
    vm.set_io_hooks(std::move(hooks));

    std::printf(
        "retrovm: restore '%s' (%zu bytes) frame_index=%u -> resumed from cycle=%llu\n",
        bin.c_str(), bytes.size(),
        static_cast<unsigned>(cstate.frame_index),
        static_cast<unsigned long long>(restored.cycle_count + 1u));
    std::printf(
        "  state           : %s (loaded flag=%s halted=%u)\n",
        state_path.c_str(),
        cstate.halted == 1 ? "savenormal" :
        cstate.halted == 6 ? "savecheckpoint" : "other",
        static_cast<unsigned>(cstate.halted));
    std::printf(
        "  restored regs   : R0=%u R1=%u R2=%u R3=%u R4=%u R5=%u R6=%u R7=%u\n",
        cstate.regs[0], cstate.regs[1], cstate.regs[2], cstate.regs[3],
        cstate.regs[4], cstate.regs[5], cstate.regs[6], cstate.regs[7]);
    std::printf(
        "  restored pc/sp  : 0x%08x / 0x%08x\n", cstate.pc, cstate.sp);

    HaltReason r;
    try {
        r = vm.run();
    } catch (const TraceMismatch& div) {
        std::fprintf(stderr,
            "\n*** Divergence Detected during restore! ***\n"
            "  expected at cycle=%llu  opcode=0x%02X\n",
            static_cast<unsigned long long>(div.expected_cycle()),
            static_cast<unsigned>(div.expected_op()));
        if (!div.premature_eof()) {
            std::fprintf(stderr,
                "  trace  frame#%-4zu    opcode=0x%02X  cycle=%llu\n",
                div.frame_idx(),
                static_cast<unsigned>(div.actual_op()),
                static_cast<unsigned long long>(div.actual_cycle()));
        }
        std::fprintf(stderr,
            "restore aborted: %llu frame(s) consumed of %zu\n",
            static_cast<unsigned long long>(*cursor), reader.frame_count());
        dump_state(vm, r);
        return 3;
    }

    const std::size_t consumed = *cursor;
    const std::size_t total    = reader.frame_count();
    if (consumed < total) {
        std::fprintf(stderr,
            "\n*** Divergence Detected during restore! ***\n"
            "  %zu unread frame(s) remain in trace (resumed program ended early)\n",
            total - consumed);
        dump_state(vm, r);
        return 3;
    }

    std::printf("restore OK (%zu frame(s) replayed from frame_index=%u)\n",
                consumed - cstate.frame_index,
                static_cast<unsigned>(cstate.frame_index));
    dump_state(vm, r);
    return (r == HALT_NORMAL) ? 0 : 2;
}

// =========================================================================
//        Phase 5: cmd_debug — interactive REPL debugger
// =========================================================================
//
// A single-process CTRL-C-free REPL that drives a VM through stdin
// commands (`step`, `continue`, `regs`, `where`, `breakpoint`, `save`,
// `quit`, `help`). The REPL deliberately reuses the engine's existing
// halt-and-resume primitives instead of growing a parallel execution
// API:
//
//   1. snap_interval is set to 1 so the snapshot sink fires after every
//      instruction. The sink is the SINGLE place where breakpoints and
//      step-count checks live — no duplicate bookkeeping on the VM.
//   2. The sink calls vm.request_halt(HALT_BREAKPOINT) when the pc
//      about to execute matches a user-set breakpoint, or
//      vm.request_halt(HALT_STEP) when cycle_count reaches the step
//      target. TAIL_DISPATCH already honors mid-run halts via the hook
//      in vm.cpp:124-167, so the engine itself never has to know we're
//      debugging.
//   3. Resuming after a halt rewinds pc by 4 and cycle_count by 1 so the
//      first TAIL_DISPATCH on resume re-fetches / re-cycles the exact
//      instruction we stopped at. This mirrors cmd_save's pc-4u save
//      semantics so a REPL `save` produces a checkpoint that an
//      out-of-band `restore` can replay identically.
//
// Two I/O modes are supported:
//
//   (a) REPLAY mode: `--trace <path>` replaces IOHooks with trace-driven
//       consume() closures exactly like cmd_replay. Cycle/opcode mismatch
//       throws TraceMismatch, which the REPL catches, prints, and exits.
//       This is the mode ctest runs because it is deterministic.
//   (b) FRESH mode:  no `--trace` flag. Default IN reads stdin, default
//       RAND uses a thread-local mt19937 seeded from random_device.
//       Useful for human-driven exploration.
//
// `--batch <script>` reads commands from a file (one per line) instead
// of stdin; output goes to stdout. ctest scripts write a small script
// file, invoke the REPL once, and assert against the captured output.

// ----- disassembler (used by `where`) -----------------------------------
//
// Reads the 32-bit LE instruction at `pc` from VM memory, decodes the
// (op, dst, src, imm) tuple via opcodes.hpp's constexpr helpers, and
// prints a one-line asm-style summary. Mirrors the syntax produced by
// `retrovm asm` so a maintainer can compare REPL `where` output against
// the original .asm source.

void disassemble_one(const VM& vm, uint32_t pc) {
    const uint32_t addr = pc & VM::MEM_MASK;
    const uint32_t b0 = vm.memory()[addr];
    const uint32_t b1 = vm.memory()[(addr + 1u) & VM::MEM_MASK];
    const uint32_t b2 = vm.memory()[(addr + 2u) & VM::MEM_MASK];
    const uint32_t b3 = vm.memory()[(addr + 3u) & VM::MEM_MASK];
    const uint32_t instr = b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);

    const uint32_t op  = decode_op(instr);
    const uint32_t dst = decode_dst(instr);
    const uint32_t src = decode_src(instr);
    const uint32_t imm = decode_imm(instr);

    switch (op) {
        case OP_HALT:  std::printf("HALT");                                            break;
        case OP_LOAD:  std::printf("LOAD  R%u, [0x%x]", dst, imm);                      break;
        case OP_STORE: std::printf("STORE R%u, [0x%x]", dst, imm);                     break;
        case OP_ADD:   std::printf("ADD   R%u, R%u", dst, src);                         break;
        case OP_SUB:   std::printf("SUB   R%u, R%u", dst, src);                         break;
        case OP_MUL:   std::printf("MUL   R%u, R%u", dst, src);                         break;
        case OP_DIV:   std::printf("DIV   R%u, R%u", dst, src);                         break;
        case OP_JUMP:  std::printf("JUMP  0x%x", imm);                                 break;
        case OP_JNZ:   std::printf("JNZ   R%u, 0x%x", dst, imm);                        break;
        case OP_IN:    std::printf("IN    R%u", dst);                                  break;
        case OP_RAND:  std::printf("RAND  R%u", dst);                                  break;
        case OP_LI:    std::printf("LI    R%u, 0x%x", dst, imm);                       break;
        default:       std::printf("???  opcode 0x%02x (unrecognized)", op);           break;
    }
}

// -------------------------------------------------------------------------
// ReplSnapshot: cmd_debug-local augmentation of the engine's Snapshot.
// Captures the engine Snapshot at sink-fire time, plus any REPL-private
// state we want frozen alongside it (currently just the replay cursor
// — going back N cycles must also rewind N trace frames so re-running
// IN/RAND through the rewound range consumes the same frames in order).
// Defined as a private type so the engine API doesn't grow a "debugger"
// concern.
// -------------------------------------------------------------------------

struct ReplSnapshot {
    Snapshot       core;
    std::size_t    cursor;
};

// -------------------------------------------------------------------------
// CmdDebug: per-REPL state. One struct per cmd_debug invocation; lifetime
// is the duration of the REPL session. All members are read inside the
// snapshot sink lambda, so the REPL owns its state — neither the VM nor
// any engine-internal data structure needs to grow a "debugger pointer"
// field.
// -------------------------------------------------------------------------

struct CmdDebug {
    VM*                       vm        = nullptr;
    TraceReader*              reader    = nullptr;        // replay-mode only
    std::shared_ptr<std::size_t> cursor;                  // replay-mode frame index
    bool                      replay_mode = false;
    std::string               trace_path;                 // printable in the banner (replay mode)

    // step N : target_cycle = state_.cycle_count + N, advance when sink sees >= target
    // continue : target_cycle = UINT64_MAX (effectively no step boundary)
    // step-hit/breakpoint hit via the sink: clear target_cycle back to UINT64_MAX
    // so the next 'continue' (after a manual step) doesn't immediately re-halt.
    std::uint64_t             target_cycle = UINT64_MAX;

    // Breakpoint set: the value is the PC ADDRESS ABOUT TO EXECUTE (i.e.
    // pre-TAIL-advancement). At sink-firing time the snapshot's pc field
    // is post-TAIL advanced by 4, so the sink checks `bps.count(s.pc - 4u)`.
    std::set<std::uint32_t>   breakpoints;

    // Phase 5b: in-memory snapshot ring backing `back N`. Each sink fire
    // pushes one ReplSnapshot onto the ring; eviction drops from the front
    // when size exceeds kRingCap. `history_index` always points at the
    // snapshot capturing the currently-applied VM state. `back N`
    // decrements it and rewinds memory + cursor from the target snapshot's
    // MemDeltas. Cap is sized for typical interactive use: a single IN/RAND
    // burst plus a handful of STOREs per snapshot means ~50 B/snapshot,
    // so 256 slots cost ~13 KB total — cheap even at maximum engagement.
    std::deque<ReplSnapshot>  ring;
    std::size_t               history_index = 0;
    static constexpr std::size_t kRingCap = 256u;
};

// =========================================================================
// parse_numeric_arg: REPL-side numeric literal that accepts both decimal
// and (0x / 0X prefixed) hexadecimal. Handlers like `step N` and
// `breakpoint <addr>` route through this so the source-of-truth for "is
// this string a number, and what is it?" lives in one place.
//
// Returns false on empty input, on a stray `0x`/`0X` prefix with no
// digits, or on a parse error / stray trailing characters. Set
// `out_value` only on success.
// =========================================================================
static bool parse_numeric_arg(const std::string& tok,
                              unsigned long long& out_value) noexcept {
    if (tok.empty()) return false;
    const bool hex_prefix = (tok.size() >= 2
                             && tok[0] == '0'
                             && (tok[1] == 'x' || tok[1] == 'X'));
    const int base = hex_prefix ? 16 : 10;
    const char* digits = tok.c_str() + (hex_prefix ? 2 : 0);
    if (*digits == '\0') return false;            // "0x" alone is malformed
    char* end = nullptr;
    const unsigned long long v = std::strtoull(digits, &end, base);
    if (end == digits || *end != '\0') return false;
    out_value = v;
    return true;
}

// =========================================================================
// debug_resume: clear a soft halt OR refuse a natural halt.
//
// No rewind. State on resume matches the engine's own TAIL semantics —
// the halt fires AFTER pc/cycle have already advanced past the halted-at
// instruction (vm.cpp:124-167), so state.pc is already the address of
// the NEXT instruction to dispatch, not the halted-at one. Resuming
// without rewinding:
//
//   * makes `step 1` from cycle K cleanly retire one more cycle (K->K+1),
//   * makes `continue` from a BP-hit proceed past the halted-at addr, so
//     pc never returns to the just-fired BP and the user doesn't need a
//     skip-once bookkeeping mechanism to prevent double-firing.
//
// The engine has already executed past those "halted-at-but-not-yet-
// dispatched" instructions in the sense that they're loaded into the
// dispatch buffer and will be picked up on the next TAIL if the user
// doesn't intervene. Step/continue handle this correctly because the
// state_.pc IS already pointing at the successor.
//
// Refusing natural halts (HALT_NORMAL, HALT_DIV_ZERO, HALT_TRACE_EOF,
// HALT_DIVERGED) prevents silently re-fetching at random memory after a
// finished program — the engine would happily TAIL-fetch at state.pc+4
// post-HALT without refusing.
// =========================================================================
enum class ResumeResult { Resumed, Refused };

static ResumeResult debug_resume(CmdDebug& d) {
    VMState s = d.vm->state();
    if (s.halted == HALT_RUNNING) return ResumeResult::Resumed;
    switch (s.halted) {
        case HALT_BREAKPOINT:
        case HALT_STEP:
        case HALT_CHECKPOINT:
            s.halted = HALT_RUNNING;
            d.vm->set_state(s);
            return ResumeResult::Resumed;
        default:
            std::fprintf(stderr,
                "debug: cannot resume halt reason %u "
                "(only soft halts resume; reload the program to retry)\n",
                static_cast<unsigned>(s.halted));
            return ResumeResult::Refused;
    }
}

// Sink installed by cmd_debug. Runs once per cycle (snap_interval=1).
// Order of checks:  step boundary first (cheapest, integer compare), then
// breakpoint set lookup. Either or both may request a soft halt via
// vm.request_halt(...). After the checks (whether or not a halt fired)
// the snapshot is pushed onto the in-memory ring so `back N` can rewind
// past step / breakpoint / running states uniformly.
static void install_debug_sink(CmdDebug& d) {
    d.vm->set_snapshot_interval(1);
    d.vm->set_snapshot_sink([&d](const Snapshot& s) {
        // 1. step-boundary check.
        if (d.target_cycle != UINT64_MAX
            && s.cycle_count >= d.target_cycle) {
            d.target_cycle = UINT64_MAX;
            const uint32_t halt_pc = s.pc - 4u;
            d.vm->request_halt(HALT_STEP);
            std::printf(
                "stepped to cycle=%llu pc=0x%08x\n",
                static_cast<unsigned long long>(s.cycle_count),
                halt_pc);
        }
        // 2. breakpoint check. Mutually exclusive with step-boundary
        // (else-branch) so a single cycle doesn't double-halt.
        else {
            const uint32_t halt_pc = s.pc - 4u;
            if (d.breakpoints.count(halt_pc)) {
                d.vm->request_halt(HALT_BREAKPOINT);
                std::printf("\n*** Breakpoint 0x%08x hit ***\n", halt_pc);
                std::printf("  cycles : %llu\n",
                            static_cast<unsigned long long>(s.cycle_count));
                std::printf("  pc     : 0x%08x\n", halt_pc);
            }
        }
        // 3. ALWAYS push onto the history ring (even on soft halt) so
        // `back` can rewind past halts too. resize() first drops any
        // "future" snapshots the user has already rewound past — keeps
        // the ring coherent across forward-then-back-then-forward usage.
        d.ring.resize(d.history_index + 1);
        ReplSnapshot rs;
        rs.core   = s;
        rs.cursor = d.replay_mode ? *d.cursor : 0u;
        d.ring.push_back(std::move(rs));
        if (d.ring.size() > CmdDebug::kRingCap) {
            d.ring.pop_front();
            // history_index unchanged: still points at ring.back() == size-1.
        } else {
            ++d.history_index;
        }
    });
}

// Format the "current instruction at pc" line. The post-rewind view (`pc
// - 4u` when halted, `pc` when running) is the "instruction that will
// retire next" semantic, which is what a debugger user expects to see
// when they type `where`.
static void cmd_debug_where(const CmdDebug& d) {
    const VMState& s = d.vm->state();
    const uint32_t visible_pc = (s.halted == HALT_RUNNING)
                                 ? s.pc
                                 : (s.pc - 4u);
    std::printf("where\n  pc  : 0x%08x\n  dec : ", visible_pc);
    disassemble_one(*d.vm, visible_pc);
    std::printf("\n");
}

// `regs` reuses the dump_state column layout verbatim so test scripts
// that already parse the post-halt dump (record_replay.sh, save_restore.sh,
// inspect.sh, inspect_diff.sh) can grep the same lines out of `regs`
// output without learning a second column schema.
static void cmd_debug_regs(const CmdDebug& d, HaltReason reason) {
    dump_state(*d.vm, reason);
}

// Column-aligned list of registered breakpoints.
static void cmd_debug_bp_list(const CmdDebug& d) {
    if (d.breakpoints.empty()) {
        std::puts("(no breakpoints set)");
        return;
    }
    std::puts("breakpoints:");
    for (const auto pc : d.breakpoints) {
        std::printf("  0x%08x\n", pc);
    }
}

// The parser is line-based; tokens are whitespace-separated. Each
// command returns 0 on success, 1 on bad input, or -1 to ask the REPL
// to exit (quit/exit). Handlers that drive the engine (step / continue)
// return after ONE vm.run() call completes; the REPL loop calls them
// again on the next iteration if a fresh command needs more work.

enum class DebugStatus { Continue, Exit };

static DebugStatus cmd_debug_run(CmdDebug& d,
                                 std::istream& in,
                                 const std::string& bin_path,
                                 bool batch_mode) {
    std::printf("=== RetroVM debug ===\n");
    std::printf("  bin        : %s\n", bin_path.c_str());
    if (d.replay_mode) {
        std::printf("  mode       : replay (trace=%s, %zu frame(s))\n",
                    d.trace_path.empty() ? "(uninitialised?)"
                                         : d.trace_path.c_str(),
                    d.reader ? d.reader->frame_count() : 0u);
    } else {
        std::printf("  mode       : fresh (stdin IN, mt19937 RAND)\n");
    }
    std::printf("  type 'help' for the command list\n");

    std::string line;
    while (true) {
        // Prompt only in interactive mode (caller passes batch_mode).
        if (!batch_mode) std::printf("(retrovm) ");
        if (!std::getline(in, line)) break;

        // Echo the command back so a ctest grepping for "(retrovm) <cmd>"
        // can confirm the script line was read. Trim trailing CR / leading
        // whitespace to tolerate CRLF on Windows-produced script files.
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        std::string trimmed = line;
        while (!trimmed.empty()
               && std::isspace(static_cast<unsigned char>(trimmed.front())))
            trimmed.erase(trimmed.begin());
        while (!trimmed.empty()
               && std::isspace(static_cast<unsigned char>(trimmed.back())))
            trimmed.pop_back();
        if (trimmed.empty() || trimmed[0] == '#') continue;

        std::printf("(retrovm) %s\n", trimmed.c_str());

        // Tokenize on whitespace.
        std::vector<std::string> tok;
        std::istringstream iss(trimmed);
        std::string word;
        while (iss >> word) tok.push_back(word);
        if (tok.empty()) continue;

        // Dispatch.
        const std::string& cmd = tok[0];
        if (cmd == "help" || cmd == "?") {
            std::puts(
                "commands:\n"
                "  step [N]           execute N cycles (default 1)\n"
                "  continue           run until halt or breakpoint\n"
                "  back [N]           rewind N cycles (default 1) via in-memory snapshot ring (depth=256)\n"
                "  regs               print regs/pc/sp/flags in dump_state layout\n"
                "  where              disassemble current instruction at pc\n"
                "  breakpoint         list all breakpoints\n"
                "  breakpoint <addr>  set a breakpoint at <addr> (hex)\n"
                "  breakpoint -d <a>  delete a breakpoint\n"
                "  save <path>        dump a .state checkpoint of the current state\n"
                "  quit | q | exit    exit the debugger\n"
                "  help | ?           print this list");
        }
        else if (cmd == "step") {
            std::uint64_t n = 1;
            if (tok.size() >= 2) {
                unsigned long long parsed = 0;
                if (!parse_numeric_arg(tok[1], parsed)) {
                    std::fprintf(stderr,
                        "debug: step expects a non-negative integer (got '%s')\n",
                        tok[1].c_str());
                    continue;
                }
                n = static_cast<std::uint64_t>(parsed);
            }
            if (n == 0) {
                std::puts("debug: step 0 is a no-op");
                continue;
            }
            // If currently halted from a bp/step, resume first.
            if (debug_resume(d) == ResumeResult::Refused) continue;
            d.target_cycle = d.vm->state().cycle_count + n;
            try {
                d.vm->run();
            } catch (const TraceMismatch& div) {
                std::fprintf(stderr,
                    "\n*** Divergence Detected! ***\n"
                    "  expected at cycle=%llu  opcode=0x%02X\n",
                    static_cast<unsigned long long>(div.expected_cycle()),
                    static_cast<unsigned>(div.expected_op()));
                if (!div.premature_eof()) {
                    std::fprintf(stderr,
                        "  trace  frame#%-4zu  opcode=0x%02X  cycle=%llu\n",
                        div.frame_idx(),
                        static_cast<unsigned>(div.actual_op()),
                        static_cast<unsigned long long>(div.actual_cycle()));
                }
                std::fputs("debug: replay trace diverged, exiting\n", stderr);
                return DebugStatus::Exit;
            }
        }
        else if (cmd == "back") {
            // Phase 5b: in-memory snapshot ring time-travel scrub.
            // Cycles BACK through the kRingCap-trailing snapshots.
            // Default N=1. Refuses if N would rewind past the head of
            // the ring (i.e. before the pre-run baseline, or before
            // the ring's oldest surviving snapshot if the buffer is
            // full and has evicted earlier entries).
            std::uint64_t n = 1;
            if (tok.size() >= 2) {
                unsigned long long parsed = 0;
                if (!parse_numeric_arg(tok[1], parsed)) {
                    std::fprintf(stderr,
                        "debug: back expects a non-negative integer (got '%s')\n",
                        tok[1].c_str());
                    continue;
                }
                n = static_cast<std::uint64_t>(parsed);
            }
            if (n == 0) {
                std::puts("debug: back 0 is a no-op");
                continue;
            }
            if (n > d.history_index) {
                std::fprintf(stderr,
                    "debug: cannot rewind %llu cycle(s); ring history only "
                    "holds %zu cycle(s) back\n",
                    static_cast<unsigned long long>(n),
                    d.history_index);
                continue;
            }
            // 1. Undo memory writes. Iterate snapshots
            //    [target_idx+1 .. history_index] DESCENDING; within each
            //    snapshot iterate deltas in reverse so consecutive writes
            //    to the same cell peel off in LIFO order. Each delta's
            //    `old_value` is the value before the first write of an
            //    RLE burst; run_count doesn't matter for the undo.
            // Direct byte writes (page<<12)|offset deliberately go through
            // VM::apply_old_value rather than a raw memory_mut() pointer
            // so the LE-endian byte order and MEM_MASK wrap live in one
            // place.
            const std::size_t target_idx = d.history_index - n;
            for (std::size_t i = d.history_index; i > target_idx; --i) {
                const auto& deltas = d.ring[i].core.deltas;
                for (auto it = deltas.rbegin(); it != deltas.rend(); ++it) {
                    d.vm->apply_old_value(*it);
                }
            }
            // 2. Restore architectural state from target snapshot.
            //    No manual pc/cycle offset: TAIL_DISPATCH (vm.cpp:124-130)
            //    runs `state_.pc += 4u` and `++state_.cycle_count`
            //    BEFORE firing `make_snapshot`, so Snapshot.pc /
            //    Snapshot.cycle_count are already post-increment and a
            //    verbatim restore places the engine's next TAIL at the
            //    exact instruction we stopped at.
            const ReplSnapshot& tgt = d.ring[target_idx];
            VMState vs = d.vm->state();
            vs.cycle_count = tgt.core.cycle_count;
            for (int i = 0; i < 8; ++i) vs.regs[i] = tgt.core.regs[i];
            vs.pc     = tgt.core.pc;
            vs.sp     = tgt.core.sp;
            vs.flags  = tgt.core.flags;
            vs.halted = tgt.core.halted;
            d.vm->set_state(vs);
            // 3. Rewind replay cursor so re-running IN/RAND through
            //    the rewound cycles consumes the same trace frames in
            //    the same order — divergence detection stays intact.
            if (d.replay_mode) {
                *d.cursor = tgt.cursor;
            }
            // 4. Update history pointer. Forward `step`/`continue` will
            //    `ring.resize(history_index+1)` before pushing the next
            //    snapshot, so any "future" elements beyond this point
            //    are silently discarded.
            d.history_index = target_idx;
            std::printf(
                "back %llu cycle(s); now at cycle=%llu pc=0x%08x halted=%u\n",
                static_cast<unsigned long long>(n),
                static_cast<unsigned long long>(vs.cycle_count),
                vs.pc,
                static_cast<unsigned>(vs.halted));
        }
        else if (cmd == "continue" || cmd == "c") {
            // If currently halted from a bp/step, resume first.
            if (debug_resume(d) == ResumeResult::Refused) continue;
            d.target_cycle = UINT64_MAX;   // step-boundary off
            try {
                d.vm->run();
            } catch (const TraceMismatch& div) {
                std::fprintf(stderr,
                    "\n*** Divergence Detected! ***\n"
                    "  expected at cycle=%llu  opcode=0x%02X\n",
                    static_cast<unsigned long long>(div.expected_cycle()),
                    static_cast<unsigned>(div.expected_op()));
                if (!div.premature_eof()) {
                    std::fprintf(stderr,
                        "  trace  frame#%-4zu  opcode=0x%02X  cycle=%llu\n",
                        div.frame_idx(),
                        static_cast<unsigned>(div.actual_op()),
                        static_cast<unsigned long long>(div.actual_cycle()));
                }
                std::fputs("debug: replay trace diverged, exiting\n", stderr);
                return DebugStatus::Exit;
            }
            std::printf("continue: vm halted with reason=%u (cycles=%llu)\n",
                        static_cast<unsigned>(d.vm->state().halted),
                        static_cast<unsigned long long>(
                            d.vm->state().cycle_count));
        }
        else if (cmd == "regs") {
            cmd_debug_regs(d, d.vm->state().halted);
        }
        else if (cmd == "where") {
            cmd_debug_where(d);
        }
        else if (cmd == "breakpoint" || cmd == "bp") {
            if (tok.size() == 1) {
                cmd_debug_bp_list(d);
            } else if (tok.size() == 2 && tok[1] == "-d") {
                std::fprintf(stderr,
                    "debug: breakpoint -d requires an address after the flag\n");
            } else if (tok.size() == 3 && tok[1] == "-d") {
                unsigned long long parsed = 0;
                if (!parse_numeric_arg(tok[2], parsed)) {
                    std::fprintf(stderr,
                        "debug: breakpoint -d expects a non-negative integer "
                        "(got '%s')\n",
                        tok[2].c_str());
                    continue;
                }
                // Mask to the 20-bit, 4-byte-aligned pc range so a BP
                // address is always comparable against fetch-time pc
                // values (the engine issues a +4 advance on every fetch).
                const std::uint32_t pc =
                    static_cast<std::uint32_t>(parsed)
                        & VM::MEM_MASK
                        & 0xFFFFFFFCu;  // 4-byte align
                const auto removed = d.breakpoints.erase(pc);
                if (removed) {
                    std::printf("breakpoint removed: 0x%08x\n", pc);
                } else {
                    std::printf("breakpoint not present: 0x%08x\n", pc);
                }
            } else if (tok.size() == 2) {
                unsigned long long parsed = 0;
                if (!parse_numeric_arg(tok[1], parsed)) {
                    std::fprintf(stderr,
                        "debug: breakpoint expects a non-negative integer "
                        "(got '%s')\n",
                        tok[1].c_str());
                    continue;
                }
                const std::uint32_t pc =
                    static_cast<std::uint32_t>(parsed)
                        & VM::MEM_MASK
                        & 0xFFFFFFFCu;  // 4-byte align
                d.breakpoints.insert(pc);
                std::printf("breakpoint set: 0x%08x\n", pc);
            } else {
                std::fputs("debug: too many args for breakpoint\n", stderr);
            }
        }
        else if (cmd == "save") {
            if (tok.size() != 2) {
                std::fputs("debug: save requires a path argument\n", stderr);
                continue;
            }
            const std::string state_path = tok[1];
            // If the VM is currently running (target_cycle != UINT64_MAX
            // or halted == RUNNING), rewind so the saved state represents
            // the halted-at instruction as its next-to-execute load.
            const auto& vs = d.vm->state();
            CheckpointState cstate{};
            cstate.frame_index = d.replay_mode ? *d.cursor : 0u;
            cstate.halted      = vs.halted;
            cstate.flags       = vs.flags;
            for (int i = 0; i < 8; ++i) cstate.regs[i] = vs.regs[i];
            // pc-on-disk semantic matches cmd_save: rewind by 4 so the
            // first TAIL on restore re-fetches the halted-at instr.
            const std::uint32_t save_pc =
                (vs.halted == HALT_RUNNING) ? vs.pc : (vs.pc - 4u);
            cstate.pc = save_pc;
            cstate.sp = vs.sp;
            if (!checkpoint_save(state_path, cstate)) {
                std::fprintf(stderr, "debug: save: failed to write %s\n",
                             state_path.c_str());
                continue;
            }
            std::printf("save: wrote %s (frame_index=%u, pc=0x%08x)\n",
                        state_path.c_str(),
                        static_cast<unsigned>(cstate.frame_index),
                        save_pc);
        }
        else if (cmd == "quit" || cmd == "exit" || cmd == "q") {
            std::puts("debug: goodbye");
            return DebugStatus::Exit;
        }
        else {
            std::fprintf(stderr, "debug: unknown command '%s' (type 'help')\n",
                         cmd.c_str());
        }
    }
    return DebugStatus::Continue;
}

// Top-level entry: parse CLI args, build a CmdDebug, attach IOHooks in
// playback or fresh mode, install the snapshot sink, and run the REPL.
int cmd_debug(const std::string& bin_path,
              const std::string& trace_path_or_empty,
              const std::string& batch_path_or_empty) {
    const auto bytes = read_file(bin_path);
    VM vm;
    if (!vm.load_program(bytes.data(), bytes.size())) {
        std::fprintf(stderr, "retrovm debug: program too large for 1 MiB memory\n");
        return 1;
    }

    CmdDebug d;
    d.vm = &vm;

    if (!trace_path_or_empty.empty()) {
        // Replay mode: trace-driven IOHooks, just like cmd_replay.
        d.reader = new TraceReader(trace_path_or_empty);
        d.cursor = std::make_shared<std::size_t>(0);
        d.replay_mode = true;
        d.trace_path = trace_path_or_empty;

        auto consume = [rdr = d.reader, cur = d.cursor]
            (std::uint64_t cycle, std::uint8_t want_op) -> std::uint32_t {
            const std::size_t idx = *cur;
            const TraceFrame* f = rdr->frame_at(idx);
            if (!f) {
                throw TraceMismatch(cycle, want_op, 0, 0, idx, true);
            }
            if (f->cycle != cycle || f->opcode != want_op) {
                throw TraceMismatch(cycle, want_op, f->cycle, f->opcode, idx, false);
            }
            const std::uint32_t value = f->value;
            ++(*cur);
            return value;
        };

        retrovm::IOHooks hooks;
        hooks.in   = [consume](std::uint64_t cycle) { return consume(cycle, OP_IN);   };
        hooks.rand = [consume](std::uint64_t cycle) { return consume(cycle, OP_RAND); };
        hooks.on_nondeterministic = [](std::uint64_t cycle, std::uint8_t op,
                                       std::uint32_t value) {
            std::printf("  [trace] cycle=%-4llu opcode=0x%02X value=%u\n",
                        static_cast<unsigned long long>(cycle),
                        static_cast<unsigned>(op), value);
        };
        vm.set_io_hooks(std::move(hooks));
    } else {
        // Fresh mode: stock IN-from-stdin + thread-local mt19937. Same
        // defaults as the VM ctor; cmd_debug does NOT override them.
        d.replay_mode = false;
    }

    install_debug_sink(d);

    // Phase 5b: seed the history ring with the post-load baseline (cycle=0,
    // default regs, cursor=0) so `back` can return to the very start of the
    // run. The baseline pushes BEFORE any vm.run() so on the first sink
    // fire (post-cycle-1) it becomes ring[1] with history_index=1; `back 1`
    // from there lands at ring[0] = baseline = pristine pre-run state.
    {
        ReplSnapshot baseline;
        baseline.core   = vm.take_snapshot();
        baseline.cursor = d.replay_mode ? *d.cursor : 0u;
        d.ring.push_back(std::move(baseline));
        d.history_index = 0;
    }

    std::unique_ptr<std::ifstream> batch_in;
    std::istream* in_ptr = &std::cin;
    bool batch_mode = false;
    if (!batch_path_or_empty.empty()) {
        batch_in = std::make_unique<std::ifstream>(batch_path_or_empty);
        if (!*batch_in) {
            std::fprintf(stderr, "retrovm debug: cannot open batch script '%s'\n",
                         batch_path_or_empty.c_str());
            delete d.reader;
            return 1;
        }
        in_ptr = batch_in.get();
        batch_mode = true;
    }

    const DebugStatus rc = cmd_debug_run(d, *in_ptr, bin_path,
                                          batch_mode);

    if (d.reader) delete d.reader;
    return (rc == DebugStatus::Exit) ? 0 : 0;  // exit-on-EOF also returns 0
}

// =========================================================================
//        inspect (Phase 4+): offline reader for a saved .state file
// =========================================================================
//
// Reads a `.state` checkpoint from disk and prints its contents via the
// same column layout that `dump_state()` already uses for runtime halt
// dumps (`halt reason`, `pc / sp`, `flags (ZCS)`, `regs`). One extra
// `frame_index` line is added at the top because the on-disk body has
// the trace cursor position but `dump_state()` (describing a finished
// run) does not.
//
// `cycles` is intentionally omitted — Phase 4 deliberately omits
// `cycle_count` from the .state body (the trace is its single source of
// truth), so this command cannot quote a number that wasn't on disk.
//
// `inspect` does NOT need the corresponding `.bin` or `.trace` files —
// those describe the program and nondeterministic events, not the saved
// registers. That property is what makes this useful for post-mortem
// debugging, diffing two checkpoints, and as a smoke test for the
// 64-byte wire format itself.
//
// Column layout reuse is deliberate: every field printed here has the
// exact same label and the same awk-extractable column as its
// dump_state() counterpart, so test scripts parse both files with the
// same awk '$N' indices.

int cmd_inspect(const std::string& state_path) {
    CheckpointState cstate{};
    if (!checkpoint_load(state_path, cstate)) {
        // checkpoint_load already printed the diagnostic.
        return 1;
    }
    std::puts("=== RetroVM checkpoint inspect ===");
    std::printf("file        : %s\n", state_path.c_str());
    // frame_index: only field with no dump_state() counterpart. The trace
    // cursor at save time; restore replays frames[frame_index..end].
    std::printf("frame_index : %u\n",
                static_cast<unsigned>(cstate.frame_index));
    // halt reason: split into two lines so callers can extract the value
    // with a label-anchored grep without picking up the trailing legend
    // as a false-positive column. Mirrors dump_state() exactly so the
    // same helper. The on-disk value is always HALT_CHECKPOINT (6)
    // under cmd_save's current hardcoded policy; future variants could
    // encode a real reason and the row will continue to parse cleanly.
    std::printf("halt reason: %u\n", static_cast<unsigned>(cstate.halted));
    std::printf("halt_legend: (1=normal 2=div0 3=unk 4=trace_eof 5=diverge 6=checkpoint)\n");
    std::printf("pc / sp     : 0x%08x / 0x%08x\n", cstate.pc, cstate.sp);
    std::printf("flags (ZCS): %c%c%c\n",
                cstate.flags & 0x1 ? 'Z' : '-',
                cstate.flags & 0x2 ? 'C' : '-',
                cstate.flags & 0x4 ? 'S' : '-');
    std::printf("regs        : R0=%u R1=%u R2=%u R3=%u R4=%u R5=%u R6=%u R7=%u\n",
                cstate.regs[0], cstate.regs[1], cstate.regs[2], cstate.regs[3],
                cstate.regs[4], cstate.regs[5], cstate.regs[6], cstate.regs[7]);
    return 0;
}

// =========================================================================
//        inspect --diff (Phase 4+): side-by-side per-field delta of two
//        checkpoints with equal columns suppressed
// =========================================================================
//
// Useful for eyeballing how a VM diverged between two save points on
// the same program: feed `retrovm save --at-frame=A` and
// `retrovm save --at-frame=B` into inspect --diff and only the columns
// that actually changed are printed. Equal fields collapse silently,
// which is what makes a 64-byte state file with only 2-3 differing
// fields actually long enough to fit on one screen.
//
// Phase 4 cmd_save's invariant (cstate.halted hardcoded to
// HALT_CHECKPOINT=6) means the `halt reason` line is always equal
// across any two saved checkpoints and therefore always suppressed.
// That is intentional: a future variant of cmd_save that records the
// real HaltReason at save time will start showing deltas automatically.
//
// `frame_index` differs whenever the two states were saved at
// different `--at-frame=N` values, so it's the most commonly-printed
// delta and is the row that tells you "you are comparing two snapshots
// from different points in the run" without parsing the file paths.

int cmd_inspect_diff(const std::string& a_path, const std::string& b_path) {
    CheckpointState a{}, b{};
    if (!checkpoint_load(a_path, a)) return 1;
    if (!checkpoint_load(b_path, b)) return 1;

    std::puts("=== RetroVM checkpoint diff ===");
    std::printf("state a   : %s\n", a_path.c_str());
    std::printf("state b   : %s\n", b_path.c_str());

    int n_diffs = 0;

    // frame_index (uint32): trace cursor at save time. Always included
    // if the two states were saved at different --at-frame values.
    if (a.frame_index != b.frame_index) {
        std::printf("frame_index : a=%u b=%u\n",
                    static_cast<unsigned>(a.frame_index),
                    static_cast<unsigned>(b.frame_index));
        ++n_diffs;
    }

    // halted (HaltReason): cmd_save's current hardcoded policy is
    // HALT_CHECKPOINT=6 for every save, so this row is almost always
    // suppressed. Surfacing it when the policy changes is automatic.
    if (a.halted != b.halted) {
        std::printf("halt reason: a=%u b=%u\n",
                    static_cast<unsigned>(a.halted),
                    static_cast<unsigned>(b.halted));
        ++n_diffs;
    }

    // pc (uint32): post-TAIL pc - 4u. Of all the state words this is
    // the one that moves the earliest (a one-cycle off-by-one in cmd_save
    // would surface here).
    if (a.pc != b.pc) {
        std::printf("pc          : a=0x%08x b=0x%08x\n", a.pc, b.pc);
        ++n_diffs;
    }

    // sp (uint32): stack pointer. The retrovm bytecode doesn't use
    // PUSH/POP so sp is constant across an entire run today, but the
    // field is on disk and a future change that uses the stack will
    // start producing deltas without any inspect --diff changes.
    if (a.sp != b.sp) {
        std::printf("sp          : a=0x%08x b=0x%08x\n", a.sp, b.sp);
        ++n_diffs;
    }

    // flags (ZCS bitmask): three chars, one per bit. Inlined to keep
    // the printf format symmetric with cmd_inspect's `flags (ZCS): %c%c%c`.
    if (a.flags != b.flags) {
        const char a_zcs[4] = {
            char(a.flags & 0x1u ? 'Z' : '-'),
            char(a.flags & 0x2u ? 'C' : '-'),
            char(a.flags & 0x4u ? 'S' : '-'),
            '\0'
        };
        const char b_zcs[4] = {
            char(b.flags & 0x1u ? 'Z' : '-'),
            char(b.flags & 0x2u ? 'C' : '-'),
            char(b.flags & 0x4u ? 'S' : '-'),
            '\0'
        };
        std::printf("flags (ZCS) : a=%s b=%s\n", a_zcs, b_zcs);
        ++n_diffs;
    }

    // regs[8]: per-register skip-on-equal. The label width picks the
    // longest register name in use (`frame_index` ≈ 11 chars) so all
    // rows align in a wide terminal without ragged columns.
    for (int i = 0; i < 8; ++i) {
        if (a.regs[i] != b.regs[i]) {
            std::printf("R%d         : a=%u b=%u\n", i,
                        static_cast<unsigned>(a.regs[i]),
                        static_cast<unsigned>(b.regs[i]));
            ++n_diffs;
        }
    }

    if (n_diffs == 0) {
        std::puts("(no differing fields)");
    }
    return 0;
}

// =========================================================================
//        inspect-trace (Phase 6): offline reader for a saved .trace file
// =========================================================================
//
// Reads a `<bin>.trace` file from disk and prints its contents using
// the same one-line-per-field label layout that `cmd_inspect` (Phase 4+)
// and `dump_state()` share. No VM is loaded, no replay session is
// started, and the corresponding `<bin>` is NOT required — the .trace
// is self-describing per trace.hpp's wire format.
//
// What's printed:
//   * file path, on-disk size (16 B header + 16 B * frame_count)
//   * header fields: magic (8-byte ASCII literal), version, reserved
//   * aggregate frame stats: count, first/last cycle, cycle delta
//   * opcode breakdown: counts of OP_IN (0x09) and OP_RAND (0x0A)
//     across all frames. trace.hpp:13-15 documents that
//     `on_nondeterministic` only logs these two opcodes, so a non-zero
//     "op_other" would indicate either a future trace format change or
//     wire corruption.
//
// Validation: `TraceReader`'s ctor already throws std::runtime_error
// on open / mmap / parse error (bad magic, wrong version, truncated
// file). The try/catch below translates that into a clean stderr
// diagnostic and a non-zero exit so a ctest failure surfaces the
// underlying parse error verbatim instead of an uncaught exception.

int cmd_inspect_trace(const std::string& trace_path) {
    std::unique_ptr<TraceReader> reader;
    try {
        reader = std::make_unique<TraceReader>(trace_path);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "retrovm trace: cannot open '%s': %s\n",
                     trace_path.c_str(), e.what());
        return 1;
    }
    if (!reader->valid()) {
        std::fprintf(stderr,
            "retrovm trace: '%s' opened but reader reports invalid\n",
            trace_path.c_str());
        return 1;
    }

    std::printf("=== RetroVM trace inspect ===\n");
    std::printf("file        : %s\n", trace_path.c_str());
    std::printf("size_bytes  : %zu\n", reader->size_bytes());

    // Magic is 8 bytes on disk; print as a quoted ASCII literal. The
    // on-disk wire format is "RVMTRACE\0" (7 chars + null padding),
    // so we walk the bytes until the first null and quote-print the
    // prefix. A future trace variant whose magic has a different
    // trailing-byte convention will still print correctly here
    // because the loop short-circuits on the first null.
    std::string magic_str;
    magic_str.reserve(8);
    for (std::size_t i = 0; i < 8; ++i) {
        const char c = reader->magic().data()[i];
        if (c == '\0') break;
        magic_str.push_back(c);
    }
    std::printf("magic       : \"%.8s\"\n", magic_str.c_str());
    std::printf("version     : %u\n", reader->version());

    // Aggregate frame stats in a single pass over the mmap'd frames.
    // Page-fault cost on first scan is bounded by 16 B * frame_count
    // bytes (programs/io.bin with 3 frames = 48 B, well under cold-L1).
    const std::size_t n_frames = reader->frame_count();
    std::uint64_t first_cycle = 0;
    std::uint64_t last_cycle  = 0;
    std::size_t   op_in_count   = 0;
    std::size_t   op_rand_count = 0;
    std::size_t   op_other_count = 0;
    for (std::size_t i = 0; i < n_frames; ++i) {
        const TraceFrame* f = reader->frame_at(i);
        if (!f) break;
        if (i == 0) first_cycle = f->cycle;
        last_cycle = f->cycle;
        if      (f->opcode == OP_IN)   ++op_in_count;
        else if (f->opcode == OP_RAND) ++op_rand_count;
        else                            ++op_other_count;
    }

    std::printf("frame_count : %zu\n", n_frames);
    if (n_frames > 0) {
        std::printf("cycle_first : %llu\n",
                    static_cast<unsigned long long>(first_cycle));
        std::printf("cycle_last  : %llu\n",
                    static_cast<unsigned long long>(last_cycle));
        std::printf("cycle_delta : %llu\n",
                    static_cast<unsigned long long>(last_cycle - first_cycle));
    } else {
        // Empty trace is valid (snap.bin's deterministic-only run
        // produces no IN/RAND frames). Print clearly non-numeric
        // placeholders rather than 0, so a test that parses the cycle
        // fields as integers sees an obvious "(none)" mismatch if it
        // accidentally treats an empty trace as having cycle data.
        std::printf("cycle_first : (none)\n");
        std::printf("cycle_last  : (none)\n");
        std::printf("cycle_delta : 0\n");
    }

    std::printf("op_IN       : %zu\n", op_in_count);
    std::printf("op_RAND     : %zu\n", op_rand_count);
    if (op_other_count > 0) {
        // Only printed when non-zero so a "normal" trace output stays
        // terse. A non-zero count is a wire-format anomaly worth
        // surfacing rather than silently absorbing into IN's bucket.
        std::printf("op_other    : %zu\n", op_other_count);
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 1) {
        run_builtin_demo();
        return 0;
    }
    const std::string mode = argv[1];
    if (mode == "run") {
        if (argc < 3) { std::fprintf(stderr, "retrovm run: missing <bin>\n"); return 1; }
        return cmd_run(argv[2]);
    }
    if (mode == "asm" || mode == "assemble") {
        if (argc < 4) { std::fprintf(stderr, "retrovm asm: missing <src> <bin>\n"); return 1; }
        return cmd_assemble(argv[2], argv[3]);
    }
    if (mode == "record") {
        if (argc < 3) { std::fprintf(stderr, "retrovm record: missing <bin>\n"); return 1; }
        // Optional --seed=N sets the mt19937 seed so the same stdin +
        // program + seed always produces a byte-identical .trace. Default
        // is kRecordRandSeed (0xC0FFEE) which is non-zero on every host.
        std::uint32_t seed = kRecordRandSeed;
        for (int i = 3; i < argc; ++i) {
            const std::string arg = argv[i];
            constexpr const char* kPrefix = "--seed=";
            if (arg.rfind(kPrefix, 0) != 0) {
                std::fprintf(stderr,
                    "retrovm record: unknown arg '%s' (only --seed=N is accepted)\n",
                    argv[i]);
                return 1;
            }
            const char* digits = arg.c_str() + 7;  // skip "--seed="
            if (*digits == '\0') {
                std::fprintf(stderr, "retrovm record: --seed= requires a value\n");
                return 1;
            }
            char* end = nullptr;
            const unsigned long long parsed = std::strtoull(digits, &end, 10);
            if (end == digits || *end != '\0' || parsed > 0xFFFFFFFFull) {
                std::fprintf(stderr,
                    "retrovm record: invalid --seed '%s' (expected 0..4294967295)\n",
                    argv[i]);
                return 1;
            }
            seed = static_cast<std::uint32_t>(parsed);
        }
        return cmd_record(argv[2], seed);
    }
    if (mode == "replay") {
        if (argc < 4) { std::fprintf(stderr, "retrovm replay: missing <bin> <trace>\n"); return 1; }
        return cmd_replay(argv[2], argv[3]);
    }
    if (mode == "--benchmark" || mode == "benchmark") {
        std::size_t n = 1000;
        if (argc >= 3) {
            char* end = nullptr;
            const unsigned long long parsed = std::strtoull(argv[2], &end, 10);
            if (end == argv[2] || *end != '\0' || parsed == 0u || parsed > 100000000u) {
                std::fprintf(stderr,
                    "retrovm benchmark: invalid N '%s' (expected 1..100000000)\n",
                    argv[2]);
                return 1;
            }
            n = static_cast<std::size_t>(parsed);
        }
        return cmd_benchmark(n);
    }
    if (mode == "snapshot" || mode == "snap") {
        if (argc < 3) {
            std::fprintf(stderr, "retrovm snapshot: missing <bin>\n");
            return 1;
        }
        std::uint32_t interval = 10u;  // default
        if (argc >= 4) {
            char* end = nullptr;
            const unsigned long long parsed = std::strtoull(argv[3], &end, 10);
            if (end == argv[3] || *end != '\0' || parsed == 0u || parsed > 10000000u) {
                std::fprintf(stderr,
                    "retrovm snapshot: invalid interval '%s' (expected 1..10000000)\n",
                    argv[3]);
                return 1;
            }
            interval = static_cast<std::uint32_t>(parsed);
        }
        return cmd_snapshot(argv[2], interval);
    }
    if (mode == "save") {
        // retrovm save <bin> <bin.trace> <state-file> [--at-frame=N]
        // Replays the program against the trace and stops the VM at the
        // cycle of frame[N], serialising 8 regs + pc + sp + flags +
        // frame_index to <state-file>. N defaults to 1 (after the first
        // frame is consumed) so a no-arg save still produces a usable
        // middle-of-program checkpoint.
        if (argc < 5) {
            std::fprintf(stderr,
                "retrovm save: missing <bin> <trace> <state> [--at-frame=N]\n");
            return 1;
        }
        std::size_t at_frame = 1u;
        for (int i = 5; i < argc; ++i) {
            const std::string arg = argv[i];
            constexpr const char* kPrefix = "--at-frame=";
            if (arg.rfind(kPrefix, 0) != 0) {
                std::fprintf(stderr,
                    "retrovm save: unknown arg '%s' (only --at-frame=N is accepted)\n",
                    argv[i]);
                return 1;
            }
            const char* digits = arg.c_str() + 11;  // skip "--at-frame="
            if (*digits == '\0') {
                std::fprintf(stderr,
                    "retrovm save: --at-frame= requires a value\n");
                return 1;
            }
            char* end = nullptr;
            const unsigned long long parsed = std::strtoull(digits, &end, 10);
            if (end == digits || *end != '\0') {
                std::fprintf(stderr,
                    "retrovm save: invalid --at-frame '%s' (expected 0..)\n",
                    argv[i]);
                return 1;
            }
            at_frame = static_cast<std::size_t>(parsed);
        }
        return cmd_save(argv[2], argv[3], argv[4], at_frame);
    }
    if (mode == "restore") {
        // retrovm restore <bin> <bin.trace> <state-file>
        // Loads .state, rewinds the replay cursor to the saved
        // frame_index, restores 8 regs + pc + sp + flags, and
        // continues the replay from there.
        if (argc < 5) {
            std::fprintf(stderr,
                "retrovm restore: missing <bin> <trace> <state>\n");
            return 1;
        }
        return cmd_restore(argv[2], argv[3], argv[4]);
    }
    if (mode == "debug") {
        // retrovm debug <bin> [--trace <trace>] [--batch <script>]
        //
        // Interactive REPL debugger. Modes:
        //   (no --trace)   fresh: stdin for IN, thread-local mt19937 for RAND
        //   --trace <path> replay: trace-driven consume() closures, throws
        //                  on cycle/opcode mismatch (caught -> REPL exits)
        //   --batch <path> read commands from a file rather than stdin,
        //                  echos each command line and prints output to
        //                  stdout (used by `tests/debug.sh`)
        if (argc < 3) {
            std::fprintf(stderr,
                "retrovm debug: missing <bin> [<--trace>] [<--batch>]\n");
            return 1;
        }
        std::string trace_arg;       // empty = fresh mode
        std::string batch_arg;       // empty = interactive mode
        for (int i = 3; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--trace") {
                if (i + 1 >= argc) {
                    std::fprintf(stderr,
                        "retrovm debug: --trace requires a path\n");
                    return 1;
                }
                trace_arg = argv[++i];
            } else if (arg == "--batch") {
                if (i + 1 >= argc) {
                    std::fprintf(stderr,
                        "retrovm debug: --batch requires a script path\n");
                    return 1;
                }
                batch_arg = argv[++i];
            } else {
                std::fprintf(stderr,
                    "retrovm debug: unknown arg '%s' "
                    "(only --trace and --batch are accepted)\n",
                    argv[i]);
                return 1;
            }
        }
        return cmd_debug(argv[2], trace_arg, batch_arg);
    }
    if (mode == "trace") {
        // retrovm trace <bin.trace>
        //
        // Phase 6 offline reader for a saved .trace file. Loads NO VM,
        // starts NO replay session, and does NOT require the
        // corresponding `<bin>`. Mirrors `retrovm inspect <state>`
        // (Phase 4+): same one-line-per-field label layout so the
        // `get_field` helper in tests/_lib.sh works unchanged. Tests/
        // inspect_trace.sh asserts the banner, magic, version, frame
        // count, and opcode breakdown against a real recorded trace.
        if (argc < 3) {
            std::fprintf(stderr, "retrovm trace: missing <trace>\n");
            return 1;
        }
        return cmd_inspect_trace(argv[2]);
    }
    if (mode == "inspect") {
        // retrovm inspect <state-file>
        // OR
        // retrovm inspect --diff <state-a> <state-b>
        //
        // Single-state form is an offline reader: parses a 64-byte
        // .state checkpoint and prints 8 regs + pc + sp + flags + halt
        // reason + frame_index via the dump_state() column layout. No
        // VM load, no replay session, no need for the .bin or .trace
        // files. Designed for post-mortem analysis and as a wire-format
        // smoke test.
        //
        // --diff form is a side-by-side per-field delta of two states
        // with equal columns suppressed — useful for eyeballing how
        // two checkpoints of the same program differ at different
        // points in the run.
        if (argc >= 3 && std::string(argv[2]) == "--diff") {
            if (argc < 5) {
                std::fprintf(stderr,
                    "retrovm inspect --diff: missing two <state> arguments "
                    "(got %d of 2)\n", argc - 3);
                return 1;
            }
            return cmd_inspect_diff(argv[3], argv[4]);
        }
        if (argc < 3) {
            std::fprintf(stderr,
                "retrovm inspect: missing <state>\n");
            return 1;
        }
        return cmd_inspect(argv[2]);
    }
    std::fprintf(stderr,
        "Usage:\n"
        "  retrovm                                 run built-in demo\n"
        "  retrovm run      <program.bin>          execute a binary\n"
        "  retrovm asm      <src.asm> <bin>        assemble text -> binary\n"
        "  retrovm record   <bin> [--seed=N]       run + capture .trace (deterministic)\n"
        "  retrovm replay   <bin> <log.trace>      replay with divergence check\n"
        "  retrovm snapshot <bin> [interval]       dump delta-snapshots to <bin>.snap.NNNN\n"
        "  retrovm save     <bin> <trace> <state>  snapshot mid-program regs/pc/sp/flags+frame_index\n"
        "           [--at-frame=N]                 (default N=1, halt just before frame N)\n"
        "  retrovm restore  <bin> <trace> <state>  resume replay from a saved .state\n"
        "  retrovm inspect  <state>               print a saved .state (offline, no VM / trace)\n"
        "  retrovm inspect  --diff <a> <b>        side-by-side per-field delta, equal columns suppressed\n"
        "  retrovm trace    <trace>               print a saved .trace (offline, no VM / bin)\n"
        "  retrovm debug    <bin> [--trace <t>] [--batch <script>]\n"
        "                                       interactive REPL: step/continue/regs/where/breakpoint/save\n"
        "  retrovm --benchmark [N]                 measure dispatch throughput (default N=1000)\n");
    return 1;
}
