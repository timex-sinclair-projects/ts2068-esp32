#!/usr/bin/env python3
"""Build the native disk's ROM pieces: the DOS ROM and the HOME/EXROM patches.

  tools/build_dos.py [--sjasmplus PATH]

Assembles dos/dos.asm three times (the DOS ROM, HOME's side of the switch,
the EXROM's tape hooks), checks every patch site and anchor against both
stock ROM pairs in patches/base, and writes:

  dos/out/dos.rom          the 16K DOS ROM the emulator overlays on 0000h-3FFFh
  dos/out/home-dos.rom     the genuine HOME ROM (TS2068_U16) with the patches
  dos/out/exrom-dos.rom    the genuine EXROM (TS2068_U20) with the patches
  include/DosRom.h         the patches and the DOS ROM, for the firmware: it
                           patches the ROMs it loads from the card at boot,
                           after checking the same "before" bytes

The two stock pairs: TS2068_U16/U20.BIN, dumped from an unmodified 2068 (the
reference library's TS2068_ROMS_NoMod), and 2068Home/2068Exrom.BIN, the pair
the D: patches were built on. They differ in a few hundred bytes, none of them
at a patch site or an anchor; the build fails if that ever changes.

The patch lists are declarative, in the style of TS-Pico's build-rom.py: the
address, the stock bytes, the new bytes, and why. An anchor is a patch that
changes nothing: ROM code the DOS ROM or the hooks rely on, checked so a ROM
where it has moved gets no native disk rather than a crash.
"""

import argparse
import os
import re
import subprocess
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOS = os.path.join(ROOT, "dos")
OUT = os.path.join(DOS, "out")
BASE = os.path.join(ROOT, "patches", "base")
PAIRS = [("TS2068_U16.BIN", "TS2068_U20.BIN"), ("2068Home.BIN", "2068Exrom.BIN")]
HEADER = os.path.join(ROOT, "include", "DosRom.h")

POCKET = 0x3CDC
HOOKS = 0x1800


def jp(addr):
    return bytes([0xC3, addr & 0xFF, addr >> 8]).hex()


def call(addr):
    return bytes([0xCD, addr & 0xFF, addr >> 8]).hex()


def home_patches(sym, pocket):
    return [
        (0x1946, "d0 c0 c4 c8", "d2 c2 c6 ca",
         "the syntax entries of CAT, FORMAT, MOVE, ERASE: skip the stock 0Ah 2Ch to "
         "class 05h, so the keywords parse bare (TS-Pico's hunk)"),
        (0x1A03, "06 00 9f 13", "05 d9 25 00",
         "CLOSE #'s parameter list: class 06h, class 00h, 139Fh -> class 05h, 25D9h, so "
         "CLOSE #* parses; the DOS ROM does class 06h itself and goes on to 139Fh"),
        (0x25D6, "cd 89 28 20 06 cd 69", jp(sym["H_ENTER"]) + "06d418f9",
         "the disk keywords' dead stub: JP H_ENTER, then CLOSE #'s LD B,D4h / JR 25D6h at 25D9h"),
        (POCKET, None, pocket.hex(),
         "the switch to the DOS ROM and back, in the genuine FFh filler before CHRSET"),
        (0x0010, "c3 ed 11", None, "anchor: RST 10h, print A (H_PRINT_A)"),
        (0x0053, "e1 6e fd 75 00 ed 7b 3d 5c c3 54 13", None,
         "anchor: RST 8's tail; 0055h raises the report in L (H_ERROR_3)"),
        (0x1230, "87 c6 16 6f 26 5c", None, "anchor: CHAN-OPEN (H_CHAN_OPEN)"),
        (0x139F, "cd 0f 14 78 b1 c8", None, "anchor: CLOSE #'s routine (H_CLOSE)"),
        (0x1B44, "cd 89 28 c0 c1 c1 df fe 0d", None,
         "anchor: CHECK-END, then STMT-NEXT at 1B4Ah (H_CHECK_END)"),
        (0x1BE5, "cd 54 28 fd cb 01 76 c0 cf 0b cd 54 28 fd cb 01 76 c8", None,
         "anchor: class 06h (H_EXPT_1NUM) and class 0Ah (H_EXPT_STR, 1BEFh)"),
    ]


