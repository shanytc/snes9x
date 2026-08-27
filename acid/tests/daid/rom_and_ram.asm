SECTION "header", ROM0[$104]
  ds $0147 - @, 0
  db CART_ROM_RAM
  ;db CART_ROM_MBC1_RAM
  db CART_ROM_32KB
  db CART_SRAM_2KB
  ;db CART_SRAM_8KB
  ds $0150 - @, 0
HEADER_DONE = 1

INCLUDE "common.inc"

SECTION "wram", WRAM0
wTestValue: ds 1
wInitialSRAM: ds 1
wPreEnableWriteSRAM: ds 1
wPostSRAMEnable: ds 1
wPostSRAMDisable: ds 1
wWrappedSRAMResult: ds 1

testSRAM: MACRO
    ld a, [wTestValue]
    ld [$A000], a
    ld a, [$A000]
    ld [\1], a
ENDM

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
  db "ROM+RAM:", 0

start:
  call disableLCD
  call initFont
  ld   hl, $9800
  ld   de, testStr
  call printStr

  ; Setup halt to wait for vblank
  di
  ld  a, IEF_VBLANK
  ld  [rIE], a
  xor a
  ld  [rSTAT], a

  ; turn LCD on
  ld   a, LCDCF_ON | LCDCF_BGON
  ld   [rLCDC], a

  ld   a, [$A000]
  ld   [wInitialSRAM], a
  inc  a
  ld   [wTestValue], a
  testSRAM wPreEnableWriteSRAM
  
  ld   a, CART_SRAM_ENABLE
  ld   [$0000], a
  ld   a, [wInitialSRAM]
  dec  a
  ld   [$A800], a
  testSRAM wPostSRAMEnable
  ld   a, [$A800]
  ld   [wWrappedSRAMResult], a
  ld   a, CART_SRAM_DISABLE
  ld   [$0000], a
  ld   a, [$A000]
  ld   [wPostSRAMDisable], a

  ld   hl, wTestValue
  ld   a, [wPostSRAMEnable]
  cp   [hl]
  jp   nz, noSRAM

  ld   hl, wTestValue
  ld   a, [wPostSRAMDisable]
  cp   [hl]
  call z, noSRAMEnable
  call nz, SRAMEnable

  ld   hl, wTestValue
  ld   a, [wWrappedSRAMResult]
  cp   [hl]
  jp   z, SRAM2KWrapped
  ld   hl, wInitialSRAM
  cp   [hl]
  jp   z, SRAM2KNoWrap
  jp   SRAMNot2K

haltLoop:
  halt
  jr haltLoop

noSRAMEnable:
  ld   hl, $9820
  ld   de, noSRAMEnableStr
  call printStr
  ret

noSRAMEnableStr:
  db "No SRAM enable req.", 0

SRAMEnable:
  ld   hl, $9820
  ld   de, SRAMEnableStr
  call printStr

  ld   hl, wTestValue
  ld   a, [wPreEnableWriteSRAM]
  cp   [hl]
  ret  nz

  ld   hl, $9840
  ld   de, SRAMInitEnabledStr
  call printStr
  ret

SRAMEnableStr:
  db "SRAM enable req.", 0

SRAMInitEnabledStr:
  db "SRAM initially enabled.", 0

noSRAM:
  ld   hl, $9820
  ld   de, noSRAMStr
  call printStr
  jp haltLoop

noSRAMStr:
  db "No SRAM available", 0

SRAM2KWrapped:
  ld   hl, $9840
  ld   de, SRAM2KWrappedStr
  call printStr
  jp haltLoop

SRAM2KWrappedStr:
  db "SRAM = 2K, wrapping", 0

SRAM2KNoWrap:
  ld   hl, $9840
  ld   de, SRAM2KNoWrapStr
  call printStr
  jp haltLoop

SRAM2KNoWrapStr:
  db "SRAM = 2K, no wrap", 0

SRAMNot2K:
  ld   hl, $9840
  ld   de, SRAMNot2KStr
  call printStr
  jp haltLoop

SRAMNot2KStr:
  db "SRAM > 2K", 0
