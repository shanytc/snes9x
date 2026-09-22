/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Nintendo Super System supervisor board (see nss.h / docs/nss.md).
// Register behavior follows fullsnes "NSS Memory and I/O Maps" and the
// chip-level protocols of MAME's rp5h01 / m6m80011ap / s3520cf / m50458.

#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <vector>
#include "snes9x.h"
#include "memmap.h"
#include "biosmanager.h"
#include "display.h"
#include "gfx.h"
#include "ppu.h"
#include "movie.h"
#include "apu/apu.h"
#include "z80.h"
#include "nss.h"

#ifdef UNZIP_SUPPORT
#  ifdef SYSTEM_ZIP
#    include <minizip/unzip.h>
#  else
#    include "unzip/unzip.h"
#  endif
#endif

struct SNSS	NSS;

// NSS_TRACE=1 logs the supervisor's side of the conversation (port writes,
// reset-line moves and a periodic Z80 PC sample); =2 adds every OSD word.
static int TraceLevel (void)
{
	static int	cached = -1;
	if (cached < 0)
	{
		const char	*e = getenv("NSS_TRACE");
		cached = (e && *e) ? atoi(e) : 0;
	}
	return (cached);
}

static inline int TraceEnabled (void)	{ return (TraceLevel() > 0); }

// ---------------------------------------------------------------------------
// RP5H01 key chip (72 bits, read one bit at a time through E000h)
//
// A dumped security.prm holds the logical bits; the chip's open-drain
// outputs invert them, which is what the BIOS's decryptor expects.

static inline uint8 PROMDataBit (void)
{
	const uint8	mask = NSS.PROM.SevenBit ? 0x7f : 0x3f;
	const uint8	idx = (uint8) ((NSS.PROM.Counter & mask) >> 3);
	return (uint8) (((NSS.Slot[NSS.SlotSelect].PROM[idx] >> (NSS.PROM.Counter & 7)) & 1) ^ 1);
}

static inline uint8 PROMCounterBit (void)
{
	return (uint8) (((NSS.PROM.Counter >> 5) & 1) ^ 1);
}

// The reset pin pins the counter at zero, and a clock edge that arrives
// while it is still asserted never reaches the counter. That includes the
// edge the decryptor issues in the very write that releases reset, which
// fullsnes calls out as the glitch half the games depend on getting right.
static void PROMWrite (uint8 byte)
{
	const uint8	clock = (byte & 0x08) ? 1 : 0;
	const uint8	reset = (byte & 0x01) ? 0 : 1;

	NSS.PROM.SevenBit = (byte & 0x10) ? 1 : 0;

	if (!NSS.PROM.Reset && NSS.PROM.LastClock && !clock)
		NSS.PROM.Counter++;
	NSS.PROM.LastClock = clock;

	if (reset)
		NSS.PROM.Counter = 0;
	NSS.PROM.Reset = reset;
}

// ---------------------------------------------------------------------------
// M6M80011 settings EEPROM (bit-banged through E000h, read back at A000h)

enum
{
	EE_IDLE = 0, EE_WRITE_ENABLE, EE_WRITE_DISABLE, EE_WRITE, EE_READ, EE_STATUS
};

static void EEPROMClockIn (void)
{
	struct SNSSEEPROM	*e = &NSS.EEPROM;

	switch (e->State)
	{
		case EE_IDLE:
			e->Shift = (e->Shift >> 1) | ((uint32) (e->DataIn & 1) << 7);
			if (++e->BitPos < 8)
				break;
			e->BitPos = 0;
			switch (e->Shift & 0xff)
			{
				case 0xc5:	e->State = EE_WRITE_ENABLE;		break;
				case 0x05:	e->State = EE_WRITE_DISABLE;	break;
				case 0x25:	e->State = EE_WRITE;			break;
				case 0x15:	e->State = EE_READ;				break;
				case 0x95:	e->State = EE_STATUS;			break;
				default:
					if (TraceEnabled())
						printf("[nss] EEPROM unknown command %02X\n", e->Shift & 0xff);
					break;
			}
			e->Shift = 0;
			break;

		case EE_READ:
			e->Shift = (e->Shift >> 1) | ((uint32) (e->DataIn & 1) << 23);
			e->BitPos++;
			if (e->BitPos == 8)
				e->Addr = (uint8) ((e->Shift >> 16) & (NSS_EEPROM_WORDS - 1));
			if (e->BitPos >= 8)
				e->DataOut = (uint8) ((e->Data[e->Addr] >> (23 - e->BitPos)) & 1);
			if (e->BitPos == 24)
			{
				e->State = EE_IDLE;
				e->BitPos = 0;
				e->Shift = 0;
			}
			break;

		case EE_WRITE:
			e->Shift = (e->Shift >> 1) | ((uint32) (e->DataIn & 1) << 23);
			e->BitPos++;
			if (e->BitPos == 8)
				e->Addr = (uint8) ((e->Shift >> 16) & (NSS_EEPROM_WORDS - 1));
			if (e->BitPos == 24)
			{
				if (e->WriteEnable)
				{
					e->Data[e->Addr] = (uint16) ((e->Shift >> 8) & 0xffff);
					e->Dirty = TRUE;
				}
				e->State = EE_IDLE;
				e->BitPos = 0;
				e->Shift = 0;
			}
			break;

		case EE_WRITE_ENABLE:
		case EE_WRITE_DISABLE:
			if (++e->BitPos == 8)
			{
				e->WriteEnable = (e->State == EE_WRITE_ENABLE) ? 1 : 0;
				e->State = EE_IDLE;
				e->BitPos = 0;
				e->Shift = 0;
			}
			break;

		case EE_STATUS:
			// Every status bit reads "ready / write-disabled / ECC ok".
			e->DataOut = 1;
			if (++e->BitPos == 8)
			{
				e->State = EE_IDLE;
				e->BitPos = 0;
				e->Shift = 0;
			}
			break;
	}
}

// E000h.W bit0 selects (active high on the board, active low at the chip),
// bit3 is data and bit4 the clock (idle high).
static void EEPROMWrite (uint8 byte)
{
	struct SNSSEEPROM	*e = &NSS.EEPROM;
	const uint8			cs = (byte & 0x01) ? 1 : 0;
	const uint8			clock = (byte & 0x10) ? 0 : 1;

	e->DataIn = (byte & 0x08) ? 1 : 0;

	if (!cs)
	{
		e->CS = 0;
		e->State = EE_IDLE;
		e->BitPos = 0;
		e->Shift = 0;
		e->Clock = clock;
		return;
	}

	e->CS = 1;
	if (clock && !e->Clock)
		EEPROMClockIn();
	e->Clock = clock;
}

// ---------------------------------------------------------------------------
// S-3520 real-time clock

static uint8 ToBCD (int v)	{ return (uint8) (((v / 10) << 4) | (v % 10)); }

static void RTCLoadHostTime (void)
{
	time_t		now = time(NULL);
	struct tm	*t = localtime(&now);

	if (!t)
		return;

	NSS.RTC.Sec = ToBCD(t->tm_sec);
	NSS.RTC.Min = ToBCD(t->tm_min);
	NSS.RTC.Hour = ToBCD(t->tm_hour);
	NSS.RTC.Day = ToBCD(t->tm_mday);
	NSS.RTC.Month = ToBCD(t->tm_mon + 1);
	NSS.RTC.Year = ToBCD(t->tm_year % 100);
	NSS.RTC.Weekday = (uint8) t->tm_wday;
}

static const uint8	days_in_month[12] =
	{ 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

static inline int FromBCD (uint8 v)	{ return ((v >> 4) * 10 + (v & 0x0f)); }

static void RTCTickSecond (void)
{
	struct SNSSRTC	*r = &NSS.RTC;
	int	s = FromBCD(r->Sec) + 1;

	if (s < 60)	{ r->Sec = ToBCD(s); return; }
	r->Sec = 0;

	int	m = FromBCD(r->Min) + 1;
	if (m < 60)	{ r->Min = ToBCD(m); return; }
	r->Min = 0;

	int	h = FromBCD(r->Hour) + 1;
	if (h < 24)	{ r->Hour = ToBCD(h); return; }
	r->Hour = 0;

	r->Weekday = (uint8) ((r->Weekday + 1) % 7);

	const int	mon = FromBCD(r->Month);
	const int	year = FromBCD(r->Year);
	int			last = (mon >= 1 && mon <= 12) ? days_in_month[mon - 1] : 31;
	if (mon == 2 && (year % 4) == 0)
		last = 29;

	int	d = FromBCD(r->Day) + 1;
	if (d <= last)	{ r->Day = ToBCD(d); return; }
	r->Day = ToBCD(1);

	int	nm = mon + 1;
	if (nm <= 12)	{ r->Month = ToBCD(nm); return; }
	r->Month = ToBCD(1);
	r->Year = ToBCD((year + 1) % 100);
}

// Register file as seen through the serial port: sixteen nibble-wide slots,
// the clock fields in mode 0/1 and the two battery SRAM pages above that.
static uint8 RTCReadReg (uint8 offset)
{
	struct SNSSRTC	*r = &NSS.RTC;

	if (offset == 0x0f)
		return (uint8) (r->Mode & 3);

	if (r->Mode > 1)
	{
		uint8	idx = (uint8) (offset + ((r->Mode > 2) ? 15 : 0));
		if (idx / 2 >= NSS_RTC_NVRAM)
			return (0);
		return (uint8) ((r->NVRAM[idx / 2] >> ((idx & 1) * 4)) & 0x0f);
	}

	switch (offset)
	{
		case 0x0:	return (uint8) (r->Sec & 0x0f);
		case 0x1:	return (uint8) (r->Sec >> 4);
		case 0x2:	return (uint8) (r->Min & 0x0f);
		case 0x3:	return (uint8) (r->Min >> 4);
		case 0x4:	return (uint8) (r->Hour & 0x0f);
		case 0x5:	return (uint8) (r->Hour >> 4);
		case 0x6:	return (uint8) (r->Weekday & 0x0f);
		case 0x7:	return (uint8) (r->Day & 0x0f);
		case 0x8:	return (uint8) (r->Day >> 4);
		case 0x9:	return (uint8) (r->Month & 0x0f);
		case 0xa:	return (uint8) (r->Month >> 4);
		case 0xb:	return (uint8) (r->Year & 0x0f);
		case 0xc:	return (uint8) (r->Year >> 4);
		case 0xd:	return (r->Control1);
		case 0xe:	return (r->Control2);
	}
	return (0);
}

// Writing a clock field bumps it by one (or by ten for the high nibble) —
// the chip has no load path, which is why the BIOS's set-time screen counts.
static void RTCWriteReg (uint8 offset, uint8 data)
{
	struct SNSSRTC	*r = &NSS.RTC;

	if (offset == 0x0f)
	{
		r->Mode = (uint8) (data & 3);
		if (data & 8)	// SYSR: reset the counters
		{
			r->Weekday = r->Hour = r->Min = r->Sec = 0;
			r->Year = r->Month = r->Day = 1;
		}
		return;
	}

	if (r->Mode > 1)
	{
		uint8	idx = (uint8) (offset + ((r->Mode > 2) ? 15 : 0));
		if (idx / 2 >= NSS_RTC_NVRAM)
			return;
		if (idx & 1)	r->NVRAM[idx / 2] = (uint8) ((r->NVRAM[idx / 2] & 0x0f) | (data << 4));
		else			r->NVRAM[idx / 2] = (uint8) ((r->NVRAM[idx / 2] & 0xf0) | (data & 0x0f));
		return;
	}

	const bool8	hold = (r->Control1 & 2) ? TRUE : FALSE;

	switch (offset)
	{
		case 0x0:	r->Sec     = hold ? 0 : (uint8) (r->Sec + 1);		break;
		case 0x1:	r->Sec     = hold ? 0 : (uint8) (r->Sec + 0x10);	break;
		case 0x2:	r->Min     = hold ? 0 : (uint8) (r->Min + 1);		break;
		case 0x3:	r->Min     = hold ? 0 : (uint8) (r->Min + 0x10);	break;
		case 0x4:	r->Hour    = hold ? 0 : (uint8) (r->Hour + 1);		break;
		case 0x5:	r->Hour    = hold ? 0 : r->Hour;					break;
		case 0x6:	r->Weekday = hold ? 0 : (uint8) (r->Weekday + 1);	break;
		case 0x7:	r->Day     = hold ? 1 : (uint8) (r->Day + 1);		break;
		case 0x8:	r->Day     = hold ? 1 : (uint8) (r->Day + 0x10);	break;
		case 0x9:	r->Month   = hold ? 1 : (uint8) (r->Month + 1);		break;
		case 0xa:	r->Month   = hold ? 1 : (uint8) (r->Month + 0x10);	break;
		case 0xb:	r->Year    = hold ? (uint8) (r->Year & 0xf0) : (uint8) (r->Year + 1);	break;
		case 0xc:	r->Year    = hold ? (uint8) (r->Year & 0x0f) : (uint8) (r->Year + 0x10);	break;
		case 0xd:	r->Control1 = (uint8) (data & 0x0f);	return;
		case 0xe:	r->Control2 = (uint8) (data & 0x0f);	return;
	}

	// Roll the BCD fields the increments just pushed past their limits.
	if ((r->Sec & 0x0f) > 9)	r->Sec = (uint8) ((r->Sec & 0xf0) + 0x10);
	if (r->Sec >= 0x60)			r->Sec = 0;
	if ((r->Min & 0x0f) > 9)	r->Min = (uint8) ((r->Min & 0xf0) + 0x10);
	if (r->Min >= 0x60)			r->Min = 0;
	if ((r->Hour & 0x0f) > 9)	r->Hour = (uint8) ((r->Hour & 0xf0) + 0x10);
	if (r->Hour >= 0x24)		r->Hour = 0;
	if (r->Weekday > 6)			r->Weekday = 0;
	if ((r->Day & 0x0f) > 9)	r->Day = (uint8) ((r->Day & 0xf0) + 0x10);
	if (r->Day >= 0x32)			r->Day = 1;
	if ((r->Month & 0x0f) > 9)	r->Month = (uint8) ((r->Month & 0xf0) + 0x10);
	if (r->Month >= 0x13)		r->Month = 1;
	if ((r->Year & 0x0f) > 9)	r->Year = (uint8) ((r->Year & 0xf0) + 0x10);
}

// The chip answers one byte behind: the bit shifted out on a clock edge
// belongs to the value latched at the end of the previous eight.
static void RTCClockIn (void)
{
	struct SNSSRTC	*r = &NSS.RTC;

	r->DataOut = (uint8) (r->Shift & 1);
	r->Shift = (uint8) ((r->Shift >> 1) | ((r->DataIn & 1) << 7));
	r->BitPos = (uint8) ((r->BitPos + 1) & 7);

	if (r->BitPos == 0)
	{
		const uint8	addr = (uint8) (r->Shift & 0x0f);
		if (!r->Dir)
			RTCWriteReg(addr, (uint8) (r->Shift >> 4));
		r->Shift = (uint8) (addr | (RTCReadReg(addr) << 4));
	}
}

// ---------------------------------------------------------------------------
// M50458 on-screen display. Sixteen-bit words arrive LSB first while /CS is
// low; the first names an address, the rest are data with auto-increment.

static void OSDWriteWord (uint16 addr, uint16 data)
{
	if (addr < NSS_OSD_CELLS)
	{
		NSS.OSD.VRAM[addr] = data;
		return;
	}
	if (addr >= 0x120 && addr < 0x120 + NSS_OSD_REGS)
	{
		NSS.OSD.Reg[addr - 0x120] = data;
		if (addr == 0x127 && (data & 0x20))	// RAMERS: clear the character plane
		{
			for (int i = 0; i < NSS_OSD_CELLS; i++)
				NSS.OSD.VRAM[i] = 0x007f;
		}
	}
}

static void OSDClockIn (void)
{
	struct SNSSOSD	*o = &NSS.OSD;

	o->Shift = (uint16) ((o->Shift >> 1) | ((o->DataIn & 1) << 15));
	if (++o->BitPos < 16)
		return;

	o->BitPos = 0;
	if (!o->HaveAddr)
	{
		o->Addr = o->Shift;
		o->HaveAddr = TRUE;
		if (TraceLevel() > 1)
			printf("[osd] addr %04X\n", o->Addr);
	}
	else
	{
		if (TraceLevel() > 1)
			printf("[osd] %04X <- %04X\n", o->Addr, o->Shift);
		OSDWriteWord(o->Addr, o->Shift);
		if (++o->Addr > 0x127)
			o->Addr = 0;
	}
	o->Shift = 0;
}

// ---------------------------------------------------------------------------
// Port 02h.W drives the clock and the display on separate pin groups.

static void RTCOSDWrite (uint8 byte)
{
	struct SNSSRTC	*r = &NSS.RTC;
	struct SNSSOSD	*o = &NSS.OSD;

	// RTC: bit0 /CS (high = deselected), bit1 direction, bit2 data,
	// bit3 /CLK (idle high).
	const uint8	rtc_cs = (byte & 0x01) ? 1 : 0;
	const uint8	rtc_clk = (byte & 0x08) ? 0 : 1;

	r->Dir = (byte & 0x02) ? 1 : 0;
	r->DataIn = (byte & 0x04) ? 1 : 0;

	if (rtc_cs)
	{
		r->CS = 0;
		r->BitPos = 0;
	}
	else
	{
		r->CS = 1;
		if (rtc_clk && !r->Clock)
			RTCClockIn();
	}
	r->Clock = rtc_clk;

	// OSD: bit4 /CS (high = deselected), bit5 data, bit7 clock (idle high).
	const uint8	osd_cs = (byte & 0x10) ? 1 : 0;
	const uint8	osd_clk = (byte & 0x80) ? 0 : 1;

	o->DataIn = (byte & 0x20) ? 1 : 0;

	if (osd_cs)
	{
		o->CS = 0;
		o->BitPos = 0;
		o->Shift = 0;
		o->HaveAddr = FALSE;
	}
	else
	{
		o->CS = 1;
		if (osd_clk && !o->Clock)
			OSDClockIn();
	}
	o->Clock = osd_clk;
}

// ---------------------------------------------------------------------------
// OSD rendering

// Colour index bit 0 is red on this chip, bit 2 blue — the opposite way
// round from the SFC-Box's MB90082.
static const uint8	osd_r5[8] = {  0, 31,  0, 31,  0, 31,  0, 31 };
static const uint8	osd_g5[8] = {  0,  0, 31, 31,  0,  0, 31, 31 };
static const uint8	osd_b5[8] = {  0,  0,  0,  0, 31, 31, 31, 31 };

// A glyph row is a big-endian 16-bit word with the twelve dots left-aligned
// at bit 11, so dot d is bit (11 - d).
static inline int OSDGlyphDot (uint8 ch, int row, int dot)
{
	if (row < 0 || row > 17 || dot < 0 || dot > 11)
		return (0);
	const uint32	off = (uint32) (ch & 0x7f) * 36 + (uint32) row * 2;
	const uint16	w = (uint16) ((NSS.OSD.Font[off] << 8) | NSS.OSD.Font[off + 1]);
	return ((w >> (11 - dot)) & 1);
}

// Register 7 bit7 gates the RGB output; register 0 drives the BLNK pin high
// by hand for the screens that replace the SNES picture entirely rather than
// genlocking onto it (fullsnes: 003Fh superimposed, 00BDh solid).
static inline bool8 OSDVisible (void)
{
	return (NSS.Active && NSS.OSD.FontLoaded && (NSS.OSD.Reg[7] & 0x0080)) ? TRUE : FALSE;
}

static inline bool8 OSDOpaque (void)
{
	const uint16	r0 = NSS.OSD.Reg[0];
	const bool8		blank_forced = (!(r0 & 0x0002) && (r0 & 0x0080)) ? TRUE : FALSE;
	const bool8		internal_sync = (NSS.OSD.Reg[7] & 0x0010) ? TRUE : FALSE;
	return (blank_forced || internal_sync) ? TRUE : FALSE;
}

bool8 S9xNSSOSDHires (void)
{
	return OSDVisible();
}

void S9xNSSRenderOSD (uint16 *screen, int pitch, int width, int height)
{
	struct SNSSOSD	*o = &NSS.OSD;

	if (!OSDVisible())
		return;

	if (OSDOpaque())
	{
		const uint8		raster = (uint8) (o->Reg[6] & 7);
		const uint16	fill = BUILD_PIXEL(osd_r5[raster], osd_g5[raster], osd_b5[raster]);

		for (int py = 0; py < height; py++)
		{
			uint16	*line = screen + py * pitch;
			for (int px = 0; px < width; px++)
				line[px] = fill;
		}
	}

	// Cell geometry: a 12-dot cell is about 8 SNES pixels wide, so a lores
	// frame gets 8 and the doubled frame S9xEndScreenRefresh hands us when
	// the plane is up gets 16 — enough for every dot column. The 18 glyph
	// rows map to scanlines 1:1 and the 24x12 grid is centered.
	const int	xscale = (width >= 512) ? 2 : 1;
	const int	xbase = (width - 192 * xscale) / 2;
	const int	ybase = (height > NSS_OSD_H * 18) ? (height - NSS_OSD_H * 18) / 2 : 0;

	// Character size is set for three line groups — line 1, lines 2-11 and
	// line 12 — and the plane can be scrolled by whole lines and by dots
	// within a line, which is how the BIOS keeps a fixed headline over
	// scrolling instruction text. The scroll model follows MAME's m50458;
	// the chip's own documentation does not spell it out.
	const int	hsz[3] = { (o->Reg[1] >> 6) & 3, (o->Reg[1] >> 8) & 3, (o->Reg[1] >> 10) & 3 };
	const int	vsz[3] = { (o->Reg[2] >> 6) & 3, (o->Reg[2] >> 8) & 3, (o->Reg[2] >> 10) & 3 };
	const int	scroll_dot = o->Reg[3] & 0x1f;
	const int	scroll_row = (o->Reg[3] >> 8) & 0x0f;
	const int	gap = (int) ((o->Reg[3] >> 5) & 3) * 18;	// blank lines under line 1

	const uint8	bg = (uint8) ((o->Reg[6] >> 4) & 7);
	const bool8	blink_on = ((o->BlinkFrame & ((o->Reg[5] & 4) ? 16u : 32u)) != 0);
	const bool8	opaque = OSDOpaque();

	// Lines stack: each one is as tall as its own size setting, so a
	// double-height line pushes everything under it down.
	int	top = ybase;

	for (int slot = 0; slot < NSS_OSD_H; slot++)
	{
		// Which stored line this display slot shows. Line 1 stays put and
		// the rest rotate under it; the last slot repeats line 1 so the
		// row being refilled off-screen never flickers into view.
		int	row = slot;
		if (slot != 0 && scroll_row > 1)	row += scroll_row - 1;
		if (row > 11)						row -= 11;
		if (scroll_row && slot == 11)		row = 0;

		const int	band = (row == 0) ? 0 : (row == 11) ? 2 : 1;
		const int	zoomx = hsz[band] + 1;
		const int	zoomy = vsz[band] + 1;
		const int	cellw = 8 * zoomx * xscale;
		const int	cellh = 18 * zoomy;

		// Line 1 is the fixed headline; the scrolled block below it starts
		// after the configured gap and is nudged up by the dot offset.
		if (slot == 1)
			top += gap - scroll_dot;

		for (int py = 0; py < cellh; py++)
		{
			const int	sy = top + py;
			if (sy < 0 || sy >= height)
				continue;

			uint16		*line = screen + sy * pitch;
			const int	grow = py / zoomy;

			for (int col = 0; col < NSS_OSD_W; col++)
			{
				const uint16	cell = o->VRAM[row * NSS_OSD_W + col];
				const uint8		ch = (uint8) (cell & 0x7f);
				const uint8		fg = (uint8) ((cell >> 8) & 7);
				const bool8		blink = (cell & 0x0800) && blink_on;
				const bool8		underline = (cell & 0x1000) ? TRUE : FALSE;

				for (int px = 0; px < cellw; px++)
				{
					const int	x = xbase + col * cellw + px;
					if (x < 0 || x >= width)
						continue;

					// Cells narrower than the glyph OR adjacent dot pairs so
					// single-dot strokes survive the squeeze.
					int	d0 = px * 12 / cellw;
					int	d1 = (cellw < 12) ? (px * 12 + cellw / 2) / cellw : d0;
					if (d1 > 11)	d1 = 11;

					bool8	on = FALSE;
					if (!blink)
					{
						on = OSDGlyphDot(ch, grow, d0) ||
							 (d1 != d0 && OSDGlyphDot(ch, grow, d1)) ? TRUE : FALSE;
						if (underline && grow == 17)
							on = TRUE;
					}

					if (on)
						line[x] = BUILD_PIXEL(osd_r5[fg], osd_g5[fg], osd_b5[fg]);
					else if (opaque && bg)
						line[x] = BUILD_PIXEL(osd_r5[bg], osd_g5[bg], osd_b5[bg]);
				}
			}
		}

		top += cellh;
	}
}

// ---------------------------------------------------------------------------
// Z80 bus callbacks

static uint8 NSSMemRead (uint16 addr)
{
	if (addr < NSS_BIOS_SIZE)
		return (NSS.BIOS[addr]);

	if (addr < 0xa000)
		return (NSS.WRAM[addr - 0x8000]);

	if (addr < 0xc000)
	{
		// EEPROM data on bit7, its ready line on bit6 (never busy here).
		return (uint8) ((NSS.EEPROM.DataOut ? 0x80 : 0) | 0x40);
	}

	if (addr < 0xe000)
	{
		// Only the top 8K of the 32K instruction EPROM is wired up, and only
		// the socket the supervisor has selected answers: an empty one floats
		// high, which is how the BIOS counts the cartridges.
		if (NSS.SlotSelect >= NSS_SLOTS || !NSS.Slot[NSS.SlotSelect].Present)
			return (0xff);
		return (NSS.Slot[NSS.SlotSelect].INST[NSS_INST_SIZE - NSS_INST_WINDOW + (addr - 0xc000)]);
	}

	// The key chip reads back as an RST opcode with two of its bits carrying
	// data, which is how the BIOS runs decryptor code straight out of it.
	uint8	byte = 0xe7;
	if (NSS.SlotSelect < NSS_SLOTS && NSS.Slot[NSS.SlotSelect].Present)
	{
		byte |= (uint8) (PROMCounterBit() << 4);
		byte |= (uint8) (PROMDataBit() << 3);
	}
	else
		byte |= 0x18;	// empty socket: the pull-ups make it RST 38h
	return (byte);
}

static void NSSMemWrite (uint16 addr, uint8 byte)
{
	if (addr < NSS_BIOS_SIZE)
		return;

	if (addr < 0xa000)
	{
		// 9000h-9FFFh only takes writes while port 00h bit2 unlocks it.
		if (addr < 0x9000 || NSS.WRAMUnlock)
			NSS.WRAM[addr - 0x8000] = byte;
		return;
	}

	if (addr < 0xe000)
		return;

	if (NSS.SlotSelect < NSS_SLOTS && NSS.Slot[NSS.SlotSelect].Present)
		PROMWrite(byte);
	EEPROMWrite(byte);
}

// Port 00h.R bit6 goes low for the three scanlines of the vertical sync
// pulse; the BIOS's Wait_Vblank token polls it.
static inline int32 VBlankLine (void)
{
	return ((int32) PPU.ScreenHeight + FIRST_VISIBLE_LINE);
}

static inline uint8 VsyncBit (void)
{
	const int32	start = VBlankLine();
	return (CPU.V_Counter >= start && CPU.V_Counter < start + 3) ? 0 : 1;
}

static uint8 NSSIORead (uint16 port)
{
	switch (port & 7)
	{
		case 0:
		{
			const uint16	pad = MovieGetJoypad(0);
			uint8			res = (uint8) (NSS.JoyReadFlag ? 0x80 : 0);

			res |= (uint8) (VsyncBit() << 6);
			res |= (uint8) (((pad >> 15) & 1) << 5);	// B
			res |= (uint8) (((pad >>  7) & 1) << 4);	// A
			res |= (uint8) (((pad >> 10) & 1) << 3);	// Down
			res |= (uint8) (((pad >> 11) & 1) << 2);	// Up
			res |= (uint8) (((pad >>  9) & 1) << 1);	// Left
			res |= (uint8) (((pad >>  8) & 1) << 0);	// Right
			return (res);
		}

		case 1:
		{
			uint8	res = (uint8) (NSS.GameOverFlag ? 0x80 : 0);
			res |= (uint8) (S9xNSSGetButtons() & 0x7f);
			return (res);
		}

		case 2:
		{
			uint8	res = 0;
			if (NSS.CoinPulse[0] > 0)				res |= 0x01;
			if (NSS.CoinPulse[1] > 0)				res |= 0x02;
			if (S9xNSSGetButtons() & NSS_BTN_SERVICE)	res |= 0x04;
			return (res);
		}

		case 3:
			return (uint8) (0x7e | (NSS.RTC.DataOut & 1));

		default:
			return (0xff);
	}
}

static void NSSIOWrite (uint16 port, uint8 byte)
{
	switch (port & 7)
	{
		case 0:		// NMI control and work-RAM protect
			NSS.Port00W = byte;
			NSS.WRAMUnlock = (byte & 0x04) ? 1 : 0;
			NSS.NMIEnable = (byte & 0x01) ? 1 : 0;
			break;

		case 1:		// slot select and the SNES control lines
		{
			const uint8	prev = NSS.Port01W;
			NSS.Port01W = byte;
			NSS.SlotSelect = (uint8) ((byte >> 2) & 3);
			// The selected socket is wired to both CPUs, so the SNES side
			// follows. The supervisor holds the 65816 in reset across the
			// change and reboots it afterwards.
			if (NSS.SlotSelect < NSS_SLOTS && NSS.Slot[NSS.SlotSelect].Present &&
				NSS.MappedSlot != (int8) NSS.SlotSelect)
				S9xNSSMapSlot(NSS.SlotSelect);
			NSS.InputDisabled = (byte & 0x80) ? FALSE : TRUE;
			NSS.SoundMuted = (byte & 0x20) ? TRUE : FALSE;

			const bool8	held = (!(byte & 1) || !(byte & 2)) ? TRUE : FALSE;
			if (!(prev & 1) && (byte & 1))
				NSS.PendingSNESReset = TRUE;	// reset line released
			if ((prev & 1) && !(byte & 1))
				NSS.PendingAPUReset = TRUE;		// reset line asserted
			NSS.SNESHeld = held;

			if (TraceEnabled() && ((prev ^ byte) & 0x8f))
				printf("[nss] port01=%02X slot=%d reset=%d halt=%d pads=%d\n",
					   byte, NSS.SlotSelect, byte & 1, (byte >> 1) & 1, (byte >> 7) & 1);
			break;
		}

		case 2:		// RTC and OSD pins
			RTCOSDWrite(byte);
			break;

		case 3:		// front-panel LEDs and the layer enables
			NSS.Port03W = byte;
			break;

		case 4:		// coin counters
			NSS.Port04W = byte;
			break;

		case 7:		// acknowledge the joypad watchdog
			NSS.JoyReadFlag = 1;
			break;

		default:
			break;
	}
}

// ---------------------------------------------------------------------------
// SNES-visible hardware

uint8 S9xNSSReadDIP (void)
{
	return (NSS.DipSwitches);
}

void S9xNSSSetJoypadStrobe (uint8 byte)
{
	// OUT1 carries the Game Over flag back to the supervisor; strobing the
	// pads at all is what feeds the watchdog.
	NSS.GameOverFlag = (uint8) ((byte >> 2) & 1);
	NSS.JoyReadFlag = 0;
}

void S9xNSSJoypadRead (void)
{
	NSS.JoyReadFlag = 0;
}

bool8 S9xNSSInputDisabled (void)
{
	return (NSS.Active && NSS.InputDisabled) ? TRUE : FALSE;
}

void S9xNSSInsertCoin (int slot)
{
	if (slot < 0 || slot > 1)
		return;
	NSS.CoinPulse[slot] = 6;	// a coin switch stays closed for a few frames
}

void S9xNSSSetButtons (uint16 mask)	{ NSS.Buttons = mask; }
uint16 S9xNSSGetButtons (void)		{ return (uint16) (NSS.Buttons | NSS.PulseButtons); }

void S9xNSSPulseButton (uint16 mask)
{
	NSS.PulseButtons |= mask;
	NSS.PulseLeft = 8;		// long enough for the supervisor's poll to see it
}

// ---------------------------------------------------------------------------
// Cartridge images

static uint32 CRC32 (const uint8 *d, uint32 n)
{
	uint32	crc = 0xffffffff;

	for (uint32 i = 0; i < n; i++)
	{
		crc ^= d[i];
		for (int b = 0; b < 8; b++)
			crc = (crc >> 1) ^ (0xedb88320u & (uint32) (-(int32) (crc & 1)));
	}
	return (~crc);
}

// Biggest cartridge image the format allows: a 4M program plus the tail.
#define CART_STAGING_MAX	(0x400000u + NSS_INST_SIZE + NSS_PROM_SIZE)
//
// fullsnes's merged layout is PRG-ROM, then the 32K instruction EPROM, then
// the 16-byte key. MAME ships the same three parts as separate members of a
// .zip, so both are accepted and end up in the same place.

// The unused low 24K of the instruction EPROM is blank on every known dump;
// checking it is what keeps an ordinary cart of the same length out.
static bool8 LooksLikeInstROM (const uint8 *inst)
{
	const uint8	fill = inst[0];
	if (fill != 0x00 && fill != 0xff)
		return (FALSE);
	for (uint32 i = 1; i < NSS_INST_SIZE - NSS_INST_WINDOW; i++)
		if (inst[i] != fill)
			return (FALSE);
	// The mapped window must not be blank as well, or this is just padding.
	for (uint32 i = NSS_INST_SIZE - NSS_INST_WINDOW; i < NSS_INST_SIZE; i++)
		if (inst[i] != fill)
			return (TRUE);
	return (FALSE);
}

// IC number from a member name's ".icN" suffix; -1 when it has none.
static int MemberICNumber (const char *name)
{
	const char	*dot = strrchr(name, '.');
	if (!dot || (dot[1] != 'i' && dot[1] != 'I') || (dot[2] != 'c' && dot[2] != 'C'))
		return (-1);

	int	n = 0, digits = 0;
	for (const char *p = dot + 3; *p >= '0' && *p <= '9'; p++, digits++)
		n = n * 10 + (*p - '0');
	return (digits ? n : -1);
}

#ifdef UNZIP_SUPPORT
struct NSSZipMember
{
	char	name[260];
	uint32	size;
	int		ic;
};

static bool8 NSSReadZipMember (unzFile file, const char *name, uint8 *dest, uint32 size)
{
	if (unzLocateFile(file, name, 1) != UNZ_OK || unzOpenCurrentFile(file) != UNZ_OK)
		return (FALSE);
	const int	got = unzReadCurrentFile(file, dest, size);
	unzCloseCurrentFile(file);
	return (got == (int) size) ? TRUE : FALSE;
}
#endif

uint32 S9xNSSAssembleZipSet (const char *path, uint8 *dest, uint32 maxsize)
{
#ifndef UNZIP_SUPPORT
	(void) path; (void) dest; (void) maxsize;
	return (0);
#else
	unzFile	file = unzOpen(path);
	if (!file)
		return (0);

	struct NSSZipMember	prg[8], inst, prom;
	int					nprg = 0;
	bool8				have_inst = FALSE, have_prom = FALSE;
	uint32				prg_total = 0;

	for (int pos = unzGoToFirstFile(file); pos == UNZ_OK; pos = unzGoToNextFile(file))
	{
		unz_file_info	info;
		char			name[260] = { 0 };	// minizip does not terminate a full buffer
		if (unzGetCurrentFileInfo(file, &info, name, sizeof name - 1, NULL, 0, NULL, 0) != UNZ_OK)
			continue;

		const uint32	size = (uint32) info.uncompressed_size;

		if (size == NSS_PROM_SIZE && !have_prom)
		{
			strcpy(prom.name, name);
			prom.size = size;
			have_prom = TRUE;
		}
		else if (size == NSS_INST_SIZE && !have_inst)
		{
			strcpy(inst.name, name);
			inst.size = size;
			have_inst = TRUE;
		}
		else if (size >= 0x40000 && nprg < 8)
		{
			strcpy(prg[nprg].name, name);
			prg[nprg].size = size;
			prg[nprg].ic = MemberICNumber(name);
			prg_total += size;
			nprg++;
		}
	}

	if (!nprg || !have_inst || prg_total + NSS_INST_SIZE + NSS_PROM_SIZE > maxsize)
	{
		unzClose(file);
		return (0);
	}

	// Judge the instruction EPROM before writing a byte of `dest`: an
	// ordinary game archive that happens to hold a 32K member alongside the
	// ROM must be left for the normal loader with its buffer untouched.
	std::vector<uint8>	inst_image(NSS_INST_SIZE, 0);
	if (!NSSReadZipMember(file, inst.name, inst_image.data(), NSS_INST_SIZE) ||
		!LooksLikeInstROM(inst_image.data()))
	{
		unzClose(file);
		return (0);
	}

	// Program chips are numbered downwards across the address space: on the
	// two-EPROM boards IC3 holds the lower half and IC2 the upper.
	for (int i = 1; i < nprg; i++)
	{
		struct NSSZipMember	key = prg[i];
		int					j = i - 1;
		while (j >= 0 && prg[j].ic < key.ic)
		{
			prg[j + 1] = prg[j];
			j--;
		}
		prg[j + 1] = key;
	}

	uint32	off = 0;
	for (int i = 0; i < nprg; i++)
	{
		if (!NSSReadZipMember(file, prg[i].name, dest + off, prg[i].size))
		{
			// Half a cartridge is already in the buffer; the caller has to
			// treat that as a failed load rather than fall through.
			unzClose(file);
			return (NSS_ZIPSET_UNREADABLE);
		}
		off += prg[i].size;
	}

	memcpy(dest + off, inst_image.data(), NSS_INST_SIZE);
	off += NSS_INST_SIZE;

	memset(dest + off, 0, NSS_PROM_SIZE);
	if (have_prom)
		NSSReadZipMember(file, prom.name, dest + off, NSS_PROM_SIZE);
	off += NSS_PROM_SIZE;

	unzClose(file);
	return (off);
#endif
}

bool8 S9xNSSTakeCartTail (const uint8 *image, uint32 size, uint32 *prg_size)
{
	const uint32	tail = NSS_INST_SIZE + NSS_PROM_SIZE;

	if (size <= tail)
		return (FALSE);

	const uint32	prg = size - tail;
	if (prg < 0x10000 || (prg & (prg - 1)) != 0)	// program chips are powers of two
		return (FALSE);
	if (!LooksLikeInstROM(image + prg))
		return (FALSE);

	*prg_size = prg;
	return (TRUE);
}

bool8 S9xNSSReadCartImage (const char *path, std::vector<uint8> &out)
{
	out.clear();
	if (!path || !*path)
		return (FALSE);

	// A MAME set first: its members have to be stitched into one image.
	std::vector<uint8>	staging(CART_STAGING_MAX, 0);
	const uint32			assembled = S9xNSSAssembleZipSet(path, staging.data(), CART_STAGING_MAX);

	if (assembled && assembled != NSS_ZIPSET_UNREADABLE)
	{
		out.assign(staging.begin(), staging.begin() + assembled);
		return (TRUE);
	}
	if (assembled == NSS_ZIPSET_UNREADABLE)
		return (FALSE);

	FILE	*f = fopen(path, "rb");
	if (!f)
		return (FALSE);
	fseek(f, 0, SEEK_END);
	const long	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n <= 0 || (uint32) n > CART_STAGING_MAX)
	{
		fclose(f);
		return (FALSE);
	}
	out.resize((size_t) n);
	const size_t	got = fread(out.data(), 1, (size_t) n, f);
	fclose(f);
	if (got != (size_t) n)
	{
		out.clear();
		return (FALSE);
	}
	return (TRUE);
}

