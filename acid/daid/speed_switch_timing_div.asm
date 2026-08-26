CHECK_REG equs "rDIV"
TEST_SIZE equ 32

INCLUDE "speed_switch_timing.inc"

label:
  db "DIV:", 0

preSwitch:
  ; We wait till DIV is $40, to we can see very well that DIV gets reset on a speed toggle.
  ld a, [rDIV]
  cp $40
  ret z
  jr preSwitch

expect:
  db $00, $00, $00, $00, $00, $00, $00, $00
  db $00, $00, $00, $00, $01, $01, $01, $01
  db $01, $01, $01, $01, $01, $01, $01, $01
  db $01, $02, $02, $02, $02, $02, $02, $02
