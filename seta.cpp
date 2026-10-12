/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "snes9x.h"
#include "seta.h"
#include "upd96050.h"
#include "st018.h"

uint8	(*GetSETA) (uint32)        = &S9xGetST010;
void	(*SetSETA) (uint32, uint8) = &S9xSetST010;


uint8 S9xGetSetaDSP (uint32 Address, int32 speed)
{
	// With its firmware the ST010/ST011 itself runs.
	if (S9xUPD96050Active())
		return (S9xUPD96050Read(Address, speed));
	return (GetSETA(Address));
}

void S9xSetSetaDSP (uint8 Byte, uint32 Address, int32 speed)
{
	if (S9xUPD96050Active())
	{
		S9xUPD96050Write(Address, Byte, speed);
		return;
	}
	SetSETA (Address, Byte);
}

uint8 S9xGetSetaRISC (uint32 Address, int32 speed)
{
	if (S9xST018Active())
		return (S9xST018Read(Address & 0xffff, speed));
	return (S9xGetST018(Address));
}

void S9xSetSetaRISC (uint8 Byte, uint32 Address, int32 speed)
{
	if (S9xST018Active())
	{
		S9xST018Write(Address & 0xffff, Byte, speed);
		return;
	}
	S9xSetST018(Byte, Address);
}
