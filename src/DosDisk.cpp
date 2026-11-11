// DosDisk: the native disk's firmware glue. See include/DosDisk.h.

#include "DosDisk.h"
#include "DosRom.h"
#include "DiskEngine.h"
#include "SCLD.h"
#include "DockLoader.h"

#include <cstdio>
#include <cstring>

#ifndef TS2068_HOST_TEST
#include "Z80_JLS/z80.h"
#include "Z80_JLS/z80operations.h"
#endif

namespace DosDisk {

const char* const ROOT = "/sd/2068";

namespace {

bool on = false;
bool tape = false;
DiskEngine engine(ROOT);

#ifndef TS2068_HOST_TEST
class Z80Cpu : public DiskCpu {
public:
    uint16_t bc() override { return Z80::getRegBC(); }
    uint16_t de() override { return Z80::getRegDE(); }
    uint16_t hl() override { return Z80::getRegHL(); }
    uint16_t ix() override { return Z80::getRegIX(); }
    uint16_t sp() override { return Z80::getRegSP(); }
    uint8_t peek(uint16_t a) override { return Z80Ops::peek8_2068(a); }
    void poke(uint16_t a, uint8_t v) override { Z80Ops::poke8_2068(a, v); }
    uint8_t video() override { return SCLD::videoMode; }
};
Z80Cpu z80cpu;
#endif

} // namespace

namespace {

// Whether every site of a patch list holds its stock bytes, in a ROM
// reached through `at(addr)`.
template <typename At>
bool fits(const DosRom::Patch* patches, int count, At at) {
    for (int i = 0; i < count; i++) {
        const DosRom::Patch& p = patches[i];
        for (int k = 0; k < p.len; k++)
            if (*at(p.addr + k) != p.before[k]) return false;
    }
    return true;
}

template <typename At>
void apply(const DosRom::Patch* patches, int count, At at) {
    for (int i = 0; i < count; i++) {
        const DosRom::Patch& p = patches[i];
        for (int k = 0; k < p.len; k++) *at(p.addr + k) = p.after[k];
    }
}

template <typename At>
bool patchWith(const DosRom::Patch* patches, int count, At at) {
    if (!fits(patches, count, at)) return false;
    apply(patches, count, at);
    return true;
}

// SCLD keeps each ROM as 8K chunks, written here directly (the write
// protection is the Z80's, not ours).
uint8_t* homeAt(int a) { return SCLD::homePage(a >> 13) + (a & 0x1FFF); }
uint8_t* exromAt(int a) { return SCLD::exromChunkData(a >> 13) + (a & 0x1FFF); }

} // namespace

bool patchHome(uint8_t* home) {
    return patchWith(DosRom::homePatches, DosRom::homePatchCount, [home](int a) { return home + a; });
}

bool patchExrom(uint8_t* exrom) {
    return patchWith(DosRom::exromPatches, DosRom::exromPatchCount, [exrom](int a) { return exrom + a; });
}

bool install() {
    on = false;
    tape = false;
    if (!fits(DosRom::homePatches, DosRom::homePatchCount, homeAt)) {
        printf("DOS: this HOME ROM isn't the stock one the patches fit -- native disk off\n");
        SCLD::setOverlayImage(nullptr);
        return false;
    }
    apply(DosRom::homePatches, DosRom::homePatchCount, homeAt);
    // The tape hooks, if the EXROM is the stock one too. Without them the
    // disk commands work and LOAD/SAVE are the stock tape ones.
    if (SCLD::exromChunkData(0) && fits(DosRom::exromPatches, DosRom::exromPatchCount, exromAt)) {
        apply(DosRom::exromPatches, DosRom::exromPatchCount, exromAt);
        tape = true;
    } else
        printf("DOS: this EXROM isn't the stock one the patches fit -- no TAP mounting\n");
    SCLD::setOverlayImage(DosRom::image);
    engine.reset();
    on = true;
    printf("DOS: native disk on%s, the 2068's / is %s\n", tape ? " with TAP mounting" : "", ROOT);
    return true;
}

bool tapeHooks() { return tape; }

bool active() { return on; }

const uint8_t* romImage() { return DosRom::image; }

uint8_t service(uint8_t fn) {
#ifndef TS2068_HOST_TEST
    return engine.service(fn, z80cpu);
#else
    (void) fn;
    return DiskEngine::R_UNKNOWN_FN;
#endif
}

void machineReset() {
    engine.reset();
}

bool frame() {
    std::vector<uint8_t> dck;
    if (!on) return false;
    engine.prTick();
    if (!engine.takeDock(dck)) return false;
    if (!DockLoader::loadFromBuffer(dck.data(), dck.size())) {
        printf("DOS: the cartridge didn't load\n");
        return false;
    }
    printf("DOS: cartridge docked%s, resetting\n", DockLoader::hasAutostart() ? " (LROS)" : "");
    return true;
}

} // namespace DosDisk
