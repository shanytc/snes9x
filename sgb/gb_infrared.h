/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _SGB_GB_INFRARED_H_
#define _SGB_GB_INFRARED_H_

#include <cstdint>

namespace SGB {

struct Memory;

// Game Boy Color infrared port (RP, $FF56): an LED and a light sensor on the
// console's top edge. Two consoles' ports can be paired to face each other;
// an unpaired port sees no light.
void IrPair(Memory *a, Memory *b);   // nullptr for either unpairs
void IrUnpair();
bool IrPaired();
// What the paired consoles see of each other: the GBC port, the cart LEDs.
void IrSetMedia(bool port, bool cart);

// A paired port is lit or reading: the split loop steps the seats finer.
bool IrActive();

uint8_t IrReadRP(const Memory &m);
void    IrWriteRP(Memory &m, uint8_t value);

// HuC1/HuC3 cartridge infrared (GB KISS): its LED and sensor take the $A000-$BFFF
// window in mode $0E. Bit 0 reads 1 while the facing cart's LED is lit.
bool    IrCartSelected(const Memory &m);
uint8_t IrReadCart(const Memory &m);
void    IrWriteCart(Memory &m, uint8_t value);

} // namespace SGB

#endif
