#!/usr/bin/env bash
# tests/save_restore.sh — round-trip a save+restore for a given program.
#
# Args:
#   $1 = path to retrovm binary
#   $2 = path to .bin program (relative to cwd)
#   $3 = stdin text (literal, may contain \n)
#   $4 = (optional) at-frame index for cmd_save. Default 1.
#        Accepts either a bare decimal ("1") or a "--at-frame=N" token.
#
# What it does:
#   1. Records a trace for the program with stdin $3.
#   2. If the trace is non-empty:
#      a. Saves .state at --at-frame=$4 (middle of run).
#      b. Replays the .bin + .trace all the way through, capturing
#         the cycle count + halt reason ("full replay").
#      c. Restores from .state, resumes replay, captures cycle+halt.
#      d. Asserts: restore path's final cycle_count == full-replay
#         path's cycle_count, and halt reasons match.
#   3. If the trace is EMPTY (e.g. snap.bin has no IN/RAND frames):
#      cmd_save would reject any at-frame as out-of-range, so we just
#      verify record+replay cycle parity. The ctest delegation is
#      still useful as a no-ND regression guard.
#
# Field extraction is label-anchored via the `get_field` helper below —
# no awk column counters, no `$NF` traps. Same approach as
# tests/record_replay.sh and tests/inspect.sh so a future layout tweak
# only updates the label string, not column indices.
#
# This is the load-bearing Phase-4 property: resuming from a saved
# checkpoint must produce the same observable state as a full run
# would, just shifted by the pre-checkpoint prefix.

# Use plain `set -e` (NOT `set -eu`): bash 3.2 trips `set -eu` on
# `${VAR}` reads even via `${VAR:-}` defaults in some shells. The
# script is small enough that misnamed variables surface immediately
# in ctest output, and the `${VAR:-}` idiom handles every
# truly-optional read.
set -e

RETROVM="$1"
BIN="$2"
STDIN="$3"
RAW_AT_FRAME="${4:-1}"

# Accept either a bare number ("1") or a "--at-frame=N" token so callers
# in CMakeLists can pass whichever reads clearer.
case "$RAW_AT_FRAME" in
    --at-frame=*) AT_FRAME="${RAW_AT_FRAME#--at-frame=}" ;;
    *)            AT_FRAME="$RAW_AT_FRAME" ;;
esac
if ! [[ "$AT_FRAME" =~ ^[0-9]+$ ]]; then
    echo "FAIL: at-frame argument must be a non-negative integer (got '$RAW_AT_FRAME')" >&2
    exit 2
fi

TRACE="${BIN}.trace"
STATE="$(mktemp -t retrovm_state.XXXXXX)"
trap 'rm -f "$STATE"' EXIT

# get_field — literal-label-anchored extractor. awk's index() does a raw
# substring search (no regex interpretation), so labels containing `(`,
# `)`, `:` etc. — like `cycles      :`, `flags (ZCS):`, `halt reason:`
# — match literally without the caller needing to escape anything.
# Shared helper get_field — sourced from tests/_lib.sh (next to this
# script). See _lib.sh for the rationale behind the label-anchored
# extractor. Also used by tests/record_replay.sh and tests/inspect.sh.
. "$(dirname -- "${BASH_SOURCE[0]}")/_lib.sh"

# 1. Record (capture stdout so we can read frame count WITHOUT byte-counting
#    against the on-disk trace format — coupling the script to a magic
#    header size has bitten us before: trace header is 16 B, NOT 32 B).
RECORD_OUT=$(printf '%s' "$STDIN" | "$RETROVM" record "$BIN" 2>&1)
echo "$RECORD_OUT"
FRAME_COUNT=$(echo "$RECORD_OUT" | awk '/^trace[[:space:]]*:/ { print $3; exit }')
TRACE_BYTES=$(wc -c < "$TRACE" | tr -d ' ')
echo "trace       : $TRACE_BYTES B, $FRAME_COUNT frame(s)"

# Capture full-replay cycles + halt via the label-anchored helper.
# NEW layout: `halt reason: N` on its own line, has a separate
# `halt_legend: (...)` line. Old layout had a single trailing legend
# suffix that needed `$4` vs `$NF` discrimination — that trap is gone.
FULL_OUT=$(printf '%s' "$STDIN" | "$RETROVM" replay "$BIN" "$TRACE" 2>/dev/null || true)
FULL_CYCLES=$(get_field "cycles      :" "$FULL_OUT")
FULL_HALT=$(  get_field "halt reason:" "$FULL_OUT")

# 2. No-ND path: program has no IN/RAND. cmd_save would reject any
# at-frame as out of range; just confirm record+replay cycle parity
# trivially holds. The ctest still gates "record→replay doesn't drift
# on pure-deterministic programs" as a regression.
if [ "$FRAME_COUNT" -eq 0 ]; then
    if [ -z "$FULL_CYCLES" ] || [ "$FULL_HALT" != "1" ]; then
        echo "FAIL: $BIN replay should HALT_NORMAL with deterministic cycles" >&2
        echo "  full_halt=$FULL_HALT  full_cycles=$FULL_CYCLES" >&2
        exit 1
    fi
    echo "OK: $BIN record(replay) [no-ND] halt=$FULL_HALT cycles=$FULL_CYCLES (no save/restore: trace has 0 frames)"
    exit 0
fi

# 3. at_frame must be in range. cmd_save would also reject out-of-range
# frames, but checking up front lets the ctest message blame the test
# argument, not the runtime.
if [ "$AT_FRAME" -ge "$FRAME_COUNT" ]; then
    echo "FAIL: at-frame=$AT_FRAME out of range (trace has $FRAME_COUNT frames)" >&2
    exit 1
fi

# 4. Save at the requested frame index.
printf '%s' "$STDIN" | \
    "$RETROVM" save "$BIN" "$TRACE" "$STATE" --at-frame="$AT_FRAME" >/dev/null

if [ ! -s "$STATE" ]; then
    echo "FAIL: --at-frame=$AT_FRAME produced empty $STATE" >&2
    exit 1
fi

# 5. Restore: must reproduce the same final halt/cycles as a full replay.
RESTORED_OUT=$(printf '%s' "$STDIN" | "$RETROVM" restore "$BIN" "$TRACE" "$STATE" 2>&1)
echo "$RESTORED_OUT"
RESTORE_CYCLES=$(get_field "cycles      :" "$RESTORED_OUT")
RESTORE_HALT=$(  get_field "halt reason:" "$RESTORED_OUT")

if [ "$RESTORE_HALT" != "$FULL_HALT" ]; then
    echo "FAIL: halt reason mismatch (full=$FULL_HALT restore=$RESTORE_HALT)" >&2
    exit 1
fi
if [ "$RESTORE_CYCLES" != "$FULL_CYCLES" ]; then
    echo "FAIL: cycle parity mismatch (full=$FULL_CYCLES restore=$RESTORE_CYCLES)" >&2
    exit 1
fi

echo "OK: $BIN save(restore) at-frame=$AT_FRAME halt=$RESTORE_HALT cycles=$RESTORE_CYCLES (= full-replay cycles)"
