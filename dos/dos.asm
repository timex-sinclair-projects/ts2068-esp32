; ESP2068 -- native disk for the TS2068: the ROM side.
;
; Assembled three times by tools/build_dos.py, with -DSIDE=n:
;
;   SIDE=0  the DOS ROM, 16K. The emulator overlays it on Z80 0000h-3FFFh
;           (chunks 0 and 1) while the module runs, in place of HOME; the
;           2068's own bank registers (ports F4h, FFh) are never touched,
;           so a cartridge or the EXROM is exactly as it was afterwards.
;   SIDE=1  HOME's side of the switch: the bytes that go in HOME's free
;           3CDCh-3CFFh pocket (genuine FFh filler), and the channel stubs
;           and hook trampolines in HOME's dead 1488h-14C6h.
;   SIDE=2  the EXROM's tape hooks, in its unused 1800h-1BFFh block (BLK2,
;           FFh filler in every EXROM): PGPSTR's call in SLVM, R_TAPE,
;           W_TAPE and SAVE. They make service calls directly; the EXROM
;           stays paged in.
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
; code in A (REPORT resets the stack to the module's entry first), and
; HOME raises the report once the DOS ROM is gone. A report raised by HOME
; code the module called (BREAK at "scroll?", say) unwinds with HOME in
; place, which is also correct. There is no bank stack to leak.
;
; The engine is the emulator itself: OUT (P_SVC),A with the function in A
; runs it synchronously and puts its answer in A.

        DEVICE  NOSLOT64K

; --- The emulator's ports and services (include/DiskEngine.h) ------------------
P_SVC           EQU $D1            ; OUT: A = function; A comes back with the answer
P_DOS_ON        EQU $D3
P_DOS_OFF       EQU $D5

SVC_CAT_OPEN    EQU $01            ; HL = 0: the current folder; HL = 1: BC = length,
                                   ;   DE = text ("" the mounted TAP, x.tap, a folder)
SVC_CAT_CHAR    EQU $02            ; A = the listing's next character, 0 at the end
SVC_TAPE_NAME   EQU $03            ; DE/BC the name SLVM popped; (TADDR) the command
SVC_R_TAPE      EQU $04            ; (SP) = the caller's AF, IX, DE: R_TAPE's
SVC_W_TAPE      EQU $05            ; (SP) = the caller's AF, IX, DE: W_TAPE's
SVC_TAPE_OURS   EQU $06            ; A = 1 if SAVE goes to the engine (a TAP or a file)
SVC_CLOSE_ALL   EQU $07            ; CLOSE #*: unmount the TAP
SVC_FORMAT      EQU $08            ; DE/BC: a new TAP, mounted
SVC_REPLACE     EQU $09            ; L = 1: Y to "Replace?", 0: N (the answer: D)
SVC_MOVE_TO     EQU $0A            ; DE/BC: change folder
SVC_MOVE        EQU $0B            ; DE/BC TO IX/HL: rename
SVC_COPY        EQU $0C            ; DE/BC TO IX/HL: copy
SVC_ERASE       EQU $0D            ; DE/BC: delete
SVC_LIST        EQU $0E            ; DE/BC: show a file
SVC_ANSWER      EQU $0F            ; L = 1 Y, 0 N, 2 another key
SVC_END         EQU $10            ; the report the text ends with, or 0
OUT_ASK         EQU $01            ; CAT_CHAR: a question; read a key, then ANSWER
SVC_CH_OPEN     EQU $11            ; DE/BC the spec, IX/HL the mode; (STREAM_N), (CH_ARG)
SVC_CH_OUT      EQU $12            ; E = a byte for CURCHL's channel
SVC_CH_IN       EQU $13            ; CURCHL's next byte at CH_BYTE (0), CH_EOF (1)
SVC_CH_CLOSE    EQU $14            ; L = the stream
SVC_CH_RESTORE  EQU $15            ; L = the stream: back to its start
SVC_CH_CAT      EQU $16            ; CAT #s: HL, DE/BC as CAT_OPEN; IX = the stream
SVC_PR_COPY     EQU $17            ; COPY: the screen to /print/copyNNN.bmp
SVC_PICK_CHAR   EQU $18            ; the picker: A = the next byte to print, FFh at the end
SVC_PICK_KEY    EQU $19            ; E/D/L/H = keyboard rows EFh/F7h/BFh/7Fh: 0 the next
                                   ;   key, 1 print again
SVC_PICK_END    EQU $1A            ; T_PICK: still picking; else the command's answer
T_PICK          EQU $FE            ; TAPE_NAME, MOVE_TO: the picker is open
PICK_DONE       EQU $FF            ; PICK_CHAR: nothing more to print

; Answers of the tape services, below the report codes (all 4 or more).
T_NOT_OURS      EQU 0              ; the stock routine runs
T_DONE          EQU 1              ; R_TAPE: loaded (carry); TAPE_NAME: a TAP was
                                   ;   mounted, load from its start (the name goes)
T_FAILED        EQU 2              ; R_TAPE: no carry, as a tape error
T_NATIVE        EQU 2              ; TAPE_NAME: a native file; the name SLVM is to
                                   ;   use is NAME_LEN bytes at NAME_BUF
T_ASK           EQU 3              ; TAPE_NAME: the same, once PROMPT_BUF (the
                                   ;   question, 0-ended) is answered Y

; Where the engine leaves things for the hooks (5CCCh-5EE9h is free).
NAME_LEN        EQU $5E0F
NAME_BUF        EQU $5E10          ; 10 bytes
PROMPT_BUF      EQU $5E20          ; up to 64 bytes and a 0
CH_ARG          EQU $5E0C          ; OPEN #'s record length, for CH_OPEN
CH_BYTE         EQU $5E0E          ; CH_IN's byte

; --- RST 8 codes (the report is code + 1) --------------------------------------
R_NONSENSE      EQU $0B            ; C Nonsense in BASIC
R_MEMORY        EQU $03            ; 4 Out of memory
R_STREAM        EQU $17            ; O Invalid stream

; --- The 2068 -------------------------------------------------------------------
CH_ADD          EQU $5C5D          ; the next character of the BASIC line
X_PTR           EQU $5C5F          ; where the syntax error marker goes
FLAGS           EQU $5C3B          ; bit 7 set: run time
STKEND          EQU $5C65          ; the calculator stack's end
DOS_SP          EQU $5E00          ; the module's entry SP (5CCCh-5EE9h is free;
                                   ;   TS-Pico uses 5DCDh-5DDBh)
