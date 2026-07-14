#!/usr/bin/env bash
# tests/inspect_frame.sh — Phase 6+7 partial-trace inspection ctest.
#
# Ensures `<bin>.trace` exists (records if not), then exercises
# `retrovm trace <bin.trace> --frame=N` for N in {0, 1, 2, 99} and
# asserts each invocation's per-frame block against the trace wire
# format fixed in trace.hpp.  Mirrors tests/inspect_trace.sh's
# label-anchored extraction via `get_field` from _lib.sh.
#
# Phase 7 additionally inserts a SYNTHETIC-FIXTURE OK-path ctest
# above the real-trace section: a hand-minted 48 B trace carrying
# OP_IN (frame 0: cycle=3, value=5) and OP_RAND (frame 1: cycle=7,
# value=42).  Byte-level pinned via `od -An -tx1` + `printf '%02x'`
# so the per-frame branch's KNOWN-mnemonic paths are pinned at the
# bytestream layer independent of programs/io.bin's recorded trace.
# (Real-trace tests above only carry one entry of each; trace
# mutations on those frames would change the recorded value and hide
# the bytestream-level pin — the synth isolates the printing path.)
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

# === Phase 7: Synthetic OK-path fixture (runs BEFORE the real-trace
#   section so a byte-level regression here fails fast, before any
#   `retrovm record` cost). Reuses _lib.sh: get_field for per-frame
#   integration assertions. NB: the EXIT trap below cleans up only
#   $TMP_OK; ${BIN}.trace is intentionally persistent (real-trace
#   section caches it for re-run speed). ===
TMP_OK=$(mktemp -t retrovm_frame_ok.XXXXX.trace)
trap 'rm -f "$TMP_OK"' EXIT

# 1. Mint 48 B synthetic .trace: 16 B header + 2 × 16 B frames.
#    Wire format ref: trace.hpp:23-44.
#      Frame 0: cycle=3 opcode=OP_IN (0x09)  value=5
#      Frame 1: cycle=7 opcode=OP_RAND (0x0A) value=42
#    8 ASCII + 40 escapes = 48 B content + 1 trailing NUL from bash
#    3.2's printf builtin = 49 B; `head -c 48` strips the trailing
#    NUL so the file is exactly 48 B. Same single-printf + head-cN
#    pattern as tests/inspect_unknown_opcode.sh.
#    NB: post-\x01 zero count is 7 (header only: \x01 + version-high[3] +
#    reserved[4]). Frame-0 cycle byte 0 is \x03 (NOT \x00), so the zero
#    run breaks earlier than in inspect_unknown_opcode.sh (where it's
#    15 because cycle=0 makes all 8 cycle bytes zero). Mirror that
#    15-zero count here and you'll reintroduce the byte-shift bug
#    this very test is designed to catch.
#    Same trap applies to the EXPECTED_BYTES array further below —
#    it encodes the same wire-format, so edits to either side must
#    be lockstep or this assertion fires; the multi-position diff
#    IS the 8-byte byte-shift signature your zero-count edit caused.
printf 'RVMTRACE\x01\x00\x00\x00\x00\x00\x00\x00\x03\x00\x00\x00\x00\x00\x00\x00\x09\x00\x00\x00\x05\x00\x00\x00\x07\x00\x00\x00\x00\x00\x00\x00\x0a\x00\x00\x00\x2a\x00\x00\x00' \
    | head -c 48 > "$TMP_OK"

