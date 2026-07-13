// RetroVM — Phase 2/3 trace reader (mmap-backed) + writer.
//
// Both ends are now mmap-based on POSIX so a record/replay round trip
// touches disk exactly once per side:
//   * Reader opens O_RDONLY + MAP_SHARED over the existing on-disk trace
//     and reads frames via pointer arithmetic — no stdio churn.
//   * Writer opens O_RDWR + O_CREAT + O_TRUNC, ftruncates to the exact
//     final size, mmaps the region PROT_WRITE + MAP_SHARED, memcpy's the
//     out the header + frame ring, msync's (MS_SYNC) and munmaps. No
//     stdio buffer is ever built, so a SIGKILL just before msync loses at
//     most the dirty bytes from the last page (handful per million frames).
//
// On Windows we fall back to a heap vector + fwrite — same on-disk
// layout, no stdio-per-write overhead but no zero-copy either.

#include "retrovm/trace.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

#if !defined(_WIN32)
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace retrovm {

// ---------- little-endian byte helpers ----------
//
// Only the u32 LE helpers are actually called by TraceReader / TraceWriter
// for the on-disk TraceHeader (version + reserved). The u64 LE helpers were
// originally there for an alternate TraceFrame layout that used a u32 cycle
// field — that layout shipped as u64 cycle instead (so the frame reader
// does an in-place `std::memcpy` of the TriviallyCopyable frame struct, no
// explicit per-field shift loop). Dead code; do not reintroduce without
// making sure something actually calls it.

static inline void write_u32_le(uint8_t* p, uint32_t v) noexcept {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>((v >> (i * 8)) & 0xFFu);
}
static inline uint32_t read_u32_le(const uint8_t* p) noexcept {
    uint32_t r = 0;
    for (int i = 3; i >= 0; --i) r = (r << 8) | p[i];
    return r;
}

// ---------- TraceMismatch ----------

TraceMismatch::TraceMismatch(uint64_t expected_cycle, uint8_t expected_op,
                             uint64_t actual_cycle,   uint8_t actual_op,
                             std::size_t frame_idx,   bool premature_eof)
    : std::runtime_error(
        premature_eof
            ? (std::string("trace exhausted at cycle ") + std::to_string(expected_cycle)
               + ": program expected opcode " + std::to_string(expected_op)
               + " but trace has only " + std::to_string(frame_idx) + " frame(s)")
            : (std::string("trace mismatch at cycle ") + std::to_string(expected_cycle)
               + ": expected opcode " + std::to_string(expected_op)
               + " but trace frame #" + std::to_string(frame_idx)
               + " has opcode " + std::to_string(actual_op)
               + " (cycle " + std::to_string(actual_cycle) + ")")),
      expected_cycle_(expected_cycle), expected_op_(expected_op),
      actual_cycle_(actual_cycle),     actual_op_(actual_op),
      frame_idx_(frame_idx),           premature_eof_(premature_eof) {}

// ---------- TraceReader ----------

TraceReader::TraceReader(const std::string& path) {
#if !defined(_WIN32)
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
        throw std::runtime_error("cannot open trace file: " + path);

    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        throw std::runtime_error("fstat failed for: " + path);
    }
    if (st.st_size <= 0) {
        ::close(fd);
        throw std::runtime_error("trace file is empty: " + path);
    }
    size_ = static_cast<std::size_t>(st.st_size);

    void* m = ::mmap(nullptr, size_, PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);
    if (m == MAP_FAILED || m == nullptr) {
        throw std::runtime_error("mmap failed for: " + path);
    }
    map_ = m;
#else
    // Windows fallback: heap buffer (no mmap). Same semantics for callers.
    std::ifstream is(path, std::ios::binary);
    if (!is) throw std::runtime_error("cannot open trace file: " + path);
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(is)),
                              std::istreambuf_iterator<char>());
    if (buf.empty())
        throw std::runtime_error("trace file is empty: " + path);
    size_ = buf.size();
    auto* hold = new uint8_t[size_];
    std::memcpy(hold, buf.data(), size_);
    map_ = hold;
