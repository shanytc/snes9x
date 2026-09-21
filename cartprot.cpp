// Security chip on the unlicensed "Tekken 2" cart (CRC32 066687CA).
//
// The game talks to the chip through the cartridge ROM window. A burst of
// writes to $xx:8000 resets it; further bursts to $xx:8200/8300/8400/8500
// clock in the four argument bits and $xx:8600/8700 the two command bits.
// Reading $xx:8100-$81FF then returns the answer nibble. Answers supply the
// bank byte of two pointers the game compares at every round start (a
// mismatch runs JSR $00A9 into cleared RAM), the bank the chip itself is
// addressed at, and the character/round-marker indices.
//
// The answer table is derived from what the game does with each answer, not
// from the chip: rows 0 and 2 are pinned by the range the round-marker jump
// table accepts, row 1 is the AND mask the index builder needs, and the two
// row-3 entries the game asks for are pinned by the pointer compare and by
// the character slots. Entries the game has not been seen to ask for are
// extrapolated from the same rules.

#include "snes9x.h"
#include "memmap.h"
#include "cartprot.h"

extern uint8	OpenBus;

struct SCartProt	CartProt;

static uint8	*ProtRomBase[0x40];

// answer[(command << 4) | argument]
static const uint8	kAnswer[64] =
{
	// command 0: argument + 2 - the round-marker jump table only accepts 2..9
	0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 0x8, 0x9, 0xA, 0xB, 0xC, 0xD, 0xE, 0xF, 0x0, 0x1,
	// command 1: the AND mask the index builder applies to a command-0 answer
	0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF,
	// command 2: argument doubled - Tekken needs 6 -> $C, Street Fighter EX
	// (same chip, bank $01 code) needs 5 -> $A, and 2x is the only rule fitting both
	0x0, 0x2, 0x4, 0x6, 0x8, 0xA, 0xC, 0xE, 0x0, 0x2, 0x4, 0x6, 0x8, 0xA, 0xC, 0xE,
	// command 3: a GF(2) bit scramble. Street Fighter EX pins 6 -> 3 and 8 -> 2,
	// Tekken pins $C -> 5; those three force f(2)=4, f(4)=7, f(8)=2 if the map is
	// linear, and f(1)=8 is the low choice that keeps it a bijection and keeps
	// Tekken's compare pointer on a blank bank.
	0x0, 0x8, 0x4, 0xC, 0x7, 0xF, 0x3, 0xB, 0x2, 0xA, 0x6, 0xE, 0x5, 0xD, 0x1, 0x9
};

// The protocol routines at $00:B5DF (write a burst) and $00:B693 (read the
// answer) are the signature; the blank header alone is not specific enough.
bool8 S9xCartProtDetect (uint8 *rom, uint32 size)
{
	static const uint8	kBurst[] = { 0x08, 0xA0, 0x00, 0x00, 0xAE, 0x1A, 0x03, 0xA9, 0x0F, 0x00, 0x97, 0x16 };
	static const uint8	kFetch[] = { 0x08, 0xA0, 0x00, 0x01, 0xA2, 0x00, 0x01, 0xB7, 0x16, 0x8D, 0x74, 0x02 };

	// Only the 2 MB image is verified. The 8 MB "Tekken 2 (USA) (Pirate)"
	// 2-in-1 carries the same code twice but needs a multicart mapper it
	// does not have, and its banks $80-$BF hold real ROM - leave it alone.
	if (size != 0x200000)
		return (FALSE);

	return (memcmp(rom + 0x35DF, kBurst, sizeof(kBurst)) == 0 &&
	        memcmp(rom + 0x3693, kFetch, sizeof(kFetch)) == 0);
}

void S9xCartProtReset (void)
{
	memset(&CartProt, 0, sizeof(CartProt));
	CartProt.Answer = kAnswer[0];
}

void S9xCartProtSetRomBase (uint32 bank, uint8 *base)
{
	if (bank >= 0x80 && bank <= 0xBF)
		ProtRomBase[bank - 0x80] = base;
}

// Every write the cart window sees; only $8000-$87FF means anything.
void S9xCartProtWrite (uint32 address)
{
	if ((address & 0xF800) != 0x8000)
		return;

	switch ((address >> 8) & 7)
	{
		case 0:		// reset strobe: a new transaction starts here
			CartProt.Command  = 0;
			CartProt.Argument = 0;
			CartProt.Armed    = TRUE;
			break;

		case 2:		CartProt.Argument |= 0x01;	break;
		case 3:		CartProt.Argument |= 0x02;	break;
		case 4:		CartProt.Argument |= 0x04;	break;
		case 5:		CartProt.Argument |= 0x08;	break;
		case 6:		CartProt.Command  |= 0x01;	break;
		case 7:		CartProt.Command  |= 0x02;	break;
		default:	break;
	}

	CartProt.Answer = kAnswer[((CartProt.Command & 3) << 4) | (CartProt.Argument & 0x0F)];
}

// $8100-$81FF answers once a transaction has been strobed; everything else in
// the window, and every read before the first strobe, is plain ROM - the boot
// code runs out of $8100-$820A.
uint8 S9xCartProtRead (uint32 address)
{
	if (CartProt.Armed && (address & 0xFF00) == 0x8100)
		return (CartProt.Answer);

	const uint32	bank = (address >> 16) & 0xFF;
	uint8			*base = (bank >= 0x80 && bank <= 0xBF) ? ProtRomBase[bank - 0x80] : NULL;

	return (base ? base[address & 0xFFFF] : OpenBus);
}
