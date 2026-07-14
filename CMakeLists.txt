cmake_minimum_required(VERSION 3.20)
project(RetroVM CXX)

# ---- Language / standard --------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to Release; the dispatch hot path needs real optimisations.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---- Hard requirement: Clang or GCC ---------------------------------------
# vm.cpp uses &&label (computed gotos) which is GCC/Clang-only. Fail the
# configure step early so the user gets a clear message instead of a
# confusing compile error from inside the engine.
if(NOT (CMAKE_CXX_COMPILER_ID MATCHES "^(Clang|AppleClang|GNU)$"))
    message(FATAL_ERROR
        "RetroVM requires Clang, AppleClang or GCC (computed-goto dispatch). Detected: "
        "${CMAKE_CXX_COMPILER_ID}")
endif()

# ---- Warning hygiene ------------------------------------------------------
add_compile_options(
    -Wall -Wextra -Wpedantic
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-missing-field-initializers
)

# ---- Optimisation for the hot dispatch loop -------------------------------
# -O3 + strict aliasing lets the compiler coalesce the decode/decode-again
# blocks in the tail-dispatch macro and inline mem_load_u32 into four
# narrow loads with no spill.
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O3 -fstrict-aliasing)
endif()

# ---- Library + executable --------------------------------------------------
add_library(retrovm_core STATIC
    src/vm.cpp
    src/assembler.cpp
    src/trace.cpp
    src/checkpoint.cpp
)
target_include_directories(retrovm_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_executable(retrovm src/main.cpp)
target_link_libraries(retrovm PRIVATE retrovm_core)

# ---- Tests ----------------------------------------------------------------
enable_testing()
# Built-in demo is a no-stdin sanity check. Returns 0 on success.
add_test(NAME retrovm_builtin_demo
         COMMAND retrovm)
set_tests_properties(retrovm_builtin_demo PROPERTIES
         ENVIRONMENT "LC_ALL=C")

# Phase 3 record+replay round-trip tests. Each test invokes the shared
# tests/record_replay.sh helper, which asserts (a) record exits 0,
# (b) replay exits 0, (c) the cycle count printed by dump_state matches
# across the two runs (cycle parity), and (d) the halt reason matches.
# WORKING_DIRECTORY is set to the source dir so the `programs/<bin>`
# relative paths in the helper resolve correctly (ctest otherwise runs
# from build/).
add_test(NAME retrovm_io_record_replay
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/record_replay.sh
                 $<TARGET_FILE:retrovm>
                 programs/io.bin
                 "5
7
9
")
set_tests_properties(retrovm_io_record_replay PROPERTIES
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
         ENVIRONMENT "LC_ALL=C")

add_test(NAME retrovm_divzero_record_replay
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/record_replay.sh
                 $<TARGET_FILE:retrovm>
                 programs/divzero.bin
                 "5
7
")
set_tests_properties(retrovm_divzero_record_replay PROPERTIES
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
         ENVIRONMENT "LC_ALL=C")

# Phase 1 trap-path coverage. stdin "0\n7\n" makes divzero.bin take the
# R3!=0 branch with R1=0 (default divisor), so cycle 8 fires
# `state_.pc -= 4u; state_.halted = HALT_DIV_ZERO;` on the DIV at pc=0x24,
# halting at cycles=8 with pc rewound to 0x24. The --expect-halt=2 and
# --expect-cycles=8 flags bound the assertion in addition to the parity
# check, so a regression that swaps the trap handler for "DIV returns 0
# on b==0" (no halt) would still trip the absolute-value gate. Negates
# "currently only the safe-path is covered, leaving one half of the
# trap-pc-rewind behaviour untested".
add_test(NAME retrovm_divzero_record_replay_trap
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/record_replay.sh
                 $<TARGET_FILE:retrovm>
                 programs/divzero.bin
                 "0
7
"
                 --expect-halt=2
                 --expect-cycles=8)
set_tests_properties(retrovm_divzero_record_replay_trap PROPERTIES
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
         ENVIRONMENT "LC_ALL=C")

# Phase 4 save+restore round-trip test. The helper records a trace, saves
# a .state file mid-run, then restores from it and re-runs replay from the
# saved frame_index. The load-bearing assertion is that the restore path's
# final cycle count and halt reason match a clean full-replay path
# exactly — the resumption must be byte-equivalent to having run the
# program from the start.
add_test(NAME retrovm_io_save_restore
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/save_restore.sh
                 $<TARGET_FILE:retrovm>
                 programs/io.bin
                 "5
7
9
"
                 --at-frame=1)
set_tests_properties(retrovm_io_save_restore PROPERTIES
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
         ENVIRONMENT "LC_ALL=C")

# Phase 4: same round-trip but for divzero.bin (deterministic RAND seed
# + 2 IN + 1 RAND = 3 frames at cycles 2/3/4). --at-frame=1 halts just
# before the second IN is consumed.
add_test(NAME retrovm_divzero_save_restore
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/save_restore.sh
                 $<TARGET_FILE:retrovm>
                 programs/divzero.bin
                 "5
7
"
                 --at-frame=1)
set_tests_properties(retrovm_divzero_save_restore PROPERTIES
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
         ENVIRONMENT "LC_ALL=C")

# Phase 4 edge case: save BEFORE any frame is consumed (--at-frame=0).
# This exercises the boundary condition where the restore re-fetches
# the very first IN/RAND instruction from the saved pc and consumes
# frame[0] on the very first cycle.
add_test(NAME retrovm_io_save_restore_at_frame_0
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/save_restore.sh
                 $<TARGET_FILE:retrovm>
                 programs/io.bin
                 "5
7
9
"
                 --at-frame=0)
set_tests_properties(retrovm_io_save_restore_at_frame_0 PROPERTIES
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
         ENVIRONMENT "LC_ALL=C")

# Phase 4 edge case: save AFTER the last frame has been recorded but
# before it is consumed (--at-frame=N-1). Restore must replay the
# final nondeterministic event then halt normally with cycles equal
# to the full-replay cycle count.
add_test(NAME retrovm_io_save_restore_at_frame_last
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/save_restore.sh
                 $<TARGET_FILE:retrovm>
                 programs/io.bin
                 "5
7
9
"
                 --at-frame=2)
set_tests_properties(retrovm_io_save_restore_at_frame_last PROPERTIES
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
         ENVIRONMENT "LC_ALL=C")

# Phase 4: 0-frame / no-nondeterministic path. snap.bin has only
# LI/STORE/HALT (no IN, no RAND) so the trace is empty and cmd_save
# would reject any --at-frame index. The helper detects this case
# and gates on record+replay cycle+halt parity instead, providing a
# regression guard for the deterministic-only path.
add_test(NAME retrovm_snap_save_restore
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/save_restore.sh
                 $<TARGET_FILE:retrovm>
                 programs/snap.bin
                 "")
set_tests_properties(retrovm_snap_save_restore PROPERTIES
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
         ENVIRONMENT "LC_ALL=C")

# Phase 4+ offline checkpoint reader. The helper saves to /tmp/<bin>.state
# via cmd_save (which spins up a full replay session), then calls
# `retrovm inspect <state>` and asserts the printed columns follow the
# dump_state() layout verbatim — frame_index, halt reason (always 6 =
# HALT_CHECKPOINT under cmd_save's current hardcoded policy), pc/sp hex,
# flags (ZCS) bitmask, and exactly 8 regs tokens. Also asserts the
# on-disk file is exactly 64 B (16 B header + 48 B body), catching any
# future wire-format regression that pads/trims the body.
add_test(NAME retrovm_io_inspect
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/inspect.sh
                 $<TARGET_FILE:retrovm>
                 programs/io.bin
                 "5
7
9
"
                 --at-frame=1)
set_tests_properties(retrovm_io_inspect PROPERTIES
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
         ENVIRONMENT "LC_ALL=C")

# Phase 4+ offline checkpoint inspect — early-boundary mirror of
# retrovm_io_inspect. --at-frame=0 makes cmd_save's sink fire BEFORE
# any IN handler has run, so R0=R1=0 (both LI placeholders are still
# in regs). This gates the off-by-one in the snapshot cycle: a
# regression that fires the sink one cycle too late would show
# R0=11 instead of R0=0 — the opposite failure mode of
# retrovm_io_inspect's "too late would show R1=22" gate. Together
# the two ctests pin both edges of the save-sink cycle window for
# io.bin, distinguishing "save sink captured the wrong cycle" from
# "trace was rebuilt incorrectly".
add_test(NAME retrovm_io_inspect_at_frame_0
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/inspect.sh
                 $<TARGET_FILE:retrovm>
                 programs/io.bin
                 "5
7\n9\n"
                 --at-frame=0)
set_tests_properties(retrovm_io_inspect_at_frame_0 PROPERTIES
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
         ENVIRONMENT "LC_ALL=C")

# Phase 5 REPL debugger smoke test. tests/debug.sh records a trace,
# then scripts a debug session (regs / where / breakpoint 0x08 /
# continue / regs / breakpoint -d 0x08 / step 1 / where / save <path> /
# quit) and asserts (a) every command is echoed as `(retrovm) <cmd>`,
# (b) the BP-fire panic appears with cycles>=3 and halt reason 7, (c)
# the REPL `save` writes a 64-byte checkpoint with the RVMSTATE magic,
# (d) the final halt reason is 8 (HALT_STEP) after the post-BP step.
# Replay mode (`--trace`) makes the run deterministic for ctest.
add_test(NAME retrovm_io_debug
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/debug.sh
                 $<TARGET_FILE:retrovm>
                 programs/io.bin
                 "5\n7\n9\n")
set_tests_properties(retrovm_io_debug PROPERTIES
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
         ENVIRONMENT "LC_ALL=C")

# Phase 5 REPL smoke test, fresh-mode sibling. tests/debug_fresh.sh
# runs `retrovm debug <bin> --batch <script>` WITHOUT `--trace`,
# exercising the parallel IOHook path: IN reads from stdin via the
# VM's default `scanf("%u")` hook, RAND draws from a thread-local
# mt19937 seeded from random_device, and `on_nondeterministic` is
# empty so the replay-mode `[trace]` lines do NOT appear. The
# helper scripts regs / where / step 5 / regs / where / quit and
# asserts (a) every command echoes back, (b) `where` emits both a
# `pc  : 0x...` and a `dec : ...` line, (c) no `[trace]` line
# appears (load-bearing fresh-mode signal), (d) cycle progression
# 0 → 5 lands at HALT_STEP=8. Together with retrovm_io_debug this
# ctest pair pins Phase 5's two IOHook code paths in main.cpp.
add_test(NAME retrovm_io_debug_fresh
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/debug_fresh.sh
                 $<TARGET_FILE:retrovm>
                 programs/io.bin
                 "5
7
9
")
set_tests_properties(retrovm_io_debug_fresh PROPERTIES
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
         ENVIRONMENT "LC_ALL=C")

# Phase 4+ inspect --diff: side-by-side per-field delta of two
# checkpoints with equal columns suppressed. The helper saves at
# --at-frame=0 and --at-frame=1 on programs/io.bin, runs inspect
# --diff on the two .state files, and asserts the diff output
# contains EXACTLY the fields that actually changed (frame_index,
# pc, R0) and NONE of the equal fields (halt reason — always
# HALT_CHECKPOINT=6; sp — constant 0xfffffc; flags — neither
# path sets any bits; R1..R7 — both still LI placeholders). This
# is the load-bearing regression guard for the "suppress equal
# columns" contract.
add_test(NAME retrovm_io_inspect_diff
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/inspect_diff.sh
                 $<TARGET_FILE:retrovm>
                 programs/io.bin
                 "5
7\n9\n"
                 --at-frame=0
                 --at-frame=1)
set_tests_properties(retrovm_io_inspect_diff PROPERTIES
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
         ENVIRONMENT "LC_ALL=C")

# Phase 6 offline .trace reader. The helper records a trace on
# programs/io.bin (re-using any pre-existing trace to keep re-runs
# cheap), runs `retrovm trace programs/io.bin.trace`, and asserts
# the label-based summary against the trace wire-format invariants:
# banner, magic `"RVMTRACE"`, version 1, frame_count equals the
# geometric (size-16)/16, and for io.bin's known 2 IN + 1 RAND
# layout (cycles 3/4/5) the op_IN / op_RAND / cycle_first /
# cycle_last / cycle_delta counts all match. Mirrors the Phase 4+
# `inspect` ctest suite in style and labelling.
add_test(NAME retrovm_io_inspect_trace
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/inspect_trace.sh
                 $<TARGET_FILE:retrovm>
                 programs/io.bin
                 "5
7
9
")
set_tests_properties(retrovm_io_inspect_trace PROPERTIES
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
         ENVIRONMENT "LC_ALL=C")