#endif

    // Validate header.
    if (size_ < sizeof(TraceHeader))
        throw std::runtime_error("trace file too small (< header): " + path);
    // "RVMTRACE" is 8 bytes — there is no implicit null terminator on disk
    // because the magic field is exactly char[8]. Comparing 9 bytes would
    // trip on the version's LSB. Strict 8-byte compare.
    if (std::memcmp(static_cast<const char*>(map_), "RVMTRACE", 8) != 0)
        throw std::runtime_error("trace file has bad magic: " + path);

    std::memcpy(magic_, static_cast<const char*>(map_), 8);
    // NOTE: magic_ is exactly 8 bytes with no on-disk null terminator (the
    // struct field is char[8]). Callers using printf must use %.8s (NOT
    // %s) to avoid reading past the array. The current CLI passes %.8s.
    version_ = read_u32_le(static_cast<const uint8_t*>(map_) + 8);
    if (version_ != 1u)
        throw std::runtime_error("unsupported trace version " + std::to_string(version_));

    if (size_ == sizeof(TraceHeader)) {
        frame_count_ = 0;
    } else {
        std::size_t frames_bytes = size_ - sizeof(TraceHeader);
        if (frames_bytes % sizeof(TraceFrame) != 0)
            throw std::runtime_error("trace file: frame count is not integer");
        frame_count_ = frames_bytes / sizeof(TraceFrame);
    }
}

TraceReader::~TraceReader() {
#if !defined(_WIN32)
    if (map_) ::munmap(map_, size_);
#else
    if (map_) delete[] static_cast<uint8_t*>(map_);
#endif
}

const TraceFrame* TraceReader::frame_at(std::size_t i) const noexcept {
    if (i >= frame_count_) return nullptr;
    auto* base = static_cast<const uint8_t*>(map_);
    return reinterpret_cast<const TraceFrame*>(base + sizeof(TraceHeader) + i * sizeof(TraceFrame));
}

// ---------- TraceWriter ----------

void TraceWriter::append(uint64_t cycle, std::uint8_t opcode, std::uint32_t value) noexcept {
    TraceFrame f{};
    f.cycle  = cycle;
    f.opcode = opcode;
    f.value  = value;
    frames_.push_back(f);
}

void TraceWriter::flush_to_file(const std::string& path) const {
    // Total on-disk size = TraceHeader (16) + frames_ × TraceFrame (16).
    const std::size_t total = sizeof(TraceHeader)
                            + frames_.size() * sizeof(TraceFrame);

#if !defined(_WIN32)
    // Phase 2 zero-copy path: open → ftruncate to exact size → mmap the
    // exact region (MAP_SHARED so writes go straight to the page cache)
    // → memcpy the header + frame bytes from the in-memory accumulator
    // into the mapped region (single syscall-equivalent at msync).
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        throw std::runtime_error("cannot open trace for write: " + path);
    if (::ftruncate(fd, static_cast<off_t>(total)) != 0) {
        ::close(fd);
        throw std::runtime_error("ftruncate failed for trace: " + path);
    }
    void* m = ::mmap(nullptr, total, PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED || m == nullptr) {
        ::close(fd);
        throw std::runtime_error("mmap failed for trace: " + path);
    }

    uint8_t* p = static_cast<uint8_t*>(m);
    // Header — 8 bytes of magic ("RVMTRACE", no implicit null on disk)
    // + version (u32 LE) + reserved (u32 LE).
    std::memcpy(p, "RVMTRACE", 8);
    write_u32_le(p + 8,  1u);
    write_u32_le(p + 12, 0u);
    // Frames — bulk memcpy from the in-memory ring. TraceFrame is
    // trivially-copyable (no padding holes per static_assert), so a single
    // std::memcpy of the whole vector onto the mmap'd region is byte-exact.
    if (!frames_.empty()) {
        std::memcpy(p + sizeof(TraceHeader), frames_.data(),
                    frames_.size() * sizeof(TraceFrame));
    }
    // Force the page cache to disk before unmapping so the bytes survive
    // a SIGKILL on the writer. MS_SYNC, not MS_ASYNC, because the caller
    // expects this flush to imply durability.
    ::msync(m, total, MS_SYNC);
    ::munmap(m, total);
    ::close(fd);
#else
    // Windows fallback: stage into a heap vector (mmap-equivalent for
    // layout purposes) then fwrite the whole buffer at once. The
    // serialization is identical to the POSIX path — only the medium
    // differs.
    std::vector<uint8_t> buf(total);
    uint8_t* p = buf.data();
    std::memcpy(p, "RVMTRACE", 8);
    write_u32_le(p + 8,  1u);
    write_u32_le(p + 12, 0u);
    if (!frames_.empty()) {
        std::memcpy(p + sizeof(TraceHeader), frames_.data(),
                    frames_.size() * sizeof(TraceFrame));
    }
    std::ofstream os(path, std::ios::binary | std::ios::trunc);
    if (!os)
        throw std::runtime_error("cannot open trace for write: " + path);
    os.write(reinterpret_cast<const char*>(buf.data()),
             static_cast<std::streamsize>(total));
    if (!os)
        throw std::runtime_error("write failed for trace: " + path);
#endif
}

} // namespace retrovm
