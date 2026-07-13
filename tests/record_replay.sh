#!/usr/bin/env bash
# tests/record_replay.sh — record + replay round-trip with cycle/halt/pc parity.
#
# Usage: tests/record_replay.sh <retrovm-bin> <program.bin> <stdin-text>
#                                [seed]
#                                [--expect-halt=N]
#                                [--expect-cycles=N]
#
# <stdin-text> is piped literally to `retrovm record`'s stdin via `printf %s`,
# so embedded newlines round-trip cleanly without `eval`.
#
# Field extraction uses the per-file `get_field` helper: it walks the input
# line-by-line via awk's `index($0, L) == 1` (literal substring match — no
# regex specials are interpreted) and prints everything after the label,
# with leading/trailing whitespace trimmed. This sidesteps the `$NF` / `$4`
# vs `$3` column-fragility trap the previous version carried: a single
# column drift (e.g. legend appended, label reorganised) used to silently
# swap a value for the trailing-legend literal.
#
# Exits 0 only if BOTH record and replay succeed AND:
#   - cycle_count printed by dump_state matches across the two runs
#   - halt reason printed by dump_state matches across the two runs
#   - pc printed by dump_state matches across the two runs
# (parity is the core property; `--expect-halt=N` / `--expect-cycles=N`
# add absolute-value assertions for tests where the ctest wants to bound
# the expected outcome, e.g. divzero trap-path halt=2 cycles=8.)
#
# Non-zero-but-correct exit codes from record/replay are NOT failures:
# `retrovm record` returns 2 when the program halts with anything other
# than HALT_NORMAL (divzero on stdin "0\n7\n", for example). That's the
# right outcome — only the dump_state divergence indicates a real bug.

# Use `set -e` (NOT `set -eu`): we deliberately avoid nounset because
# `${SEED_ARGS[@]}` on an empty array trips "unbound variable" on bash 3.2
# (the system bash shipped with macOS), and that's exactly the no-seed
# path of every 3-arg call. The script is small enough that implicit
# variable typos are obvious from ctest output, and we ${VAR:-} every
# truly-optional read.
set -e

RETROVM="$1"
BIN="$2"
STDIN="$3"

if [ -z "$RETROVM" ] || [ -z "$BIN" ] || [ -z "$STDIN" ]; then
    echo "usage: $0 <retrovm-bin> <program.bin> <stdin-text> [seed] [--expect-halt=N] [--expect-cycles=N]" >&2
    exit 2
fi

SEED=""                   # absent => retrovm uses its default kRecordRandSeed
EXPECT_HALT=""            # absent => no absolute-value assertion
EXPECT_CYCLES=""

# Argument parsing: 4th positional is either a seed (decimal) or one of the
# --expect-* flags; subsequent args may be in any order. Keep things
# forgiving so callers can write seeds or expectations in either style.
shift 3
while [ $# -gt 0 ]; do
    case "$1" in
        --expect-halt=*)   EXPECT_HALT="${1#--expect-halt=}"   ;;
        --expect-cycles=*) EXPECT_CYCLES="${1#--expect-cycles=}" ;;
        --expect-halt)     EXPECT_HALT="${2:-}"; shift ;;
        --expect-cycles)   EXPECT_CYCLES="${2:-}"; shift ;;
        *)
            # Bare integer => seed
            if [[ "$1" =~ ^[0-9]+$ ]] && [ -z "$SEED" ]; then
                SEED="$1"
            else
                echo "FAIL: unknown argument '$1'" >&2
                exit 2
            fi
            ;;
    esac
    shift
done

if [ -n "$EXPECT_HALT" ] && ! [[ "$EXPECT_HALT" =~ ^[0-9]+$ ]]; then
    echo "FAIL: --expect-halt must be a non-negative integer (got '$EXPECT_HALT')" >&2
    exit 2
fi
if [ -n "$EXPECT_CYCLES" ] && ! [[ "$EXPECT_CYCLES" =~ ^[0-9]+$ ]]; then
    echo "FAIL: --expect-cycles must be a non-negative integer (got '$EXPECT_CYCLES')" >&2
    exit 2
fi