CR              EQU $0D
TOK_CAT         EQU $CF
TOK_FORMAT      EQU $D0
TOK_CLOSE       EQU $D4
TOK_MOVE        EQU $D1
TOK_ERASE       EQU $D2
TOK_COPY        EQU $FF
TOK_TO          EQU $CC
TOK_INKEY       EQU $A6
TOK_VALS        EQU $AE
TOK_STRS        EQU $C1
TOK_CHRS        EQU $C2
T_ADDR          EQU $5C74
T_LIST          EQU $19B0          ; T_ADDR in a LIST statement: its parameter list
                                   ;   (19AFh) + 1, where class 05h leaves it
T_RESTORE       EQU $19D1          ; T_ADDR in a RESTORE statement (19D0h + 1)
STREAM_N        EQU $5CCB          ; the stream OPEN #/CLOSE # work on (140Fh sets it)
STRMS           EQU $5C10          ; streams -3..15, two bytes each
CHANS           EQU $5C4F
CURCHL          EQU $5C51
PROG            EQU $5C53

H_PRINT_A       EQU $0010          ; HOME: RST 10h, print A on the current channel
H_ERROR_3       EQU $0055          ; HOME: LD (IY+0),L / LD SP,(ERR_SP) / JP 1354h,
                                   ;   RST 8 after it has fetched the code
H_CLOSE         EQU $139F          ; HOME: CLOSE #'s routine (class 06h stacked the n)
H_CHAN_OPEN     EQU $1230          ; HOME: CHAN-OPEN, select stream A
H_CHECK_END     EQU $1B44          ; HOME: CHECK-END: CALL SYNTAX-Z / RET NZ / POP BC /
                                   ;   POP BC / STMT-NEXT (1B4Ah)
H_EXPT_1NUM     EQU $1BE5          ; HOME: class 06h, a numeric expression
H_EXPT_STR      EQU $1BEF          ; HOME: class 0Ah, a string expression
H_EXPT_STR_ONLY EQU $1BEF          ; (the same: class 0Ah doesn't check the end)
H_LIST          EQU $1545          ; HOME: LIST, class 05h: it parses its own #n and n
H_RESTORE       EQU $1E9D          ; HOME: RESTORE after class 03h (the number stacked)
H_USE_ZERO      EQU $1C51          ; HOME: a 0 on the calculator stack at run time
H_FIND_INT2     EQU $1F23          ; HOME: the number on the stack -> BC (Report B)
H_STREAM        EQU $140F          ; HOME: the number on the stack is a stream: Report O
                                   ;   past 15; (STREAM_N), HL = its STRMS entry, BC = it
H_MAKE_ROOM     EQU $12BB          ; HOME: BC bytes after (HL), Report 4
H_RECLAIM       EQU $1750          ; HOME: remove BC bytes at HL
H_OPEN_STOCK    EQU $1465          ; HOME: OPEN # of K, S or P (HL = the STRMS entry)
H_CLOSE_STOCK   EQU $13BE          ; HOME: CLOSE # of K, S or P (HL, BC)
H_CLOSE_RESET   EQU $13A8          ; HOME: CLOSE #'s reset of the STRMS entry at HL
H_EXPT_STR_RT   EQU $1BEF          ; (H_EXPT_STR)

; The EXROM (SIDE=2).
X_ERROR         EQU $0010          ; EXROM: XRST8 after it has fetched the code:
                                   ;   LD (IY+0),L / LD SP,(ERRSP) / back to HOME
X_CALL_BANK     EQU $0F99          ; EXROM: CALL BANK, with a frame (the HOME routine,
                                   ;   FF00h, 0, 0) pushed; A, BC, DE, HL go through
X_SLVM_RET      EQU $0218          ; SLVM after CALL BANK PGPSTR: POP IX ...
X_R_TAPE_3      EQU $00FF          ; R_TAPE after INC D / EX AF,AF' / DEC D
X_W_TAPE_3      EQU $006B          ; W_TAPE after LD HL,00E5h
X_SAVE_3        EQU $0854          ; SAVE after PUSH HL / LD A,FDh
HOOKS           EQU $1800          ; BLK2

;==============================================================================
; The switch, at the same addresses in both ROMs: HOME 3CDCh-3CFCh
;==============================================================================

POCKET          EQU $3CDC
POCKET_END      EQU $3D00          ; CHRSET starts here

; The pocket's layout, fixed: both sides check against these.
L_D_ENTER       EQU POCKET+3       ; 3CDFh  the DOS ROM: CALL DOS_MAIN
L_H_DONE        EQU POCKET+8       ; 3CE4h  HOME: back from the module
L_D_HOMECALL    EQU POCKET+22      ; 3CF2h  the DOS ROM: HOMECALL
L_H_CALL        EQU POCKET+24      ; 3CF4h  HOME: HOMECALL's HOME half
L_D_CALL_RET    EQU POCKET+31      ; 3CFBh  the DOS ROM: HOMECALL's RET

; HOME's dead 1488h-14C6h (a SYSCON remnant nothing reaches; TS-Pico puts its
; trampolines there too): the hooks' switch, and the channel stubs.
DEAD            EQU $1488
DEAD_END        EQU $14C7          ; 14C7h is OPEN #'s K/S/P table
L_D_HOOK        EQU DEAD+9         ; 1491h  the DOS ROM: CALL D_HOOK / OUT (P_DOS_OFF),A
L_H_HOOKED      EQU DEAD+14        ; 1496h  HOME: back from D_HOOK
A_CH_OUT        EQU DEAD+20        ; 149Ch  HOME: an F record's output routine
A_CH_IN         EQU DEAD+29        ; 14A5h  HOME: its input routine
A_T_XHL         EQU DEAD+56        ; 14C0h  HOME: EX DE,HL / JP (IX)

        IF SIDE == 1

        ORG     POCKET
