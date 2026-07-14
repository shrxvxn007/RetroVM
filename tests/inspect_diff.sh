#!/usr/bin/env bash
# tests/inspect_diff.sh — saves two .state files at different --at-frame
# values for the same program, then runs `retrovm inspect --diff` and
# asserts the output contains EXACTLY the expected set of differing
# fields, nothing more and nothing less.
#
# This verifies two invariants:
#   1. Equal columns are suppressed (the whole point of --diff).
#   2. Differing columns show BOTH values side-by-side in `a=X b=Y`
#      syntax (parseable by `awk -F'[ab]=' '{print $2}'` etc.).
#
# Default test: programs/io.bin at --at-frame=0 vs --at-frame=1.
#
# At frame=0 the snapshot sink fires BEFORE the first IN R0 handler
# runs (R0=0, R1=0, both LI placeholders, pc=0x08).
# At frame=1 the first IN R0 has already been consumed (R0=5, R1=0,
# pc=0x0C) given STDIN payload "5\n7\n9\n" (3 integers). Hence the
# diff MUST show frame_index, pc, and R0. It MUST NOT show halted
# (always HALT_CHECKPOINT=6), sp (constant 0xfffffc), flags (no flags
# set), R1-R7 (all 0 in both).
#
# Args:
#   $1 = path to retrovm binary
#   $2 = path to .bin program (default programs/io.bin)
#   $3 = stdin text (default "5\n7\n9\n")
#   $4 = --at-frame= for state a (default 0)
#   $5 = --at-frame= for state b (default 1)
#
# Exit 0 iff the inspect --diff output matches expectations.
#
# Robustness note (mirrors tests/inspect.sh): bash 3.2 (macOS default)
# trips `set -eu` on empty arrays, so we use plain `set -e`. Optional
# reads use `${VAR:-}`.

set -e

RETROVM="$1"
BIN="${2:-programs/io.bin}"
STDIN="${3:-5
7
9}"
RAW_A="${4:-0}"
RAW_B="${5:-1}"

for v in "$RAW_A" "$RAW_B"; do
    case "$v" in
        --at-frame=*) AT="${v#--at-frame=}" ;;
        *)           AT="$v" ;;
    esac
    if ! [[ "$AT" =~ ^[0-9]+$ ]]; then
        echo "FAIL: at-frame must be a non-negative integer (got '$v')" >&2
        exit 2
    fi
done

TRACE="${BIN}.trace"
A_STATE="$(mktemp -t retrovm_diff_a.XXXXXX)"
B_STATE="$(mktemp -t retrovm_diff_b.XXXXXX)"
trap 'rm -f "$A_STATE" "$B_STATE"' EXIT

# 1. Ensure trace exists (seed once via cmd_record for determinism).
[ -s "$TRACE" ] || printf '%s' "$STDIN" | "$RETROVM" record "$BIN" --seed=42 >/dev/null 2>&1

# 2. Save both states.
# Decide on the literal --at-frame flag for each save. Some callers
# (default-arg in this helper, also tests/save_restore.sh) pass a bare
# integer; ctest entries pass --at-frame=N verbatim. Adding a literal
# prefix unconditionally would double-prefix the ctest case to
# `--at-frame=--at-frame=0`. Mirror tests/save_restore.sh's pattern:
# accept either form, never re-prefix.
case "$RAW_A" in
    --at-frame=*) A_FLAG="$RAW_A" ;;
    *)            A_FLAG="--at-frame=$RAW_A" ;;
esac
case "$RAW_B" in
    --at-frame=*) B_FLAG="$RAW_B" ;;
    *)            B_FLAG="--at-frame=$RAW_B" ;;
esac
printf '%s' "$STDIN" | \
    "$RETROVM" save "$BIN" "$TRACE" "$A_STATE" "$A_FLAG" >/dev/null
printf '%s' "$STDIN" | \
    "$RETROVM" save "$BIN" "$TRACE" "$B_STATE" "$B_FLAG" >/dev/null

# 3. 64 B wire-format invariant on both files.
for f in "$A_STATE" "$B_STATE"; do
    SZ=$(wc -c < "$f" | tr -d ' ')
    if [ "$SZ" != "64" ]; then
        echo "FAIL: $f must be 64 B (got $SZ B)" >&2
        exit 1
    fi
done

# 4. Run inspect --diff.
DIFF_OUT=$("$RETROVM" inspect --diff "$A_STATE" "$B_STATE")
echo "$DIFF_OUT"

# 5. Header invariants.
if ! echo "$DIFF_OUT" | head -1 | grep -q '^=== RetroVM checkpoint diff ===$'; then
    echo "FAIL: missing or wrong banner" >&2
    exit 1
fi
if ! echo "$DIFF_OUT" | grep -q "^state a   : $A_STATE\$"; then
    echo "FAIL: state a line missing or wrong path" >&2
    exit 1
fi
if ! echo "$DIFF_OUT" | grep -q "^state b   : $B_STATE\$"; then
    echo "FAIL: state b line missing or wrong path" >&2
    exit 1
fi

# 6. frame_index MUST appear and match (RAW_A vs RAW_B).
FI_LINE=$(echo "$DIFF_OUT" | awk '/^frame_index : / { print; exit }')
if [ -z "$FI_LINE" ]; then
    echo "FAIL: frame_index row missing" >&2
    exit 1
