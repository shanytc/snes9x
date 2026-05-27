#include "CMemoryRegions.h"
#include "CDebuggerSnes.h"
#include "../snes9x.h"
#include "../memmap.h"
#include "../ppu.h"
#include "../sgb/sgb.h"

static uint32_t SnesCpuBusSize()  { return 0x1000000u; }
static uint8_t  SnesCpuBusRead(uint32_t a) { return SnesBackend::ReadByte(a & 0xFFFFFF); }
static uint32_t SnesCpuBusAddr(uint32_t off) { return off & 0xFFFFFFu; }

static uint32_t SnesRomSize()
{
	return (uint32_t)Memory.CalculatedSize;
}
static uint8_t SnesRomRead(uint32_t off)
{
	if (Memory.ROM && off < (uint32_t)Memory.CalculatedSize)
		return Memory.ROM[off];
	return 0;
}

static uint32_t SnesWramSize() { return 0x20000u; }
static uint8_t  SnesWramRead(uint32_t off)
{
	if (off < 0x20000u) return Memory.RAM[off];
	return 0;
}
static uint32_t SnesWramAddr(uint32_t off)
{
	if (off < 0x20000u) return 0x7E0000u + off;
	return kNoCpuAddr;
}

static uint32_t SnesVramSize() { return 0x10000u; }
static uint8_t  SnesVramRead(uint32_t off)
{
	if (off < 0x10000u) return Memory.VRAM[off];
	return 0;
}

static uint32_t SnesOamSize() { return 0x220u; }
static uint8_t  SnesOamRead(uint32_t off)
{
	if (off < 0x220u) return PPU.OAMData[off];
	return 0;
}

static uint32_t SnesCgramSize() { return 0x200u; }
static uint8_t  SnesCgramRead(uint32_t off)
{
	if (off < 0x200u)
	{
		const uint8_t *p = (const uint8_t *)PPU.CGDATA;
		return p[off];
	}
	return 0;
}

static uint32_t SnesSramSize()
{
	return Memory.SRAMMask ? (uint32_t)(Memory.SRAMMask + 1) : 0u;
}
static uint8_t SnesSramRead(uint32_t off)
{
	if (Memory.SRAM && Memory.SRAMMask && off <= (uint32_t)Memory.SRAMMask)
		return Memory.SRAM[off];
	return 0;
}

static uint32_t SnesFillRamSize() { return 0x8000u; }
static uint8_t  SnesFillRamRead(uint32_t off)
{
	if (off < 0x8000u) return Memory.FillRAM[off];
	return 0;
}
static uint32_t SnesFillRamAddr(uint32_t off)
{
	if (off < 0x8000u) return off;
	return kNoCpuAddr;
}

static const MemRegion kSnesRegions[] =
{
	{ "CPU Bus",       SnesCpuBusSize,  SnesCpuBusRead,  6, SnesCpuBusAddr  },
	{ "PRG ROM",       SnesRomSize,     SnesRomRead,     6, nullptr         },
	{ "Work RAM",      SnesWramSize,    SnesWramRead,    5, SnesWramAddr    },
	{ "Video RAM",     SnesVramSize,    SnesVramRead,    4, nullptr         },
	{ "OAM",           SnesOamSize,     SnesOamRead,     4, nullptr         },
	{ "CG RAM",        SnesCgramSize,   SnesCgramRead,   4, nullptr         },
	{ "SRAM",          SnesSramSize,    SnesSramRead,    6, nullptr         },
	{ "Registers",     SnesFillRamSize, SnesFillRamRead, 4, SnesFillRamAddr }
};

static uint32_t GbCpuBusSize() { return 0x10000u; }
static uint8_t  GbCpuBusRead(uint32_t a) { return S9xSGBPeekRAByte(a & 0xFFFFu); }
static uint32_t GbCpuBusAddr(uint32_t off) { return off & 0xFFFFu; }

static uint32_t GbRomSize() { return S9xSGBGetROMSize(); }
static uint8_t  GbRomRead(uint32_t off) { return S9xSGBPeekROMByte(off); }