// The cart header sits where every LoROM cart keeps it. These are the same
// two bytes and the same title trim InitROM takes for the running cart.
static void NSSCaptureHeader (struct SNSSSlot *s)
{
	s->ROMSizeByte = 0;
	s->SRAMSizeByte = 0;
	s->Name[0] = 0;

	if (s->PrgSize < 0x8000)
		return;

	const uint8	*hdr = s->Prg + 0x7fb0;
	s->ROMSizeByte = hdr[0x27];
	s->SRAMSizeByte = hdr[0x28];

	int	len = 0;
	for (int i = 0; i < 21 && len < NSS_SLOT_NAME - 1; i++)
	{
		const uint8	c = hdr[0x10 + i];
		s->Name[len++] = (c >= 0x20 && c < 0x7f) ? (char) c : ' ';
	}
	while (len > 0 && s->Name[len - 1] == ' ')
		len--;
	s->Name[len] = 0;
}

// Splits a merged image into one socket. The program gets its own buffer so
// the other slots keep theirs while this one is the mapped cartridge.
bool8 S9xNSSLoadSlot (int slot, const uint8 *image, uint32 size, const char *path)
{
	uint32	prg = 0;

	if (slot < 0 || slot >= NSS_SLOTS || !S9xNSSTakeCartTail(image, size, &prg))
		return (FALSE);

	struct SNSSSlot	*s = &NSS.Slot[slot];
	uint8			*buf = (uint8 *) malloc(prg);
	if (!buf)
		return (FALSE);

	free(s->Prg);
	memset(s, 0, sizeof(*s));

	s->Prg = buf;
	s->PrgSize = prg;
	memcpy(s->Prg, image, prg);
	memcpy(s->INST, image + prg, NSS_INST_SIZE);
	memcpy(s->PROM, image + prg + NSS_INST_SIZE, NSS_PROM_SIZE);
	for (uint32 i = 0; i < NSS_PROM_SIZE; i++)
		if (s->PROM[i])
			s->PROMPresent = TRUE;

	s->CRC = CRC32(s->Prg, prg);
	s->SRAMValid = TRUE;	// blank battery until told otherwise
	NSSCaptureHeader(s);
	if (path)
	{
		strncpy(s->Path, path, NSS_SLOT_PATH - 1);
		s->Path[NSS_SLOT_PATH - 1] = 0;
	}
	s->Present = TRUE;
	return (TRUE);
}

