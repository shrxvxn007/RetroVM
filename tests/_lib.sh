#!/usr/bin/env bash
# tests/_lib.sh — shared ctest helper extractors. Sourced by the test
# scripts in this directory (`record_replay.sh`, `save_restore.sh`,
# `inspect.sh`). `inspect_diff.sh` does label-anchored regex directly
# (no get_field calls) and does not currently source this lib.
#
# This file is NOT executable on its own — it exports two helpers and
# has no side effects. Sourcing leaves the caller's `set -e` and
# positional arguments untouched; the helpers themselves set neither.
# A future caller that wants strict mode can layer `set -eu` on top.
#
# cmd_save's contract (recorded in main.cpp's dump_state output and
# mirrored by cmd_inspect) is "one labeled line per field, with the
# legend on its own line". Treating those labels as the parsing
# anchor makes the extraction immune to whether the legend text grows,
# the label whitespace is reabridged, or any column ever moves.
#
# Helpers:
#
#   get_field "LABEL" "$ARG"
#     Returns the trimmed text following `LABEL:` on the first line
#     of input whose first characters match LABEL literally. Uses
#     awk's `index()` (raw substring match — no regex specials are
#     interpreted in LABEL), so labels like `flags (ZCS):` and
#     `pc / sp     :` match without escaping. Returns nothing if no
#     matching line is found.
#
#     Dual-mode $ARG — the helper accepts either:
#       (a) a path to an existing readable file, e.g. `get_field "x :"
#           "$OUT_P"` — awk reads the file line-by-line.
#       (b) a string of file CONTENTS captured via `$()`, e.g.
#           `get_field "x :" "$INSPECT_OUT"` where `INSPECT_OUT=$("$RETROVM"
#           inspect … 2>&1)` — awk receives the content via `<<<`.
#     This means tests can use whichever convention fits the script:
#     short inputs go through (a) directly to the disk, longer /
#     already-captured content goes through (b) without a re-read.
#     The collision risk (a content string that happens to be a path
#     to an existing file) is acceptable in practice because ctest
#     input strings either lack a `/` or are unique /tmp paths that
#     don't contain label-bearing text.
#
#   pc_or_sp pc|sp "$PCSP_LINE"
#     Splits the value of a parsed `pc / sp     : 0xPC / 0xSP` line on
#     the 3-char separator ` / `. The separator doesn't appear inside
#     either hex token, so the split is unambiguous without any awk
#     column counter. pc_or_sp 'sp' "0xPC / 0xSP" prints "0xSP";
#     pc_or_sp 'pc' "0xPC / 0xSP" prints "0xPC".
#
#   mint_synth_trace <size> <tmpfile> <byte...>
#     Mints a synthetic .trace file of exactly <size> bytes into
#     <tmpfile> and verifies its byte geometry against the varargs
#     before returning.  Caller is responsible for `mktemp` + EXIT-trap
#     registration; on any assertion failure the helper removes
#     <tmpfile> before exiting non-zero so the file invariant holds
#     regardless.
#
#     Usage (callers declare the authoritative array, then expand it
#     by VALUE so the helper can anti-drift-derive EXPECTED_HEX from
#     the same byte list that the printf mints with):
#
#         EXPECTED_BYTES=( 0x52 0x56 ... )
#         mint_synth_trace "${#EXPECTED_BYTES[@]}" "$TMP_FILE" \
#             "${EXPECTED_BYTES[@]}"
#
#     Sidesteps bash 3.2's lack of namerefs (`declare -n`) by accepting
#     the array contents by VALUE through `"${ARR[@]}"` expansion
#     rather than eval-ing the array name — tighter, more portable,
#     no scoping/security concerns around dynamic-name dereferencing.
#     Codifies the synthetic-fixture defense-in-depth pattern that
#     had lived inline in tests/inspect_unknown_opcode.sh and
#     tests/inspect_frame.sh: single-printf mint + `head -c N`
#     (strips bash 3.2 trailing C-string NUL); `od -An -tx1 | tr -d
#     ' \n'` drain (collapses spaces/newlines to a single hex
#     string); locale-stability guard [-eq 2N] with inline LANG/LC_ALL
#     diagnostic (a future ctest failure in an exotic locale triages
#     in one line, no separate `echo $LANG $LC_ALL` step); per-byte
#     equality vs EXPECTED_HEX computed from the SAME byte varargs
#     (anti-transcription-drift) with side-by-side hex diff on
#     failure.
#
#     WARNING (carry-forward from prior Phase-7 commits): do NOT
#     inline `printf '%02x' arg1 arg2 \<NL>arg \<NL>...\` directly
#     inside `$(...)`.  Empirically breaks at runtime (rc=127, byte
#     literals re-interpreted as commands because line continuations
#     are stripped before comment recognition) and `bash -n` does NOT
#     catch it.  This helper builds the byte stream via `printf "%b"`
#     on a format string assembled through inner `printf '\\x%02x'` --
#     a single substitution with NO line continuation, so the parser
#     pitfall doesn't fire.
#
#     Diagnosability (carry-forward from the per-test NB that the
#     refactor dropped): the per-byte equality catches HELPER-SIDE
#     mint pipeline corruption (an inner-printf / outer-%b / head-cN
#     edit that produces wrong bytes) and CALLER-SIDE size-arg drift
#     (e.g. declaring 32-byte EXPECTED_BYTES but passing size=33).
#     When that fires the diff is NOT a single-byte mismatch -- it's
#     a CASCADE off an N-byte shift (the signature of an offset or
#     token-drop in the format assembly). A byte-level mistake in
#     EXPECTED_BYTES propagates consistently to BOTH the mint and
#     the expected_hex (both read the same `${bytes[@]}` varargs),
#     so per-byte equality HAPPILY approves the (jointly wrong)
#     result -- that class of error is caught LATER by the
#     end-to-end per-frame printer assertion in the caller script.
#     The pre-refactor per-test NB that warned about the
#     post-first-non-zero zero-count asymmetry (15 vs 7 between the
#     two fixtures) is preserved here as historical context for
#     why the cascade diff signature exists at all.
#
# Why `set -e` and not `set -eu`: bash 3.2 (the default macOS shell)
# trips "unbound variable" on `${ARR[@]}` for an unset array. None of
# the current callers use arrays, but cooperating callers may add
# them later; `set -e` keeps the floor soft without inviting silent
# typos. The scripts that source this lib use `set -e` consistently.

