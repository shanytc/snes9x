INCLUDE "hardware.inc"

SECTION "entry", ROM0[$100]
  jp start
IF !DEF(HEADER_DONE)
  ds $0150 - @, 0
ENDC

SECTION "lib", ROMX
disableLCD:
  ld   a, [rLCDC]
  bit  7, a
  ret  z
  ; Disable the LCD, needs to happen during VBlank, or else we damage hardware
.waitForVBlank:
  ld   a, [rLY]
  cp   144
  jr   c, .waitForVBlank

  xor  a
  ld   [rLCDC], a ; disable the LCD by writting zero to LCDC
  ret

;   HL target
;   DE source
;   BC length
memcpy:
  ld  a, b
  or  c
  ret z
  ld  a, [de]
  ld  [hl+], a
  inc de
  dec bc
  jr  memcpy

initFont:
  ld  hl, $9010
  ld  de, fontData
  ld  bc, fontData.end - fontData
  jp  memcpy

; DE = string
; HL = vram
printStr:
  ld  a, [de]
  inc de
  and a
  ret z
  ld  c, a
.statWait:
  ld  a, [rSTAT]
  and $03
  jr  nz, .statWait
  ld  [hl], c
  inc hl
  jr  printStr

printHex:
  ld   c, a
  swap a
  and  $0f
  call printHexDigit
  ld   a, c
  and  $0f
  jp printHexDigit

printHexDigit:
  cp   $0a
  jr   nc, .upper
  add  $30
  ld   b, a
  jr   .statWait
.upper:
  add  $41 - 10
  ld   b, a
.statWait:
  ld  a, [rSTAT]
  and $03
  jr  nz, .statWait
  ld  [hl], b
  inc hl
  ret

; HL: vram target
; DE: results
; BC: expected results
; A:  amount of results
compareAndPrintResults:
  and a
  ret z
  push af
  ld   a, [de]
  push hl
  push bc
  call printHex
  pop  bc
  ld  a, [de]
  ld  h, a
  ld  a, [bc]
  cp  h
  pop hl
  jr  z, .ok
.error:
  ld  a, $01
  ld  [rVBK], a
  ld  a, $04
  ld  [hl+], a
  ld  [hl+], a
  xor a
  ld  [rVBK], a
  jr  .done
.ok:
  inc hl
  inc hl
.done:
  inc de
  inc bc
  pop af
  dec a
  jr  compareAndPrintResults

SECTION "font", ROMX
fontData:
  incbin "font.bin"
.end:
