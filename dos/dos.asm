; ESP2068 -- native disk for the TS2068: the DOS ROM.
;
; Assembled twice by tools/build_dos.py:
;
;   -DSIDE_HOME=0  the DOS ROM, 16K. The emulator overlays it on Z80
;                  0000h-3FFFh (chunks 0 and 1) while the module runs, in
;                  place of HOME; the 2068's own bank registers (ports F4h,
;                  FFh) are never touched, so a cartridge or the EXROM is
;                  exactly as it was afterwards.
;   -DSIDE_HOME=1  the HOME ROM's side of the switch: the bytes that go in
;                  HOME's free 3CDCh-3CFFh pocket (genuine FFh filler).
;
; The switch is two ports on the emulator, value ignored (A is preserved):
;
;   OUT (P_DOS_ON),A    the DOS ROM appears at 0000h-3FFFh
;   OUT (P_DOS_OFF),A   HOME is back
;
; Each takes effect at the very next instruction fetch, at PC, in the other
; ROM. So the code on either side of a switch sits at the same addresses in
; both images: HOME's half of the pocket continues where the DOS ROM's half
; leaves off, and the other way round. The ASSERTs keep the two sides in
; step.
;
; The module runs with interrupts off. With the DOS ROM in place, an
; interrupt would go to its 0038h, not HOME's. Every HOME call from the
; module goes through HOMECALL, which turns interrupts back on in HOME, so
; the keyboard, "scroll?" and BREAK work as usual.
;
; Errors are never raised inside the module. It returns NZ with the RST 8
; code in A, and HOME raises the report once the DOS ROM is gone. A report
; raised by HOME code the module called (BREAK at "scroll?", say) unwinds
; with HOME in place, which is also correct. There is no bank stack to leak.
;
; The engine is the emulator itself: OUT (P_SVC),A with the function in A
; runs it synchronously and puts its answer in A.

        DEVICE  NOSLOT64K

; --- The emulator's ports (include/DiskEngine.h) -------------------------------
P_SVC           EQU $D1            ; OUT: A = function; A comes back with the answer
P_DOS_ON        EQU $D3
P_DOS_OFF       EQU $D5

SVC_CAT_OPEN    EQU $01            ; BC = length, DE = text of a folder (BC = 0: the
                                   ;   current one). A = 0, or a report code
SVC_CAT_CHAR    EQU $02            ; A = the listing's next character, 0 at the end

; --- RST 8 codes (the report is code + 1) --------------------------------------
R_NONSENSE      EQU $0B            ; C Nonsense in BASIC

; --- The 2068 -------------------------------------------------------------------
CH_ADD          EQU $5C5D          ; the next character of the BASIC line
X_PTR           EQU $5C5F          ; where the syntax error marker goes
CR              EQU $0D
TOK_CAT         EQU $CF

H_PRINT_A       EQU $0010          ; HOME: RST 10h, print A on the current channel
H_ERROR_3       EQU $0055          ; HOME: LD (IY+0),L / LD SP,(ERR_SP) / JP 1354h,
                                   ;   RST 8 after it has fetched the code
H_CHAN_OPEN     EQU $1230          ; HOME: CHAN-OPEN, select stream A
H_CHECK_END     EQU $1B44          ; HOME: CHECK-END: CALL SYNTAX-Z / RET NZ / POP BC /
                                   ;   POP BC / STMT-NEXT (1B4Ah)

;==============================================================================
; The switch, at the same addresses in both ROMs: HOME 3CDCh-3CFCh
;==============================================================================

POCKET          EQU $3CDC
POCKET_END      EQU $3D00          ; CHRSET starts here

        IF SIDE_HOME

        ORG     POCKET
; H_CAT -- CAT's stub at 25C8h jumps here (stock: LD B,CFh / JR 25D6h).
H_CAT:  ld      b,TOK_CAT
; H_ENTER -- B = the keyword. Both the syntax pass and the run.
H_ENTER:
        di
        out     (P_DOS_ON),a
        ASSERT  $ == L_D_ENTER
        ds      5,$FF              ; the DOS ROM's CALL DOS_MAIN / OUT (P_DOS_OFF),A
        ASSERT  $ == L_H_DONE
