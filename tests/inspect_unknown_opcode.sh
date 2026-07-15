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
# Mint + per-byte hex assertion + locale-stability guard delegate to
# `tests/_lib.sh: mint_synth_trace`. This file only declares the
# EXPECTED_BYTES array (the wire-format spec) and the call. See
# `_lib.sh`'s docstring for the bash 3.2 trailing-NUL trap, the
# `<NL>+# comment` `$(...)` parser pitfall, and the rationale for
# the anti-drift EXPECTED_HEX derivation from the same byte list.
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

EXPECTED_BYTES=(
    # bytes 0-7 : magic
    0x52 0x56 0x4d 0x54 0x52 0x41 0x43 0x45
    # bytes 8-11 : version
    0x01 0x00 0x00 0x00
    # bytes 12-15 : reserved
    0x00 0x00 0x00 0x00
    # bytes 16-23 : cycle
    0x00 0x00 0x00 0x00 0x00 0x00 0x00 0x00
    # byte 24 : opcode=0x42
    0x42
    # bytes 25-27 : _pad0
    0x00 0x00 0x00
    # bytes 28-31 : value
    0x00 0x00 0x00 0x00
)
mint_synth_trace "${#EXPECTED_BYTES[@]}" "$TMP_TRACE" "${EXPECTED_BYTES[@]}"

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

echo "OK: synthetic-fixture unknown-opcode branch verified (32-byte trace, opcode=0x42 -> 'unknown')"
exit 0
