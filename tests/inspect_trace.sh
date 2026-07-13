#!/usr/bin/env bash
# tests/inspect_trace.sh — Phase 6 offline `<bin>.trace` reader ctest.
#
# Ensures `<bin>.trace` exists (records if not), then runs
# `retrovm trace <bin.trace>` and asserts every column of the
# label-based summary matches the wire format. Mirrors tests/inspect.sh
# in shape (label-anchored extraction via `get_field` from _lib.sh,
# exit-0 iff every label resolves cleanly), but no .state file or VM
# is involved — the .trace is self-describing per trace.hpp.
#
# What's checked:
#   (a) banner matches "=== RetroVM trace inspect ==="
#   (b) `magic` line says "RVMTRACE" (8-byte ASCII literal wire magic)
#   (c) `version` is 1
#   (d) `frame_count` matches file size geometry: 16 B header + 16 B *
#       frames. A future wire-format change that pads or trims the
#       frame would surface here.
#   (e) per-opcode breakdown: `op_IN` and `op_RAND` counts match
#       programs/io.bin's known 2-IN + 1-RAND layout for stdin
#       "11\n22\n" (frames at cycles 3/4/5).
#   (f) cycle range: cycle_first=3 cycle_last=5 cycle_delta=2 (matches
#       the cycle stamps the IN/RAND events fired at during record).
#
# Args:
#   $1 = path to retrovm binary
#   $2 = path to .bin program (relative to cwd)
#   $3 = stdin text (literal, may contain \n)
#
# Exit 0 iff retrovm trace exits 0 AND every label resolves cleanly
# AND the per-program opcode cycle range matches.

set -e
. "$(dirname -- "${BASH_SOURCE[0]}")/_lib.sh"

RETROVM="$1"
BIN="$2"
STDIN="$3"

TRACE="${BIN}.trace"

# 1. Ensure trace exists. Don't clobber an existing one — earlier
#    record+save tests in this directory may have left it on disk;
#    re-using it keeps re-runs cheap.
[ -s "$TRACE" ] || printf '%s' "$STDIN" | "$RETROVM" record "$BIN" >/dev/null 2>&1

# 2. Run retrovm trace and capture output. We do NOT swallow stderr
#    so a wire-format decode failure surfaces as a clear test failure
#    instead of an uncaught std::runtime_error.
TRACE_OUT=$("$RETROVM" trace "$TRACE" 2>&1)
echo "$TRACE_OUT"
"$RETROVM" trace "$TRACE" >/dev/null 2>&1 || {
    echo "FAIL: retrovm trace exited non-zero for $TRACE" >&2
    exit 1
}

# 3. Banner invariant.
BANNER=$(printf '%s\n' "$TRACE_OUT" | head -n1)
if [ "$BANNER" != "=== RetroVM trace inspect ===" ]; then
    echo "FAIL: missing or wrong banner (got '$BANNER')" >&2
    exit 1
fi

# 4. Magic must be "RVMTRACE".
MAGIC=$(get_field 'magic       :' "$TRACE_OUT")
if [ "$MAGIC" != '"RVMTRACE"' ]; then
    echo "FAIL: magic token malformed (got '$MAGIC', want '\"RVMTRACE\"')" >&2
    exit 1
fi

# 5. Version must be 1.
VERSION=$(get_field 'version     :' "$TRACE_OUT")
if [ "$VERSION" != "1" ]; then
    echo "FAIL: version should be 1, got '$VERSION'" >&2
    exit 1
fi

# 6. frame_count must equal the actual frame count computed from
#    on-disk size geometry. trace.hpp fixes the wire format as 16 B
#    header + 16 B * frame_count; a regression that pads or trims
#    would surface as a frame_count mismatch here.
SIZE_B=$(get_field 'size_bytes  :' "$TRACE_OUT")
if [ "$SIZE_B" -lt 16 ] || [ $((SIZE_B % 16)) -ne 0 ]; then
    echo "FAIL: size_bytes=$SIZE_B is not a 16 B-multiple >= 16 (header)" >&2
    exit 1
fi
COMPUTED_FRAMES=$(( (SIZE_B - 16) / 16 ))
FRAME_COUNT=$(get_field 'frame_count :' "$TRACE_OUT")
if [ "$FRAME_COUNT" != "$COMPUTED_FRAMES" ]; then
    echo "FAIL: frame_count=$FRAME_COUNT vs computed=$COMPUTED_FRAMES disagree" >&2
    exit 1
fi

# 7. Per-opcode breakdown matches programs/io.bin's known layout for
#    stdin "11\n22\n": two IN events + one RAND event (cycles 3/4/5).
#    Adapt if BIN is a program without those events.
case "$BIN" in
    programs/io.bin)
        OP_IN=$(get_field 'op_IN       :' "$TRACE_OUT")
        OP_RAND=$(get_field 'op_RAND     :' "$TRACE_OUT")
        if [ "$OP_IN" != "2" ] || [ "$OP_RAND" != "1" ]; then
            echo "FAIL: io.bin opcode breakdown must be op_IN=2 op_RAND=1 (got op_IN=$OP_IN op_RAND=$OP_RAND)" >&2
            exit 1
        fi
        CYCLE_FIRST=$(get_field 'cycle_first :' "$TRACE_OUT")
        CYCLE_LAST=$(get_field 'cycle_last  :' "$TRACE_OUT")
        CYCLE_DELTA=$(get_field 'cycle_delta :' "$TRACE_OUT")
        if [ "$CYCLE_FIRST" != "3" ] || [ "$CYCLE_LAST" != "5" ] || [ "$CYCLE_DELTA" != "2" ]; then
            echo "FAIL: io.bin cycle range (got ${CYCLE_FIRST}..${CYCLE_LAST} delta=${CYCLE_DELTA}; want 3..5 delta=2)" >&2
            exit 1
        fi
        ;;
esac

echo "OK: $BIN trace size=${SIZE_B}B frame_count=$FRAME_COUNT op_IN=${OP_IN:-0} op_RAND=${OP_RAND:-0} cycle ${CYCLE_FIRST:-?}..${CYCLE_LAST:-?} delta=${CYCLE_DELTA:-?}"
exit 0
