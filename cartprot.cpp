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
//
// "A Bug's Life" (Pirate) checks ports at $xx:6000-$6FFF instead ($00:CAC0):
// $60xx clears, $62/64/66/68xx XOR into four nibbles, $61/63/65/67xx read them
// and $6Fxx reads all four XORed. Model derived from the game's $E compare.
//
// The "Pocket Monster" family (MAME lorom_poke) latches any write to a bank with
// A23 set and A17/A18 clear, and reads it back there bit-permuted (bits
// 0,6,7,1,2,3,4,5). MAME decodes A16 too, but Digimon talks to it at $81/$89:40xx.
// Marvel Super Heroes vs. Street Fighter has the same latch on A22: $40-$7D:8000+.
//
// Soul Blade / Hercules (MAME lorom_sbld) read 55,0F,AA,F0 by A1-A0 in $80-$BF:8000+.
// Super 20 Hab (MAME lorom_20col) is a multicart: $00:8000-$8FFF writes pick the bank.

#include "snes9x.h"
#include "memmap.h"
#include "cartprot.h"
#include "ppu.h"

struct SCartProt	CartProt;

static uint8	*ProtRomBase[0x40];
static int		ProtKind = CARTPROT_NONE;
static bool8	BankOverlay;	// KOF '98: bitswap chip plus the bank $00 overlay register

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

// CRC32s of the bitswap carts, as listed by MAME (their code shares no signature).
static const uint32	kBitswapCarts[] =
{
	0x40242231,	// X-Men vs. Street Fighter
	0x6C303FC9,	// The King of Fighters '98
	0xA7813943,	// The King of Fighters 2000
	0x5E4ADA04,	// Soul Edge vs Samurai
	0x4F660972,	// Digimon Adventure
	0x752A25D3,	// Aladdin 2000
	0x7C0B798D,	// Pokemon Gold & Silver
	0xBAD1D9B8,	// Squirrel
	0xF863C642,	// Pokemon Stadium
	0x892C6765,	// Pocket Monster ("Picachu")
};

static uint32 Crc32 (const uint8 *p, uint32 size)
{
	static uint32	table[256];
	if (!table[1])
		for (uint32 i = 0; i < 256; i++)
		{
			uint32	c = i;
			for (int k = 0; k < 8; k++)
				c = (c & 1) ? 0xEDB88320 ^ (c >> 1) : c >> 1;
			table[i] = c;
		}

	uint32	crc = 0xFFFFFFFF;
	for (uint32 i = 0; i < size; i++)
		crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
	return (~crc);
}

// Super 20 Hab's menu bank (32K bank 4), served patched; Memory.ROM stays original.
static uint8	Super20Menu[0x8000];

// The song loader the menu copies to $7F:0000 (every game calls it) ends each
// stream with "execute at $0000", RAM with no sound code in it; whether the SPC
// stumbles back to its IPL depends on DSP power-on garbage. Aim it at $FFC0.
static void PatchSuper20Songs (void)
{
	static const uint8	kLoader[] = { 0xC2, 0x30, 0xDA, 0x5A, 0xA8, 0xE2, 0x30, 0xAD, 0x00, 0x00 };
	// address store: callers inherit B = its high byte (the menu TAXes it), so store
	// it as one word and zero B: REP #$20 LDA [$00],Y STA $2142 SEP #$20 INY INY LDA #0 XBA
	static const uint8	kStore[]    = { 0xB7, 0x00, 0xEB, 0xC8, 0xB7, 0x00, 0x8D, 0x43, 0x21, 0xC8, 0xEB, 0x8D, 0x42, 0x21 };
	static const uint8	kStoreNew[] = { 0xC2, 0x20, 0xB7, 0x00, 0x8D, 0x42, 0x21, 0xE2, 0x20, 0xC8, 0xC8, 0xA9, 0x00, 0xEB };
	const uint32		at = 0x010E, table = at + 0xCB;
	uint8				*b = Super20Menu;

	if (memcmp(b + at, kLoader, sizeof(kLoader)) != 0 || memcmp(b + 0x0194, kStore, sizeof(kStore)) != 0)
		return;
	memcpy(b + 0x0194, kStoreNew, sizeof(kStoreNew));

	const uint32	first = b[table] | b[table + 1] << 8;
	for (uint32 i = 0; table + 2 * i < at + first; i++)
	{
		for (uint32 p = at + (b[table + 2 * i] | b[table + 2 * i + 1] << 8); p + 4 <= sizeof(Super20Menu); )
		{
			const uint32	n = b[p] | b[p + 1] << 8;
			if (n == 0)
			{
				if (b[p + 2] == 0 && b[p + 3] == 0)
					b[p + 2] = 0xC0, b[p + 3] = 0xFF;
				break;
			}
			p += 4 + n;
		}
	}
}

