/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "snes9x.h"
#include "memmap.h"
#include "upd7725.h"
#ifdef DEBUGGER
#include "missing.h"
#endif

uint8	(*GetDSP) (uint16)        = NULL;
void	(*SetDSP) (uint8, uint16) = NULL;


void S9xResetDSP (void)
{
	memset(&DSP1, 0, sizeof(DSP1));
	DSP1.waiting4command = TRUE;
	DSP1.first_parameter = TRUE;

	memset(&DSP2, 0, sizeof(DSP2));
	DSP2.waiting4command = TRUE;

	memset(&DSP3, 0, sizeof(DSP3));
	DSP3_Reset();

	memset(&DSP4, 0, sizeof(DSP4));
	DSP4.waiting4command = TRUE;

	S9xUPD7725Reset();
}

// Which of the chip's two registers an address reaches. DSP-2's HLE window
// has no status register; the board decodes A14 like the LoROM DSP-1.
static inline bool8 StatusRegister (uint16 address)
{
	if (DSP0.maptype == M_DSP2_LOROM)
		return (address >= 0xc000);
	return (address >= DSP0.boundary);
}

uint8 S9xGetDSP (uint16 address, int32 speed)
{
	// With its firmware the chip itself runs; a peek mustn't step its handshake.
	if (S9xUPD7725Active())
		return (speed < 0 ? 0 : S9xUPD7725Read(StatusRegister(address), speed, address));

#ifdef DEBUGGER
	if (Settings.TraceDSP)
	{
		sprintf(String, "DSP read: 0x%04X", address);
		S9xMessage(S9X_TRACE, S9X_TRACE_DSP1, String);
	}
#endif

	return ((*GetDSP)(address));
}

void S9xSetDSP (uint8 byte, uint16 address, int32 speed)
{
	if (S9xUPD7725Active())
	{
		if (speed >= 0)
			S9xUPD7725Write(byte, StatusRegister(address), speed);
		return;
	}

#ifdef DEBUGGER
	missing.unknowndsp_write = address;
	if (Settings.TraceDSP)
	{
		sprintf(String, "DSP write: 0x%04X=0x%02X", address, byte);
		S9xMessage(S9X_TRACE, S9X_TRACE_DSP1, String);
	}
#endif

	(*SetDSP)(byte, address);
}
