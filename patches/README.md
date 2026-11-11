# Stock TS2068 ROMs

`base/` holds the stock Timex TS2068 ROMs that `tools/build_dos.py` checks
the native disk's patches against. Timex's own stock TS2068 ROMs are public
domain (see [`../test/dock/README.md`](../test/dock/README.md) for the
provenance note), which is why they can be committed.

- **`base/TS2068_U16.BIN` / `base/TS2068_U20.BIN`**: the HOME ROM and
  EXROM dumped from an unmodified TS2068 (the reference library's
  `TS2068_ROMS_NoMod`), the same ROMs as on the board's card.
  `build_dos.py` builds `dos/out/home-dos.rom` and `exrom-dos.rom` from
  this pair.
- **`base/2068Home.BIN` / `base/2068Exrom.BIN`**: a second stock pair. It
  differs from the first in a few hundred bytes, none of them at a patch
  site or an anchor. Every patch is checked against both pairs, and the
  build fails if that ever changes.

The `D:` virtual disk's ROM patches used to live here too, along with their
checks (`verify_patches.py`, `test_zesarux.py`). The native disk replaced
them, and Phase 6 removed them. They are in git history before the
`native-disk-phase6` merge.
