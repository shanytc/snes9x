/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "gb_cart.h"

namespace SGB {

bool CartLoad(Cart & /*c*/, const uint8_t * /*data*/, size_t /*size*/, const char * /*path*/)
{
	// P2 parses header and selects MBC.
	return false;
}

void CartUnload(Cart &c)
{
	c.rom.clear();
	c.sram.clear();
	c.header = {};
	c.path.clear();
	c.has_battery = false;
	c.has_rtc     = false;
	c.has_rumble  = false;
}

bool CartSaveBattery(const Cart & /*c*/)
{
	return false;
}

bool CartLoadBattery(Cart & /*c*/)
{
	return false;
}

} // namespace SGB