// The protocol routines at $00:B5DF (write a burst) and $00:B693 (read the
// answer) are the signature; the blank header alone is not specific enough.
static int DetectKind (uint8 *rom, uint32 size)
{
	BankOverlay = FALSE;

	if (size >= 0x100000 && size <= 0x400000)
	{
		const uint32	crc = Crc32(rom, size);
		if (crc == 0xCDB590E4)		// Marvel Super Heroes vs. Street Fighter
			return (CARTPROT_BITSWAP40);
		if (crc == 0xC97D1D7B || crc == 0x45874D3D)		// Soul Blade, Hercules
			return (CARTPROT_SOULBLADE);
		if (crc == 0xAC19B8F4)		// Super 20 Hab
		{
			memcpy(Super20Menu, rom + 4 * 0x8000, sizeof(Super20Menu));
			PatchSuper20Songs();
			return (CARTPROT_SUPER20);
		}
		for (uint32 i = 0; i < sizeof(kBitswapCarts) / sizeof(kBitswapCarts[0]); i++)
			if (crc == kBitswapCarts[i])
			{
				BankOverlay = (crc == 0x6C303FC9);
				return (CARTPROT_BITSWAP);
			}
	}

	static const uint8	kBurst[] = { 0x08, 0xA0, 0x00, 0x00, 0xAE, 0x1A, 0x03, 0xA9, 0x0F, 0x00, 0x97, 0x16 };
	static const uint8	kFetch[] = { 0x08, 0xA0, 0x00, 0x01, 0xA2, 0x00, 0x01, 0xB7, 0x16, 0x8D, 0x74, 0x02 };

	// Only the 2 MB image is verified. The 8 MB "Tekken 2 (USA) (Pirate)" is
	// that image with every bank doubled, which the loader folds back first.
	if (size != 0x200000)
		return (CARTPROT_NONE);

	// A Bug's Life: PHB/PHA, LDX #$5F55, DB=$38, then $AA to the $6055 clear port
	static const uint8	kBugsClear[] = { 0x8B, 0x48, 0xA2, 0x55, 0x5F, 0xA9, 0x38, 0x48, 0xAB, 0xA9, 0xAA, 0x9D, 0x00, 0x01 };

	if (memcmp(rom + 0x4AC0, kBugsClear, sizeof(kBugsClear)) == 0)
		return (CARTPROT_BUGSLIFE);

	// Street Fighter EX / EX Plus Alpha (Pirate) drives the same chip from bank
	// $01 with direct page $86 and registers $0100/$0102/$0104.
	static const uint8	kSfexBurst[] = { 0x08, 0xC2, 0x30, 0xA0, 0x00, 0x00, 0xAE, 0x14, 0x01, 0xA9, 0x0F, 0x00, 0x97, 0x86 };
	static const uint8	kSfexFetch[] = { 0x08, 0xA0, 0x00, 0x01, 0xAE, 0x14, 0x01, 0xB7, 0x86, 0x8D, 0x04, 0x01 };

	if (memcmp(rom + 0xCFBB, kSfexBurst, sizeof(kSfexBurst)) == 0 &&
	    memcmp(rom + 0xD071, kSfexFetch, sizeof(kSfexFetch)) == 0)
		return (CARTPROT_TEKKEN2);

	if (memcmp(rom + 0x35DF, kBurst, sizeof(kBurst)) == 0 &&
	    memcmp(rom + 0x3693, kFetch, sizeof(kFetch)) == 0)
		return (CARTPROT_TEKKEN2);

	return (CARTPROT_NONE);
}

