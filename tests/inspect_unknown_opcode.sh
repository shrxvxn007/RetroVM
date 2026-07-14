#!/usr/bin/env bash
# tests/inspect_unknown_opcode.sh — synthetic-fixture ctest for the
# per-frame branch's unknown-opcode coverage.
#
# Closes the coverage gap from `tests/inspect_frame.sh`: real run traces
# only ever carry OP_IN (0x09) / OP_RAND (0x0A) frames so the printer's
# third branch (`op_name = "unknown"`) is unreachable in normal
# recordings.  This test hand-mints a 32 B trace (16 B header + 16 B
# frame, opcode=0x42) and asserts the per-frame printer formats it as
# `opcode      : 0x42 (unknown)` — the only call site of the
# "unknown" string in the per-frame branch.
#
# Wire-format reference (see include/retrovm/trace.hpp):
#   TraceHeader (16 B):
#     magic[8]    = "RVMTRACE"     (8 ASCII bytes; NO trailing NUL
#                                    within the field — reader compares
#                                    8-char literal `"RVMTRACE"` against
#                                    bytes 0..7 in `src/trace.cpp`.)
#     version[4]  = 1 (LE)
#     reserved[4] = 0
#   TraceFrame (16 B):
#     cycle[8]    = 0 (LE)
#     opcode[1]   = 0x42 (deliberately outside {0x09, 0x0A})
#     _pad0[3]    = 0, 0, 0
#     value[4]    = 0 (LE)
#
# NOTE on the mint step: bash 3.2's printf builtin emits a trailing
# C-string NUL after EACH invocation (verified live via `bash -c
# 'printf "RVMTRACE" | wc -c'` returning 9 on bash 3.2.57 — 8 ASCII
# letters + 1 trailing NUL).  A multi-call block emits extra NULs
# interleaved between content bytes that scramble the wire format,
# so this test uses a SINGLE printf call for the full 32-byte content;
# the trailing NUL is then stripped by `head -c 32` so the file is
# exactly 32 B with the byte layout below:
#       bytes 0..7 : "RVMTRACE"     (magic[8])
#       bytes 8..11: 0x01 LE        (version=1)
#       bytes 12..15: 0x00 × 4      (reserved=0)
#       bytes 16..23: 0x00 × 8      (cycle=0)
#       byte 24    : 0x42           (opcode=0x42, the unknown path)
#       bytes 25..27: 0x00 × 3      (_pad0)
#       bytes 28..31: 0x00 × 4      (value=0)
#
# Args:
#   $1 = path to retrovm binary
#
# Exit 0 iff retrovm trace --frame=0 on the synthetic trace prints
# `0x42 (unknown)` for the `opcode` row AND the banner is preserved.
# Cleanup removes the temp trace on exit regardless (via trap).

set -e
. "$(dirname -- "${BASH_SOURCE[0]}")/_lib.sh"

RETROVM="$1"
TMP_TRACE=$(mktemp -t retrovm_unknown_opcode.XXXXX.trace)
trap 'rm -f "$TMP_TRACE"' EXIT

# 1. Mint the 32 B synthetic .trace.  Format breakdown (verified by
#    hand against include/retrovm/trace.hpp:53-58):
#      RVMTRACE\x01          (8 + 1 = 9 B; magic + version LSB)
#      ×15 \x00              (15 B; version high + reserved + cycle)
#      \x42                  (1 B; opcode, the unknown-opcode path)
#      ×7 \x00               (7 B; _pad0 + value)
#    Total escapes = 24 + 8 ASCII = 32 B content; + 1 trailing NUL =
#    33 B from printf; `head -c 32` strips the trailing NUL so the
#    file is exactly 32 B.
# Format breakdown matches the wire-format reference in the file-level
# docstring above (8 ASCII + 24 escapes = 32 B content; +1 trailing
# NUL from bash 3.2 printf; `head -c 32` strips it).
printf 'RVMTRACE\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x42\x00\x00\x00\x00\x00\x00\x00' \
    | head -c 32 > "$TMP_TRACE"

# Sanity-check the geometry (16 B header + 16 B frame) before any
# per-row assertion; a truncated file would let an unrelated code
# path run silently without tripping the checks below.
ACTUAL=$(wc -c < "$TMP_TRACE" | tr -d ' ')
if [ "$ACTUAL" != "32" ]; then
    echo "FAIL: synthetic trace is $ACTUAL bytes, want 32" >&2
    exit 1
fi

OUT=$("$RETROVM" trace "$TMP_TRACE" --frame=0 2>&1)
echo "$OUT"

BANNER=$(printf '%s\n' "$OUT" | head -n 1)
if [ "$BANNER" != "=== RetroVM trace inspect ===" ]; then
    echo "FAIL: per-frame banner missing (got '$BANNER')" >&2
    exit 1
fi

OPCODE=$(get_field 'opcode      :' "$OUT")
if [ "$OPCODE" != "0x42 (unknown)" ]; then
    echo "FAIL: unknown-opcode printer expected '0x42 (unknown)', got '$OPCODE'" >&2
    exit 1
fi

# The frame_index / cycle / value rows must still parse cleanly so a
# future regression in the format string doesn't leak past the
# targeted assertion (lower bar than full byte-equality).
FRAME_IDX=$(get_field 'frame_index :' "$OUT")
CYCLE=$(get_field 'cycle       :' "$OUT")
VALUE=$(get_field 'value       :' "$OUT")
if [ "$FRAME_IDX" != "0" ] || [ "$CYCLE" != "0" ] || [ "$VALUE" != "0" ]; then
    echo "FAIL: non-target rows drifted (frame=$FRAME_IDX cycle=$CYCLE value=$VALUE; want 0/0/0)" >&2
    exit 1
fi

echo "OK: synthetic-fixture unknown-opcode branch verified ($ACTUAL-byte trace, opcode=0x42 -> 'unknown')"
exit 0