# When $2 is a path to an existing file, awk can read it directly and
# avoids the here-string detour (which on macOS bash 3.2 can mangle
# very large payloads and is also unnecessary when the file is already
# on disk). Auto-detect file vs content: if the second argument names
# a readable file, hand the path to awk; otherwise pipe it through
# `<<<` so callers passing command-substituted stdout (the inspect.sh
# style) keep working unchanged. Collision risk — a content string
# that happens to be a path to an existing file — is acceptable in
# practice because ctest input strings either lack a `/` or are
# unique /tmp paths that don't contain label-bearing text.
get_field() {
    if [ -r "$2" ]; then
        awk -v L="$1" '
            index($0, L) == 1 {
                rest = substr($0, length(L) + 1)
                sub(/^[[:space:]]+/, "", rest)
                sub(/[[:space:]]+$/, "", rest)
                print rest
                exit
            }
        ' "$2"
    else
        awk -v L="$1" '
            index($0, L) == 1 {
                rest = substr($0, length(L) + 1)
                sub(/^[[:space:]]+/, "", rest)
                sub(/[[:space:]]+$/, "", rest)
                print rest
                exit
            }
        ' <<< "$2"
    fi
}

pc_or_sp() {
    case "$1" in
        pc) printf '%s' "${2% / *}" ;;
        sp) printf '%s' "${2#* / }" ;;
    esac
}

# See the function-doc above for full semantics; the inline comments
# below track each step of the mintage + verify flow.
mint_synth_trace() {
    local size="$1"
    local tmpfile="$2"
    shift 2
    local bytes=("$@")

    # 1. Mint binary trace.  Inner printf `\\x%02x` builds a literal
    #    string of `\xNN` escapes (one per byte).  Outer printf `"%b"`
    #    interprets those escapes as actual bytes.  `head -c N` strips
    #    bash 3.2's trailing C-string NUL byte (which a multi-printf
    #    block would emit between every call, scrambling the wire
    #    format).
    local format_str
    format_str=$(printf '\\x%02x' "${bytes[@]}")
    printf '%b' "$format_str" | head -c "$size" > "$tmpfile"

    # 2. Drain actual hex string.  `tr -d ' \n'` collapses od's
    #    "xx yy zz" output + line breaks into a single hex string.
    local actual_hex
    actual_hex=$(od -An -tx1 "$tmpfile" | tr -d ' \n')

    # 3. Compute expected hex baseline from the SAME bytes (the
    #    vararg is the source of truth -- no transcription drift
    #    between the mint and the EXPECTED_HEX computation, since
    #    both derive from the same `${bytes[@]}` list).
    local expected_hex
    expected_hex=$(printf '%02x' "${bytes[@]}")

    # 4. Locale-stability guard.  Expected length is precisely 2N
    #    hex chars (N bytes times 2 hex chars per byte).  Inline
    #    LANG/LC_ALL so a future ctest failure in an exotic locale
    #    triages in ONE line.
    local expected_len=$(( size * 2 ))
    if [ "${#actual_hex}" -ne "$expected_len" ]; then
        echo "FAIL: synthetic-trace od pipeline produced ${#actual_hex} hex chars (LANG='$LANG' LC_ALL='$LC_ALL'), want $expected_len" >&2
        rm -f "$tmpfile"
        exit 1
    fi

    # 5. Per-byte equality check.  Side-by-side expected/got hex
    #    strings surface the byte-positions that disagree on failure.
    if [ "$actual_hex" != "$expected_hex" ]; then
        echo "FAIL: synthetic trace hex mismatch" >&2
        echo "  expected: $expected_hex" >&2
        echo "  got:      $actual_hex" >&2
        rm -f "$tmpfile"
        exit 1
    fi
}
