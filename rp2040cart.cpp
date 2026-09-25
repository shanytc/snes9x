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

#ifdef UNZIP_SUPPORT
#ifdef SYSTEM_ZIP
#include <minizip/unzip.h>
#else
#include "unzip/unzip.h"
#endif
#endif

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

// The libretro save image is kept current once a frontend has asked for it.
static bool8	save_image_on;
static void		RefreshSaveImage (void);

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

static bool ReadFirmwareFile (const std::string &path)
{
	FILE *f = fopen(path.c_str(), "rb");
	if (!f)
		return false;
	firmware.assign(16 << 20, 0xff);
	size_t n = fread(&firmware[0], 1, firmware.size(), f);
	fclose(f);
	firmware.resize(n);
	return n > 0;
}

// A zip can carry the firmware beside the ROM: the entry called wanted
// wins, else the first *_rp2040.bin in it.
static bool ReadFirmwareFromZip (const char *zip_path, const std::string &wanted)
{
#ifdef UNZIP_SUPPORT
	SplitPath zp = splitpath(zip_path);
	if (!zp.ext_is(".zip") && !zp.ext_is(".msu1"))
		return false;
	unzFile z = unzOpen(zip_path);
	if (!z)
		return false;

	std::string pick;
	char name[260];
	unz_file_info info;
	for (int port = unzGoToFirstFile(z); port == UNZ_OK; port = unzGoToNextFile(z))
	{
		if (unzGetCurrentFileInfo(z, &info, name, sizeof(name), NULL, 0, NULL, 0) != UNZ_OK)
			break;
		size_t len = strlen(name);
		if (len <= 11 || strcasecmp(name + len - 11, "_rp2040.bin") != 0 || info.uncompressed_size > (16u << 20))
			continue;
		if (pick.empty() || !strcasecmp(S9xBasename(name).c_str(), wanted.c_str()))
			pick = name;
	}

	bool ok = false;
	if (!pick.empty() && unzLocateFile(z, pick.c_str(), 0) == UNZ_OK &&
		unzGetCurrentFileInfo(z, &info, NULL, 0, NULL, 0, NULL, 0) == UNZ_OK &&
		unzOpenCurrentFile(z) == UNZ_OK)
	{
		firmware.resize(info.uncompressed_size);
		int got = unzReadCurrentFile(z, firmware.data(), (unsigned) firmware.size());
		ok = unzCloseCurrentFile(z) == UNZ_OK && got == (int) firmware.size() && got > 0;
		if (!ok)
			firmware.clear();
	}
	unzClose(z);
	return ok;
#else
	(void) zip_path;
	(void) wanted;
	return false;
#endif
}

// libretro frontends unpack archived content themselves and pass "X.zip#rom";
// the firmware is still inside X.zip.
static std::string archive_path;

void S9xRP2040CartSetArchive (const char *path)
{
	archive_path = path ? path : "";
}

bool8 S9xRP2040CartActivate (const char *rom_path)
{
	S9xRP2040CartDeactivate();

	std::string path = FirmwarePath(rom_path);
	std::string name = S9xBasename(path);
	if (!ReadFirmwareFile(path) && !ReadFirmwareFromZip(rom_path, name) &&
		(archive_path.empty() || !ReadFirmwareFromZip(archive_path.c_str(), name)))
	{
		std::string msg = "RP2040 firmware missing - place " + name + " next to the ROM or in its zip";
		S9xSetBiosNotice(msg.c_str());
		return FALSE;
	}

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
	save_image_on = FALSE;
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
		if (save_image_on)
			RefreshSaveImage();
	}
}

#define FLASH_SECTOR	0x1000
#define SAVE_IMAGE_SIZE	0x10000		// room for 15 sectors; the game saves to one

// The flash sectors that differ from the firmware file.
static std::vector<uint32> ChangedSectors (void)
{
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
	return changed;
}

