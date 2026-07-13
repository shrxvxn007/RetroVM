; programs/snap.asm
; Phase-4 delta-checkpointing test program.
;
; Layout (10 instructions = 40 B):
;   0x00  LI    R0, 0x11              ; cycle 1
;   0x04  LI    R1, 0x22              ; cycle 2
;   0x08  LI    R2, 0x33              ; cycle 3 -- snapshot #1 (no deltas)
;   0x0C  STORE R0, 0x100             ; cycle 4
;   0x10  STORE R0, 0x100             ; cycle 5 -- RLE fold
;   0x14  STORE R0, 0x100             ; cycle 6 -- snapshot #2 (1 MemDelta, run=3, old=0)
;   0x18  STORE R1, 0x200             ; cycle 7
;   0x1C  STORE R2, 0x300             ; cycle 8
;   0x20  STORE R2, 0x300             ; cycle 9 -- RLE fold; snapshot #3 (2 MemDeltas, last run=2)
;   0x24  HALT                        ; cycle 10
;
; Expected snap files (interval=3):
;   programs/snap.bin.snap.0000  72 B   (32 hdr + 40 regs + 0 deltas)
;   programs/snap.bin.snap.0001  80 B   (32 hdr + 40 regs + 1 delta)
;   programs/snap.bin.snap.0002  88 B   (32 hdr + 40 regs + 2 deltas)
;
; Delta contents at each snapshot (page=0 in all):
;   snap.0001: (run=3, page=0, offset=0x100, old_value=0)
;   snap.0002: (run=1, page=0, offset=0x200, old_value=0)
;              (run=2, page=0, offset=0x300, old_value=0)

LI    R0, 0x11
LI    R1, 0x22
LI    R2, 0x33
STORE R0, 0x100
STORE R0, 0x100
STORE R0, 0x100
STORE R1, 0x200
STORE R2, 0x300
STORE R2, 0x300
HALT