# 2. Per-byte hex assertion: drain via `od -An -tx1` (collapses
#    spaces/newlines to a 96-char hex string for a 48 B file) and
#    compare against an EXPECTED_HEX computed from the SAME byte
#    mint via `printf '%02x'` — so a transcription typo on the
#    expected side cannot drift from the actual side.
ACTUAL_HEX=$(od -An -tx1 "$TMP_OK" | tr -d ' \n')
# Self-check: header 8+4+4 + frame-0 8+1+3+4 + frame-1 8+1+3+4 = 48 B.
# WARNING: do NOT inline `printf '%02x' arg1 arg2 \<NL>arg \<NL>...\` directly
# into `$(...)` — empirically breaks at runtime (rc=127, byte literals
# re-interpreted as commands) and `bash -n` does NOT catch it. Array
# form sidesteps the parser pitfall. See git history for full diagnosis.
EXPECTED_BYTES=(
    # bytes 0-7 : magic
    0x52 0x56 0x4d 0x54 0x52 0x41 0x43 0x45
    # bytes 8-11 : version=1
    0x01 0x00 0x00 0x00
    # bytes 12-15 : reserved
    0x00 0x00 0x00 0x00
    # bytes 16-23 : cycle0=3
    0x03 0x00 0x00 0x00 0x00 0x00 0x00 0x00
    # byte 24 : opcode0=OP_IN (0x09)
    0x09
    # bytes 25-27 : _pad0_0
    0x00 0x00 0x00
    # bytes 28-31 : value0=5 (LE)
    0x05 0x00 0x00 0x00
    # bytes 32-39 : cycle1=7
    0x07 0x00 0x00 0x00 0x00 0x00 0x00 0x00
    # byte 40 : opcode1=OP_RAND (0x0A)
    0x0a
    # bytes 41-43 : _pad0_1
    0x00 0x00 0x00
    # bytes 44-47 : value1=42 (LE; 0x2a)
    0x2a 0x00 0x00 0x00
)
EXPECTED_HEX=$(printf '%02x' "${EXPECTED_BYTES[@]}")
# Locale-stability guard (-eq 96 since file is 48 B × 2 hex chars/B).
# Inline LANG/LC_ALL so a future ctest failure in an exotic locale
# names the culprit env in one line, no separate `echo $LANG` triage.
[ "${#ACTUAL_HEX}" -eq 96 ] || { echo "FAIL: od pipeline produced ${#ACTUAL_HEX} hex chars (LANG='$LANG' LC_ALL='$LC_ALL'), want 96" >&2; exit 1; }
if [ "$ACTUAL_HEX" != "$EXPECTED_HEX" ]; then
    echo "FAIL: OK-path synth-fixture hex mismatch" >&2
    echo "  expected: $EXPECTED_HEX" >&2
    echo "  got:      $ACTUAL_HEX" >&2
    exit 1
fi

# 3. Per-frame integration. retrovm trace $TMP_OK --frame=0 must
#    print OP_IN (NOT "unknown") with cycle=3 value=5; --frame=1 must
#    print OP_RAND with cycle=7 value=42. Format specifiers confirmed
#    against main.cpp:1751-1785 (opcode is 0x%02X uppercase; value is
#    decimal %u; cycle is decimal %llu; frame_index is decimal %zu).
OUT0=$("$RETROVM" trace "$TMP_OK" --frame=0 2>&1)
echo "$OUT0"
FRAME_IDX=$(get_field 'frame_index :' "$OUT0")
CYCLE=$(get_field 'cycle       :' "$OUT0")
OPCODE=$(get_field 'opcode      :' "$OUT0")
VALUE=$(get_field 'value       :' "$OUT0")
if [ "$FRAME_IDX" != "0" ] || [ "$CYCLE" != "3" ] \
   || [ "$VALUE" != "5" ] || [ "$OPCODE" != "0x09 (OP_IN)" ]; then
    echo "FAIL: synth frame 0 (got frame=$FRAME_IDX cycle=$CYCLE opcode=$OPCODE value=$VALUE; want 0/3/0x09 (OP_IN)/5)" >&2
    exit 1
fi

OUT1=$("$RETROVM" trace "$TMP_OK" --frame=1 2>&1)
FRAME_IDX=$(get_field 'frame_index :' "$OUT1")
CYCLE=$(get_field 'cycle       :' "$OUT1")
OPCODE=$(get_field 'opcode      :' "$OUT1")
VALUE=$(get_field 'value       :' "$OUT1")
if [ "$FRAME_IDX" != "1" ] || [ "$CYCLE" != "7" ] \
   || [ "$VALUE" != "42" ] || [ "$OPCODE" != "0x0A (OP_RAND)" ]; then
    echo "FAIL: synth frame 1 (got frame=$FRAME_IDX cycle=$CYCLE opcode=$OPCODE value=$VALUE; want 1/7/0x0A (OP_RAND)/42)" >&2
    exit 1
fi

# === Phase 6: Real-trace section resumes ===

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
