// DosDisk: the native disk's firmware glue. See include/DosDisk.h.

#include "DosDisk.h"
#include "DosRom.h"
#include "DiskEngine.h"
#include "SCLD.h"

#include <cstdio>
#include <cstring>

#ifndef ESP2068_HOST_TEST
#include "Z80_JLS/z80.h"
#include "Z80_JLS/z80operations.h"
#endif

namespace DosDisk {

const char* const ROOT = "/sd/2068";

namespace {

bool on = false;
DiskEngine engine(ROOT);

#ifndef ESP2068_HOST_TEST
class Z80Cpu : public DiskCpu {
public:
    uint16_t bc() override { return Z80::getRegBC(); }
    uint16_t de() override { return Z80::getRegDE(); }
    uint16_t hl() override { return Z80::getRegHL(); }
    uint8_t peek(uint16_t a) override { return Z80Ops::peek8_2068(a); }
    void poke(uint16_t a, uint8_t v) override { Z80Ops::poke8_2068(a, v); }
};
Z80Cpu z80cpu;
#endif

} // namespace

namespace {

// Checks, then patches, a HOME ROM reached through `at(addr)`.
template <typename At>
bool patchWith(At at) {
    for (int i = 0; i < DosRom::homePatchCount; i++) {
        const DosRom::Patch& p = DosRom::homePatches[i];
        for (int k = 0; k < p.len; k++)
            if (*at(p.addr + k) != p.before[k]) return false;
    }
    for (int i = 0; i < DosRom::homePatchCount; i++) {
        const DosRom::Patch& p = DosRom::homePatches[i];
        for (int k = 0; k < p.len; k++) *at(p.addr + k) = p.after[k];
    }
    return true;
}

} // namespace

bool patchHome(uint8_t* home) {
    return patchWith([home](int a) { return home + a; });
}

bool install() {
    // SCLD keeps HOME's ROM as two 8K chunks, written here directly (the
    // write protection is the Z80's, not ours).
    bool fits = patchWith([](int a) { return SCLD::homePage(a >> 13) + (a & 0x1FFF); });
    if (!fits) {
        printf("DOS: this HOME ROM isn't the stock one the patches fit -- native disk off\n");
        on = false;
        SCLD::setOverlayImage(nullptr);
        return false;
    }
    SCLD::setOverlayImage(DosRom::image);
    engine.reset();
    on = true;
    printf("DOS: native disk on, the 2068's / is %s\n", ROOT);
    return true;
}

bool active() { return on; }

const uint8_t* romImage() { return DosRom::image; }

uint8_t service(uint8_t fn) {
#ifndef ESP2068_HOST_TEST
    return engine.service(fn, z80cpu);
#else
    (void) fn;
    return DiskEngine::R_UNKNOWN_FN;
#endif
}

void machineReset() {
    engine.reset();
}

} // namespace DosDisk
