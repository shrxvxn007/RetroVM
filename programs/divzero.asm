; programs/divzero.asm
; Deterministic-but-non-traceable Phase-1 test program: two IN reads pick
; divisor candidates, one RAND picks which candidate is the divisor, and
; EITHER chosen divisor being 0 trips HALT_DIV_ZERO.
;
; Two-branch dispatch driven by R3 (RAND):
;   * R3 != 0  -> divisor = R1 (default), divide 7 / R1
;   * R3 == 0  -> divisor = R2 (swap),  divide 7 / R2
;
;   stdin "5\n7\n"  -> safe:        R3!=0, DIV 7/5=1, HALT (cycles = 10)
;   stdin "0\n7\n"  -> trap:        R3!=0, DIV 7/0, HALT_DIV_ZERO (cycles = 10)
;   stdin "7\n0\n"  -> safe or trap: depends on whether RAND draws zero/non-zero
;                                (R3!=0 -> R1=7 safe; R3==0 -> R2=0 traps)
;
; Trace (`retrovm record`, deterministic-RAND seed 0xC0FFEE): same 3 frames at
; cycles 2, 3, 4 (IN, IN, RAND) -- the BRANCHING makes replay cycle counts
; sensitive to the recorded RAND value.
;
; Layout (each instruction = 4 bytes):
;   0x00  LI    R4, 7         ; numerator
;   0x04  IN    R1             ; cycle 2 (trace frame: OP_IN)
;   0x08  IN    R2             ; cycle 3 (trace frame: OP_IN)
;   0x0C  RAND  R3             ; cycle 4 (trace frame: OP_RAND)
;   0x10  LI    R5, 0
;   0x14  ADD   R5, R1         ; R5 = R1 (default divisor)
;   0x18  JNZ   R3, 0x24       ; if RAND != 0 -> skip the swap, use R1
;   0x1C  LI    R5, 0          ; swap path begins: R5 = 0
;   0x20  ADD   R5, R2         ; swap path: R5 = R2
;   0x24  DIV   R4, R5         ; <- JNZ target AND swap path's natural next
;   0x28  STORE R4, 0x100      ; only reached on safe path
;   0x2C  HALT

LI    R4, 7
IN    R1
IN    R2
RAND  R3
LI    R5, 0
ADD   R5, R1
JNZ   R3, 0x24
LI    R5, 0
ADD   R5, R2
DIV   R4, R5
STORE R4, 0x100
HALT
