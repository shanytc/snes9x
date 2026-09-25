/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// The cart wiring, from the firmware's own PIO setup: D0-D7 on GPIO 0-7,
// /RD on GPIO 8, /WR on GPIO 9, the $3000 decode on GPIO 10 and the level
// shifter enable on GPIO 15. One state machine answers every read with the
// next byte of a DMA-fed FIFO (0 when it runs dry), another pushes every
// written byte into a DMA ring. The RP2040 never sees the address.

#include "snes9x.h"
#include "memmap.h"
#include "display.h"
#include "rp2040.h"
#include "rp2040cart.h"
#include "fscompat.h"

#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#define PIN_RD		(1u << 8)
#define PIN_WR		(1u << 9)
#define PIN_CS		(1u << 10)
#define PINS_IDLE	0x3fffffffu

static RP2040::Chip	*chip = NULL;
void (*S9xRP2040CartTraceHook) (bool, uint32, uint8) = NULL;
static std::vector<uint8_t> firmware;

// SNES master cycles at the start of the current scanline, the SNES time the
// chip was last brought up to, and the chip's clock at that point.
static uint64	snes_line_base;
static uint64	snes_synced;
static uint64	rp_target;
static uint64	rp_rem;

// The opcode the S-CPU fetched ahead of a DMA, handed back to its real fetch.
static bool8	prefetched;
static uint32	prefetch_addr;
static uint8	prefetch_byte;

RP2040::Chip *S9xRP2040CartChip (void)
{
	return chip;
}

bool8 S9xRP2040CartDetect (const uint8 *rom, uint32 size)
{
	// LoROM header, chipset byte $63 (no Nintendo chip uses nibble 6),
	// maker "BM" in the extended header.
	if (size < 0x8000)
		return FALSE;
	return rom[0x7fd6] == 0x63 && rom[0x7fda] == 0x33 && memcmp(rom + 0x7fb0, "BM", 2) == 0;
}

// The header only has room for "XENOCRISIS"; titles by the game code.
const char *S9xRP2040CartTitle (void)
{
	static const struct { char code[5]; const char *title; } titles[] =
	{
		{ "XCRI", "Xeno Crisis" }
	};
	for (unsigned i = 0; i < sizeof(titles) / sizeof(titles[0]); i++)
		if (Memory.CalculatedSize >= 0x8000 && !memcmp(Memory.ROM + 0x7fb2, titles[i].code, 4))
			return titles[i].title;
	return Memory.ROMName;
}

// Beside the ROM, found the way an MSU-1 pack is: <rom name>_rp2040.bin.
static std::string FirmwarePath (const char *rom_path)
{
	return S9xGetFilename(rom_path, "_rp2040.bin", ROMFILENAME_DIR);
}

bool8 S9xRP2040CartActivate (const char *rom_path)
{
	S9xRP2040CartDeactivate();

	std::string path = FirmwarePath(rom_path);
	FILE *f = fopen(path.c_str(), "rb");
	if (!f)
	{
		std::string msg = "RP2040 firmware missing - place " + S9xBasename(path) + " next to the ROM";
		S9xSetBiosNotice(msg.c_str());
		return FALSE;
	}
	firmware.assign(16 << 20, 0xff);
	size_t n = fread(&firmware[0], 1, firmware.size(), f);
	fclose(f);
	firmware.resize(n);

	chip = new RP2040::Chip;
	if (!chip->LoadFlash(&firmware[0], firmware.size()))
	{
		delete chip;
		chip = NULL;
		S9xSetBiosNotice("RP2040 firmware image is not a flash dump");
		return FALSE;
	}
	S9xRP2040CartPowerOn();
	return TRUE;
}

void S9xRP2040CartDeactivate (void)
{
	delete chip;
	chip = NULL;
	firmware.clear();
}

void S9xRP2040CartPowerOn (void)
{
	if (!chip)
		return;
	chip->SetGpioInputs(PINS_IDLE, 0);
	chip->PowerOn();
	snes_line_base = 0;
	snes_synced = 0;
	rp_target = 0;
	rp_rem = 0;
	prefetched = FALSE;
}

// The reset button reaches the cart too: the firmware reboots with the SNES.
void S9xRP2040CartSoftReset (void)
{
	S9xRP2040CartPowerOn();
}

static inline void Sync (void)
{
	uint64 t = snes_line_base + (uint64) (int64) CPU.Cycles;
	if (t <= snes_synced)
		return;
	uint64 master = Settings.PAL ? 21281370 : 21477273;
	uint64 acc = (t - snes_synced) * chip->SysHz() + rp_rem;
	snes_synced = t;
	rp_target += acc / master;
	rp_rem = acc % master;
	chip->RunUntil(rp_target);
}

void S9xRP2040CartEndScanline (void)
{
	if (!chip)
		return;
	Sync();
	snes_line_base += Timings.H_Max;
	if (chip->FlashDirty())
	{
		chip->ClearFlashDirty();
		CPU.SRAMModified = TRUE;
	}
}

#define FLASH_SECTOR	0x1000