; H_ENTER -- the disk keywords' stub at 25D6h jumps here, B = the keyword
; (25C8h-25D4h: LD B,token / JR 25D6h; CLOSE #'s at 25D9h). Both the
; syntax pass and the run.
H_ENTER:
        di
        out     (P_DOS_ON),a
        ASSERT  $ == L_D_ENTER
        ds      5,$FF              ; the DOS ROM's CALL DOS_MAIN / OUT (P_DOS_OFF),A
        ASSERT  $ == L_H_DONE
; H_DONE -- back from the module:
;   C        go on to the HOME routine at HL (CLOSE #n: the stock CLOSE #)
;   NC, Z    done, through CHECK-END: at run time it returns, and so do we,
;            to the statement loop; on the syntax pass it drops our return
;            address (the statement loop's) and goes on to the next
;            statement. A plain RET there would run the statement loop's
;            run-time code, which jumps to the line in NEWPPC and
;            syntax-checks a program line in place, inserting its hidden
;            numbers a second time.
;   NC, NZ   the report whose code is in A
H_DONE: ei
        jp      c,H_JP_HL
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
H_JP_HL:
        jp      (hl)
        ASSERT  $ <= POCKET_END

        ; HOME's side of the pocket only (build_dos.py patches these bytes).
        SAVEBIN "out/home_pocket.bin",POCKET,$-POCKET

        ORG     DEAD
; T_OPEN -- OPEN #'s CALL 1465h at 145Eh comes here: HL = the STRMS entry,
; the spec string on the calculator stack, CH_ADD on what follows it.
T_OPEN: ld      a,1
        jr      T_GO
; T_CLOSE -- CLOSE #'s CALL 13BEh at 13A5h: BC = the stream's offset, HL =
; its STRMS entry.
T_CLOSE:
        ld      a,2
; T_GO -- into the DOS ROM's D_HOOK with A = 1 or 2. It comes back Z to
; return (to the stock routine, if it pushed one), NZ with a report code.
T_GO:   di
        out     (P_DOS_ON),a
        ASSERT  $ == L_D_HOOK
        ds      5,$FF              ; the DOS ROM's CALL D_HOOK / OUT (P_DOS_OFF),A
        ASSERT  $ == L_H_HOOKED
        ei
        ret     z
CH_ERR: ld      l,a
        jp      H_ERROR_3
; CH_OUT -- an F record's output routine: A = the byte (RST 10h).
        ASSERT  $ == A_CH_OUT
CH_OUT: ld      e,a
        ld      a,SVC_CH_OUT
        out     (P_SVC),a
        and     a
        ret     z
        jr      CH_ERR
; CH_IN -- its input routine: C with A = a byte; NC NZ at the end of the
; file, which WAIT-KEY (11D9h) makes Report 8 and INKEY$ # an empty string.
        ASSERT  $ == A_CH_IN
CH_IN:  ld      a,SVC_CH_IN
        out     (P_SVC),a
        cp      2
        jr      nc,CH_ERR
        and     a
        ret     nz
        ld      a,(CH_BYTE)
        scf
        ret
; T_OSYN -- OPEN #'s syntax pass, at 1438h, with CH_ADD on the comma after
; the spec: the mode (a string), then perhaps a comma and the record length.
; The stock 2569h skipped the rest unchecked, and with it the hidden form of
; a number. 143Bh's CHECK-END then wants the end of the statement.
T_OSYN: rst     20h
        call    H_EXPT_STR
        rst     18h
        cp      ','
        ret     nz
        rst     20h
        jp      H_EXPT_1NUM
; T_XHL -- for HOMECALL: call IX with HL = DE (HOMECALL can't pass HL).
        ASSERT  $ == A_T_XHL
T_XHL:  ex      de,hl
        jp      (ix)
        ASSERT  $ <= DEAD_END

        SAVEBIN "out/home_dead.bin",DEAD,$-DEAD

        ENDIF

;==============================================================================
; The EXROM's tape hooks (SIDE=2)
;==============================================================================

        IF SIDE == 2

        ORG     HOOKS

; HOMECALLX -- call a HOME routine from the EXROM, the way the EXROM does
; (SAVE's CHAN-OPEN at 0857h): the routine's address goes in a CALL BANK frame.
        MACRO   HOMECALLX addr
        push    ix
        exx
        ld      hl,addr
        push    hl
        ld      hl,$FF00           ; HOME, no horizontal select
        push    hl
        ld      hl,0
        push    hl
        push    hl
        exx
        call    X_CALL_BANK
        pop     ix
        ENDM

; X_PGPSTR -- SLVM's CALL BANK PGPSTR at 0215h calls here instead (run time
; only: SLVM skips it on the syntax pass). The name goes to the engine, which
; may mount a TAP (LOAD "game.tap": the name then goes, so the LOAD takes the
; TAP's first program), take a native file (SLVM then gets the name the
; engine left at NAME_BUF: for SAVE the tape header's, for LOAD none), ask
; first ("Replace?"), or refuse the command.
X_PGPSTR:
        inc     sp
        inc     sp                 ; drop the return to 0218h: CALL BANK reads the
                                   ;   frame SLVM pushed above it
        call    X_CALL_BANK        ; PGPSTR: DE = the text, BC = its length
        push    af
        ld      a,SVC_TAPE_NAME
        out     (P_SVC),a
        cp      T_PICK
        call    z,X_PICK           ; LOAD "": the picker, then the name picked
        and     a
        jr      z,.go              ; T_NOT_OURS
        cp      T_DONE
        jr      z,.noname
        cp      T_NATIVE
        jr      z,.native
        cp      T_ASK
        jr      nz,X_REPORT
        call    X_ASK              ; Y, or a report
.native:
        ld      de,NAME_BUF
        ld      a,(NAME_LEN)
        ld      c,a
        ld      b,0
        jr      .go
.noname:
        ld      bc,0               ; LOAD "": the first program on the TAP
.go:    pop     af
        jp      X_SLVM_RET

; X_ASK -- print the question at PROMPT_BUF on the lower screen and wait for
; Y or N (SPACE is N too). The engine hears the answer; N is its report.
X_ASK:  ld      a,$FD              ; the lower screen
        HOMECALLX H_CHAN_OPEN
        ld      hl,PROMPT_BUF
.print: ld      a,(hl)
        and     a
        jr      z,.keys
        push    hl
        HOMECALLX H_PRINT_A
        pop     hl
        inc     hl
        jr      .print
.keys:  set     5,(iy+2)           ; TV_FLAG: clear the lower screen next time
.up:    xor     a                  ; wait until no key is down
        in      a,($FE)
        and     $1F
        cp      $1F
        jr      nz,.up
.down:  ld      a,$DF              ; Y: row P-Y, bit 4
        in      a,($FE)
        bit     4,a
        ld      l,1
        jr      z,.answer
        ld      a,$7F              ; N: row SPACE-B, bit 3; SPACE bit 0
        in      a,($FE)
        and     $09
        cp      $09
        jr      z,.down
        ld      l,0
.answer:
        ld      a,SVC_REPLACE
        out     (P_SVC),a
        and     a
        ret     z
        jr      X_REPORT

; X_REPORT -- raise the report whose code is in A, from the EXROM.
X_REPORT:
        ld      l,a
        push    hl
        ld      hl,(CH_ADD)
        ld      (X_PTR),hl
        pop     hl
        jp      X_ERROR

; X_PICK -- the file picker for LOAD/VERIFY/MERGE "": print what the
; engine sends on the upper screen, and hand it the keys, until it has an
; answer for TAPE_NAME (in A).
X_PICK: ld      a,$FE              ; the upper screen
        HOMECALLX H_CHAN_OPEN
.print: ld      a,SVC_PICK_CHAR
        out     (P_SVC),a
        cp      PICK_DONE
        jr      z,.end
        HOMECALLX H_PRINT_A
        jr      .print
.end:   ld      a,SVC_PICK_END
        out     (P_SVC),a
        cp      T_PICK
        ret     nz
.up:    xor     a                  ; wait until no key is down
        in      a,($FE)
        and     $1F
        cp      $1F
        jr      nz,.up
.down:  xor     a                  ; then for one
        in      a,($FE)
        and     $1F
        cp      $1F
        jr      z,.down
        ld      a,$EF              ; 6 7 8 (and 0 9)
        in      a,($FE)
        ld      e,a
        ld      a,$F7              ; 5
        in      a,($FE)
        ld      d,a
        ld      a,$BF              ; ENTER
        in      a,($FE)
        ld      l,a
        ld      a,$7F              ; SPACE
        in      a,($FE)
        ld      h,a
        ld      a,SVC_PICK_KEY
        out     (P_SVC),a
        and     a
        jr      z,.up
        jr      .print

; X_R_TAPE -- R_TAPE (00FCh) jumps here. A = the flag wanted (00h a header,
; FFh data), carry set to LOAD (clear to VERIFY), IX = where, DE = how many.
X_R_TAPE:
        push    af
        ld      a,SVC_R_TAPE
        out     (P_SVC),a
        and     a
        jr      z,.stock
        inc     sp
        inc     sp                 ; drop the AF
        cp      3
        jp      nc,X_REPORT        ; not on the TAP: F
        rra                        ; T_DONE: carry; T_FAILED: none
        ret
.stock: pop     af
        inc     d                  ; R_TAPE's first three bytes
        ex      af,af'
        dec     d
        jp      X_R_TAPE_3

; X_W_TAPE -- W_TAPE (0068h) jumps here. A = the flag, IX = from, DE = how many.
X_W_TAPE:
        push    af
        ld      a,SVC_W_TAPE
        out     (P_SVC),a
        and     a
        jr      z,.stock
        inc     sp
        inc     sp
        cp      3
        jp      nc,X_REPORT
        ret
.stock: pop     af
        ld      hl,$00E5           ; W_TAPE's first instruction
        jp      X_W_TAPE_3

; X_SAVE -- SAVE (0851h) jumps here. HL = the data, IX = the header. With a
; TAP mounted or a native file: no "Start tape, then press any key", no
; second's pause, the header and the data straight to the engine.
X_SAVE: ld      a,SVC_TAPE_OURS
        out     (P_SVC),a
        and     a
        jr      nz,.tap
        push    hl                 ; SAVE's first two instructions
        ld      a,$FD
        jp      X_SAVE_3
.tap:   push    hl
        push    ix
        ld      de,17
        xor     a
        call    X_W_TAPE
        pop     ix
        ld      e,(ix+$0B)
        ld      d,(ix+$0C)         ; the data's length, from the header
        ld      a,$FF
        pop     ix
        jp      X_W_TAPE

        ASSERT  $ <= $1C00

        SAVEBIN "out/exrom_hooks.bin",HOOKS,$-HOOKS

        ENDIF

;==============================================================================
; The DOS ROM (SIDE=0)
;==============================================================================

        IF SIDE == 0

        ORG     0
        BLOCK   $4000,$FF          ; unused DOS ROM is FFh, like an empty ROM

        ORG     $0038
        ret                        ; never reached (the module runs under DI)

        ORG     POCKET
        ds      3,$FF              ; HOME's H_ENTER
        ASSERT  $ == L_D_ENTER
D_ENTER:
        call    DOS_MAIN
        out     (P_DOS_OFF),a      ; flags and HL survive the OUT
        ASSERT  $ == L_H_DONE
        ds      14,$FF             ; HOME's H_DONE
        ASSERT  $ == L_D_HOMECALL
; HOMECALL -- call the HOME routine at HL. A, F, BC, DE, IX go in; what it
; returns comes back. Interrupts are on while it runs.
HOMECALL:
        out     (P_DOS_OFF),a
        ASSERT  $ == L_H_CALL
        ds      7,$FF              ; HOME's H_CALL
        ASSERT  $ == L_D_CALL_RET
        ret

        ORG     L_D_HOOK
        call    D_HOOK
        out     (P_DOS_OFF),a      ; flags and registers survive the OUT
        ASSERT  $ == L_H_HOOKED

;------------------------------------------------------------------------------
; The module
;------------------------------------------------------------------------------

        ORG     $0100

; DOS_MAIN -- B = the keyword (LIST comes with no token in B: T_ADDR says
; it's LIST). Returns NC Z when done, NC NZ with A = the RST 8 code of the
; report HOME is to raise, or C with HL = a HOME routine to go on to.
DOS_MAIN:
        ld      (DOS_SP),sp
        ld      hl,(T_ADDR)
        ld      de,T_LIST
        and     a
        sbc     hl,de
        jp      z,LIST
        ld      hl,(T_ADDR)
        ld      de,T_RESTORE
        and     a
        sbc     hl,de
        jp      z,RESTORE
        ld      a,b
        cp      TOK_CAT
        jr      z,CAT
        cp      TOK_FORMAT
        jp      z,FORMAT
        cp      TOK_MOVE
        jp      z,MOVE
        cp      TOK_ERASE
        jp      z,ERASE
        cp      TOK_CLOSE
        jp      z,CLOSE
        cp      TOK_COPY
        jp      z,COPY
NONSENSE:
        ld      a,R_NONSENSE
; REPORT -- A = a report code: back to DOS_MAIN's caller from any depth,
; NC NZ, with X_PTR set the way RST 8 sets it.
REPORT: ld      sp,(DOS_SP)
        ld      hl,(CH_ADD)
        ld      (X_PTR),hl
        or      a
        ret

;------------------------------------------------------------------------------
; CAT          the current folder
; CAT "dir"    another folder; CAT "*.tap" the matches
; CAT "x.tap"  the blocks in a TAP
; CAT ""       the mounted TAP's blocks, > at the pointer
;------------------------------------------------------------------------------
CAT:    call    AT_END
        ld      hl,0
        jr      nz,.arg
        call    RUNTIME
        ret     z                  ; the syntax pass: accepted
        jr      .open
.arg:   cp      '#'
        jr      z,CAT_CH
        call    EXPT_STR_END
        ret     z
        call    POP_STR
        ld      hl,1
.open:  ld      a,SVC_CAT_OPEN
        jp      RUN_OUT

;------------------------------------------------------------------------------
; CAT #s         the listing CAT gives, as text into the file open on stream s
; CAT #s;"x"     CAT "x"'s
;------------------------------------------------------------------------------
CAT_CH: call    NEXT_CHAR
        ld      hl,H_EXPT_1NUM
        call    HOMECALL
        call    AT_END
        ld      hl,0
        jr      z,.end
        cp      ';'
        jp      nz,NONSENSE
        call    NEXT_CHAR
        call    EXPT_STR_END
        ret     z
        call    POP_STR
        ld      hl,1
        jr      .go
.end:   call    RUNTIME
        ret     z
.go:    push    hl
        push    de
        push    bc
        call    F_STREAM           ; L = the stream, or Report O
        push    hl
        pop     ix
        pop     bc
        pop     de
        pop     hl
        ld      a,SVC_CH_CAT
        jp      SVC_ONLY

;------------------------------------------------------------------------------
; FORMAT "x.tap" / FORMAT "x"   a new, empty TAP, mounted
; FORMAT "dir/"                 a new folder
;------------------------------------------------------------------------------
FORMAT: call    AT_END
        jp      z,NONSENSE         ; the name is required
        call    EXPT_STR_END
        ret     z
        call    POP_STR
        ld      a,SVC_FORMAT
        jr      SVC_ONLY

;------------------------------------------------------------------------------
; MOVE TO "dir"        change folder ("..", "/", "-" the previous one)
; MOVE "old" TO "new"  rename; into a folder if "new" is one
;------------------------------------------------------------------------------
MOVE:   call    SKIP_SPACES
        cp      TOK_TO
        jr      nz,.two
        call    NEXT_CHAR
        call    EXPT_STR_END
        ret     z
        call    POP_STR
        ld      a,SVC_MOVE_TO
        out     (P_SVC),a
        cp      T_PICK
        call    z,PICK             ; MOVE TO "": the picker
        and     a
        jp      nz,REPORT
        ret
.two:   call    TWO_NAMES
        ret     z
        ld      a,SVC_MOVE
; SVC_ONLY -- a service with nothing to print: OK or its report.
SVC_ONLY:
        out     (P_SVC),a
        and     a
        jp      nz,REPORT
        ret

;------------------------------------------------------------------------------
; PICK -- the folder picker for MOVE TO "": as the EXROM's X_PICK, through
; HOMECALL. Answers MOVE_TO's answer in A.
;------------------------------------------------------------------------------
PICK:   ld      a,2                ; the upper screen
        ld      hl,H_CHAN_OPEN
        call    HOMECALL
.print: ld      a,SVC_PICK_CHAR
        out     (P_SVC),a
        cp      PICK_DONE
        jr      z,.end
        ld      hl,H_PRINT_A
        call    HOMECALL
        jr      .print
.end:   ld      a,SVC_PICK_END
        out     (P_SVC),a
        cp      T_PICK
        ret     nz
.up:    xor     a
        in      a,($FE)
        and     $1F
        cp      $1F
        jr      nz,.up
.down:  xor     a
        in      a,($FE)
        and     $1F
        cp      $1F
        jr      z,.down
        ld      a,$EF
        in      a,($FE)
        ld      e,a
        ld      a,$F7
        in      a,($FE)
        ld      d,a
        ld      a,$BF
        in      a,($FE)
        ld      l,a
        ld      a,$7F
        in      a,($FE)
        ld      h,a
        ld      a,SVC_PICK_KEY
        out     (P_SVC),a
        and     a
        jr      z,.up
        jr      .print

;------------------------------------------------------------------------------
; COPY                 the screen to the printer, which is the card: the
;                      engine writes /print/copyNNN.bmp
; COPY "a" TO "b"      copy a file; a pattern copies into a folder, listing
;                      each name as copied or exists
;------------------------------------------------------------------------------
COPY:   call    AT_END
        jr      nz,.two
        call    RUNTIME
        ret     z                  ; the syntax pass: accepted
        ld      a,SVC_PR_COPY
        jr      SVC_ONLY
.two:   call    TWO_NAMES
        ret     z
        ld      a,SVC_COPY
        jr      RUN_OUT

;------------------------------------------------------------------------------
; ERASE "x"       delete a file, silently
; ERASE "dir/"    remove an empty folder
; ERASE "*.bak"   "Erase name (Y/N)?" for each match: Y erases, N stops,
;                 any other key skips
;------------------------------------------------------------------------------
ERASE:  call    EXPT_STR_END
        ret     z
        call    POP_STR
        ld      a,SVC_ERASE
        jr      RUN_OUT

;------------------------------------------------------------------------------
; LIST "file"            show a file: text as text, anything else in hex
; LIST, LIST n, LIST #n  the stock LIST, on both passes (it checks itself)
; Which form it is goes by how the expression starts: scanning it to find
; out would insert hidden numbers twice on the syntax pass, and evaluate it
; twice at run time.
;------------------------------------------------------------------------------
LIST:   call    AT_END
        jr      z,.home
        cp      '#'
        jr      z,.home
        call    STR_NEXT
        jr      nz,.home
        call    EXPT_STR_END
        ret     z
        call    POP_STR
        ld      a,SVC_LIST
        jr      RUN_OUT
.home:  ld      hl,H_LIST
        scf
        ret

;------------------------------------------------------------------------------
; RUN_OUT -- A = a service whose text goes to the upper screen. Opens
; channel 2, makes the call, then prints what it left (CAT_CHAR), asking
; for a key where it says to (OUT_ASK, then ANSWER), and ends with END's
; report, if any.
;------------------------------------------------------------------------------
RUN_OUT:
        push    af
        push    hl
        push    de
        push    bc
        push    ix
        ld      a,2
        ld      hl,H_CHAN_OPEN
        call    HOMECALL           ; the upper screen
        pop     ix
        pop     bc
        pop     de
        pop     hl
        pop     af
        out     (P_SVC),a
        and     a
        jp      nz,REPORT
.next:  ld      a,SVC_CAT_CHAR
        out     (P_SVC),a
        and     a
        jr      z,.end
        cp      OUT_ASK
        jr      z,.ask
        ld      hl,H_PRINT_A
        call    HOMECALL
        jr      .next
.ask:   call    GET_KEY
        ld      a,SVC_ANSWER
        out     (P_SVC),a
        and     a
        jp      nz,REPORT
        jr      .next
.end:   ld      a,SVC_END
        out     (P_SVC),a
        and     a
        jp      nz,REPORT
        ret                        ; NC Z

; GET_KEY -- wait for a key: L = 1 Y, 0 N (or SPACE), 2 any other. Reads
; the keyboard ports directly (the module runs under DI).
GET_KEY:
.up:    xor     a                  ; first, until no key is down
        in      a,($FE)
        and     $1F
        cp      $1F
        jr      nz,.up
.down:  ld      a,$DF              ; Y: row P-Y, bit 4
        in      a,($FE)
        bit     4,a
        ld      l,1
        ret     z
        ld      a,$7F              ; N: row SPACE-B, bit 3; SPACE bit 0
        in      a,($FE)
        and     $09
        cp      $09
        ld      l,0
        ret     nz
        xor     a                  ; any other key
        in      a,($FE)
        and     $1F
        cp      $1F
        ld      l,2
        ret     nz
        jr      .down

;------------------------------------------------------------------------------
; CLOSE #*    close every file channel and unmount the TAP
; CLOSE #n    the stock CLOSE #: class 06h here, then 139Fh at run time (an F
;             record comes back to D_CLOSE through 13A5h)
;------------------------------------------------------------------------------
CLOSE:  call    SKIP_SPACES
        cp      '*'
        jr      nz,.stream
        call    NEXT_CHAR
        call    AT_END
        jp      nz,NONSENSE
        call    RUNTIME
        ret     z
        xor     a
.files: push    af
        ld      (STREAM_N),a
        call    STRMS_HL
        ld      c,(hl)
        inc     hl
        ld      b,(hl)
        call    F_RECORD
        call    z,CLOSE_REC        ; its report waits: the rest close first
        pop     af
        inc     a
        cp      16
        jr      c,.files
        ld      a,SVC_CLOSE_ALL
        jp      SVC_ONLY
.stream:
        ld      hl,H_EXPT_1NUM
        call    HOMECALL
        call    AT_END
        jp      nz,NONSENSE
        call    RUNTIME
        ret     z
        ld      hl,H_CLOSE
        scf
        ret

;------------------------------------------------------------------------------
; RESTORE #n            back to the start of the file open on stream n
; RESTORE, RESTORE n    the stock RESTORE: class 03h's part here, then 1E9Dh
;------------------------------------------------------------------------------
RESTORE:
        call    AT_END
        jr      z,.zero
        cp      '#'
        jr      z,.chan
        ld      hl,H_EXPT_1NUM
        call    HOMECALL
        call    AT_END
        jp      nz,NONSENSE
        jr      .home
.zero:  ld      hl,H_USE_ZERO      ; nothing on the syntax pass
        call    HOMECALL
.home:  call    RUNTIME
        ret     z
        ld      hl,H_RESTORE
        scf
        ret
.chan:  call    NEXT_CHAR
        ld      hl,H_EXPT_1NUM
        call    HOMECALL
        call    AT_END
        jp      nz,NONSENSE
        call    RUNTIME
        ret     z
        call    F_STREAM
        ld      a,SVC_CH_RESTORE
        jp      SVC_ONLY

;------------------------------------------------------------------------------
; File channels: OPEN # and CLOSE #'s hooks
;
; An F record is 6 bytes in CHANS, just before its closing 80h: CH_OUT,
; CH_IN (HOME's stubs, so the record holds no address that moves), 'F' and
; the stream. The engine keeps the file. Sixteen records keep every STRMS
; offset below 80h, as CHAN-OPEN (1239h) needs.
;------------------------------------------------------------------------------
REC_LEN         EQU 6

; D_HOOK -- T_GO comes here with A = 1 (OPEN #, 145Eh) or 2 (CLOSE #, 13A5h)
; and their registers. Returns Z (with the stock routine's address pushed
; under the return, when the channel is K, S or P), or NC NZ with a report.
D_HOOK: ld      (DOS_SP),sp
        dec     a
        jr      z,D_OPEN

; D_CLOSE -- BC = the stream's offset, HL = its STRMS entry. An F record:
; closed, then HL = the entry for 13A8h to reset.
D_CLOSE:
        push    hl
        call    F_RECORD
        jr      nz,.stock
        pop     de
        call    CLOSE_REC
        and     a
        jp      nz,REPORT
        call    STRMS_HL
        xor     a
        ret
.stock: pop     hl
        ld      c,(hl)
        inc     hl
        ld      b,(hl)
        dec     hl
        ld      de,H_CLOSE_STOCK
        jr      HOOK_STOCK

; HOOK_STOCK -- go back to HOME, on to the stock routine at DE.
HOOK_STOCK:
        pop     ix                 ; the return to L_D_HOOK's OUT
        push    de
        push    ix
        xor     a
        ret

; D_OPEN -- the spec is on the calculator stack. "x:..." is the engine's; K,
; S, P and anything else go to the stock 1465h.
D_OPEN: ld      hl,(STKEND)
        dec     hl
        ld      b,(hl)
        dec     hl
        ld      c,(hl)
        dec     hl
        ld      d,(hl)
        dec     hl
        ld      e,(hl)             ; DE/BC: the spec, still stacked
        ld      a,b
        and     a
        jr      nz,.long
        ld      a,c
        cp      2
        jr      c,.stock
.long:  inc     de
        ld      a,(de)
        cp      ':'
        jr      z,.ours
.stock: call    STRMS_HL
        ld      de,H_OPEN_STOCK
        jp      HOOK_STOCK
.ours:  ld      hl,0
        ld      (CH_ARG),hl
        call    AT_END
        jr      z,.nomode
        cp      ','
        jp      nz,NONSENSE
        call    NEXT_CHAR
        ld      hl,H_EXPT_STR
        call    HOMECALL           ; the mode
        call    SKIP_SPACES
        cp      ','
        jr      nz,.endarg
        call    NEXT_CHAR
        ld      hl,H_EXPT_1NUM
        call    HOMECALL           ; the record length
        ld      hl,H_FIND_INT2
        call    HOMECALL
        ld      (CH_ARG),bc
.endarg:
        call    AT_END
        jp      nz,NONSENSE
        call    POP_STR            ; the mode
        jr      .spec
.nomode:
        ld      bc,0
.spec:  push    de
        push    bc
        call    POP_STR            ; the spec
        push    de
        push    bc
        ; the record goes where CHANS' 80h is (on the 2068 a spare byte may
        ; sit between it and PROG)
        call    CHANS_END
        push    hl
        ld      de,(CHANS)
        and     a
        sbc     hl,de
        ld      de,REC_LEN+1       ; its offset, and the next one's, below 80h
        add     hl,de
        ld      a,h
        and     a
        jr      nz,.full
        ld      a,l
        cp      $80
        jr      c,.room
.full:  ld      a,R_MEMORY
        jp      REPORT
.room:  pop     de
        push    de                 ; [record]
        dec     de                 ; MAKE-ROOM opens the space after (HL)
        ld      bc,REC_LEN
        ld      ix,H_MAKE_ROOM
        ld      hl,A_T_XHL
        call    HOMECALL
        ; Everything above the record moved up REC_LEN, the strings too.
        pop     hl                 ; the record
        pop     bc
        pop     de
        ex      de,hl
        ld      a,REC_LEN
        call    ADD_HL_A
        ex      de,hl              ; DE/BC: the spec
        pop     ix                 ; the mode's length
        ex      (sp),hl            ; [record], HL = the mode's text
        ld      a,REC_LEN
        call    ADD_HL_A
        push    ix
        ex      (sp),hl            ; HL = its length
        pop     ix                 ; IX = its text
        ld      a,SVC_CH_OPEN
        out     (P_SVC),a
        pop     hl                 ; the record
        and     a
        jr      z,.made
        push    af
        ex      de,hl
        ld      bc,REC_LEN
        ld      ix,H_RECLAIM
        ld      hl,A_T_XHL
        call    HOMECALL           ; no file: no record
        pop     af
        jp      REPORT
.made:  push    hl
        ld      (hl),low A_CH_OUT
        inc     hl
        ld      (hl),high A_CH_OUT
        inc     hl
        ld      (hl),low A_CH_IN
        inc     hl
        ld      (hl),high A_CH_IN
        inc     hl
        ld      (hl),'F'
        inc     hl
        ld      a,(STREAM_N)
        ld      (hl),a
        pop     hl
        ld      de,(CHANS)
        and     a
        sbc     hl,de
        inc     hl
        ex      de,hl              ; DE = the offset, which 1461h stores
        call    STRMS_HL
        xor     a
        ret

; CLOSE_REC -- HL = an F record, BC = its offset, (STREAM_N) its stream. The
; engine closes the file; the record goes, every later offset moves down,
; CURCHL leaves it for S, and the STRMS entry is reset. A = the engine's answer.
CLOSE_REC:
        push    hl
        push    bc
        ld      bc,5
        add     hl,bc
        ld      l,(hl)             ; the stream
        ld      a,SVC_CH_CLOSE
        out     (P_SVC),a
        pop     bc
        pop     hl
        push    af                 ; [answer]
        push    hl                 ; [record]
        ld      hl,STRMS
        ld      a,19
.fix:   push    af
        ld      e,(hl)
        inc     hl
        ld      d,(hl)             ; DE = an entry, HL on its high byte
        bit     7,d
        jr      nz,.next           ; not in CHANS
        push    hl
        ex      de,hl
        and     a
        sbc     hl,bc
        jr      c,.keep            ; below ours
        jr      z,.keep            ; ours: 13A8h resets it
        add     hl,bc
        ld      de,-REC_LEN
        add     hl,de
        ex      de,hl
        pop     hl
        ld      (hl),d
        dec     hl
        ld      (hl),e
        inc     hl
        jr      .next
.keep:  pop     hl
.next:  inc     hl
        pop     af
        dec     a
        jr      nz,.fix
        pop     de                 ; the record
        ld      hl,(CURCHL)
        and     a
        sbc     hl,de
        push    hl                 ; [Z: CURCHL is the record]
        ld      bc,REC_LEN
        ld      ix,H_RECLAIM
        ld      hl,A_T_XHL
        call    HOMECALL
        pop     hl
        ld      a,h
        or      l
        jr      nz,.reset
        ld      a,2                ; the upper screen
        ld      hl,H_CHAN_OPEN
        call    HOMECALL
.reset: call    STRMS_HL
        ex      de,hl
        ld      ix,H_CLOSE_RESET
        ld      hl,A_T_XHL
        call    HOMECALL
        pop     af
        ret

; F_STREAM -- the number on the calculator stack is a stream open to an F
; record: L = the stream; otherwise Report O.
F_STREAM:
        ld      hl,H_STREAM
        call    HOMECALL           ; HL = the entry, BC = the offset
        call    F_RECORD
        ld      a,R_STREAM
        jp      nz,REPORT
        ld      a,(STREAM_N)
        ld      l,a
        ret

; F_RECORD -- BC = a STRMS offset. Z if it is an F record, HL = the record.
F_RECORD:
        ld      a,b
        or      c
        jr      z,.no
        bit     7,b
        jr      nz,.no
        ld      hl,(CHANS)
        add     hl,bc
        dec     hl
        push    hl
        inc     hl
        inc     hl
        inc     hl
        inc     hl
        ld      a,(hl)
        pop     hl
        cp      'F'
        ret
.no:    or      1
        ret

; STRMS_HL -- HL = stream (STREAM_N)'s STRMS entry.
STRMS_HL:
        ld      a,(STREAM_N)
        add     a,a
        add     a,low (STRMS+6)
        ld      l,a
        ld      h,high STRMS
        ret

; CHANS_END -- HL = CHANS' closing 80h.
CHANS_END:
        ld      hl,(PROG)
        dec     hl
        ld      a,(hl)
        cp      $80
        ret     z
        dec     hl
        ret

; ADD_HL_A -- HL += A.
ADD_HL_A:
        add     a,l
        ld      l,a
        ret     nc
        inc     h
        ret

;------------------------------------------------------------------------------
; Parsing
;------------------------------------------------------------------------------

; SKIP_SPACES -- A = the character at CH_ADD, CH_ADD stepped over spaces.
SKIP_SPACES:
        ld      hl,(CH_ADD)
.skip:  ld      a,(hl)
        cp      ' '
        jr      nz,.done
        inc     hl
        jr      .skip
.done:  ld      (CH_ADD),hl
        ret

; AT_END -- Z if the statement ends here (':' or the end of the line).
AT_END: call    SKIP_SPACES
        cp      CR
        ret     z
        cp      ':'
        ret

; NEXT_CHAR -- step CH_ADD over one character.
NEXT_CHAR:
        ld      hl,(CH_ADD)
        inc     hl
        ld      (CH_ADD),hl
        ret

; STR_NEXT -- A = the character at CH_ADD. Z if a string expression starts
; with it: a quote, a string variable (a letter and $), INKEY$, VAL$, STR$
; or CHR$.
STR_NEXT:
        cp      '"'
        ret     z
        cp      TOK_INKEY
        ret     z
        cp      TOK_VALS
        ret     z
        cp      TOK_STRS
        ret     z
        cp      TOK_CHRS
        ret     z
        or      $20                ; a letter, either case
        cp      'a'
        jr      c,.no
        cp      'z'+1
        jr      nc,.no
        ld      hl,(CH_ADD)
        inc     hl
        ld      a,(hl)
        cp      '$'
        ret
.no:    or      1                  ; NZ
        ret

; TWO_NAMES -- string TO string, ending the statement. Z on the syntax pass;
; at run time NZ, with DE/BC the first, IX/HL the second.
TWO_NAMES:
        ld      hl,H_EXPT_STR_ONLY
        call    HOMECALL
        call    SKIP_SPACES
        cp      TOK_TO
        jp      nz,NONSENSE
        call    NEXT_CHAR
        call    EXPT_STR_END
        ret     z
        call    POP_STR            ; the second
        push    de
        push    bc
        call    POP_STR            ; the first
        pop     hl
        pop     ix
        ld      a,1
        or      a                  ; NZ
        ret

; EXPT_STR_END -- a string expression that ends the statement. Z on the
; syntax pass (nothing more to do), NZ at run time (the string stacked).
EXPT_STR_END:
        ld      hl,H_EXPT_STR
        call    HOMECALL
        call    AT_END
        jp      nz,NONSENSE
        ; fall through
; RUNTIME -- NZ at run time, Z on the syntax pass; NC.
RUNTIME:
        ld      a,(FLAGS)
        and     $80
        ret

; POP_STR -- pop a string off the calculator stack: DE = text, BC = length.
POP_STR:
        ld      hl,(STKEND)
        dec     hl
        ld      b,(hl)
        dec     hl
        ld      c,(hl)
        dec     hl
        ld      d,(hl)
        dec     hl
        ld      e,(hl)
        dec     hl
        ld      (STKEND),hl
        ret

        ASSERT  $ <= DEAD          ; the hooks' switch sits at 1491h

        SAVEBIN "out/dos.rom",0,$4000

        ENDIF
