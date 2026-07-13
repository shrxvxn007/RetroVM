// RetroVM — Phase 1 opcode definitions & instruction codec.
//
// 32-bit fixed-width instruction layout (little-endian on the wire):
//   bits[31:26] : 6-bit opcode
//   bits[25:23] : 3-bit destination register (R0..R7)
//   bits[22:20] : 3-bit source register A / operand
//   bits[19: 0] : 20-bit immediate / address
//
// Decoding is `constexpr`, so callers can fold it into the dispatch hot loop.

#pragma once

#include <cstdint>

namespace retrovm {

// Opcode IDs are intentionally dense so the dispatch table is small &
// cache-friendly. 0x00 = HALT, then memory, then ALU, then control, then I/O.
enum Opcode : uint8_t {
    OP_HALT  = 0x00,
    OP_LOAD  = 0x01,
    OP_STORE = 0x02,
    OP_ADD   = 0x03,
    OP_SUB   = 0x04,
    OP_MUL   = 0x05,
    OP_DIV   = 0x06,
    OP_JUMP  = 0x07,
    OP_JNZ   = 0x08,
    OP_IN    = 0x09,
    OP_RAND  = 0x0A,
    OP_LI    = 0x0B,        // load-immediate (no memory traffic)
    OP_COUNT = 0x0C,        // sentinel used to size the dispatch table
};

// Compile-time builders / extractors.
constexpr uint32_t encode(uint32_t op,
                          uint32_t dst,
                          uint32_t src,
                          uint32_t imm) noexcept {
    return ((op  & 0x3Fu) << 26)
         | ((dst & 0x07u) << 23)
         | ((src & 0x07u) << 20)
         |  (imm & 0xFFFFFu);
}
constexpr uint32_t decode_op (uint32_t i) noexcept { return (i >> 26) & 0x3Fu; }
constexpr uint32_t decode_dst(uint32_t i) noexcept { return (i >> 23) & 0x07u; }
constexpr uint32_t decode_src(uint32_t i) noexcept { return (i >> 20) & 0x07u; }
constexpr uint32_t decode_imm(uint32_t i) noexcept { return  i        & 0xFFFFFu; }

// Reason the VM stopped. Stored in `state_.halted`. 0 means still running.
enum HaltReason : uint32_t {
    HALT_RUNNING      = 0,
    HALT_NORMAL       = 1,
    HALT_DIV_ZERO     = 2,
    HALT_UNKNOWN_OP   = 3,
    HALT_TRACE_EOF    = 4, // --replay ran past the end of the .trace
    HALT_DIVERGED     = 5, // --replay saw a cycle/opcode mismatch in a frame
    HALT_CHECKPOINT   = 6, // soft-halt: --save captured state at the target cycle
    HALT_BREAKPOINT   = 7, // soft-halt: cmd_debug breakpoint hit (sink fired before dispatching the BP pc's instruction)
    HALT_STEP         = 8, // soft-halt: cmd_debug single-/multi-step count reached
};

// Flag register bit positions.
constexpr uint32_t FLAG_ZF = 1u << 0;  // zero
constexpr uint32_t FLAG_CF = 1u << 1;  // carry / borrow / mul overflow / div remainder
constexpr uint32_t FLAG_SF = 1u << 2;  // sign (MSB set)

} // namespace retrovm