def exrom_patches(sym, hooks):
    return [
        (0x0068, "21 e5 00", jp(sym["X_W_TAPE"]), "W_TAPE: LD HL,00E5h -> JP X_W_TAPE"),
        (0x00FC, "14 08 15", jp(sym["X_R_TAPE"]),
         "R_TAPE: INC D / EX AF,AF' / DEC D -> JP X_R_TAPE"),
        (0x0215, "cd 99 0f", call(sym["X_PGPSTR"]),
         "SLVM: CALL BANK PGPSTR -> CALL X_PGPSTR, which makes it and shows the engine the name"),
        (0x0851, "e5 3e fd", jp(sym["X_SAVE"]),
         "SAVE: PUSH HL / LD A,FDh -> JP X_SAVE: no prompt or pause with a TAP mounted"),
        (HOOKS, None, hooks.hex(), "the tape hooks, in BLK2 (FFh filler)"),
        (0x0008, "2a 5d 5c 22 5f 5c e1 6e fd 75 00 ed 7b 3d 5c", None,
         "anchor: XRST8; 0010h raises the report in L (X_ERROR)"),
        (0x006B, "e5 21 80 1f", None, "anchor: W_TAPE after its first instruction"),
        (0x00FF, "f3 3e 0f", None, "anchor: R_TAPE after its first three bytes"),
        (0x0218, "dd e1 21 f6 ff 0b 09 03 30", None, "anchor: SLVM after CALL BANK PGPSTR"),
        (0x0854, "dd e5 d9 21", None, "anchor: SAVE after its first two instructions"),
        (0x0F99, "f5 3a c2 5c", None, "anchor: CALL BANK"),
    ]


def assemble(sjasm, side):
    sym = os.path.join(OUT, f"side{side}.sym")
    cmd = [sjasm, "--nologo", "--msg=war", f"-DSIDE={side}", f"--sym={sym}", "dos.asm"]
    r = subprocess.run(cmd, cwd=DOS, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"sjasmplus failed (SIDE={side}):\n{r.stdout}{r.stderr}")
    out = {}
    for line in open(sym):
        m = re.match(r"(\w+):\s+EQU\s+0x([0-9A-Fa-f]+)", line)
        if m:
            out[m.group(1)] = int(m.group(2), 16)
    return out


def apply(name, rom, patches):
    """Checks every site first, then applies. Returns the ROM and (addr, old, new, why)."""
    rom = bytearray(rom)
    done = []
    for addr, before, after, why in patches:
        new = bytes.fromhex(after if after is not None else before)
        old = bytes([0xFF]) * len(new) if before is None else bytes.fromhex(before)
        if len(old) != len(new):
            sys.exit(f"{addr:04X}h: before and after differ in length")
        if bytes(rom[addr:addr + len(old)]) != old:
            sys.exit(f"{name} {addr:04X}h: the ROM has {rom[addr:addr + len(old)].hex(' ')}, "
                     f"not {old.hex(' ')}")
        done.append((addr, old, new, why))
    for addr, old, new, why in done:
        rom[addr:addr + len(new)] = new
    return bytes(rom), done


def c_bytes(data, indent="    ", per_line=16):
    return "\n".join(indent + ", ".join(f"0x{b:02x}" for b in data[i:i + per_line]) + ","
                     for i in range(0, len(data), per_line))


