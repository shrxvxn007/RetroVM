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
