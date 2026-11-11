# TS2068 Port of ESPectrum — Build Plan and SCLD Register Contract

v0.2. v0.1 was written from a phone against no checkout. The two flagged TODOs have now been verified against a real clone of [EremusOne/ESPectrum](https://github.com/EremusOne/ESPectrum) — see "Three things verified against a checkout" below — and a third finding turned up in the process. See [CLAUDE.md](CLAUDE.md) for project setup and [PLAN.md](PLAN.md) for the phased execution plan this document feeds.

This plan ports EremusOne/ESPectrum (ESP32, VGA, 48K/128K/Pentagon Spectrum emulator) to the Timex Sinclair 2068. The 2068's SCLD replaces the Sinclair ULA. The Z80 core, VGA DMA driver, AY code, PS/2 keyboard, SD/file-nav, and OSD are reused as a chassis. The work is memory management (the horizontal MMU), a hi-res display renderer, a machine timing profile, and a cartridge loader. Nothing here needs PSRAM.

## Why this scopes cleanly

Every 2068 memory region is an 8K multiple on an 8K boundary, so the whole address space is eight independent 8K chunks. That is the SCLD's native shape, and it maps 1:1 onto an 8-entry pointer array indexed by `address >> 13`. ESPectrum already uses a pointer-array bank model (`MemESP::ramCurrent[]`), just at 16K granularity with four slots. The port doubles the granularity; it does not change the mechanism.

The SCLD also removes work. The 2068 solved memory contention, so the per-access `VIDEO::Draw(n, ramContended[page])` contention hook in the Z80 core's peek/poke path is deleted for this machine, not replaced. Snow does not exist. Floating-bus reads are out of scope because Spectrum software is not a target.

## Memory budget (nopsram, ~320K usable internal SRAM)

Guest memory: 16K HOME ROM + 48K HOME RAM + 8K EXROM + up to 24K DOCK = 96K. Real DOCK carts top out near 24K; there is no 64K cartridge case. Framebuffer for 512×192 at the driver's one-byte-per-output-pixel depth is roughly 96K single-buffered. Total live footprint is under 200K. Fits nopsram with margin. Double-buffering the framebuffer is the only thing that would erode that margin; treat it as the reason to keep a PSRAM board on the bench, not as a requirement.

A 2068 build does not need ESPectrum's eight 128K RAM pages. It is a 48K-class machine. Allocate HOME (64K), EXROM (8K), and a DOCK region (up to 24K) rather than inheriting the full 128K page set.

## The two SCLD ports

Everything routes through two I/O ports. Both are decoded on their full low byte on the 2068, unlike the Spectrum's partial decoding (a single bit for the ULA port) — but **not** on the full 16-bit address; a real bug (found 2026-07-10 via cross-referencing the FUSE emulator's source, see PLAN.md) traced back to this doc's original "fully decoded... must match the full port address" phrasing being read too literally. Real `OUT (n),A`/`IN A,(n)` traffic carries the Z80 accumulator on the address bus's upper byte, so `LD A,val; OUT ($F4),A` sends `(val<<8)|0xF4` for any `val` — matching only `address & 0xFF == 0xF4`, never a bare `address == 0x00F4`, is what real hardware (and every real program) actually does.

### Port 0xFF — Display Enhancement Control (power-on value 0x00)

**Corrected 2026-07-10** against a reference library of TS2068 documentation and ROM disassembly (`/Users/david/Documents/Projects/TS2068 Ref Library/docs/`, three independent files agree — `ts2068_video_and_cartridges.md`, `ts2068_quick_reference.md`, `ts2068_memory_map.md`). v0.1 of this doc modeled bits 2..0 as a single 3-bit enum ("110 = hi-res"); they are actually **three independent flag bits**, not a mutually-exclusive mode number — the reference library explicitly documents a `$01+$02` combination ("dual-file + hi-color"). The bit *positions* below are otherwise as v0.1 had them (`00`=standard, `01`=dual-file, `10`=hi-color all still land on the same bit values); only bit 2's value changes, from `110` (0x06) to `100` (0x04) alone.