bool8 S9xNSSInsertCart (int slot, const char *path)
{
	std::vector<uint8>	image;

	if (slot < 0 || slot >= NSS_SLOTS || !NSS.Active)
		return (FALSE);
	if (!S9xNSSReadCartImage(path, image) || image.empty())
		return (FALSE);

	uint32	prg = 0;
	if (!S9xNSSTakeCartTail(image.data(), (uint32) image.size(), &prg))
		return (FALSE);

	// The supervisor rejects two cartridges with the same game id, so it
	// would throw this one out anyway; say so here instead.
	const uint32	crc = CRC32(image.data(), prg);
	for (int i = 0; i < NSS_SLOTS; i++)
		if (i != slot && NSS.Slot[i].Present && NSS.Slot[i].CRC == crc)
			return (FALSE);

	if (!S9xNSSLoadSlot(slot, image.data(), (uint32) image.size(), path))
		return (FALSE);

	// Changing a cartridge is a power-off job on the real cabinet, and the
	// supervisor only scans its sockets at boot. Credits survive: they live
	// in the EEPROM.
	S9xNSSPowerOn();
	return (TRUE);
}

// A cabinet with nothing in it has nothing to run, so the last cartridge
// stays put.
bool8 S9xNSSCanEject (int slot)
{
	if (!S9xNSSSlotPresent(slot))
		return (FALSE);
	for (int i = 0; i < NSS_SLOTS; i++)
		if (i != slot && NSS.Slot[i].Present)
			return (TRUE);
	return (FALSE);
}