// .srm layout: "RP2040FL", u32 count, then count x (u32 offset, 4K sector).
static std::vector<uint8> EncodeFlash (size_t cap)
{
	const std::vector<uint8_t> &flash = chip->Flash();
	std::vector<uint32> changed = ChangedSectors();
	if (changed.size() > (cap - 12) / (4 + FLASH_SECTOR))
	{
		changed.resize((cap - 12) / (4 + FLASH_SECTOR));
		S9xMessage(S9X_WARNING, S9X_NO_INFO, "RP2040: flash save is larger than the save image; sectors dropped");
	}
	uint32 count = (uint32) changed.size();
	std::vector<uint8> out(12 + count * (4 + FLASH_SECTOR));
	memcpy(&out[0], "RP2040FL", 8);
	memcpy(&out[8], &count, 4);
	uint8 *p = &out[12];
	for (uint32 off : changed)
	{
		memcpy(p, &off, 4);
		memcpy(p + 4, &flash[off], FLASH_SECTOR);
		p += 4 + FLASH_SECTOR;
	}
	return out;
}

// Rebuilds flash as the firmware file plus the saved sectors. A malformed
// image leaves flash alone; bytes after the last sector are ignored.
static bool DecodeFlash (const uint8 *buf, size_t size)
{
	std::vector<uint8_t> &flash = chip->FlashMutable();
	uint32 count;
	if (size < 12 || memcmp(buf, "RP2040FL", 8))
		return false;
	memcpy(&count, buf + 8, 4);
	if (count > (size - 12) / (4 + FLASH_SECTOR))
		return false;
	for (uint32 i = 0; i < count; i++)
	{
		uint32 off;
		memcpy(&off, buf + 12 + i * (4 + FLASH_SECTOR), 4);
		if (off % FLASH_SECTOR || off + FLASH_SECTOR > flash.size())
			return false;
	}

	size_t n = firmware.size() < flash.size() ? firmware.size() : flash.size();
	memcpy(&flash[0], firmware.data(), n);
	memset(&flash[n], 0xff, flash.size() - n);
	for (uint32 i = 0; i < count; i++)
	{
		const uint8 *e = buf + 12 + i * (4 + FLASH_SECTOR);
		uint32 off;
		memcpy(&off, e, 4);
		memcpy(&flash[off], e + 4, FLASH_SECTOR);
	}
	return true;
}

bool8 S9xRP2040CartLoadFlash (const char *srm_path)
{
	if (!chip)
		return FALSE;
	FILE *f = fopen(srm_path, "rb");
	if (!f)
		return FALSE;
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	std::vector<uint8> buf(len > 0 ? (size_t) len : 0);
	bool8 ok = !buf.empty() && fread(buf.data(), 1, buf.size(), f) == buf.size() && DecodeFlash(buf.data(), buf.size());
	fclose(f);
	chip->ClearFlashDirty();
	return ok;
}

bool8 S9xRP2040CartSaveFlash (const char *srm_path)
{
	if (!chip)
		return FALSE;
	std::vector<uint8> data = EncodeFlash((size_t) -1);
	if (data.size() == 12)
		return TRUE;		// nothing saved yet
	FILE *f = fopen(srm_path, "wb");
	if (!f)
		return FALSE;
	bool8 ok = fwrite(data.data(), 1, data.size(), f) == data.size();
	fclose(f);
	return ok;
}

// save_built: what the image held after the core last wrote it, so a
// change means the frontend loaded a save into it.
static uint8	save_image[SAVE_IMAGE_SIZE];
static uint8	save_built[SAVE_IMAGE_SIZE];

static void RefreshSaveImage (void)
{
	std::vector<uint8> data = EncodeFlash(SAVE_IMAGE_SIZE);
	memset(save_image, 0, SAVE_IMAGE_SIZE);
	memcpy(save_image, data.data(), data.size());
	memcpy(save_built, save_image, SAVE_IMAGE_SIZE);
}

uint8 *S9xRP2040CartSaveImage (void)
{
	if (!chip)
		return NULL;
	if (!save_image_on)
	{
		RefreshSaveImage();
		save_image_on = TRUE;
	}
	return save_image;
}

size_t S9xRP2040CartSaveImageSize (void)
{
	return chip ? SAVE_IMAGE_SIZE : 0;
}

void S9xRP2040CartSyncSaveImage (void)
{
	if (!chip || !save_image_on || !memcmp(save_image, save_built, SAVE_IMAGE_SIZE))
		return;
	if (!DecodeFlash(save_image, SAVE_IMAGE_SIZE))
		S9xMessage(S9X_WARNING, S9X_NO_INFO, "RP2040: the save file is not an RP2040 flash save; ignored");
	chip->ClearFlashDirty();
	RefreshSaveImage();
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
