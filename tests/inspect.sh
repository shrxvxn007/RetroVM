#!/usr/bin/env bash
# tests/inspect.sh — produce a .state via cmd_save, then run cmd_inspect
# and assert the printed columns follow dump_state()'s layout verbatim.
#
# Field extraction uses the per-file `get_field` helper. awk's `index()`
# does a raw substring search with no regex interpretation, so labels
# containing `(`, `)`, `:` etc. match literally — the column-counter
# trap the previous version carried (NF-2 vs NF-3, $4 vs $NEVER, etc.)
# is gone. A future layout tweak that needs a wider label only updates
# the label strings, never any awk column positions.
#
# Args:
#   $1 = path to retrovm binary
#   $2 = path to .bin program (relative to cwd)
#   $3 = stdin text (literal, may contain \n)
#   $4 = --at-frame=N token (or bare integer). Default 1.
#
# Exit 0 iff cmd_inspect exits 0 AND every column parses correctly AND
# the on-disk size is exactly 64 B (Phase-4 wire-format invariant).

set -e

RETROVM="$1"
BIN="$2"
STDIN="$3"
RAW_AT_FRAME="${4:-1}"

case "$RAW_AT_FRAME" in
    --at-frame=*) AT_FRAME="${RAW_AT_FRAME#--at-frame=}" ;;
    *)           AT_FRAME="$RAW_AT_FRAME" ;;
esac
if ! [[ "$AT_FRAME" =~ ^[0-9]+$ ]]; then
    echo "FAIL: at-frame must be a non-negative integer (got '$RAW_AT_FRAME')" >&2
    exit 2
fi

TRACE="${BIN}.trace"
STATE="$(mktemp -t retrovm_inspect.XXXXXX)"
trap 'rm -f "$STATE"' EXIT

# get_field — literal-label-anchored extractor. awk's index() does a raw
# substring search (no regex interpretation), so labels containing `(`,
# `)`, `:` etc. match literally without the caller needing to escape
# anything. Whitespace-trim on the value-extracted substring uses a
# stable regex with no special chars.
# Shared helper get_field — sourced from tests/_lib.sh (next to this
# script). See _lib.sh for the rationale behind the label-anchored
# extractor. pc_or_sp (also exported there) is used below for the
# pc / sp line split. Also used by tests/record_replay.sh and
# tests/save_restore.sh so any tweak to label formats or value
# extraction updates one place.
. "$(dirname -- "${BASH_SOURCE[0]}")/_lib.sh"

# 1. Ensure trace exists. Don't clobber an existing trace so re-runs are
#    cheap; cmd_save can replay from a pre-recorded .trace.
[ -s "$TRACE" ] || printf '%s' "$STDIN" | "$RETROVM" record "$BIN" >/dev/null 2>&1

# 2. Save to .state at the requested frame.
printf '%s' "$STDIN" | \
    "$RETROVM" save "$BIN" "$TRACE" "$STATE" --at-frame="$AT_FRAME" >/dev/null

# 3. File-size invariant: the .state wire format is exactly 64 B
#    (16-byte header + 48-byte body).
STATE_BYTES=$(wc -c < "$STATE" | tr -d ' ')
if [ "$STATE_BYTES" != "64" ]; then
    echo "FAIL: .state must be exactly 64 B (got $STATE_BYTES B)" >&2
    exit 1
fi

# 4. Run retrovm inspect and capture output. We do NOT swallow stderr
#    so a wire-format decode failure surfaces as a clear test failure.
INSPECT_OUT=$("$RETROVM" inspect "$STATE" 2>&1)
echo "$INSPECT_OUT"
"$RETROVM" inspect "$STATE" >/dev/null 2>&1 || {
    echo "FAIL: retrovm inspect exited non-zero for $STATE" >&2
    exit 1
}

# 5. Header invariant. cmd_inspect and dump_state now share an identical
#    `halt reason:` + `halt_legend:` split so the same helper pattern
#    works for both. Decode banner, then field-by-field.
BANNER=$(printf '%s\n' "$INSPECT_OUT" | head -n1)
if [ "$BANNER" != "=== RetroVM checkpoint inspect ===" ]; then
    echo "FAIL: missing or wrong banner (got '$BANNER')" >&2
    exit 1
fi

# frame_index must equal the requested at-frame value.
FI=$(get_field "frame_index :" "$INSPECT_OUT")
if [ -z "$FI" ]; then
    echo "FAIL: frame_index line missing from inspect output" >&2
    exit 1
fi
if [ "$FI" != "$AT_FRAME" ]; then
    echo "FAIL: frame_index mismatch (printed=$FI expected=$AT_FRAME)" >&2
    exit 1
