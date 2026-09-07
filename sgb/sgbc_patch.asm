; ===========================================================================
;  Super Game Boy Color patch
;
;  The BIOS runs in bank $00 (bank $80 is its FastROM mirror), so every JML
;  back into it names bank $00.
;
;  Three hooks, all exact-size drop-ins:
;    $80:848F  JSR nullsub_1 / JMP $847B  ->  JSL sgbc_frame  / BRA $847B
;    $80:BA84  LDA $0280 / EOR #1         ->  JML sgbc_gbframe / NOP
;    $80:C4BF  SEI / LDA $4210            ->  JML sgbc_commit
; ===========================================================================

; --- BIOS WRAM.  Every one of these is reached with DB = $7E. --------------
MODE      = $0100       ; top-level mode; 15 = SGB run mode
SUBSTATE  = $0101       ; state within the mode; 4 = cart running
PALDIRTY  = $0210       ; sub_808570 uploads SGB palettes 0-3 when set
REFRESH   = $0217       ; makes the IRQ take its full-refresh path
SGBPAL    = $0400       ; SGB palettes 0-3: 4 x 4 x BGR555
LIVEPAL   = $C220
HDR_DEST  = $0652       ; cart header $014A, destination code
BAND      = $0294       ; ICD2 scanline band sub_80B9BE last processed, 0-17

; --- our state block ------------------------------------------------------
; $7E:CE00 sits in the gap between the highest WRAM
ST        = $CE00
ST_MAG0   = ST+0
ST_MAG1   = ST+1
ST_STATE  = ST+2
ST_BAND   = ST+3        ; unused since the per-frame hook moved to sub_80BA84
ST_FIXES  = ST+4        ; diagnostic: reassert count, saturating
ST_TMP    = ST+5
ST_SAVE   = ST+8

MAGIC0    = $53         ; 'S'
MAGIC1    = $43         ; 'C'
MARKER    = $43         ; 'C', what the emulator stamps into $014A

STATE_IDLE  = 0
STATE_KEYED = 1
STATE_LEFT  = 2         ; no marker: this cart is not ours

    .org $82D9A7

; ===========================================================================
;  sgbc_frame -- called once per frame from the BIOS main loop, after the
;  joypad read and the state machine.  JSL/RTL.
; ===========================================================================
    .width ax=8
sgbc_frame:
    PHP
    PHB
    REP #$30
    PHA
    PHX
    PHY
    SEP #$30
    CLD
    LDA #$7E
    PHA
    PLB                     ; DB = $7E from here down

    LDA ST_MAG0             ; the state block is self-validating, so a stray
    CMP #MAGIC0             ; writer can never leave us acting on garbage
    BNE sf_init
    LDA ST_MAG1
    CMP #MAGIC1
    BEQ sf_ready
sf_init:
    LDA #MAGIC0
    STA ST_MAG0
    LDA #MAGIC1
    STA ST_MAG1
    STZ ST_STATE
    STZ ST_BAND
    STZ ST_FIXES
sf_ready:
    LDA MODE
    CMP #15
    BEQ sf_inmode
    STZ ST_STATE            ; left SGB run mode entirely: start over
    BRA sf_done
sf_inmode:
    LDA ST_STATE
    CMP #STATE_KEYED
    BEQ sf_live
    CMP #STATE_LEFT
    BEQ sf_done

    LDA SUBSTATE            ; idle: wait for the cart to take over
    CMP #4
    BCC sf_done
    LDA HDR_DEST            ; the emulator's marker, or hands off this cart
    CMP #MARKER
    BEQ sf_arm
    LDA #STATE_LEFT
    STA ST_STATE
    BRA sf_done
sf_arm:
    LDA #STATE_KEYED
    STA ST_STATE
    STZ ST_FIXES
sf_live:
    JSR service
sf_done:
    REP #$30
    PLY
    PLX
    PLA
    PLB
    PLP
    RTL

; ===========================================================================
;  sgbc_gbframe -- displaces the LDA $0280 / EOR #1 at the head of sub_80BA84,
;  which the ICD2 row scanner calls once per Game Boy frame (band wrap).  Cold:
;  the scanner's own entry is polled hundreds of times a frame.
; ===========================================================================
    .width ax=8
sgbc_gbframe:
    PHP
    PHB
    REP #$30
    PHA
    PHX
    PHY
    SEP #$30
    CLD
    LDA #$7E
    PHA
    PLB
    LDA ST_MAG0             ; sgbc_frame owns initialisation; if it has not
    CMP #MAGIC0             ; run yet there is nothing to maintain
    BNE sg_out
    JSR service
sg_out:
    REP #$30
    PLY
    PLX
    PLA
    PLB
    PLP
    .width ax=8             ; PLP put M back; the assembler cannot see that
    LDA $0280               ; the two instructions we displaced (DB = $7E here)
    EOR #1
    JML $00BA89

