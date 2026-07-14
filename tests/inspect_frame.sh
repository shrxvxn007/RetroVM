#!/usr/bin/env bash
# tests/inspect_frame.sh — Phase 6 partial-trace inspection ctest.
#
# Ensures `<bin>.trace` exists (records if not), then exercises
# `retrovm trace <bin.trace> --frame=N` for N in {0, 1, 2, 99} and
# asserts each invocation's per-frame block against the trace wire
# format fixed in trace.hpp.  Mirrors tests/inspect_trace.sh's
# label-anchored extraction via `get_field` from _lib.sh.
#
# Expected stable shape for programs/io.bin under stdin "5\n7\n9\n"
# (caller passes $3 verbatim):
#   frame 0: cycle=3 opcode=OP_IN   value=5
#   frame 1: cycle=4 opcode=OP_IN   value=7
#   frame 2: cycle=5 opcode=OP_RAND value=<seed-derived>
# The seed-derived RAND value is NOT asserted (the byte-stable
# kRecordRandSeed stream draws a specific uint32_t but pinning it would
# make the test fragile across mt19937 vendor changes).  Out-of-range
# is also exercised: --frame=99 must exit non-zero with a stderr
# message containing 'out of range'.
#
# Args:
#   $1 = path to retrovm binary
#   $2 = path to .bin program (relative to cwd)
#   $3 = stdin text (literal, may contain \n)

set -e
. "$(dirname -- "${BASH_SOURCE[0]}")/_lib.sh"

RETROVM="$1"
BIN="$2"
STDIN="$3"

TRACE="${BIN}.trace"

# 1. Ensure trace exists; don't clobber pre-existing traces so re-runs
#    stay cheap and don't burn an extra `record` invocation's seed.
[ -s "$TRACE" ] || printf '%s' "$STDIN" | "$RETROVM" record "$BIN" >/dev/null 2>&1

# 2. Out-of-range frame must exit 1 with a stderr message that says
#    'out of range'.  Capturing stderr separately so the next assertion
#    grep is unambiguous.
if "$RETROVM" trace "$TRACE" --frame=99 >/dev/null 2>/tmp/inspect_frame_oor.err; then
    echo "FAIL: --frame=99 should exit non-zero" >&2
    exit 1
fi
grep -q 'out of range' /tmp/inspect_frame_oor.err || {
    echo "FAIL: --frame=99 stderr did not include 'out of range' (got:" >&2
    cat /tmp/inspect_frame_oor.err >&2
    echo ")" >&2
    exit 1
}

# 3. Frame 0 (cycle=3, OP_IN, value=5).  This is the first IN frame,
#    capturing `5` from the first newline-delimited stdin token.
OUT0=$("$RETROVM" trace "$TRACE" --frame=0 2>&1)
echo "$OUT0"
FRAME_IDX=$(get_field 'frame_index :' "$OUT0")
CYCLE=$(get_field 'cycle       :' "$OUT0")
OPCODE=$(get_field 'opcode      :' "$OUT0")
VALUE=$(get_field 'value       :' "$OUT0")
if [ "$FRAME_IDX" != "0" ] || [ "$CYCLE" != "3" ] \
   || [ "$VALUE" != "5" ] || [ "$OPCODE" != "0x09 (OP_IN)" ]; then
    echo "FAIL: frame 0 mismatch (got frame=$FRAME_IDX cycle=$CYCLE opcode=$OPCODE value=$VALUE; want 0/3/0x09 (OP_IN)/5)" >&2
    exit 1
fi

# 4. Frame 1 (cycle=4, OP_IN, value=7).  Second IN capturing `7`.
OUT1=$("$RETROVM" trace "$TRACE" --frame=1 2>&1)
FRAME_IDX=$(get_field 'frame_index :' "$OUT1")
CYCLE=$(get_field 'cycle       :' "$OUT1")
OPCODE=$(get_field 'opcode      :' "$OUT1")
VALUE=$(get_field 'value       :' "$OUT1")
if [ "$FRAME_IDX" != "1" ] || [ "$CYCLE" != "4" ] \
   || [ "$VALUE" != "7" ] || [ "$OPCODE" != "0x09 (OP_IN)" ]; then
    echo "FAIL: frame 1 mismatch (got frame=$FRAME_IDX cycle=$CYCLE opcode=$OPCODE value=$VALUE; want 1/4/0x09 (OP_IN)/7)" >&2
    exit 1
fi

# 5. Frame 2 (cycle=5, OP_RAND).  Don't pin `value` because RAND is
#    driven by kRecordRandSeed's mt19937 stream — pinning would make
#    the test brittle across mt19937 vendor changes.  Pin only cycle
#    + opcode so a future wire-format change that breaks the third
#    frame's positioning surfaces here without dragging in the
#    seed-stream value.
OUT2=$("$RETROVM" trace "$TRACE" --frame=2 2>&1)
CYCLE=$(get_field 'cycle       :' "$OUT2")
OPCODE=$(get_field 'opcode      :' "$OUT2")
if [ "$CYCLE" != "5" ] || [ "$OPCODE" != "0x0A (OP_RAND)" ]; then
    echo "FAIL: frame 2 mismatch (got cycle=$CYCLE opcode=$OPCODE; want 5/0x0A (OP_RAND))" >&2
    exit 1
fi

# 6. --frame with empty value must exit 1 with a clear stderr message.
#    Compatibility: the parse layer is in main(), not cmd_inspect_trace;
#    any future refactor that moves parsing should preserve this guard.
if "$RETROVM" trace "$TRACE" --frame= >/dev/null 2>/tmp/inspect_frame_empty.err; then
    echo "FAIL: --frame= (empty value) should exit non-zero" >&2
    exit 1
fi
grep -q 'requires a value' /tmp/inspect_frame_empty.err || {
    echo "FAIL: --frame= stderr did not include 'requires a value' (got:" >&2
    cat /tmp/inspect_frame_empty.err >&2
    echo ")" >&2
    exit 1
}

# 7. Non-numeric --frame value must exit 1.  Catches accidental '--frame=abc'
#    through the same parse path that the empty-value case exercises.
if "$RETROVM" trace "$TRACE" --frame=abc >/dev/null 2>/tmp/inspect_frame_bad.err; then
    echo "FAIL: --frame=abc should exit non-zero" >&2
    exit 1
fi
grep -q 'invalid --frame' /tmp/inspect_frame_bad.err || {
    echo "FAIL: --frame=abc stderr did not include 'invalid --frame' (got:" >&2
    cat /tmp/inspect_frame_bad.err >&2
    echo ")" >&2
    exit 1
}

echo "OK: $BIN partial-trace inspection verified (frames 0/1/2 + bounds + parser errors)"
exit 0