bool8 S9xCartProtDetect (uint8 *rom, uint32 size)
{
	ProtKind = DetectKind(rom, size);
	return (ProtKind != CARTPROT_NONE);
}

int S9xCartProtKind (void)
{
	return (ProtKind);
}

// KOF '98 moved routines out of bank $00 and runs them from WRAM with their 32K
// ROM bank paged over $00:8000-$FFFF (bit 7 set); writing $00 pages bank 0 back.
static void SetBankOverlay (uint8 data)
{
	const uint32	bank = (data & 0x80) ? (data & 0x7F) % (Memory.CalculatedSize / 0x8000) : 0;

	CartProt.Overlay = data;

	for (uint32 block = 8; block < 16; block++)
		Memory.Map[block] = Memory.ROM + bank * 0x8000 - 0x8000;
}

// Super 20 Hab: CPU bank b shows 32K ROM bank base + b, or bank base alone when
// bit 7 is set and bit 6 clear (the 32K NES ports). The menu is at base 4.
static void SetSuper20Bank (uint8 data)
{
	const uint32	banks = Memory.CalculatedSize / 0x8000;
	const bool8		single = (data & 0xC0) == 0x80;

	CartProt.Latch = data & 0xDF;

	for (uint32 c = 0; c < 0x100; c++)
	{
		if (c == 0x7E || c == 0x7F)
			continue;
		const uint32	rom = ((data & 0x1F) + (single ? 0 : (c & 0x7F))) % banks;
		uint8			*base = (rom == 4) ? Super20Menu : Memory.ROM + rom * 0x8000;
		for (uint32 block = (c & 0x40) ? 0 : 8; block < 16; block++)
			Memory.Map[(c << 4) | block] = base - (block & 8) * 0x1000;
	}

	S9xSetPCBase(Registers.PBPC);
}

void S9xCartProtReset (void)
{
	memset(&CartProt, 0, sizeof(CartProt));
	CartProt.Answer = kAnswer[0];

	if (ProtKind == CARTPROT_BITSWAP && BankOverlay)
		SetBankOverlay(0);
	if (ProtKind == CARTPROT_SUPER20)
		SetSuper20Bank(4);
}

uint32 S9xCartProtStateSize (void)
{
	return (sizeof(CartProt));
}

void S9xCartProtStateSave (uint8 *buf)
{
	memcpy(buf, &CartProt, sizeof(CartProt));
}

void S9xCartProtStateLoad (const uint8 *buf, uint32 size)
{
	memcpy(&CartProt, buf, size < sizeof(CartProt) ? size : sizeof(CartProt));

	if (ProtKind == CARTPROT_BITSWAP && BankOverlay)
		SetBankOverlay(CartProt.Overlay);
	if (ProtKind == CARTPROT_SUPER20)
		SetSuper20Bank(CartProt.Latch);
}

// A Bug's Life: the chip decodes A11-A8 of $6000-$6FFF only.
static void BugsLifeWrite (uint32 address, uint8 data)
{
	switch ((address >> 8) & 0xF)
	{
		case 0x0:	memset(CartProt.Xor, 0, sizeof(CartProt.Xor));	break;
		case 0x2:	CartProt.Xor[0] ^= data & 0xF;	break;
		case 0x4:	CartProt.Xor[1] ^= data & 0xF;	break;
		case 0x6:	CartProt.Xor[2] ^= data & 0xF;	break;
		case 0x8:	CartProt.Xor[3] ^= data & 0xF;	break;
		default:	break;
	}
}