void S9xNSSEjectCart (int slot)
{
	if (!S9xNSSCanEject(slot))
		return;
	S9xNSSStashMappedSRAM();
	free(NSS.Slot[slot].Prg);
	memset(&NSS.Slot[slot], 0, sizeof(NSS.Slot[slot]));
	if (NSS.Active)
		S9xNSSPowerOn();
}

void S9xNSSStashMappedSRAM (void)
{
	if (NSS.MappedSlot < 0 || NSS.MappedSlot >= NSS_SLOTS ||
		!NSS.Slot[NSS.MappedSlot].Present)
		return;
	memcpy(NSS.Slot[NSS.MappedSlot].SRAM, Memory.SRAM, NSS_SLOT_SRAM);
	NSS.Slot[NSS.MappedSlot].SRAMValid = TRUE;
}

bool8 S9xNSSSlotPresent (int slot)
{
	return (slot >= 0 && slot < NSS_SLOTS && NSS.Slot[slot].Present) ? TRUE : FALSE;
}

const char *S9xNSSSlotName (int slot)
{
	return S9xNSSSlotPresent(slot) ? NSS.Slot[slot].Name : "";
}

const char *S9xNSSSlotPath (int slot)
{
	return S9xNSSSlotPresent(slot) ? NSS.Slot[slot].Path : "";
}