# DON'T pass a file path to get_field.
# Shared helpers -- see tests/_lib.sh.
. "$(dirname -- "${BASH_SOURCE[0]}")/_lib.sh"

# Local temp paths are kept private (`_p` suffix) so the public
# $record_log / $replay_log variables hold the captured CONTENT, not a
# path. mktemp is iterable and `set +e`-safe; bash 3.2 `${ARR[@]}`
# pitfalls are sidestepped by avoiding arrays entirely.
record_p=$(mktemp)
replay_p=$(mktemp)
trap 'rm -f "$record_p" "$replay_p"' EXIT

# 1. Record: feed <stdin-text> to `retrovm record` and capture stdout.
#    Non-zero exit (e.g. 2) means the program halted non-normally — a
#    legitimate outcome we still compare against replay.
if [ -n "$SEED" ]; then
    printf '%s' "$STDIN" | "$RETROVM" record "$BIN" --seed="$SEED" > "$record_p" 2>&1 || true
else
    printf '%s' "$STDIN" | "$RETROVM" record "$BIN" > "$record_p" 2>&1 || true
fi
# dump_state layout: cycles      : N\nhalt reason: N\nhalt_legend: (...)\npc / sp     : 0x... / 0x...\n
# Read the mktemp file's contents INTO $record_log (so $record_log now
# holds the public content string, not a path).
record_log="$(<"$record_p")"
# The label-anchored get_field sidesteps the previous $NF / $NF trap on
# the trailing-legend literal that previously lived on the halt line.
record_cycles=$(get_field "cycles      :" "$record_log")
record_halt=$(  get_field "halt reason:" "$record_log")
record_pcsp=$(  get_field "pc / sp     :" "$record_log")
record_pc=$(pc_or_sp pc "$record_pcsp")

# 2. Replay: same shape as record. Don't clobber an existing trace so
# re-runs are cheap.
"$RETROVM" replay "$BIN" "${BIN}.trace" > "$replay_p" 2>&1 || true
replay_log="$(<"$replay_p")"
replay_cycles=$(get_field "cycles      :" "$replay_log")
replay_halt=$(  get_field "halt reason:" "$replay_log")
replay_pcsp=$(  get_field "pc / sp     :" "$replay_log")
replay_pc=$(pc_or_sp pc "$replay_pcsp")

# 3. Extract sanity.
if [ -z "$record_cycles" ] || [ -z "$replay_cycles" ]; then
    echo "FAIL: could not extract cycles from $BIN:" >&2
    echo "--- record log ---" >&2; cat "$record_p" >&2
    echo "--- replay log ---" >&2; cat "$replay_p" >&2
    exit 1
fi

# 4. Cycle parity.
if [ "$record_cycles" != "$replay_cycles" ]; then
    echo "FAIL: cycle parity mismatch for $BIN: record=$record_cycles replay=$replay_cycles" >&2
    exit 1
fi

# 5. Halt reason parity. With the legend-on-its-own-line layout the
#    value is everything after `halt reason:` — clean and one-token.
if [ "$record_halt" != "$replay_halt" ]; then
    echo "FAIL: halt reason mismatch for $BIN: record=$record_halt replay=$replay_halt" >&2
    exit 1
fi

# 6. pc parity: divzero trap-path makes this load-bearing.
if [ -n "$record_pc" ] && [ -n "$replay_pc" ] && [ "$record_pc" != "$replay_pc" ]; then
    echo "FAIL: pc parity mismatch for $BIN: record=$record_pc replay=$replay_pc" >&2
    exit 1
fi

# 7. Optional absolute-value assertions.
if [ -n "$EXPECT_HALT" ] && [ "$record_halt" != "$EXPECT_HALT" ]; then
    echo "FAIL: $BIN record halt=$record_halt (expected $EXPECT_HALT)" >&2
    exit 1
fi
if [ -n "$EXPECT_CYCLES" ] && [ "$record_cycles" != "$EXPECT_CYCLES" ]; then
    echo "FAIL: $BIN record cycles=$record_cycles (expected $EXPECT_CYCLES)" >&2
    exit 1
fi

echo "OK: $BIN record(replay) halt=$record_halt cycles=$record_cycles pc=$record_pc"
exit 0