bool8 S9xRP2040CartLoadFlash (const char *srm_path)
{
	if (!chip)
		return FALSE;
	FILE *f = fopen(srm_path, "rb");
	if (!f)
		return FALSE;
	char magic[8];
	uint32 count = 0;
	bool8 ok = fread(magic, 1, 8, f) == 8 && !memcmp(magic, "RP2040FL", 8) && fread(&count, 4, 1, f) == 1;
	std::vector<uint8_t> &flash = chip->FlashMutable();
	for (uint32 i = 0; ok && i < count; i++)
	{
		uint32 offset;
		std::vector<uint8_t> sector(FLASH_SECTOR);
		ok = fread(&offset, 4, 1, f) == 1 && fread(sector.data(), 1, FLASH_SECTOR, f) == FLASH_SECTOR &&
			 !(offset % FLASH_SECTOR) && offset + FLASH_SECTOR <= flash.size();
		if (ok)
			memcpy(&flash[offset], sector.data(), FLASH_SECTOR);
	}
	fclose(f);
	chip->ClearFlashDirty();
	return ok;
}

bool8 S9xRP2040CartSaveFlash (const char *srm_path)
{
	if (!chip)
		return FALSE;
	const std::vector<uint8_t> &flash = chip->Flash();
	std::vector<uint32> changed;
	for (uint32 off = 0; off + FLASH_SECTOR <= flash.size(); off += FLASH_SECTOR)
	{
		bool differs;
		if (off + FLASH_SECTOR <= firmware.size())
			differs = memcmp(&flash[off], &firmware[off], FLASH_SECTOR) != 0;
		else
		{
			differs = false;
			for (uint32 i = 0; i < FLASH_SECTOR && !differs; i++)
				differs = flash[off + i] != (off + i < firmware.size() ? firmware[off + i] : 0xff);
		}
		if (differs)
			changed.push_back(off);
	}
	if (changed.empty())
		return TRUE;
	FILE *f = fopen(srm_path, "wb");
	if (!f)
		return FALSE;
	uint32 count = (uint32) changed.size();
	fwrite("RP2040FL", 1, 8, f);
	fwrite(&count, 4, 1, f);
	for (uint32 off : changed)
	{
		fwrite(&off, 4, 1, f);
		fwrite(&flash[off], 1, FLASH_SECTOR, f);
	}
	fclose(f);
	return TRUE;
}

uint8 S9xRP2040CartRead (uint32 address)
{
	if (!chip)
		return OpenBus;
	if (prefetched && !CPU.InDMAorHDMA && address == prefetch_addr)
	{
		prefetched = FALSE;
		return prefetch_byte;
	}
	Sync();
	chip->SetGpioInputs(PINS_IDLE & ~(PIN_CS | PIN_RD), 0);
	chip->StepPio();
	uint8 byte = (uint8) chip->GpioOutputs();
	chip->SetGpioInputs(PINS_IDLE, 0);
	chip->StepPio();
	if (S9xRP2040CartTraceHook)
		S9xRP2040CartTraceHook(false, address, byte);
	return byte;
}

void S9xRP2040CartDMAPrefetch (void)
{
	const uint32	pc = Registers.PBPC & 0xffffff;
	if (!chip || prefetched || Memory.Map[pc >> MEMMAP_SHIFT] != (uint8 *) CMemory::MAP_RP2040)
		return;
	prefetch_byte = S9xRP2040CartRead(pc);
	prefetch_addr = pc;
	prefetched = TRUE;
}

// The SNES drives D0-D7 through the transceiver, overpowering whatever the
// chip's own data pins still output.
void S9xRP2040CartWrite (uint8 byte, uint32 address)
{
	if (!chip)
		return;
	if (S9xRP2040CartTraceHook)
		S9xRP2040CartTraceHook(true, address, byte);
	Sync();
	uint32 bus = (PINS_IDLE & ~0xffu) | byte;
	chip->SetGpioInputs(bus & ~(PIN_CS | PIN_WR), 0xff);
	chip->StepPio();
	chip->SetGpioInputs(PINS_IDLE, 0);
	chip->StepPio();
}

size_t S9xRP2040CartStateSize (void)
{
	if (!chip)
		return 0;
	return 40 + chip->StateSize();
}

void S9xRP2040CartStateSave (uint8 *buf)
{
	if (!chip)
		return;
	memcpy(buf + 0, &snes_line_base, 8);
	memcpy(buf + 8, &snes_synced, 8);
	memcpy(buf + 16, &rp_target, 8);
	memcpy(buf + 24, &rp_rem, 8);
	buf[32] = prefetched;
	buf[33] = prefetch_byte;
	memcpy(buf + 36, &prefetch_addr, 4);
	chip->SaveState(buf + 40);
}

bool8 S9xRP2040CartStateLoad (const uint8 *buf, size_t size)
{
	if (!chip || size != S9xRP2040CartStateSize())
		return FALSE;
	if (!chip->LoadState(buf + 40, size - 40))
		return FALSE;
	memcpy(&snes_line_base, buf + 0, 8);
	memcpy(&snes_synced, buf + 8, 8);
	memcpy(&rp_target, buf + 16, 8);
	memcpy(&rp_rem, buf + 24, 8);
	prefetched = buf[32];
	prefetch_byte = buf[33];
	memcpy(&prefetch_addr, buf + 36, 4);
	return TRUE;
}