// Only D0-D3 are driven; the upper nibble floats.
static uint8 BugsLifeRead (uint32 address)
{
	uint8	v;

	switch ((address >> 8) & 0xF)
	{
		case 0x1:	v = CartProt.Xor[0];	break;
		case 0x3:	v = CartProt.Xor[1];	break;
		case 0x5:	v = CartProt.Xor[2];	break;
		case 0x7:	v = CartProt.Xor[3];	break;
		case 0xF:	v = CartProt.Xor[0] ^ CartProt.Xor[1] ^ CartProt.Xor[2] ^ CartProt.Xor[3];	break;
		default:	return (OpenBus);
	}

	return ((OpenBus & 0xF0) | v);
}

// Bitswap block 4 of the system banks: the joypad ports and $4200 up stay the CPU's.
static bool8 BitswapPassThrough (uint32 address)
{
	const uint32	a = address & 0xFFFF;
	return (!(address & 0x400000) && (a & 0xF000) == 0x4000 && (a >= 0x4200 || a == 0x4016 || a == 0x4017));
}

void S9xCartProtSetRomBase (uint32 bank, uint8 *base)
{
	if (bank >= 0x80 && bank <= 0xBF)
		ProtRomBase[bank - 0x80] = base;
}

// Every write the cart window sees; only $8000-$87FF means anything.
void S9xCartProtWrite (uint32 address, uint8 data)
{
	if (ProtKind == CARTPROT_BITSWAP40)
	{
		if ((address & 0xC08000) == 0x408000 && address < 0x7E0000)
			CartProt.Latch = data;
		return;
	}

	if (ProtKind == CARTPROT_SOULBLADE)
		return;

	if (ProtKind == CARTPROT_SUPER20)
	{
		if ((address & 0xFFF000) == 0x008000)
			SetSuper20Bank(data);
		return;
	}

	if (ProtKind == CARTPROT_BITSWAP)
	{
		if (BankOverlay && (address & 0xC08000) == 0xC08000)
			SetBankOverlay(data);
		if (((address >> 16) & 0x86) != 0x80)
			return;
		if (BitswapPassThrough(address))
			S9xSetCPU(data, address & 0xFFFF);
		CartProt.Latch = data;
		return;
	}

	if (ProtKind == CARTPROT_BUGSLIFE)
	{
		if ((address & 0xF000) == 0x6000)
			BugsLifeWrite(address, data);
		return;
	}

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
	if (ProtKind == CARTPROT_SOULBLADE)
	{
		static const uint8	kSequence[4] = { 0x55, 0x0F, 0xAA, 0xF0 };
		return ((address & 0x400000) ? OpenBus : kSequence[address & 3]);
	}

	if (ProtKind == CARTPROT_BITSWAP || ProtKind == CARTPROT_BITSWAP40)
	{
		if (ProtKind == CARTPROT_BITSWAP && BitswapPassThrough(address))
			return (S9xGetCPU(address & 0xFFFF));

		const uint8	v = CartProt.Latch;
		return ((v & 0x01) << 7 | (v & 0x40) | (v & 0x80) >> 2 | (v & 0x02) << 3 |
		        (v & 0x04) << 1 | (v & 0x08) >> 1 | (v & 0x10) >> 3 | (v & 0x20) >> 5);
	}

	if (ProtKind == CARTPROT_BUGSLIFE)
		return (((address & 0xF000) == 0x6000) ? BugsLifeRead(address) : OpenBus);

	if (CartProt.Armed && (address & 0xFF00) == 0x8100)
		return (CartProt.Answer);

	const uint32	bank = (address >> 16) & 0xFF;
	uint8			*base = (bank >= 0x80 && bank <= 0xBF) ? ProtRomBase[bank - 0x80] : NULL;

	return (base ? base[address & 0xFFFF] : OpenBus);
}