| Bits | Field | Values |
|------|-------|--------|
| 0 | Second display file | 1 = display relocates to the second display file at $6000 (page-flip / dual-file). |
| 1 | Hi-color (extended colour) | 1 = 256×192 pixels, one attribute byte per pixel *pair* (finer than the standard 32×24 cell grid) — attributes come from the second display file. |
| 2 | 64-column / hi-res | 1 = 512×192 monochrome, 64-column. **This is bit 2 alone (0x04), not bits `110`/0x06 as v0.1 stated** — confirmed by three independent sources, see above. Combines with bit 0 or bit 1 is undocumented in the reference library; treat as unsupported until proven otherwise. |
| 5..3 | Hi-res ink/paper | Global colour pair for 512-mode. 000 black/white, 001 blue/yellow, 010 red/cyan, 011 magenta/green, 100 green/magenta, 101 cyan/red, 110 yellow/blue, 111 white/black. (The reference library describes this more vaguely as "paper color, with ink from individual pixel pairs" — read as consistent with this fixed-pair table, not contradicting it: bits 5..3 select one of 8 fixed ink/paper pairs, and each source pixel bit picks which of the pair to show.) |
| 6 | Interrupt inhibit | 1 = disable the 1/60s timer interrupt (documented elsewhere in the reference library as "disable keyboard interrupt" — same interrupt, since the ROM's keyboard scan runs from the periodic timer handler; not a conflict). |
| 7 | EXROM/DOCK select | 0 = DOCK, 1 = EXROM. Selects which alternative bank the 0xF4 chunk bits point at. Reference library confirms: must be preserved by software, matching this doc's original framing. |

Reading 0xFF returns the last value written (not screen/attribute data as on the Spectrum). This breaks Arkanoid-class floating-bus tricks; not a concern here.

### Port 0xF4 — Horizontal MMU chunk select

Eight bits, one per 8K chunk of the Z80 address space. Bit D0 = 0x0000–0x1FFF, D1 = 0x2000–0x3FFF, and so on to D7 = 0xE000–0xFFFF.

Bit = 0: that chunk is HOME (normal Spectrum-layout memory — ROM in the low chunks, RAM above 0x4000).
Bit = 1: that chunk is the alternative bank, DOCK or EXROM per 0xFF bit 7.

DOCK and EXROM are mutually exclusive across the whole space at any instant; you never see both. Any chunk not selected into DOCK/EXROM falls back to HOME. That default-to-HOME rule is the reset state (0xF4 = 0x00, all HOME).

Reading 0xF4 returns the last value written.

## The 8-slot memory model

**Implemented (2026-07-10):** this section is now real code, not just a contract — see `include/SCLD.h` / `src/SCLD.cpp` and `PLAN.md`'s Phase 2 slice 1 writeup for what's actually built (real HOME ROM/RAM backing store, DOCK/EXROM resolving to a shared empty-socket page until loaded, EXROM chip-select-mirrored across chunks) versus what's still open (a real ROM image, hardware bring-up). The pseudocode below matches `SCLD::resolveMemChunks()` closely but isn't kept byte-for-byte in sync — treat the code as authoritative once they disagree.

Replace the 4×16K pointer array with 8×8K. One pointer per chunk, each aimed at HOME-ROM, HOME-RAM, an EXROM 8K page, or a DOCK 8K page.

```
uint8_t* memChunk[8];   // one per 8K Z80 chunk, indexed address >> 13
// resolve on any write to 0xF4 or 0xFF bit 7:
for (int c = 0; c < 8; c++) {
    if (mmuSelect & (1 << c)) {          // 0xF4 bit set → alternative bank
        memChunk[c] = exromSelect ? exromPage(c) : dockPage(c);
    } else {
        memChunk[c] = homePage(c);       // ROM below 0x4000, RAM above
    }
}
```

Peek/poke in the Z80 core become:

```
page = address >> 13;
result = memChunk[page][address & 0x1fff];   // no VIDEO::Draw contention call
```

That is the entire hot-path change: `>> 14`→`>> 13`, `& 0x3fff`→`& 0x1fff`, array widened to 8, contention call dropped. No new branches. The compiler folds the new shift/mask immediates; per-access cost is unchanged.

Writes to HOME-ROM chunks and to DOCK/EXROM chunks must be masked (they are read-only). ESPectrum's existing write-protect handling for the ROM chunk generalizes to this.

## Display

**Implemented (2026-07-10)**, across two increments — see `PLAN.md`'s Phase 2 slice 2 writeup for the full breakdown and honest limits. Summary: the fixed-512-column physical VGA mode is real (`src/ESP32Lib/VGA/VGA.h`'s `VgaMode_512x480_60_TS2068`, computed via the actual APLL clock-divider algorithm, not bench-verified yet), the byte-to-pixel expansion math both modes need is real and tested (`include/SCLDVideo.h`/`src/SCLDVideo.cpp`), and — as of the second increment — that math is wired into an actual renderer, `VIDEO::RenderFrame2068()` (`src/Video.cpp`), that runs every frame.

