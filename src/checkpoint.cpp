// RetroVM — Phase 4 checkpoint (.state) file I/O.
//
// `cmd_save` and `cmd_restore` exchange CheckpointState structs via
// these two file-format functions. The on-disk format is documented in
// include/retrovm/checkpoint.hpp; this file is strictly the wire
// codec — header magic + version, fixed-width writes/reads, and a sane
// stderr diagnostic on every failure path so CI failures are
// attributable.

#include "retrovm/checkpoint.hpp"

#include <cstdio>
#include <cstring>

namespace retrovm {

namespace {

// Emit a one-line diagnostic matching the format the rest of the CLI
// uses (`retrovm: <message>`), with `path` quoted. Caller controls
// exit code; this never aborts.
void diag(const char* what, const std::string& path, const char* detail) {
    std::fprintf(stderr, "retrovm: %s '%s' (%s)\n", what, path.c_str(), detail);
}

bool write_all(std::FILE* f, const void* buf, std::size_t n) {
    return std::fwrite(buf, 1, n, f) == n;
}

bool read_all(std::FILE* f, void* buf, std::size_t n) {
    return std::fread(buf, 1, n, f) == n;
}

// Build a CheckpointFileBody from an in-memory CheckpointState. Pure
// endian-fixed-width copy — no byte-swap handling, the format spec
// pins the wire to little-endian and the build is x86/ARM-LE only.
CheckpointFileBody make_body(const CheckpointState& s) {
    CheckpointFileBody b{};
    b.halted = static_cast<std::uint32_t>(s.halted);
    b.flags  = s.flags;
    for (int i = 0; i < 8; ++i) b.regs[i] = s.regs[i];
    b.pc = s.pc;
    b.sp = s.sp;
    return b;
}

} // anonymous namespace

bool checkpoint_save(const std::string& path, const CheckpointState& state) {
    // Truncate-then-write. If a previous save exists with a different
    // frame_index it gets fully replaced — append isn't supported and
    // would risk mixing old/new bodies.
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        diag("cannot open for writing", path, "fopen wb failed");
        return false;
    }

    CheckpointFileHeader hdr{};
    std::memcpy(hdr.magic, "RVMSTATE", 8);   // 8 chars, no null terminator
    hdr.version     = 1u;
    hdr.frame_index = state.frame_index;

    const CheckpointFileBody body = make_body(state);

    if (!write_all(f, &hdr,  sizeof(hdr))
        || !write_all(f, &body, sizeof(body))) {
        diag("write failed", path, "fwrite returned short");
        std::fclose(f);
        return false;
    }

    if (std::fclose(f) != 0) {
        diag("close failed", path, "fclose returned non-zero");
        return false;
    }
    return true;
}

bool checkpoint_load(const std::string& path, CheckpointState& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        diag("cannot open for reading", path, "fopen rb failed");
        return false;
    }

    CheckpointFileHeader hdr{};
    CheckpointFileBody   body{};

    if (!read_all(f, &hdr,  sizeof(hdr))
        || !read_all(f, &body, sizeof(body))) {
        diag("read failed", path, "file shorter than expected (60 B)");
        std::fclose(f);
        return false;
    }

    if (std::memcmp(hdr.magic, "RVMSTATE", 8) != 0) {
        diag("bad magic", path, "expected RVMSTATE in header");
        std::fclose(f);
        return false;
    }
    if (hdr.version != 1u) {
        // Future-proof: future schema versions go through a dispatch
        // table; v0 / unknown versions fail loudly rather than try-
        // to reinterpret.
        char buf[64];
        std::snprintf(buf, sizeof(buf), "unknown version %u (want 1)",
                      hdr.version);
        diag("unsupported .state", path, buf);
        std::fclose(f);
        return false;
    }

    std::fclose(f);

    out.frame_index = hdr.frame_index;
    out.halted      = static_cast<HaltReason>(body.halted);
    out.flags       = body.flags;
    for (int i = 0; i < 8; ++i) out.regs[i] = body.regs[i];
    out.pc          = body.pc;
    out.sp          = body.sp;
    return true;
}

} // namespace retrovm