int S9xNSSMappedSlot (void)
{
	return (NSS.MappedSlot);
}

// ---------------------------------------------------------------------------
// BIOS images

static bool AcceptExactSize (const uint8 *data, uint32 size, uint32 full_size, void *ctx)
{
	(void) data; (void) size;
	return full_size == *(const uint32 *) ctx;
}

// The BIOS set ships all three revisions at the same length, so they are
// told apart by checksum: "03" runs every title, "02" only the three oldest.
struct NSSBiosPick { uint8 want_rev; uint8 seen_rev; };

// Every revision starts its reset path with LD A,I / JP Z,nnnn.
static bool8 LooksLikeNSSBios (const uint8 *d, uint32 n)
{
	return (n >= NSS_BIOS_SIZE && d[0] == 0xed && d[1] == 0x57 && d[2] == 0xca) ? TRUE : FALSE;
}

static uint8 NSSBiosRevision (const uint8 *d, uint32 n)
{
	if (!LooksLikeNSSBios(d, n))
		return (0);

	switch (CRC32(d, NSS_BIOS_SIZE))
	{
		case 0xa8e202b3u:	return (2);	// nss-c.ic14       "02", three oldest games
		case 0xe06cb58fu:	return (3);	// nss-ic14.02.ic14 "03"
		case 0xac385b53u:	return (3);	// nss-v3.ic14      "03", later patch
	}
	return (1);	// an unrecognised but plausible image; treat it as usable
}

static bool AcceptNSSBios (const uint8 *data, uint32 size, uint32 full_size, void *ctx)
{
	struct NSSBiosPick	*p = (struct NSSBiosPick *) ctx;
	if (full_size != NSS_BIOS_SIZE)
		return (false);

	const uint8	rev = NSSBiosRevision(data, size);
	if (rev && p->seen_rev == 0)
		p->seen_rev = rev;
	return (rev == p->want_rev);
}

// The charset set ships two dumps of the same glyphs, one of them rotated
// so the dots wrap across the cell. A sound dump keeps all twelve dots
// left-aligned at bit 11, which leaves the top four bits of every row clear.
static bool AcceptOSDFont (const uint8 *data, uint32 size, uint32 full_size, void *ctx)
{
	(void) ctx;
	if (full_size != NSS_FONT_SIZE || size < NSS_FONT_SIZE)
		return (false);

	uint32	ink = 0;
	for (uint32 i = 0; i < NSS_FONT_SIZE; i += 2)
	{
		if (data[i] & 0xf0)
			return (false);
		if (data[i] || data[i + 1])
			ink++;
	}
	return ink > 256;
}

bool8 S9xNSSLoadBIOS (void)
{
	std::vector<uint8>	img;
	const std::string	bios_path = S9xResolveBiosPath(S9X_BIOS_NSS);
	bool8				ok = FALSE;

	NSS.BIOSRevision = 0;

	if (!bios_path.empty())
	{
		// Two passes so a .zip holding every revision still yields the "03"
		// one, which is the only image all twelve games run under.
		static const uint8	prefer[2] = { 3, 2 };
		for (int i = 0; i < 2 && !ok; i++)
		{
			const uint8			want = prefer[i];
			struct NSSBiosPick	pick = { want, 0 };
			if (S9xReadBiosImage(bios_path.c_str(), img, NSS_BIOS_SIZE, AcceptNSSBios, &pick) &&
				img.size() >= NSS_BIOS_SIZE)
			{
				memcpy(NSS.BIOS, img.data(), NSS_BIOS_SIZE);
				NSS.BIOSRevision = want;
				ok = TRUE;
			}
		}

		// An image we do not recognise but which is the right size and opens
		// like Z80 code is still worth running.
		if (!ok)
		{
			uint32	want = NSS_BIOS_SIZE;
			if (S9xReadBiosImage(bios_path.c_str(), img, NSS_BIOS_SIZE, AcceptExactSize, &want) &&
				img.size() >= NSS_BIOS_SIZE)
			{
				memcpy(NSS.BIOS, img.data(), NSS_BIOS_SIZE);
				NSS.BIOSRevision = 1;
				ok = TRUE;
			}
		}
	}

	if (!ok)
		printf("NSS: BIOS missing - assign the Nintendo Super System BIOS in File -> BIOS Manager.\n");

	NSS.OSD.FontLoaded = FALSE;
	const std::string	font_path = S9xResolveBiosPath(S9X_BIOS_NSS_FONT);
	if (!font_path.empty() &&
		S9xReadBiosImage(font_path.c_str(), img, NSS_FONT_SIZE, AcceptOSDFont, NULL) &&
		img.size() >= NSS_FONT_SIZE)
	{
		memcpy(NSS.OSD.Font, img.data(), NSS_FONT_SIZE);
		NSS.OSD.FontLoaded = TRUE;
	}
	else
		printf("NSS: M50458 charset missing - assign it in File -> BIOS Manager; the menu overlay will be invisible.\n");

	return (ok);
}

// ---------------------------------------------------------------------------
// Battery-backed settings

// The cabinet's own saved data: the coinage EEPROM, the clock's SRAM and
// the batteries of sockets 2 and 3. Socket 1 keeps the ordinary .srm the
// loader already reads and writes for it.
#define NSS_NVRAM_HEAD	(NSS_EEPROM_WORDS * 2 + NSS_RTC_NVRAM)
#define NSS_NVRAM_FULL	(NSS_NVRAM_HEAD + (NSS_SLOTS - 1) * NSS_SLOT_SRAM)

bool8 S9xNSSLoadNVRAM (void)
{
	std::string	name = S9xGetFilename(".nss", SRAM_DIR);
	FILE		*fp = fopen(name.c_str(), "rb");

	if (!fp)
		return (FALSE);

	std::vector<uint8>	buf(NSS_NVRAM_FULL, 0);
	const size_t		got = fread(buf.data(), 1, NSS_NVRAM_FULL, fp);
	fclose(fp);

	// A file from before the extra sockets existed stops after the clock.
	if (got != NSS_NVRAM_HEAD && got != NSS_NVRAM_FULL)
		return (FALSE);

	for (int i = 0; i < NSS_EEPROM_WORDS; i++)
		NSS.EEPROM.Data[i] = (uint16) (buf[i * 2] | (buf[i * 2 + 1] << 8));
	memcpy(NSS.RTC.NVRAM, buf.data() + NSS_EEPROM_WORDS * 2, NSS_RTC_NVRAM);

	if (got == NSS_NVRAM_FULL)
	{
		for (int i = 1; i < NSS_SLOTS; i++)
		{
			memcpy(NSS.Slot[i].SRAM, buf.data() + NSS_NVRAM_HEAD + (i - 1) * NSS_SLOT_SRAM,
			       NSS_SLOT_SRAM);
			NSS.Slot[i].SRAMValid = TRUE;
		}
	}
	return (TRUE);
}

bool8 S9xNSSSaveNVRAM (void)
{
	std::string	name = S9xGetFilename(".nss", SRAM_DIR);
	FILE		*fp = fopen(name.c_str(), "wb");

	if (!fp)
		return (FALSE);

	S9xNSSStashMappedSRAM();

	std::vector<uint8>	buf(NSS_NVRAM_FULL, 0);
	for (int i = 0; i < NSS_EEPROM_WORDS; i++)
	{
		buf[i * 2]     = (uint8) NSS.EEPROM.Data[i];
		buf[i * 2 + 1] = (uint8) (NSS.EEPROM.Data[i] >> 8);
	}
	memcpy(buf.data() + NSS_EEPROM_WORDS * 2, NSS.RTC.NVRAM, NSS_RTC_NVRAM);
	for (int i = 1; i < NSS_SLOTS; i++)
		memcpy(buf.data() + NSS_NVRAM_HEAD + (i - 1) * NSS_SLOT_SRAM,
		       NSS.Slot[i].SRAM, NSS_SLOT_SRAM);

	fwrite(buf.data(), 1, NSS_NVRAM_FULL, fp);
	fclose(fp);
	NSS.EEPROM.Dirty = FALSE;
	return (TRUE);
}

