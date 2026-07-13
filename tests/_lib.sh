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
#   get_field "LABEL" "$CONTENT"
#     Returns the trimmed text following `LABEL:` on the first line of
#     $CONTENT whose first characters match LABEL literally. Uses
#     awk's `index()` (raw substring match — no regex specials are
#     interpreted in LABEL), so labels like `flags (ZCS):` and
#     `pc / sp     :` match without escaping. Returns nothing if no
#     line in $CONTENT begins with LABEL.
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

get_field() {
    awk -v L="$1" '
        index($0, L) == 1 {
            rest = substr($0, length(L) + 1)
            sub(/^[[:space:]]+/, "", rest)
            sub(/[[:space:]]+$/, "", rest)
            print rest
            exit
        }
    ' <<< "$2"
}

pc_or_sp() {
    case "$1" in
        pc) printf '%s' "${2% / *}" ;;
        sp) printf '%s' "${2#* / }" ;;
    esac
}
