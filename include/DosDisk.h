/*

ESP2068 -- TS2068 port of ESPectrum

DosDisk.h -- the native disk's firmware glue: puts the DOS patches into
the HOME ROM at boot, gives SCLD the DOS ROM to overlay, and turns
service calls on port D1h into DiskEngine calls with the Z80's registers
and memory. See include/DiskEngine.h for the design.

The native disk is on only when the HOME ROM loaded from the card is one
the patches fit: every patch site holds the stock bytes include/DosRom.h
expects. Any other HOME ROM (the TS-Pico ROM, a modified ROM) runs as it
is, with the three ports doing nothing.

Based on ESPectrum, a Sinclair ZX Spectrum emulator for Espressif ESP32 SoC
Copyright (c) 2023-2025 Víctor Iborra [Eremus] and 2023 David Crespo [dcrespo3d]
https://github.com/EremusOne/ESPectrum

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.

*/

#ifndef DosDisk_h
#define DosDisk_h

#include <inttypes.h>

namespace DosDisk {

// The 2068's "/" on the card.
extern const char* const ROOT;

// Checks every patch site in a 16K HOME ROM image against the stock
// bytes, then applies the patches. Returns false, leaving the image
// untouched, if any site differs.
bool patchHome(uint8_t* home);

// After the ROMs are loaded: patches SCLD's HOME ROM if it fits and turns
// the native disk on. Returns whether it did.
bool install();

bool active();

// The 16K DOS ROM SCLD overlays (include/DosRom.h's image).
const uint8_t* romImage();

// OUT (D1h),A: the service call. Returns the new A.
uint8_t service(uint8_t fn);

// A machine reset: the engine forgets its state.
void machineReset();

}

#endif