// ---------------------------------------------------------------------------
// Savestates. The dynamic half of the board travels as one packed mirror;
// loader-owned data (BIOS, instruction EPROM, key, OSD charset) is rebuilt
// at ROM load and deliberately stays out.

// magic(4) + version(1) + reserved(3) + payload length(4), then the mirror.
// The length is 32-bit because three sockets' batteries alone run past 64K.
#define NSS_STATE_VERSION	1
#define NSS_STATE_HEADER	12

struct SNSSSaveState
{
	struct SZ80		Cpu;

	uint8	WRAM[NSS_WRAM_SIZE];
	uint8	Port00W, Port01W, Port03W, Port04W;
	uint8	SlotSelect, SNESHeld, PendingSNESReset, PendingAPUReset;
	uint8	InputDisabled, SoundMuted;
	uint8	GameOverFlag, JoyReadFlag, DipSwitches;
	uint16	Buttons, PulseButtons;
	int32	PulseLeft;
	int32	CoinPulse[2];
	uint8	NMIEnable, WRAMUnlock;
	int32	LastVCounter;
	int64	CycleRemainder;

	int8	MappedSlot;
	uint8	SlotSRAMValid[NSS_SLOTS];

	uint8	PROMCounter, PROMSevenBit, PROMLastClock, PROMReset;

	uint16	EEPROMData[NSS_EEPROM_WORDS];
	uint8	EECS, EEClock, EEDataIn, EEDataOut, EEState, EEBitPos, EEAddr, EEWriteEnable;
	uint32	EEShift;

	struct SNSSRTC	RTC;

	uint8	OSDCS, OSDClock, OSDDataIn, OSDBitPos, OSDHaveAddr;
	uint16	OSDShift, OSDAddr;
	uint16	OSDVRAM[NSS_OSD_CELLS];
	uint16	OSDReg[NSS_OSD_REGS];
	uint32	OSDBlinkFrame;
};

// Batteries are appended after the fixed mirror, and only for the sockets
// that actually have one: carrying three blank 32K buffers would cost more
// than the rest of the board put together, in every rewind frame.
static uint32 NSSSlotSRAMBytes (int slot)
{
	const struct SNSSSlot	*s = &NSS.Slot[slot];
	if (!s->Present || !s->SRAMSizeByte)
		return (0);
	const uint32	n = (uint32) ((1 << (s->SRAMSizeByte + 3)) * 128);
	return (n < NSS_SLOT_SRAM) ? n : NSS_SLOT_SRAM;
}

size_t S9xNSSStateSize (void)
{
	size_t	n = NSS_STATE_HEADER + sizeof(struct SNSSSaveState);
	for (int i = 0; i < NSS_SLOTS; i++)
		n += NSSSlotSRAMBytes(i);
	return (n);
}

void S9xNSSStateSave (uint8 *buf)
{
	struct SNSSSaveState	s;

	const uint32	payload = (uint32) (S9xNSSStateSize() - NSS_STATE_HEADER);

	memcpy(buf, "NSS!", 4);
	buf[4] = NSS_STATE_VERSION;
	buf[5] = buf[6] = buf[7] = 0;
	buf[8]  = (uint8) payload;
	buf[9]  = (uint8) (payload >> 8);
	buf[10] = (uint8) (payload >> 16);
	buf[11] = (uint8) (payload >> 24);

	memset(&s, 0, sizeof s);
	s.Cpu = Z80;
	memcpy(s.WRAM, NSS.WRAM, NSS_WRAM_SIZE);
	s.Port00W = NSS.Port00W;	s.Port01W = NSS.Port01W;
	s.Port03W = NSS.Port03W;	s.Port04W = NSS.Port04W;
	s.SlotSelect = NSS.SlotSelect;
	s.SNESHeld = (uint8) NSS.SNESHeld;
	s.PendingSNESReset = (uint8) NSS.PendingSNESReset;
	s.PendingAPUReset = (uint8) NSS.PendingAPUReset;
	s.InputDisabled = (uint8) NSS.InputDisabled;
	s.SoundMuted = (uint8) NSS.SoundMuted;
	s.GameOverFlag = NSS.GameOverFlag;
	s.JoyReadFlag = NSS.JoyReadFlag;
	s.DipSwitches = NSS.DipSwitches;
	s.Buttons = NSS.Buttons;
	s.PulseButtons = NSS.PulseButtons;
	s.PulseLeft = NSS.PulseLeft;
	s.CoinPulse[0] = NSS.CoinPulse[0];
	s.CoinPulse[1] = NSS.CoinPulse[1];
	s.NMIEnable = NSS.NMIEnable;
	s.WRAMUnlock = NSS.WRAMUnlock;
	s.LastVCounter = NSS.LastVCounter;
	s.CycleRemainder = NSS.CycleRemainder;

	s.MappedSlot = NSS.MappedSlot;
	for (int i = 0; i < NSS_SLOTS; i++)
		s.SlotSRAMValid[i] = NSS.Slot[i].SRAMValid;

	s.PROMCounter = NSS.PROM.Counter;
	s.PROMSevenBit = NSS.PROM.SevenBit;
	s.PROMLastClock = NSS.PROM.LastClock;
	s.PROMReset = NSS.PROM.Reset;

	memcpy(s.EEPROMData, NSS.EEPROM.Data, sizeof s.EEPROMData);
	s.EECS = NSS.EEPROM.CS;				s.EEClock = NSS.EEPROM.Clock;
	s.EEDataIn = NSS.EEPROM.DataIn;		s.EEDataOut = NSS.EEPROM.DataOut;
	s.EEState = NSS.EEPROM.State;		s.EEBitPos = NSS.EEPROM.BitPos;
	s.EEAddr = NSS.EEPROM.Addr;			s.EEWriteEnable = NSS.EEPROM.WriteEnable;
	s.EEShift = NSS.EEPROM.Shift;

	s.RTC = NSS.RTC;

	s.OSDCS = NSS.OSD.CS;				s.OSDClock = NSS.OSD.Clock;
	s.OSDDataIn = NSS.OSD.DataIn;		s.OSDBitPos = NSS.OSD.BitPos;
	s.OSDHaveAddr = NSS.OSD.HaveAddr;	s.OSDShift = NSS.OSD.Shift;
	s.OSDAddr = NSS.OSD.Addr;
	memcpy(s.OSDVRAM, NSS.OSD.VRAM, sizeof s.OSDVRAM);
	memcpy(s.OSDReg, NSS.OSD.Reg, sizeof s.OSDReg);
	s.OSDBlinkFrame = NSS.OSD.BlinkFrame;

	memcpy(buf + NSS_STATE_HEADER, &s, sizeof s);

	uint8	*tail = buf + NSS_STATE_HEADER + sizeof s;
	for (int i = 0; i < NSS_SLOTS; i++)
	{
		const uint32	n = NSSSlotSRAMBytes(i);
		if (!n)
			continue;
		memcpy(tail, NSS.Slot[i].SRAM, n);
		tail += n;
	}
}