; H_DONE -- back from the module: Z done, NZ with the report code in A.
; Done goes through CHECK-END: at run time it returns, and so do we, to
; the statement loop; on the syntax pass it drops our return address (the
; statement loop's) and goes on to the next statement. A plain RET there
; would run the statement loop's run-time code, which jumps to the line in
; NEWPPC and syntax-checks a program line in place, inserting its hidden
; numbers a second time.
H_DONE: ei
        jr      nz,.report
        call    H_CHECK_END
        ret
.report:
        ld      l,a
        jp      H_ERROR_3
        ASSERT  $ == L_D_HOMECALL
        ds      2,$FF              ; the DOS ROM's OUT (P_DOS_OFF),A
        ASSERT  $ == L_H_CALL
; H_CALL -- HOMECALL's HOME half: call HL with interrupts on.
H_CALL: ei
        call    H_JP_HL
        di
        out     (P_DOS_ON),a
        ASSERT  $ == L_D_CALL_RET
        ds      1,$FF              ; the DOS ROM's RET
        ds      1,$FF
H_JP_HL:
        jp      (hl)
        ASSERT  $ <= POCKET_END

        ; HOME's side of the pocket only (build_dos.py patches these bytes).
        SAVEBIN "out/home_pocket.bin",POCKET,$-POCKET

        ELSE

        ORG     0
        BLOCK   $4000,$FF          ; unused DOS ROM is FFh, like an empty ROM

        ORG     $0038
        ret                        ; never reached (the module runs under DI)

        ORG     POCKET
        ds      5,$FF              ; HOME's H_CAT / H_ENTER
        ASSERT  $ == L_D_ENTER
D_ENTER:
        call    DOS_MAIN
        out     (P_DOS_OFF),a      ; flags survive the OUT
        ASSERT  $ == L_H_DONE
        ds      11,$FF             ; HOME's H_DONE
        ASSERT  $ == L_D_HOMECALL
; HOMECALL -- call the HOME routine at HL. A, F, BC, DE, IX go in; what it
; returns comes back. Interrupts are on while it runs.
HOMECALL:
D_HOMECALL:
        out     (P_DOS_OFF),a
        ASSERT  $ == L_H_CALL
        ds      7,$FF              ; HOME's H_CALL
        ASSERT  $ == L_D_CALL_RET
D_CALL_RET:
        ret

        ENDIF

; The pocket's layout, fixed: both sides check against these.
L_D_ENTER       EQU POCKET+5       ; 3CE1h  the DOS ROM: CALL DOS_MAIN
L_H_DONE        EQU POCKET+10      ; 3CE6h  HOME: back from the module
L_D_HOMECALL    EQU POCKET+21      ; 3CF1h  the DOS ROM: HOMECALL
L_H_CALL        EQU POCKET+23      ; 3CF3h  HOME: HOMECALL's HOME half
L_D_CALL_RET    EQU POCKET+30      ; 3CFAh  the DOS ROM: HOMECALL's RET

        IF SIDE_HOME == 0

;==============================================================================
; The module
;==============================================================================

        ORG     $0100

; DOS_MAIN -- B = the keyword. Returns Z when done, or NZ with A = the RST 8
; code of the report HOME is to raise.
DOS_MAIN:
        ld      a,b
        cp      TOK_CAT
        jr      z,CAT
NONSENSE:
        ld      a,R_NONSENSE
; REPORT -- A = a report code: NZ, and X_PTR set the way RST 8 sets it.
REPORT: ld      hl,(CH_ADD)
        ld      (X_PTR),hl
        or      a
        ret

;------------------------------------------------------------------------------
; CAT -- the current folder. (CAT "x" comes in a later phase.)
;------------------------------------------------------------------------------
CAT:    call    AT_END
        jr      nz,NONSENSE
        bit     7,(iy+1)
        ret     z                  ; the syntax pass: accepted
        ld      a,2
        ld      hl,H_CHAN_OPEN
        call    HOMECALL           ; the upper screen
        ld      bc,0               ; the current folder
        ld      a,SVC_CAT_OPEN
        out     (P_SVC),a
        and     a
        jr      nz,REPORT
.next:  ld      a,SVC_CAT_CHAR
        out     (P_SVC),a
        and     a
        ret     z                  ; the end of the listing
        ld      hl,H_PRINT_A
        call    HOMECALL
        jr      .next

;------------------------------------------------------------------------------
; Parsing
;------------------------------------------------------------------------------

; AT_END -- Z if the statement ends here (':' or the end of the line), with
; CH_ADD stepped over any spaces. A = the character.
AT_END: ld      hl,(CH_ADD)
.skip:  ld      a,(hl)
        cp      ' '
        jr      nz,.done
        inc     hl
        jr      .skip
.done:  ld      (CH_ADD),hl
        cp      CR
        ret     z
        cp      ':'
        ret

        ASSERT  $ <= POCKET

        SAVEBIN "out/dos.rom",0,$4000

        ENDIF
