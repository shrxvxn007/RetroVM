// RetroVM — Phase 3 trace format + reader/writer.
//
// File format (little-endian byte order):
//
//   TraceHeader (16 B):
//     char     magic[8];      // "RVMTRACE\0"
//     uint32_t version;       // currently 1
//     uint32_t reserved;      // 0; future flags
//
//   followed by `frame_count` consecutive TraceFrame structs (16 B each):
//     uint64_t cycle;     // state_.cycle_count at the IN/RAND event
//     uint8_t  opcode;    // OP_IN (0x09) or OP_RAND (0x0A)
//     uint8_t  _pad0[3];  // pad to 12 so value is naturally aligned
//     uint32_t value;     // 32-bit payload (consumed by replay)
//
// File size = 16 + frame_count * 16.

#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace retrovm {

// One recorded non-deterministic event — exactly 16 B so a mmap'd array
// is plain pointer arithmetic with no per-element math.
struct TraceFrame {
    std::uint64_t cycle;     // bytes 0..7
    std::uint8_t  opcode;    // byte 8
    std::uint8_t  _pad0[3];  // bytes 9..11 (round up for the uint32)
    std::uint32_t value;     // bytes 12..15
};
static_assert(sizeof(TraceFrame) == 16, "TraceFrame must remain 16 bytes");

struct TraceHeader {
    char          magic[8];   // bytes 0..7  ("RVMTRACE\0")
    std::uint32_t version;    // bytes 8..11 (currently 1)
    std::uint32_t reserved;   // bytes 12..15 (0)
};
static_assert(sizeof(TraceHeader) == 16, "TraceHeader must remain 16 bytes");

// Thrown when replay encounters a frame whose cycle/opcode does not match
// what the VM is asking for, or when the trace ends before the program
// is done consuming events. The CLI catches this and prints the formatted
// 'Divergence Detected!' panic.
class TraceMismatch : public std::runtime_error {
public:
    TraceMismatch(std::uint64_t expected_cycle, std::uint8_t expected_op,
                  std::uint64_t actual_cycle, std::uint8_t actual_op,
                  std::size_t frame_idx, bool premature_eof);

    std::uint64_t expected_cycle() const noexcept { return expected_cycle_; }
    std::uint8_t  expected_op()    const noexcept { return expected_op_;   }
    std::uint64_t actual_cycle()   const noexcept { return actual_cycle_;  }
    std::uint8_t  actual_op()      const noexcept { return actual_op_;     }
    std::size_t   frame_idx()      const noexcept { return frame_idx_;     }
    bool          premature_eof()  const noexcept { return premature_eof_; }

private:
    std::uint64_t expected_cycle_;
    std::uint8_t  expected_op_;
    std::uint64_t actual_cycle_;
    std::uint8_t  actual_op_;
    std::size_t   frame_idx_;
    bool          premature_eof_;
};

// Memory-mapped reader. Throws std::runtime_error on open/mmap/parse error.
class TraceReader {
public:
    explicit TraceReader(const std::string& path);
    ~TraceReader();
    TraceReader(const TraceReader&)            = delete;
    TraceReader& operator=(const TraceReader&) = delete;

    bool          valid()       const noexcept { return map_ != nullptr; }
    std::size_t   frame_count() const noexcept { return frame_count_; }
    std::uint32_t version()     const noexcept { return version_; }
    // 8-byte on-disk magic. Returned as `std::string_view` (not
    // `const char*`) so the type system enforces a bounded view: a
    // caller can't accidentally pass it to `printf("%s", reader.magic())`
    // and read past the 8-byte buffer into whatever follows `magic_` in
    // memory. That call would not compile because `std::string_view` has
    // no implicit conversion to `const char*`. The intended usage is
    // either `std::printf("%.8s", reader.magic().data())` (explicit
    // bounded format) or `std::cout << reader.magic();` (stream
    // operator).
    std::string_view magic() const noexcept {
        return std::string_view(magic_, sizeof(magic_));
    }
    std::size_t   size_bytes()  const noexcept { return size_; }

    // Direct indexed access into the mmap'd frames. nullptr if out of range.
    const TraceFrame* frame_at(std::size_t i) const noexcept;

private:
    void*          map_         = nullptr;
    std::size_t    size_        = 0;
    std::size_t    frame_count_ = 0;
    std::uint32_t  version_     = 0;
    char           magic_[8]    = {};
};

// In-memory trace accumulator. After the run completes, flush_to_file()
// writes the complete `.trace` (header + frames) to disk.
class TraceWriter {
public:
    TraceWriter()  = default;
    ~TraceWriter() = default;

    void append(std::uint64_t cycle, std::uint8_t opcode, std::uint32_t value) noexcept;
    std::size_t frame_count() const noexcept { return frames_.size(); }
    void clear() noexcept { frames_.clear(); }

    // Throws std::runtime_error on I/O error.
    void flush_to_file(const std::string& path) const;

private:
    std::vector<TraceFrame> frames_;
};

} // namespace retrovm