The build boots into one fixed 512-column-wide physical VGA mode always (see the verified finding above — ESPectrum has no precedent for switching `vga.init()`/framebuffer layout mid-session, so the port doesn't add one). The 0xFF video-mode bits select which renderer fills that fixed-width buffer each frame, not the VGA timing itself.

**New finding while building the first increment (2026-07-10):** `VIDEO::Draw`/`VIDEO::Draw_Opcode` aren't selected once and left alone — they're self-retargeting function pointers that the existing renderers (`MainScreen`, `MainScreen_2A3`, etc.) repoint to each other as `CPU::tstates` crosses scanline/frame boundaries. That's how ESPectrum gets cycle-accurate, interleaved-with-CPU-execution rendering without a separate video thread — necessary because the Spectrum machines have memory contention, where mid-instruction attribute changes need to show up at the exact right pixel.

**Resolved differently in the second increment: the SCLD has no contention, so that whole mechanism turned out to be unnecessary, not just hard.** `VIDEO::Draw`/`Draw_Opcode` stay pointed at the existing `&VIDEO::Blank` for the whole TS2068 session; `RenderFrame2068()` instead renders the complete frame in one batch, called once per frame from `CPU::loop()`'s existing per-frame hook (the same place `VIDEO::EndFrame()` already gets called for every other machine). Much smaller, much lower-risk than replicating the interleaved state machine — the tradeoff is losing mid-frame raster-effect precision (a video-mode or color change partway down the screen won't show at the exact right row), which isn't verifiable without a monitor anyway and wasn't attempted.

Standard mode (0xFF bits 2..0 = 0x00, or 0x01 for the second-display-file variant) is nearly identical to the Spectrum's attribute renderer already in `Video.cpp` — one attribute byte drives eight pixels through the `AluByte[nibble][att]` lookup. `MainScreen` mostly works; the screen-base pointer follows the primary/secondary DFILE select and the `grmem` selection already has the shape for this (it is how ESPectrum picks the shadow screen via `videoLatch`). Because the physical mode is fixed at 512 columns, this renderer now needs to double each output pixel horizontally to fill the line — a change `AluByte` itself doesn't need, done in the write-to-framebuffer step around it.

Extended colour mode (bit 1, 0x02) is still 256 source-wide. Attributes come from the second display file at +0x2000 rather than from the interleaved-thirds Spectrum layout, giving 32×192 colour resolution. New attribute-fetch addressing, same pixel-doubling as standard mode.