def c_patches(out, prefix, patches):
    for i, (addr, old, new, why) in enumerate(patches):
        out.append(f"// {addr:04X}h: {why}")
        out.append(f"static const uint8_t {prefix}{i}Before[] = {{")
        out.append(c_bytes(old))
        out.append("};")
        if old != new:
            out.append(f"static const uint8_t {prefix}{i}After[] = {{")
            out.append(c_bytes(new))
            out.append("};")
    out.append("")
    out.append(f"static const Patch {prefix}es[] = {{")
    for i, (addr, old, new, why) in enumerate(patches):
        after = f"{prefix}{i}After" if old != new else f"{prefix}{i}Before"
        out.append(f"    {{ 0x{addr:04x}, {len(new)}, {prefix}{i}Before, {after} }},")
    out.append("};")
    out.append(f"static const int {prefix}Count = {len(patches)};")
    out.append("")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sjasmplus", default=os.environ.get("SJASMPLUS", "sjasmplus"))
    args = ap.parse_args()

    os.makedirs(OUT, exist_ok=True)
    assemble(args.sjasmplus, 0)
    sym1 = assemble(args.sjasmplus, 1)
    sym2 = assemble(args.sjasmplus, 2)

    dos = open(os.path.join(OUT, "dos.rom"), "rb").read()
    pocket = open(os.path.join(OUT, "home_pocket.bin"), "rb").read()
    hooks = open(os.path.join(OUT, "exrom_hooks.bin"), "rb").read()
    if len(dos) != 0x4000:
        sys.exit(f"dos.rom is {len(dos)} bytes, not 16K")
    if POCKET + len(pocket) > 0x3D00:
        sys.exit("HOME's pocket runs into CHRSET")
    if HOOKS + len(hooks) > 0x1C00:
        sys.exit("the EXROM hooks run out of BLK2's filler")

    hp = home_patches(sym1, pocket)
    xp = exrom_patches(sym2, hooks)
    for i, (home_name, exrom_name) in enumerate(PAIRS):
        home = open(os.path.join(BASE, home_name), "rb").read()
        exrom = open(os.path.join(BASE, exrom_name), "rb").read()
        new_home, home_done = apply(home_name, home, hp)
        new_exrom, exrom_done = apply(exrom_name, exrom, xp)
        print(f"  {home_name} (CRC {zlib.crc32(home):08x}) and {exrom_name} "
              f"(CRC {zlib.crc32(exrom):08x}): every site fits")
        if i == 0:
            open(os.path.join(OUT, "home-dos.rom"), "wb").write(new_home)
            open(os.path.join(OUT, "exrom-dos.rom"), "wb").write(new_exrom)
            home_list, exrom_list = home_done, exrom_done

    for addr, old, new, why in home_list:
        print(f"  HOME  {addr:04X}h {len(new):3d} bytes  {why[:72]}")
    for addr, old, new, why in exrom_list:
        print(f"  EXROM {addr:04X}h {len(new):3d} bytes  {why[:72]}")
    used = max(i for i, b in enumerate(dos[:POCKET]) if b != 0xFF) + 1
    print(f"  DOS ROM: module 0100h-{used - 1:04X}h ({used - 0x100} bytes), CRC {zlib.crc32(dos):08x}")
    print(f"  EXROM hooks: {HOOKS:04X}h-{HOOKS + len(hooks) - 1:04X}h ({len(hooks)} bytes)")

    out = [
        "// Generated by tools/build_dos.py from dos/dos.asm. Do not edit.",
        "//",
        "// The native disk's ROM pieces: the HOME and EXROM patches (checked against",
        "// the stock bytes before they are applied; an anchor has the same bytes",
        "// before and after) and the 16K DOS ROM the emulator overlays on",
        "// 0000h-3FFFh while the module runs. See include/DiskEngine.h.",
        "",
        "#ifndef DosRom_h",
        "#define DosRom_h",
        "",
        "#include <inttypes.h>",
        "",
        "namespace DosRom {",
        "",
        "struct Patch {",
        "    uint16_t addr;",
        "    uint16_t len;",
        "    const uint8_t* before;",
        "    const uint8_t* after;",
        "};",
        "",
    ]
    c_patches(out, "homePatch", home_list)
    c_patches(out, "exromPatch", exrom_list)
    out.append(f"// The DOS ROM, 16K (CRC32 {zlib.crc32(dos):08x}).")
    out.append("static const uint8_t image[16384] = {")
    out.append(c_bytes(dos))
    out.append("};")
    out.append("")
    out.append("} // namespace DosRom")
    out.append("")
    out.append("#endif")
    open(HEADER, "w").write("\n".join(out) + "\n")
    print(f"  wrote {os.path.relpath(HEADER, ROOT)}")


if __name__ == "__main__":
    main()