fi

# halt reason must always be 6 (HALT_CHECKPOINT, hardcoded by cmd_save).
# The old layout had the legend as a trailing `(...)` on the same line;
# legend is now its own row so it doesn't pollute value extraction.
HALT=$(get_field "halt reason:" "$INSPECT_OUT")
if [ "$HALT" != "6" ]; then
    echo "FAIL: halt reason should be 6 (HALT_CHECKPOINT), got '$HALT'" >&2
    exit 1
fi
# halt_legend must also be present (proves the split is wired up and
# future variants that change the legend wording still parse cleanly).
LEGEND=$(get_field "halt_legend:" "$INSPECT_OUT")
if [ -z "$LEGEND" ]; then
    echo "FAIL: halt_legend line missing" >&2
    exit 1
fi

# pc / sp: split via the shared `pc_or_sp` helper (defined in
# tests/_lib.sh alongside get_field).
PCSP=$(get_field "pc / sp     :" "$INSPECT_OUT")
PC=$(pc_or_sp pc "$PCSP")
SP=$(pc_or_sp sp "$PCSP")
if ! [[ "$PC" =~ ^0x[0-9a-fA-F]{8}$ ]]; then
    echo "FAIL: pc token malformed (got '$PC')" >&2
    exit 1
fi
if ! [[ "$SP" =~ ^0x[0-9a-fA-F]{8}$ ]]; then
    echo "FAIL: sp token malformed (got '$SP')" >&2
    exit 1
fi

# flags ZCS bitmask: 3 chars (Z/C/-), one per bit.
FLAGS=$(get_field "flags (ZCS):" "$INSPECT_OUT")
if ! [[ "$FLAGS" =~ ^[ZC\-]{3}$ ]]; then
    echo "FAIL: flags line malformed (got '$FLAGS')" >&2
    exit 1
fi

# regs line: 8 R<n>=<v> tokens. With `NF` (the count of whitespace
# tokens on the line) we don't need to know which column is `:`, which
# is the first register, or which side of the value the parser lives.
# The previous version used `NF - 2` and `NF - 3` which silently broke
# whenever the label width changed.
REGS_LINE=$(get_field "regs        :" "$INSPECT_OUT")
N_REGS=$(echo "$REGS_LINE" | awk '{print NF}')
if [ "$N_REGS" != "8" ]; then
    echo "FAIL: regs line must have exactly 8 R<n>=<v> tokens (got $N_REGS)" >&2
    exit 1
fi

# get_reg — extract the value of register $1 from a regs line. sed
# regex captures the digits after `Rn=` followed by either space or
# end-of-line. Works because R0..R7 are all single-digit, so R1 is not
# ambiguous with R10 (which doesn't exist for 8 regs).
get_reg() {
    echo "$2" | sed -nE "s/.*R${1}=([0-9]+)( |\$).*/\\1/p"
}

# 6. Spot-check specific registers to verify the snapshot sink fires
# at the right cycle. Programs/io.bin reads two INs — first into R0,
# then R1 — so we have three meaningful boundary conditions:
#
#   --at-frame=0  : sink fires BEFORE any IN handler runs. Both R0 and
#                  R1 are still the LI placeholders (0).
#   --at-frame>=1 : first IN handler has run (R0=11) but the second
#                  IN's IN-handler hasn't yet executed (R1=0 placeholder).
#   --at-frame>=2 : both IN handlers have run.
if [ "$AT_FRAME" -eq 0 ]; then
    R0=$(get_reg 0 "$REGS_LINE")
    R1=$(get_reg 1 "$REGS_LINE")
    case "$BIN" in
        programs/io.bin)
            if [ "$R0" != "0" ] || [ "$R1" != "0" ]; then
                echo "FAIL: io.bin --at-frame=0 must show R0=0 R1=0 (got R0=$R0 R1=$R1)" >&2
                exit 1
            fi
            ;;
    esac
elif [ "$AT_FRAME" -ge 1 ]; then
    R0=$(get_reg 0 "$REGS_LINE")
    R1=$(get_reg 1 "$REGS_LINE")
    case "$BIN" in
        programs/io.bin)
            # R0=11 (first IN consumed); R1=0 (placeholder; second IN's
            # IN-handler hasn't run yet).
            if [ "$R0" != "11" ] || [ "$R1" != "0" ]; then
                echo "FAIL: io.bin --at-frame>=1 must show R0=11 R1=0 (got R0=$R0 R1=$R1)" >&2
                exit 1
            fi
            ;;
    esac
fi

echo "OK: $BIN inspect at-frame=$AT_FRAME frame_index=$FI halted=$HALT pc=$PC size=${STATE_BYTES}B"
exit 0