static uint32_t GbWramSize() { return 0x2000u; }
static uint8_t  GbWramRead(uint32_t off)
{
	if (off < 0x2000u) return S9xSGBPeekRAByte(0xC000u + off);
	return 0;
}
static uint32_t GbWramAddr(uint32_t off)
{
	if (off < 0x2000u) return 0xC000u + off;
	return kNoCpuAddr;
}

static uint32_t GbVramSize() { return 0x2000u; }
static uint8_t  GbVramRead(uint32_t off)
{
	if (off < 0x2000u) return S9xSGBPeekRAByte(0x8000u + off);
	return 0;
}
static uint32_t GbVramAddr(uint32_t off)
{
	if (off < 0x2000u) return 0x8000u + off;
	return kNoCpuAddr;
}

static uint32_t GbOamSize() { return 0xA0u; }
static uint8_t  GbOamRead(uint32_t off)
{
	if (off < 0xA0u) return S9xSGBPeekRAByte(0xFE00u + off);
	return 0;
}
static uint32_t GbOamAddr(uint32_t off)
{
	if (off < 0xA0u) return 0xFE00u + off;
	return kNoCpuAddr;
}

static uint32_t GbHramSize() { return 0x7Fu; }
static uint8_t  GbHramRead(uint32_t off)
{
	if (off < 0x7Fu) return S9xSGBPeekRAByte(0xFF80u + off);
	return 0;
}
static uint32_t GbHramAddr(uint32_t off)
{
	if (off < 0x7Fu) return 0xFF80u + off;
	return kNoCpuAddr;
}

static uint32_t GbBootRomSize() { return S9xSGBGetBootROMSize(); }
static uint8_t  GbBootRomRead(uint32_t off) { return S9xSGBPeekBootROMByte(off); }

static uint32_t GbIoSize() { return 0x80u; }
static uint8_t  GbIoRead(uint32_t off)
{
	if (off < 0x80u) return S9xSGBPeekRAByte(0xFF00u + off);
	return 0;
}
static uint32_t GbIoAddr(uint32_t off)
{
	if (off < 0x80u) return 0xFF00u + off;
	return kNoCpuAddr;
}

static uint32_t GbSramSize() { return 0x2000u; }
static uint8_t  GbSramRead(uint32_t off)
{
	if (off < 0x2000u) return S9xSGBPeekRAByte(0xA000u + off);
	return 0;
}
static uint32_t GbSramAddr(uint32_t off)
{
	if (off < 0x2000u) return 0xA000u + off;
	return kNoCpuAddr;
}

static const MemRegion kGbRegions[] =
{
	{ "CPU Memory",     GbCpuBusSize,  GbCpuBusRead,  4, GbCpuBusAddr },
	{ "PRG ROM",        GbRomSize,     GbRomRead,     6, nullptr      },
	{ "Work RAM",       GbWramSize,    GbWramRead,    4, GbWramAddr   },
	{ "High RAM",       GbHramSize,    GbHramRead,    4, GbHramAddr   },
	{ "Boot ROM",       GbBootRomSize, GbBootRomRead, 4, nullptr      },
	{ "Video RAM",      GbVramSize,    GbVramRead,    4, GbVramAddr   },
	{ "Sprite RAM",     GbOamSize,     GbOamRead,     4, GbOamAddr    },
	{ "I/O Registers",  GbIoSize,      GbIoRead,      4, GbIoAddr     },
	{ "SRAM",           GbSramSize,    GbSramRead,    4, GbSramAddr   }
};

int MemRegionCount(DbgSystem sys)
{
	if (sys == DbgSystem::Snes) return (int)(sizeof(kSnesRegions)/sizeof(kSnesRegions[0]));
	if (sys == DbgSystem::Gb)   return (int)(sizeof(kGbRegions)/sizeof(kGbRegions[0]));
	return 0;
}

const MemRegion *MemRegionAt(DbgSystem sys, int index)
{
	if (sys == DbgSystem::Snes)
	{
		const int n = MemRegionCount(sys);
		if (index < 0 || index >= n) return nullptr;
		return &kSnesRegions[index];
	}
	if (sys == DbgSystem::Gb)
	{
		const int n = MemRegionCount(sys);
		if (index < 0 || index >= n) return nullptr;
		return &kGbRegions[index];
	}
	return nullptr;
}
