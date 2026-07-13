// RetroVM — Phase 4 checkpoint (.state) format.
//
// A `.state` file captures enough VM state to resume a replay session
// mid-program after `retrovm save`. On restore the replay driver
// re-injects trace frames from the saved `frame_index` onwards and the
// dispatch loop picks up exactly where it left off.
//
// On-disk layout (little-endian byte order, fixed-width):
//
//   CheckpointFileHeader (16 B):
//     char     magic[8];      // "RVMSTATE" — 8 bytes, no null terminator
//     uint32_t version;       // currently 1
//     uint32_t frame_index;   // trace cursor position at save time
//
//   CheckpointFileBody (48 B):
//     uint32_t halted;        // HaltReason at save time
//     uint32_t flags;         // ZF|CF|SF bitmask at save time
//     uint32_t regs[8];       // R0..R7
//     uint32_t pc;
//     uint32_t sp;
//
// Total = 64 B (one cache line). The body deliberately omits
// `cycle_count`: it is re-derived from `frame_index` (and the recorded
// trace) at restore time, and `memory` is implicitly the .bin program
// that the user already supplied to both `save` and `restore`. 64 B
// end-to-end means a single `pread`/stream-read pulls the whole file
// into a cache line on most machines.
//
// The on-disk structs are exactly 16 / 48 / 64 B — `static_assert` below
// guards against any future field addition that would break historical
// .state files.

#pragma once

#include "retrovm/opcodes.hpp"
#include "retrovm/vm.hpp"

#include <cstdint>
#include <cstring>

namespace retrovm {

// In-memory mirror of the on-disk layout. Used as the single exchange
// type between `cmd_save` (builds it via VM inspect) and `cmd_restore`
// (consumes it via VM::set_state).
struct CheckpointState {
    std::uint32_t  frame_index;     // trace cursor (frames consumed so far)
    retrovm::HaltReason halted;    // halt tag at save time
    std::uint32_t  flags;           // ZF|CF|SF bitmask at save time
    std::uint32_t  regs[8];         // R0..R7 at save time
    std::uint32_t  pc;              // program counter at save time
    std::uint32_t  sp;              // full-descending stack top
};

struct CheckpointFileHeader {
    char          magic[8];
    std::uint32_t version;
    std::uint32_t frame_index;
};
static_assert(sizeof(CheckpointFileHeader) == 16,
              "CheckpointFileHeader must be 16 B");

struct CheckpointFileBody {
    std::uint32_t halted;
    std::uint32_t flags;
    std::uint32_t regs[8];
    std::uint32_t pc;
    std::uint32_t sp;
};
static_assert(sizeof(CheckpointFileBody) == 48,
              "CheckpointFileBody must be 48 B");

// Read a .state file from disk into `out`. Returns true on success.
// On failure (open error, short read, bad magic, wrong version) prints
// a diagnostic to stderr, sets `errno`-ish exit code 1, and returns false.
bool checkpoint_load(const std::string& path, CheckpointState& out);

// Serialise `state` to `path` in the on-disk layout above. Returns true
// on success; prints a diagnostic to stderr and returns false on
// open/write failure. The destination is overwritten (truncate-then-write).
bool checkpoint_save(const std::string& path, const CheckpointState& state);

} // namespace retrovm