bool8 S9xNSSStateLoad (const uint8 *buf, size_t size)
{
	struct SNSSSaveState	s;

	if (size < NSS_STATE_HEADER || memcmp(buf, "NSS!", 4) != 0 ||
		buf[4] != NSS_STATE_VERSION)
		return (FALSE);

	const size_t	payload = (size_t) buf[8] | ((size_t) buf[9] << 8) |
							  ((size_t) buf[10] << 16) | ((size_t) buf[11] << 24);
	if (payload < sizeof s || size < NSS_STATE_HEADER + payload)
		return (FALSE);

	memcpy(&s, buf + NSS_STATE_HEADER, sizeof s);

	Z80 = s.Cpu;
	memcpy(NSS.WRAM, s.WRAM, NSS_WRAM_SIZE);
	NSS.Port00W = s.Port00W;	NSS.Port01W = s.Port01W;
	NSS.Port03W = s.Port03W;	NSS.Port04W = s.Port04W;
	NSS.SlotSelect = s.SlotSelect;
	NSS.SNESHeld = s.SNESHeld ? TRUE : FALSE;
	NSS.PendingSNESReset = s.PendingSNESReset ? TRUE : FALSE;
	NSS.PendingAPUReset = s.PendingAPUReset ? TRUE : FALSE;
	NSS.InputDisabled = s.InputDisabled ? TRUE : FALSE;
	NSS.SoundMuted = s.SoundMuted ? TRUE : FALSE;
	NSS.GameOverFlag = s.GameOverFlag;
	NSS.JoyReadFlag = s.JoyReadFlag;
	NSS.DipSwitches = s.DipSwitches;
	NSS.Buttons = s.Buttons;
	NSS.PulseButtons = s.PulseButtons;
	NSS.PulseLeft = s.PulseLeft;
	NSS.CoinPulse[0] = s.CoinPulse[0];
	NSS.CoinPulse[1] = s.CoinPulse[1];
	NSS.NMIEnable = s.NMIEnable;
	NSS.WRAMUnlock = s.WRAMUnlock;
	NSS.LastVCounter = s.LastVCounter;
	NSS.CycleRemainder = s.CycleRemainder;

	NSS.MappedSlot = s.MappedSlot;
	for (int i = 0; i < NSS_SLOTS; i++)
		NSS.Slot[i].SRAMValid = s.SlotSRAMValid[i];

	// The batteries follow, in socket order, for whichever sockets have one.
	// A snapshot taken with a different set of cartridges in simply runs out
	// of tail, and those slots keep what they have.
	const uint8	*tail = buf + NSS_STATE_HEADER + sizeof s;
	const uint8	*end = buf + NSS_STATE_HEADER + payload;
	for (int i = 0; i < NSS_SLOTS; i++)
	{
		const uint32	n = NSSSlotSRAMBytes(i);
		if (!n || tail + n > end)
			continue;
		memcpy(NSS.Slot[i].SRAM, tail, n);
		tail += n;
	}

	NSS.PROM.Counter = s.PROMCounter;
	NSS.PROM.SevenBit = s.PROMSevenBit;
	NSS.PROM.LastClock = s.PROMLastClock;
	NSS.PROM.Reset = s.PROMReset;

	memcpy(NSS.EEPROM.Data, s.EEPROMData, sizeof s.EEPROMData);
	NSS.EEPROM.CS = s.EECS;				NSS.EEPROM.Clock = s.EEClock;
	NSS.EEPROM.DataIn = s.EEDataIn;		NSS.EEPROM.DataOut = s.EEDataOut;
	NSS.EEPROM.State = s.EEState;		NSS.EEPROM.BitPos = s.EEBitPos;
	NSS.EEPROM.Addr = s.EEAddr;			NSS.EEPROM.WriteEnable = s.EEWriteEnable;
	NSS.EEPROM.Shift = s.EEShift;

	NSS.RTC = s.RTC;

	NSS.OSD.CS = s.OSDCS;				NSS.OSD.Clock = s.OSDClock;
	NSS.OSD.DataIn = s.OSDDataIn;		NSS.OSD.BitPos = s.OSDBitPos;
	NSS.OSD.HaveAddr = s.OSDHaveAddr;	NSS.OSD.Shift = s.OSDShift;
	NSS.OSD.Addr = s.OSDAddr;
	memcpy(NSS.OSD.VRAM, s.OSDVRAM, sizeof s.OSDVRAM);
	memcpy(NSS.OSD.Reg, s.OSDReg, sizeof s.OSDReg);
	NSS.OSD.BlinkFrame = s.OSDBlinkFrame;

	return (TRUE);
}

// ---------------------------------------------------------------------------
// Power / reset / main-loop pump

void S9xNSSDeactivate (void)
{
	NSS.Active = FALSE;
	NSS.MappedSlot = -1;
	for (int i = 0; i < NSS_SLOTS; i++)
	{
		free(NSS.Slot[i].Prg);
		memset(&NSS.Slot[i], 0, sizeof(NSS.Slot[i]));
	}
}

void S9xNSSPowerOn (void)
{
	// Whatever was running keeps its battery across the power cycle, and
	// stops making noise.
	S9xNSSStashMappedSRAM();
	NSS.PendingAPUReset = TRUE;

	// Everything the loader owns — the BIOS, the charset and whatever is in
	// the sockets — outlives a power cycle; only the volatile board does not.
	// Clearing field by field rather than wiping the struct is what keeps the
	// slots' heap buffers.
	memset(NSS.WRAM, 0, sizeof(NSS.WRAM));
	NSS.Port00W = NSS.Port01W = NSS.Port03W = NSS.Port04W = 0;
	NSS.SlotSelect = 0;
	NSS.PendingSNESReset = FALSE;
	NSS.SoundMuted = FALSE;
	NSS.Buttons = NSS.PulseButtons = 0;
	NSS.PulseLeft = 0;
	NSS.CoinPulse[0] = NSS.CoinPulse[1] = 0;
	NSS.NMIEnable = NSS.WRAMUnlock = 0;
	NSS.CycleRemainder = 0;
	memset(&NSS.PROM, 0, sizeof(NSS.PROM));
	memset(&NSS.EEPROM, 0, sizeof(NSS.EEPROM));
	memset(&NSS.RTC, 0, sizeof(NSS.RTC));

	NSS.OSD.CS = NSS.OSD.Clock = NSS.OSD.DataIn = 0;
	NSS.OSD.BitPos = 0;
	NSS.OSD.Shift = 0;
	NSS.OSD.HaveAddr = FALSE;
	NSS.OSD.Addr = 0;
	NSS.OSD.BlinkFrame = 0;
	memset(NSS.OSD.Reg, 0, sizeof(NSS.OSD.Reg));

	NSS.Active = TRUE;
	NSS.SNESHeld = TRUE;		// the BIOS lets the game go when it is paid for
	NSS.InputDisabled = TRUE;
	NSS.JoyReadFlag = 1;
	NSS.GameOverFlag = 1;
	NSS.LastVCounter = -1;
	NSS.MappedSlot = -1;

	for (int i = 0; i < NSS_EEPROM_WORDS; i++)
		NSS.EEPROM.Data[i] = 0xffff;
	for (int i = 0; i < NSS_OSD_CELLS; i++)
		NSS.OSD.VRAM[i] = 0x007f;

	S9xNSSLoadNVRAM();
	RTCLoadHostTime();

	Z80CB.MemRead = NSSMemRead;
	Z80CB.MemWrite = NSSMemWrite;
	Z80CB.IORead = NSSIORead;
	Z80CB.IOWrite = NSSIOWrite;
	Z80CB.IntAck = NULL;
	Z80_Reset();

	printf("NSS: supervisor board powered on (BIOS rev %s, slots:",
		   NSS.BIOSRevision == 2 ? "02" : NSS.BIOSRevision == 3 ? "03" : "?");
	for (int i = 0; i < NSS_SLOTS; i++)
		printf(" %d=%s", i + 1, NSS.Slot[i].Present ? NSS.Slot[i].Name : "-");
	printf(").\n");
}

bool8 S9xNSSSNESHeld (void)
{
	return (NSS.Active && NSS.SNESHeld);
}

bool8 S9xNSSPendingReset (void)
{
	return (NSS.Active && NSS.PendingSNESReset);
}

bool8 S9xNSSPendingAPUReset (void)
{
	return (NSS.Active && NSS.PendingAPUReset);
}

// Taken at an instruction boundary in the main loop, the same place the
// 65816's own reset lands, so the APU is not re-timed mid-scanline.
void S9xNSSApplyAPUReset (void)
{
	NSS.PendingAPUReset = FALSE;
	S9xSoftResetAPU();
}

void S9xNSSApplySNESReset (void)
{
	NSS.PendingSNESReset = FALSE;
	S9xSoftReset();
}

void S9xNSSEndScanline (void)
{
	if (!NSS.Active)
		return;

	const int32	vblank = VBlankLine();

	// Frame-granular housekeeping at wraparound
	if (CPU.V_Counter == 0 && NSS.LastVCounter != 0)
	{
		NSS.OSD.BlinkFrame++;

		for (int i = 0; i < 2; i++)
			if (NSS.CoinPulse[i] > 0)
				NSS.CoinPulse[i]--;

		if (NSS.PulseLeft > 0 && --NSS.PulseLeft == 0)
			NSS.PulseButtons = 0;

		if (++NSS.RTC.FrameAccum >= (Settings.PAL ? 50 : 60))
		{
			NSS.RTC.FrameAccum = 0;
			RTCTickSecond();
		}

		if (TraceEnabled())
			printf("[nss] Z80 PC=%04X SP=%04X halted=%d IFF=%d 00W=%02X 01W=%02X held=%d osd7=%04X\n",
				   Z80.PC, Z80.SP, Z80.Halted, Z80.IFF1,
				   NSS.Port00W, NSS.Port01W, NSS.SNESHeld, NSS.OSD.Reg[7]);
	}

	// The supervisor's only interrupt is the vertical one.
	if (CPU.V_Counter == vblank && NSS.LastVCounter != vblank && NSS.NMIEnable)
		Z80_PulseNMI();

	NSS.LastVCounter = CPU.V_Counter;

	// Z80 clocks for one SNES scanline (H_Max master clocks)
	const int64	master = Settings.PAL ? 21281370 : 21477272;
	int64		num = (int64) Timings.H_Max * NSS_PHI + NSS.CycleRemainder;
	int32		cycles = (int32) (num / master);
	NSS.CycleRemainder = num % master;

	Z80_Execute(cycles);
}
