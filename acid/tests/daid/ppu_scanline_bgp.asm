INCLUDE "common.inc"

SECTION "vblankInt", ROM0[$040]
  jp vblankInt

SECTION "statInt", ROM0[$048]
  jp statInt

SECTION "main", ROM0
start:
  ld a, [rLY]
  cp 145
  jr nz, start
  ld a, 1
  ld hl, $9000
REPT 7
  ld [hl+], a
  inc hl
ENDR
  ld a, $ff
  ld [hl+], a

  ld a, LCDCF_BGON | LCDCF_ON
  ld [rLCDC], a
  ld a, STATF_LYC
  ld [rSTAT], a
  ld a, IEF_LCDC | IEF_VBLANK
  ld [rIE], a
  xor a
  ld [rLYC], a
  ld c, low(rBGP)
  ei
  halt

vblankInt:
  pop hl
  ei
  halt

statInt:
  pop hl
  ei
  nop
  ld hl, data
loop:
REPT 10
  ld a, [hl+]
  ld [c], a
ENDR
REPT 98 - 12 - 16
  nop
ENDR
  jp loop

data:
REPT 8
  db $e4, $e4, $e4, $e4, $e4, $e4, $e4, $e4, $e4, $e4
ENDR
REPT 16
  db $e4, $e4, $aa, $aa, $aa, $aa, $aa, $aa, $e4, $e4
ENDR
REPT 16
  db $e4, $aa, $55, $55, $55, $55, $55, $55, $aa, $e4
ENDR
REPT 16
  db $e4, $aa, $55, $aa, $55, $55, $aa, $55, $aa, $e4
ENDR
REPT 16
  db $e4, $aa, $55, $55, $55, $55, $55, $55, $aa, $e4
ENDR
REPT 8
  db $e4, $aa, $55, $55, $55, $55, $55, $55, $aa, $e4
ENDR
REPT 8
  db $e4, $aa, $55, $aa, $55, $55, $aa, $55, $aa, $e4
ENDR
REPT 8
  db $e4, $aa, $55, $aa, $aa, $aa, $aa, $55, $aa, $e4
ENDR
REPT 8
  db $e4, $aa, $55, $55, $aa, $aa, $55, $55, $aa, $e4
ENDR
REPT 16
  db $e4, $aa, $55, $55, $55, $55, $55, $55, $aa, $e4
ENDR
REPT 16
  db $e4, $e4, $aa, $aa, $aa, $aa, $aa, $aa, $e4, $e4
ENDR
REPT 16
  db $e4, $e4, $e4, $e4, $e4, $e4, $e4, $e4, $e4, $e4
ENDR
