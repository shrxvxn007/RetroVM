; programs/io.asm
; Phase 3 demo: reads two inputs from stdin (via IN), one random value (RAND),
; then stores all three at known memory addresses for replay verification.
;
;   R0 = IN         (stdin input #1)
;   R1 = IN         (stdin input #2)
;   R2 = RAND       (deterministic PRNG)
;   mem[0x100] = R0 ; mem[0x104] = R1 ; mem[0x108] = R2
;
; Trace frames emitted at cycles 3, 4, 5
; (each frame: [cycle=3..5, opcode=OP_IN|OP_RAND, value=consumed]).
;
; Layout (each instruction is 4 bytes):
;   0x00  LI    R0, 0          ; placeholder
;   0x04  LI    R1, 0          ; placeholder
;   0x08  IN    R0             ; cycle 3
;   0x0C  IN    R1             ; cycle 4
;   0x10  RAND  R2             ; cycle 5
;   0x14  STORE R0, 0x100
;   0x18  STORE R1, 0x104
;   0x1C  STORE R2, 0x108
;   0x20  HALT

LI    R0, 0
LI    R1, 0
IN    R0
IN    R1
RAND  R2
STORE R0, 0x100
STORE R1, 0x104
STORE R2, 0x108
HALT