fi
FI_A=$(echo "$FI_LINE" | sed 's/.*a=\([0-9]*\).*/\1/')
FI_B=$(echo "$FI_LINE" | sed 's/.*b=\([0-9]*\).*/\1/')
A_AT="${RAW_A#--at-frame=}"
B_AT="${RAW_B#--at-frame=}"
if [ "$FI_A" != "$A_AT" ] || [ "$FI_B" != "$B_AT" ]; then
    echo "FAIL: frame_index a=$FI_A b=$FI_B but expected a=$A_AT b=$B_AT" >&2
    exit 1
fi

# 7. pc MUST appear (different fetch addresses at --at-frame=0 vs 1).
# Anchor with the full-format regex so a regression that emits single-space
# labels or wrong hex width still trips here.
if ! echo "$DIFF_OUT" | grep -qE '^pc          : a=0x[0-9a-fA-F]{8} b=0x[0-9a-fA-F]{8}$'; then
    echo "FAIL: pc row missing or wrong format" >&2
    exit 1
fi
# Extract the full pc row once (mirrors the R0_LINE pattern below) so a
# single colon-greedy sed split cleanly yields a-side and b-side hex.
# The naive `awk '{ print $N }'` approach fails here because POSIX awk's
# default FS collapses the 10-space gap between `pc` and `:`, giving only
# 4 tokens (`pc`, `:`, `a=0x...`, `b=0x...`) — any column other than
# `$3` (a=) or `$4` (b=) silently picks up the wrong half. Using
# whole-line sed is independent of N whitespace tokens and avoids both
# the `print $N` column trap AND the prefix-`^a=` trap, because we
# anchor on the literal `a=` and `b=` strings inside the captured line.
PC_LINE=$(echo "$DIFF_OUT" | awk '/^pc          : / { print; exit }')
PC_A=$(echo "$PC_LINE" | sed 's/.*a=0x\([0-9a-fA-F]*\).*/\1/' | sed 's/^/0x/')
PC_B=$(echo "$PC_LINE" | sed 's/.*b=0x\([0-9a-fA-F]*\).*/\1/' | sed 's/^/0x/')
# A is frame=0 → post-TAIL pc after fetch IN R0 → save pc-4u → 0x00000008.
# B is frame=1 → post-TAIL pc after fetch IN R1 → save pc-4u → 0x0000000c.
# (The on-disk pc is uint32_t printed with %08x so both sides are 8-digit
# hex — comparing against "0x08" / "0x0c" silently fails on the trailing
# zeros. Use the full 8-digit literal.)
if [ "$BIN" = "programs/io.bin" ]; then
    if [ "$A_AT" = "0" ] && [ "$PC_A" != "0x00000008" ]; then
        echo "FAIL: io.bin frame=0 must show pc=0x00000008 (got '$PC_A')" >&2
        exit 1
    fi
    if [ "$B_AT" = "1" ] && [ "$PC_B" != "0x0000000c" ]; then
        echo "FAIL: io.bin frame=1 must show pc=0x0000000c (got '$PC_B')" >&2
        exit 1
    fi
fi

# 8. R0 MUST appear (a=0 b=5 for io.bin frame=0 vs frame=1, given
# STDIN payload "5\n7\n9\n" in CMakeLists.txt; first IN gets 5).
R0_LINE=$(echo "$DIFF_OUT" | awk '/^R0         : / { print; exit }')
if [ -z "$R0_LINE" ]; then
    echo "FAIL: R0 row missing (expected a=0 b=5 for io.bin)" >&2
    exit 1
fi
if [ "$BIN" = "programs/io.bin" ]; then
    if [ "$A_AT" = "0" ] && [ "$B_AT" = "1" ]; then
        R0_A=$(echo "$R0_LINE" | sed 's/.*a=\([0-9]*\).*/\1/')
        R0_B=$(echo "$R0_LINE" | sed 's/.*b=\([0-9]*\).*/\1/')
        if [ "$R0_A" != "0" ] || [ "$R0_B" != "5" ]; then
            echo "FAIL: io.bin frame=0..1 must show R0 a=0 b=5 (got a=$R0_A b=$R0_B)" >&2
            exit 1
        fi
    fi
fi

# 9. Negative invariants (equal columns MUST be suppressed).
if echo "$DIFF_OUT" | grep -q '^halt reason:'; then
    echo "FAIL: 'halt reason' row should be suppressed (cmd_save always hardcodes HALT_CHECKPOINT=6)" >&2
    exit 1
fi
if echo "$DIFF_OUT" | grep -q '^sp          : '; then
    # sp is 0x000ffffc in both (default initial sp, no PUSH/POP used).
    # Suppress unless the test explicitly expects a sp delta (none today).
    echo "FAIL: 'sp' row should be suppressed (both states have sp=0x000ffffc)" >&2
    exit 1
fi
if echo "$DIFF_OUT" | grep -q '^flags (ZCS) : '; then
    echo "FAIL: 'flags (ZCS)' row should be suppressed (no flags set on either path)" >&2
    exit 1
fi
if [ "$BIN" = "programs/io.bin" ]; then
    for r in 1 2 3 4 5 6 7; do
        if echo "$DIFF_OUT" | grep -q "^R$r         : "; then
            echo "FAIL: R$r row should be suppressed (both states have R$r=0 for io.bin frame=0..1)" >&2
            exit 1
        fi
    done
fi

echo "OK: $BIN inspect --diff at-frame=$A_AT..$B_AT (frame_index, pc, R0 show; halted/sp/flags/R1..R7 suppressed)"
exit 0