Hi-res mode (bit 2, 0x04 — corrected from v0.1's "110"/0x06, see the port 0xFF table above) is the one genuinely new renderer. 512×192, one bit per source pixel, two global colours from 0xFF bits 5..3. No attribute bytes. DFILE1 supplies even columns, DFILE2 (at +0x2000) supplies odd columns; the two are interleaved pixel by pixel across the 64-column line. The inner loop reads a bitmap byte and expands it to 8 one-bit pixels against the fixed ink/paper pair — simpler than the attribute loop it sits next to, and it writes 1:1 into the fixed 512-wide buffer (no doubling), but it cannot reuse the `AluByte` path. Framebuffer is still one output byte per pixel; 1bpp is the source, not the stored form.

## Timing profile

The 2068 is a 60Hz machine (1/60s interrupt). Line and frame T-state counts differ from the 50Hz Spectrum. Set `tStatesPerLine`, `tStatesScreen`, `tStatesBorder`, and `VsyncTarget` for the 2068 in the architecture-select switch in `Reset()`. The SCLD's wait behaviour is uniform, so the contention *table* becomes a flat constant (or nothing) — but the frame timing constants still have to be correct or the display rolls. Do not conflate "no contention" with "no timing work." Sub-mode: the 512-column mode consumes display bytes at twice the per-line rate, so its draw-cycle accounting is a variant of the standard line, not the same numbers.

## Cartridge loading

**Implemented (2026-07-10), first increment:** see `include/DockLoader.h` / `src/DockLoader.cpp` and `PLAN.md`'s Phase 2 slice 3 writeup for the full design and its honest limits — the summary below is now a description of shipped code, with one correction to this section's original framing.

`.DCK` is the standard container. It carries chunk-presence flags and 8K blocks, with a type marker per chunk (RAM vs ROM, DOCK vs EXROM). Loading is: parse the header, place each 8K block into its DOCK/EXROM page, set the chunk's read-only flag, and let 0xF4/0xFF paging do the rest.

**Correction on autostart:** LROS and AROS cartridges do "page themselves in and vector execution on reset" on real hardware. The chunk-paging half of that is exactly what's shipped: `DockLoader` pages in whatever chunks the .DCK's own container header says are present, once, right after `Z80::reset()`. **The "vector execution" half needed a second pass to get right.** The first attempt assumed LROS always takes over from address 0 (where the Z80 already lands after reset) and skipped parsing the cartridge's in-ROM header at all — reasonable-sounding, and wrong: testing against a real LROS cartridge (Zebra OS-64, see `PLAN.md`'s slice 3 "real ROM/DOCK end-to-end test") showed the header occupies bytes 0-4 of chunk 0 as metadata, and the *real* entry point is wherever the header's own start-address field (offset 2-3) points — for Zebra, `$0005`, not `$0000`. `DockLoader::autostartEntryPoint()` now reads that field for real, and `Z80::setRegPC()` is called with it. It does **not** use the `check_trdos` PC-watching pattern this section originally proposed — that pattern exists because TR-DOS can be entered mid-session via any jump/call, and LROS doesn't have that requirement (it only ever autostarts at boot). A `check_trdos`-style watch would become relevant for a *mid-session* cartridge launch (e.g. from a real EXROM's menu, once one exists), not for LROS's boot-time case — revisit then, don't build it now on spec.

## Work slices for the team

The four pieces share almost no code surface, which is what makes this splittable across the distributed team.

1. **MMU + memory core.** The 8-slot refactor of the six `Z80Ops::*_std` functions in `src/CPU.cpp` (`peek8`, `poke8`, `peek16`, `poke16`, `fetchOpcode`, `addressOnBus`), the `memChunk[]` model in MemESP, a new per-chunk read-only flag (there is no existing generalized one — see verified findings), and the 0xF4/0xFF port handlers. Critical path; everything else assumes its interface. Wants the most Z80/C-comfortable person. This is the one place a bug is subtle rather than visible.
2. **Hi-res renderer + VGA mode table.** `vidmodes[]` row (done, computed via APLL algorithm, needs bench time) and the byte-to-pixel expansion math (done, tested — `SCLDVideo.h`/`.cpp`) landed. Still open: the actual 512×1bpp loop wired into `Video.cpp`'s `Draw`-function-pointer state machine, pixel-doubling wired into the standard/extended-colour renderers, and — the real gate on all of it — a monitor to bench-verify the timing row against.
3. **Cartridge loader.** `.DCK` parse (done), LROS autostart via the .DCK's own chunk-presence bits rather than `check_trdos`-style PC-watching (done, see "Cartridge loading" above for the correction), AROS autostart (not done — needs a working System ROM first). Depends on slice 1's interface, not its internals; was specced and built in parallel with slice 1's later increments.
4. **DOCK image corpus + test harness.** Real `.DCK` dumps, a known-good boot set, per-mode display test cards. Unblocks testing for the other three; without it they code blind. Likely already partly in the team's archives.

## Interface to agree before anyone writes code

The `memChunk[]` contract is what every slice codes against: the 8-entry array shape, the `address >> 13` index, the 0xF4 bit→chunk mapping, the 0xFF bit-7 DOCK/EXROM toggle, the DFILE base and screen-select for `grmem`, the read-only masking rule (new per-chunk flag, not a reused mechanism), and the fixed-512-column physical VGA mode with per-renderer pixel doubling for non-hi-res modes. One page. Agreeing it up front is what keeps four people building compatible parts instead of three integrations that do not line up.

## Three things verified against a checkout (2026-07-10, EremusOne/ESPectrum @ shallow clone of `main`)

- **Z80 core peek/poke — confirmed, with a location correction.** The commented macros in `Z80_JLS.cpp` describe the shape correctly, but the *live* code is not in `include/Z80_JLS/z80.h` — that file only holds the opcode dispatch table, which calls through `Z80Ops::peek8`/`poke8`/`fetchOpcode` function pointers declared in `include/Z80_JLS/z80operations.h`. The actual bodies (`peek8_std`, `poke8_std`, `fetchOpcode_std`, plus `peek16_std`/`poke16_std`/`addressOnBus_std`, each with a `_2A3` sibling selected in `Reset()`) live in **`src/CPU.cpp`**. They match the assumed shape exactly: `page = address >> 14; ...ramCurrent[page][address & 0x3fff]`, with a `VIDEO::Draw(n, contended)` / `VIDEO::Draw_Opcode(contended)` call per access. So the `>> 13` port is real but touches **six** functions in `CPU.cpp`, not one — `peek8_std`, `poke8_std`, `peek16_std`, `poke16_std`, `fetchOpcode_std`, `addressOnBus_std`. The `_2A3` siblings are +2A/+3-only and can be ignored for this port.
  - **Write-protect detail not in v0.1:** ESPectrum has no per-page read-only flag array. ROM protection is a single hardcoded `if (page == 0) { ...; return; }` at the top of `poke8_std`/`poke16_std` (page 0 = the low 16K = ROM, always). The 8-slot port needs a real per-chunk writable/read-only flag (HOME chunks 0–1, plus whichever DOCK/EXROM chunks the loaded `.DCK` marks as ROM-type) — there is no existing generalized mechanism to lean on here, contrary to what "generalizes to this" implies. Budget this as new code, not a rename.
  - **Contention collapse confirmed:** `addressOnBus_std` degenerates to exactly `VIDEO::Draw(wstates, false)` when `ramContended[page]` is always false — i.e. it becomes identical to `addressOnBus_2A3`. That is the concrete shape of "the contention table becomes a flat constant."
- **Framebuffer allocation — confirmed sized per-mode, not per-max.** `Graphics<Color>::allocateFrameBuffers()` (`ESP32Lib/Graphics/Graphics.h`) calls the virtual `allocateFrameBuffer()`, which in `VGA6Bit::allocateFrameBuffer()` (`ESP32Lib/VGA/VGA6Bit.h`) allocates from the *current* `yres` and `vidmodes[mode][hRes]` — driven by whatever `mode` index was passed to `vga.init()`. And `vga.init()` is called from exactly one place at runtime, `VIDEO::Init()`/`vgataskinit()` in `src/Video.cpp`, once per boot, with `Mode` selected from `Config::arch[0]` via a switch. So a 512-wide TS2068 mode only allocates its larger buffer when TS2068 is the configured/booted machine — it does not raise the floor for 48K/128K/Pentagon/TK builds. v0.1's concern does not apply.
- **New finding — no existing live resolution switch.** `vga.init()` (which is what sizes and lays out the framebuffer and DMA descriptors) is called exactly twice in the whole tree, both at boot, never again. Every existing machine picks one fixed VGA mode for the entire session — even the TK 50/60Hz and aspect-ratio variants are boot-time config choices, not something the guest program changes at runtime. The TS2068 needs standard (256-wide) and hi-res (512-wide) to coexist within one running session, toggled live by the guest writing port 0xFF bits 2..0 — there is no precedent for that in ESPectrum, and no evidence it's safe to re-run `vga.init()`/reallocate mid-session without visual glitches or heap fragmentation. **Resolved design, not just a question:** boot the TS2068 build into a single fixed 512-wide physical VGA mode always, and let the 0xFF video-mode bits select which *renderer* runs per scanline into that fixed-width buffer — standard/extended-colour renderers double each source pixel to fill the 512 columns, hi-res writes 1:1. This avoids ever calling `vga.init()` outside of boot. Make this explicit in the interface contract (below).
  - **Corollary for the hi-res VGA mode-table entry:** the existing `vidmodes[][17]` rows (`VGA.h`) are not a simple `{hRes, vRes}` pair — each row hand-tunes horizontal front/back porch and sync widths plus six APLL clock-divider constants (`r1sdm0/r1sdm1/r1sdm2/r1odiv/r0sdm2/r0odiv`) to hit a specific pixel clock and stay within standard VGA monitor sync tolerances (existing rows top out at 360 active columns). A 512-wide row is new hand-tuned timing, not a copy-paste of an existing row with `hRes` changed. Since the TS2068 build now always runs at this one fixed mode (previous bullet), only one such row needs to exist — but it still needs real timing derivation/bench verification on a monitor, not just arithmetic. Budget bench time for this in slice 2.

## Open questions worth a decision

- Which SCLD variant is the reference: TS2068 (60Hz, NTSC timing) is the target. TC2068 (50Hz PAL) is a timing-profile variant that costs little to add once the 60Hz path works.
- Extended colour mode (bit 1, 0x02): ship in v1 or defer? Standard + hi-res cover most native software; extended colour is a smaller library.
- Board: cheap nopsram (TTGo VGA32 class) is capability-sufficient. Buy one PSRAM board for the double-buffering bench test rather than committing the whole team to PSRAM.
