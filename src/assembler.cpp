// RetroVM — Phase 1 assembler.
//
// A deliberately tiny line-oriented assembler that accepts opcodes in
// upper- or lower-case, comma- or whitespace-separated operands.
//
//   LOAD  R0, 32
//   ADD   R2, R3
//   STORE R2, 0x100
//   JNZ   R3, 0x24
//   HALT
//
// Lines starting with ';' or empty lines are ignored. Each non-empty line
// produces one 32-bit instruction word, written little-endian to the
// output .bin.

#include "retrovm/assembler.hpp"
#include "retrovm/opcodes.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

namespace retrovm {

namespace {

const std::unordered_map<std::string, uint32_t> kOpcodes = {
    {"HALT",  OP_HALT },
    {"LOAD",  OP_LOAD },
    {"STORE", OP_STORE},
    {"ADD",   OP_ADD  },
    {"SUB",   OP_SUB  },
    {"MUL",   OP_MUL  },
    {"DIV",   OP_DIV  },
    {"JUMP",  OP_JUMP },
    {"JNZ",   OP_JNZ  },
    {"IN",    OP_IN   },
    {"RAND",  OP_RAND },
    {"LI",    OP_LI   },
};

// ----- helpers (defined above the caller that uses them) ------------------

int parse_reg(const std::string& tok) {
    if (tok.size() < 2) return -1;
    const char c = static_cast<char>(std::toupper(tok[0]));
    if (c != 'R') return -1;
    const int n = std::atoi(tok.c_str() + 1);
    return (n >= 0 && n <= 7) ? n : -1;
}

// strip a single trailing or leading comma or whitespace from a token.
void strip_comma(std::string& s) {
    while (!s.empty() && (s.back() == ',' || std::isspace(static_cast<unsigned char>(s.back())))) s.pop_back();
    while (!s.empty() && (s.front() == ',' || std::isspace(static_cast<unsigned char>(s.front())))) s.erase(s.begin());
}

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    return s;
}

std::string strip_comment(const std::string& line) {
    const auto pos = line.find(';');
    return (pos == std::string::npos) ? line : line.substr(0, pos);
}

// Returns ~0u on error (sentinel — caller prints).
uint32_t parse_imm(const std::string& src, int line, const std::string& tok) {
    if (tok.empty()) {
        std::fprintf(stderr, "%s:%d: missing immediate\n", src.c_str(), line);
        return ~0u;
    }
    char* end = nullptr;
    const unsigned long v = std::strtoul(tok.c_str(), &end, 0);
    if (!end || *end != '\0') {
        std::fprintf(stderr, "%s:%d: bad immediate '%s'\n", src.c_str(), line, tok.c_str());
        return ~0u;
    }
    if (v > 0xFFFFFu) {
        std::fprintf(stderr, "%s:%d: immediate 0x%lx exceeds 20-bit range\n", src.c_str(), line, v);
        return ~0u;
    }
    return static_cast<uint32_t>(v);
}

void error_reg(const std::string& src, int line, const std::string& reg) {
    std::fprintf(stderr, "%s:%d: bad register '%s'\n", src.c_str(), line, reg.c_str());
}

} // anon namespace

int assemble_file(const std::string& src_path, const std::string& dst_path,
                  std::vector<uint32_t>& out_instrs) {
    std::ifstream in(src_path);
    if (!in) { std::fprintf(stderr, "asm: cannot open %s\n", src_path.c_str()); return 1; }

    std::string raw;
    int lineno = 0;
    out_instrs.clear();

    while (std::getline(in, raw)) {
        ++lineno;
        std::string line = trim(strip_comment(raw));
        if (line.empty()) continue;

        std::istringstream iss(line);
        std::string op_s, a_s, b_s;
        iss >> op_s >> a_s >> b_s;
        strip_comma(a_s);
        strip_comma(b_s);

        for (auto& ch : op_s) ch = static_cast<char>(std::toupper(ch));
        auto it = kOpcodes.find(op_s);
        if (it == kOpcodes.end()) {
            std::fprintf(stderr, "%s:%d: unknown opcode '%s'\n", src_path.c_str(), lineno, op_s.c_str());
            return 1;
        }
        const uint32_t op = it->second;
        uint32_t dst = 0, src_reg = 0, imm = 0;

        switch (op) {
            case OP_HALT:
                // No operands.
                break;

            case OP_IN:
            case OP_RAND:
                // IN Rd       /  RAND Rd
                if (parse_reg(a_s) < 0) { error_reg(src_path, lineno, a_s); return 1; }
                dst = static_cast<uint32_t>(parse_reg(a_s));
                break;

            case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV:
                // OP Rd, Rs    (rd is modified, rs is read-only)
                if (parse_reg(a_s) < 0) { error_reg(src_path, lineno, a_s); return 1; }
                if (parse_reg(b_s) < 0) { error_reg(src_path, lineno, b_s); return 1; }
                dst     = static_cast<uint32_t>(parse_reg(a_s));
                src_reg = static_cast<uint32_t>(parse_reg(b_s));
                break;

            case OP_LOAD:
            case OP_STORE:
                // OP Rd, addr  (Rd is reg, addr is 20-bit immediate)
                if (parse_reg(a_s) < 0) { error_reg(src_path, lineno, a_s); return 1; }
                dst = static_cast<uint32_t>(parse_reg(a_s));
                imm = parse_imm(src_path, lineno, b_s); if (imm == ~0u) return 1;
                break;

            case OP_JUMP:
                imm = parse_imm(src_path, lineno, a_s); if (imm == ~0u) return 1;
                break;

            case OP_JNZ:
                // JNZ Rd, addr  -- jump to addr iff Rd != 0
                if (parse_reg(a_s) < 0) { error_reg(src_path, lineno, a_s); return 1; }
                dst = static_cast<uint32_t>(parse_reg(a_s));
                imm = parse_imm(src_path, lineno, b_s); if (imm == ~0u) return 1;
                break;

            case OP_LI:
                // LI Rd, imm20 -- load immediate, no memory traffic.
                if (parse_reg(a_s) < 0) { error_reg(src_path, lineno, a_s); return 1; }
                dst = static_cast<uint32_t>(parse_reg(a_s));
                imm = parse_imm(src_path, lineno, b_s); if (imm == ~0u) return 1;
                break;

            default:
                std::fprintf(stderr, "%s:%d: opcode not handled in Phase 1: %s\n",
                             src_path.c_str(), lineno, op_s.c_str());
                return 1;
        }

        out_instrs.push_back(encode(op, dst, src_reg, imm));
    }

    std::ofstream out(dst_path, std::ios::binary | std::ios::trunc);
    if (!out) { std::fprintf(stderr, "asm: cannot open %s\n", dst_path.c_str()); return 1; }
    for (uint32_t inst : out_instrs) {
        out.put(static_cast<char>( inst        & 0xFFu));
        out.put(static_cast<char>((inst >>  8u) & 0xFFu));
        out.put(static_cast<char>((inst >> 16u) & 0xFFu));
        out.put(static_cast<char>((inst >> 24u) & 0xFFu));
    }
    std::printf("asm: %zu instructions -> %s\n", out_instrs.size(), dst_path.c_str());
    return 0;
}

} // namespace retrovm
