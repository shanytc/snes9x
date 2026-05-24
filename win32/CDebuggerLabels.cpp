#include "CDebuggerLabels.h"
#include <stdio.h>

static const char *kSnesPpu[0x40] =
{
	"INIDISP","OBSEL","OAMADDL","OAMADDH","OAMDATA","BGMODE","MOSAIC","BG1SC",
	"BG2SC","BG3SC","BG4SC","BG12NBA","BG34NBA","BG1HOFS","BG1VOFS","BG2HOFS",
	"BG2VOFS","BG3HOFS","BG3VOFS","BG4HOFS","BG4VOFS","VMAIN","VMADDL","VMADDH",
	"VMDATAL","VMDATAH","M7SEL","M7A","M7B","M7C","M7D","M7X",
	"M7Y","CGADD","CGDATA","W12SEL","W34SEL","WOBJSEL","WH0","WH1",
	"WH2","WH3","WBGLOG","WOBJLOG","TM","TS","TMW","TSW",
	"CGWSEL","CGADSUB","COLDATA","SETINI","MPYL","MPYM","MPYH","SLHV",
	"OAMDATAREAD","VMDATALREAD","VMDATAHREAD","CGDATAREAD","OPHCT","OPVCT","STAT77","STAT78"
};

static const char *kSnesApuIo[4] = { "APUIO0","APUIO1","APUIO2","APUIO3" };

static const char *kSnesWram[4] = { "WMDATA","WMADDL","WMADDM","WMADDH" };

static const char *kSnesCpu[0x20] =
{
	"NMITIMEN","WRIO","WRMPYA","WRMPYB","WRDIVL","WRDIVH","WRDIVB","HTIMEL",
	"HTIMEH","VTIMEL","VTIMEH","MDMAEN","HDMAEN","MEMSEL",NULL,NULL,
	"RDNMI","TIMEUP","HVBJOY","RDIO","RDDIVL","RDDIVH","RDMPYL","RDMPYH",
	"JOY1L","JOY1H","JOY2L","JOY2H","JOY3L","JOY3H","JOY4L","JOY4H"
};

static const char *kSnesDmaPerChan[16] =
{
	"DMAP","BBAD","A1T_L","A1T_H","A1B","DAS_L","DAS_H","DASB",
	"A2A_L","A2A_H","NLTR",NULL,NULL,NULL,NULL,"UNUSED"
};

const char *LookupSnesLabel(uint32_t addr24)
{
	const uint32_t a = addr24 & 0xFFFF;

	if (a >= 0x2100 && a <= 0x213F)
		return kSnesPpu[a - 0x2100];
	if (a >= 0x2140 && a <= 0x2143)
		return kSnesApuIo[a - 0x2140];
	if (a >= 0x2180 && a <= 0x2183)
		return kSnesWram[a - 0x2180];
	if (a == 0x4016) return "JOYSER0";
	if (a == 0x4017) return "JOYSER1";
	if (a >= 0x4200 && a <= 0x421F)
		return kSnesCpu[a - 0x4200];
	if (a >= 0x4300 && a <= 0x437F)
	{
		static char buf[16];
		const int chan = (a - 0x4300) >> 4;
		const int reg  = (a - 0x4300) & 0x0F;
		const char *name = kSnesDmaPerChan[reg];
		if (!name) return NULL;
		snprintf(buf, sizeof(buf), "%s%d", name, chan);
		return buf;
	}

	return NULL;
}

static const char *kGbIo[0x80] =
{
	/* FF00 */ "P1","SB","SC",NULL,"DIV","TIMA","TMA","TAC",
	/* FF08 */ NULL,NULL,NULL,NULL,NULL,NULL,NULL,"IF",
	/* FF10 */ "NR10","NR11","NR12","NR13","NR14",NULL,"NR21","NR22",
	/* FF18 */ "NR23","NR24","NR30","NR31","NR32","NR33","NR34",NULL,
	/* FF20 */ "NR41","NR42","NR43","NR44","NR50","NR51","NR52",NULL,
	/* FF28 */ NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,
	/* FF30 */ "WAVE0","WAVE1","WAVE2","WAVE3","WAVE4","WAVE5","WAVE6","WAVE7",
	/* FF38 */ "WAVE8","WAVE9","WAVEA","WAVEB","WAVEC","WAVED","WAVEE","WAVEF",
	/* FF40 */ "LCDC","STAT","SCY","SCX","LY","LYC","DMA","BGP",
	/* FF48 */ "OBP0","OBP1","WY","WX",NULL,"KEY1",NULL,"VBK",
	/* FF50 */ "BOOT","HDMA1","HDMA2","HDMA3","HDMA4","HDMA5","RP",NULL,
	/* FF58 */ NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,
	/* FF60 */ NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,
	/* FF68 */ "BCPS","BCPD","OCPS","OCPD","OPRI",NULL,NULL,NULL,
	/* FF70 */ "SVBK",NULL,NULL,NULL,NULL,NULL,"PCM12","PCM34",
	/* FF78 */ NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL
};

const char *LookupGbLabel(uint16_t addr)
{
	if (addr >= 0xFF00 && addr <= 0xFF7F)
		return kGbIo[addr - 0xFF00];
	if (addr == 0xFFFF)
		return "IE";
	return NULL;
}

const char *LookupLabel(DbgSystem sys, uint32_t addr)
{
	if (sys == DbgSystem::Snes)
		return LookupSnesLabel(addr);
	return LookupGbLabel((uint16_t)(addr & 0xFFFF));
}