; ===========================================================================
;  sgbc_commit -- displaces the SEI at the head of sub_80C4BF, the wrapper
;  every SGB palette and attribute handler calls to push its work to the
;  hardware.  
; ===========================================================================
    .width ax=8
sgbc_commit:
    SEI                     ; the displaced instruction, kept first so the
    PHP                     ; pushed P carries I set and PLP leaves it set
    PHB
    REP #$30
    PHA
    PHX
    PHY
    SEP #$30
    CLD
    LDA #$7E
    PHA
    PLB
    LDA ST_MAG0
    CMP #MAGIC0
    BNE sc_out
    LDA ST_STATE
    CMP #STATE_KEYED
    BNE sc_out
    JSR save_pal            ; what the cart just asked for, for the emulator
    JSR write_keys
sc_out:
    REP #$30
    PLY
    PLX
    PLA
    PLB
    PLP
    LDA $004210             ; the displaced LDA RDNMI
    JML $00C4C3

; ===========================================================================
;  service -- keep the keys installed.  Called from both per-frame hooks.
; ===========================================================================
    .width ax=8
service:
    LDA MODE
    CMP #15
    BNE sv_out
    LDA SUBSTATE            ; states below 4 are the splash and the menus,
    CMP #4                  ; which run palettes of their own
    BCC sv_out
    LDA ST_STATE
    CMP #STATE_KEYED
    BNE sv_out
    JSR check_keys
    BCS sv_out
    JSR save_pal
    JSR write_keys
    LDA #1
    STA PALDIRTY
    STA REFRESH
    LDA ST_FIXES            ; saturate: this is a diagnostic, not a limit
    CMP #$FF
    BEQ sv_out
    INC ST_FIXES
sv_out:
    RTS

; ---------------------------------------------------------------------------
;  write_keys -- the three keys into colors 1-3 of all four SGB palettes and
;  into the scheme staging buffer.  Color 0 is left alone everywhere.
; ---------------------------------------------------------------------------
    .width ax=8
write_keys:
    LDY #2                  ; SGBPAL byte offset: color 1 of palette 0
wk_pal:
    LDX #0
wk_col:
    LDA sgbc_keys,X
    STA SGBPAL,Y
    INX
    INY
    CPX #6
    BNE wk_col
    INY                     ; over color 0 of the next palette
    INY
    CPY #34                 ; 2 + 4 * 8
    BNE wk_pal
    LDX #0
wk_live:
    LDA sgbc_keys,X
    STA LIVEPAL+2,X
    INX
    CPX #6
    BNE wk_live
    RTS

; ---------------------------------------------------------------------------
;  save_pal -- colors 1-3 of SGB palettes 0-3 into ST_SAVE, unless palette 0
;  already holds the keys (then it is ours, and the last save stands).
; ---------------------------------------------------------------------------
    .width ax=8
save_pal:
    LDA SGBPAL+2
    CMP #$1F
    BNE sp_go
    LDA SGBPAL+3
    CMP #$7C
    BEQ sp_out
sp_go:
    LDY #2
    LDX #0
sp_pal:
    LDA #6
    STA ST_TMP
sp_col:
    LDA SGBPAL,Y
    STA ST_SAVE,X
    INX
    INY
    DEC ST_TMP
    BNE sp_col
    INY                     ; over color 0 of the next palette
    INY
    CPX #24
    BNE sp_pal
sp_out:
    RTS

; ---------------------------------------------------------------------------
;  check_keys -- carry set when the keys are still installed.  Once a GB frame
;  and inside the scanner, so cheap: one entry per buffer a palette write
;  cannot miss (PAL01/03 hit palette 0, PAL23/12 palette 2, schemes LIVEPAL).
; ---------------------------------------------------------------------------
    .width ax=8
check_keys:
    LDA SGBPAL+2            ; palette 0, color 1
    CMP #$1F
    BNE ck_bad
    LDA SGBPAL+3
    CMP #$7C
    BNE ck_bad
    LDA SGBPAL+18           ; palette 2, color 1
    CMP #$1F
    BNE ck_bad
    LDA SGBPAL+19
    CMP #$7C
    BNE ck_bad
    LDA LIVEPAL+2
    CMP #$1F
    BNE ck_bad
    LDA LIVEPAL+3
    CMP #$7C
    BNE ck_bad
    SEC
    RTS
ck_bad:
    CLC
    RTS

; ---------------------------------------------------------------------------
;  The keys, BGR555 little-endian, one per GB shade 1-3.  R31 B31 with G
;  0/8/16: the emulator matches them through the SNES brightness LUT, and
;  these stay apart under it except next to black, where nothing shows.
; ---------------------------------------------------------------------------
sgbc_keys:
    .db $1F, $7C            ; shade 1  $7C1F
    .db $1F, $7D            ; shade 2  $7D1F
    .db $1F, $7E            ; shade 3  $7E1F
