INCLUDE "common.inc"

SECTION "wram", WRAM0
wBuffer: ds TEST_SIZE

; Fill all interrupt vectors in case some emulator fires them incorrectly (I LOOK AT YOU VBA!)
SECTION "vblankInt", ROM0[$0040]
  reti
SECTION "statInt", ROM0[$0050]
  reti
SECTION "serialInt", ROM0[$0058]
  reti
SECTION "joypadInt", ROM0[$0060]
  reti

SECTION "main", ROM0[$150]
testStr:
  db "Speed switch timing:", 0

start:
  call disableLCD
  call initFont
  ld   hl, $9800
  ld   de, testStr
  call print

  ld   hl, $9820
  ld   de, label
  call print

  ; Setup palette
  ld  a, $80
  ld  [rBCPS], a
  ld  c, 64
  ld  hl, palette
palCopy:
  ld  a, [hl+]
  ld  [rBCPD], a
  dec c
  jr  nz, palCopy

  ; Setup halt to wait for vblank
  di
  ld  a, IEF_VBLANK
  ld  [rIE], a
  xor a
  ld  [rSTAT], a

  ; turn LCD on
  ld   a, LCDCF_ON | LCDCF_BGON
  ld   [rLCDC], a

  ; Wait for VBlank
  xor a
  ld [rIF], a
  halt

  ld   a, P1F_GET_NONE ; disable joypad input
  ld   [rP1], a
  xor  a  ; set a to zero to disable interrupts
  ld   [rIE], a
  inc  a  ; set a to 1 to switch speeds
  ld   [rKEY1], a
  call preSwitch
  stop ; switch

  ld   hl, wBuffer
REPT TEST_SIZE
  ld   a, [CHECK_REG]
  ld   [hl+], a
ENDR

  ld   de, wBuffer
  ld   bc, expect

N = 0
REPT TEST_SIZE / 8
  ld   a, 8
  ld   hl, $9842 + $20 * N
  call compareAndPrintResults
N=N+1
ENDR

loop:
  halt
  jr loop

palette:
  dw $ffff, $ffff, $0000, $0000
  dw $ffff, $ffff, $0000, $0000
  dw $ffff, $ffff, $0000, $0000
  dw $ffff, $ffff, $0000, $0000
  dw $0000, $0000, $ffff, $ffff
  dw $0000, $0000, $ffff, $ffff
  dw $0000, $0000, $ffff, $ffff
  dw $0000, $0000, $ffff, $ffff
