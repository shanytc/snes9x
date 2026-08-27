INCLUDE "common.inc"

SECTION "wram", WRAM0
wGBC: ds 1

SECTION "main", ROM0[$150]
stopTestStr:
  db "STOP in mode3:", 0
stopTestStrOk:
  db "LCD on: PASS", 0
stopTestNotStoppingStr:
  db "STOP not stopping...", 0

start:
  ld   [wGBC], a
  call disableLCD
  call initFont
  ld   hl, $9900
  ld   de, stopTestStr
  call print
  ld   hl, $9920
  ld   de, stopTestStrOk
.noGBC:
  call print

  ld   a, LCDCF_ON | LCDCF_BGON
  ld   [rLCDC], a

  ; Set the screen to black with white text
  ld a, $0f
  ld [rBGP], a
  ; React from stop for any input.
  ld a, P1F_GET_BTN & P1F_GET_DPAD
  ld [rP1], a

  ; Setup halt to wait for vblank
  di
  ld a, IEF_VBLANK
  ld [rIE], a

  xor a
  ld [rIF], a
  halt
  xor a
  ld [rIF], a
  halt
  xor a
  ld [rIF], a
  halt

.waitMode3:
  ld a, [rSTAT]
  and $03
  cp  $03
  jr  nz, .waitMode3

  stop

  ld   hl, $9920
  ld   de, stopTestNotStoppingStr
  call print

loop:
  halt
  jr loop
