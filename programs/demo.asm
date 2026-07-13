; programs/demo.asm
; Demonstrates LI, ADD (R2 += Rs), STORE, LOAD, JNZ (register-not-zero), HALT.
;
;   R0=5  R1=7  R2 = R0+R1 = 12  mem[0x100]=12  R3=mem[0x100]=12
;   JNZ R2 jumps forward (R2 != 0), skipping the two dead HALT
;   instructions, then loads R5=99 and stores to 0x104.
;
; ADD is "Rd += Rs", so we seed R2 with R0 first, then add R1.
;
; Layout (each instruction is 4 bytes):
;   0x00  LI    R0,5
;   0x04  LI    R1,7
;   0x08  LI    R2,5         ; seed R2 = R0
;   0x0C  ADD   R2,R1        ; R2 = 5 + 7 = 12
;   0x10  STORE R2,0x100
;   0x14  LOAD  R3,0x100
;   0x18  JNZ   R2,0x24      ; R2 != 0 -> jump forward
;   0x1C  HALT               ; never reached
;   0x20  HALT               ; never reached
;   0x24  LI    R5,99        ; <- jump lands here
;   0x28  STORE R5,0x104
;   0x2C  HALT

LI    R0, 5
LI    R1, 7
LI    R2, 5
ADD   R2, R1
STORE R2, 0x100
LOAD  R3, 0x100
JNZ   R2, 0x24
HALT
HALT
LI    R5, 99
STORE R5, 0x104
HALT
