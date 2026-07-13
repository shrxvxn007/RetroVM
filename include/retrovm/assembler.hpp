// Forward declarations so main.cpp can call into assembler.cpp without
// duplicating the prototype.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace retrovm {

// Translate a .asm source file into a sequence of 32-bit instructions,
// and write them little-endian to `dst_path`. Returns 0 on success.
// `out_instrs` is also populated for callers that want the in-memory
// copy (e.g. tests, in-process loading).
int assemble_file(const std::string& src_path,
                  const std::string& dst_path,
                  std::vector<uint32_t>& out_instrs);

} // namespace retrovm
