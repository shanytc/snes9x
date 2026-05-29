#include "CMapperPanel.h"
#include "../sgb/sgb.h"
#include <stdio.h>
#include <string.h>
#include <tchar.h>

static const TCHAR *kClassName = TEXT("S9xDebuggerMapperPanel");

struct MapperPanelState
{
	DbgSystem sys;
	HFONT     font;
};

static MapperPanelState *GetState(HWND h)
{
	return (MapperPanelState *)GetWindowLongPtr(h, GWLP_USERDATA);
}

static const char *MbcName(int t)
{
	switch (t)
	{
		case 0:  return "None (ROM only)";
		case 1:  return "MBC1";
		case 2:  return "MBC2";
		case 3:  return "MBC3";
		case 5:  return "MBC5";
		case 6:  return "MBC6";
		case 7:  return "MBC7";
		case 8:  return "HuC1";
		case 9:  return "HuC3";
		case 10: return "MMM01";
		case 11: return "Sachen MMC1";
		default: return "Unknown";
	}
}

// One left-aligned line of text; advances y by the line height.
static void Line(HDC dc, int x, int &y, const char *s)
{
	TextOutA(dc, x, y, s, (int)strlen(s));
	y += 18;
}

static void PaintMapper(HDC dc, MapperPanelState *st)
{
	const int x = 8;
	int y = 8;
	SetTextColor(dc, RGB(20, 20, 20));

	if (st->sys != DbgSystem::Gb)
	{
		Line(dc, x, y, "Mapper panel: Game Boy cartridges only.");
		return;
	}

	SgbMapperInfo m = {};
	if (!S9xSGBGetMapperInfo(&m))
	{
		Line(dc, x, y, "No Game Boy cartridge loaded.");
		return;
	}

	char buf[96];

	_snprintf_s(buf, sizeof(buf), _TRUNCATE, "Mapper:     %s  (type $%02X)",
	            MbcName(m.mbc_type), m.cart_type);
	Line(dc, x, y, buf);

	const uint32_t rom_banks = m.rom_size ? (m.rom_size / 0x4000u) : 0;
	const uint32_t ram_banks = m.ram_size ? (m.ram_size / 0x2000u) : 0;

	_snprintf_s(buf, sizeof(buf), _TRUNCATE, "ROM:        %u KB  (%u banks)",
	            m.rom_size / 1024u, rom_banks);
	Line(dc, x, y, buf);

	if (m.ram_size)
		_snprintf_s(buf, sizeof(buf), _TRUNCATE, "RAM:        %u KB  (%u bank%s)%s",
		            m.ram_size / 1024u, ram_banks, ram_banks == 1 ? "" : "s",
		            m.has_battery ? "  +battery" : "");
	else
		_snprintf_s(buf, sizeof(buf), _TRUNCATE, "RAM:        none");
	Line(dc, x, y, buf);

	y += 6;

	// Effective ROM bank at $4000-$7FFF (the bank register wraps modulo the
	// cart's bank count on read, exactly as MbcRead does).
	const uint32_t eff_rom = rom_banks ? (m.rom_bank % rom_banks) : m.rom_bank;
	if (eff_rom != m.rom_bank)
		_snprintf_s(buf, sizeof(buf), _TRUNCATE,
		            "ROM bank:   $%03X  (-> phys $%02X)", m.rom_bank, eff_rom);
	else
		_snprintf_s(buf, sizeof(buf), _TRUNCATE, "ROM bank:   $%03X", m.rom_bank);
	Line(dc, x, y, buf);

	_snprintf_s(buf, sizeof(buf), _TRUNCATE, "RAM bank:   $%02X", m.ram_bank);
	Line(dc, x, y, buf);

	// RAM access state — colored, since this is the field that most often
	// explains "why is $A000-$BFFF reading $FF / why did a far-call derail".
	const char *lbl = "RAM access: ";
	TextOutA(dc, x, y, lbl, (int)strlen(lbl));
	SIZE sz = {};
	GetTextExtentPoint32A(dc, lbl, (int)strlen(lbl), &sz);
	const char *val = m.ram_enable ? "ENABLED" : "DISABLED";
	SetTextColor(dc, m.ram_enable ? RGB(0, 140, 0) : RGB(200, 0, 0));
	TextOutA(dc, x + sz.cx, y, val, (int)strlen(val));
	SetTextColor(dc, RGB(20, 20, 20));
	y += 18;

	if (m.mbc_type == 1) // MBC1 banking mode
	{
		_snprintf_s(buf, sizeof(buf), _TRUNCATE, "Mode:       %s",
		            m.mbc1_mode ? "RAM banking (1)" : "ROM banking (0)");
		Line(dc, x, y, buf);
	}

	if (m.has_battery || m.has_rtc || m.has_rumble)
	{
		_snprintf_s(buf, sizeof(buf), _TRUNCATE, "Features:   %s%s%s",
		            m.has_battery ? "BATTERY " : "",
		            m.has_rtc     ? "RTC "     : "",
		            m.has_rumble  ? "RUMBLE "  : "");
		Line(dc, x, y, buf);
	}
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg)
	{
		case WM_NCCREATE:
		{
			CREATESTRUCT *cs = (CREATESTRUCT *)lp;
			MapperPanelState *st = new MapperPanelState();
			memset(st, 0, sizeof(*st));
			st->sys  = (DbgSystem)(intptr_t)cs->lpCreateParams;
			st->font = CreateFont(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
			                       DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
			                       ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, TEXT("Consolas"));
			SetWindowLongPtr(h, GWLP_USERDATA, (LONG_PTR)st);
			break;
		}

		case WM_DESTROY:
		{
			MapperPanelState *st = GetState(h);
			if (st)
			{
				if (st->font) DeleteObject(st->font);
				delete st;
			}
			return 0;
		}

		case WM_ERASEBKGND:
			return 1;

		case WM_PAINT:
		{
			MapperPanelState *st = GetState(h);
			PAINTSTRUCT ps;
			HDC dc = BeginPaint(h, &ps);

			RECT rc; GetClientRect(h, &rc);
			HBRUSH bg = CreateSolidBrush(RGB(245, 245, 245));
			FillRect(dc, &rc, bg);
			DeleteObject(bg);

			if (st)
			{
				HFONT old = (HFONT)SelectObject(dc, st->font);
				SetBkMode(dc, OPAQUE);
				SetBkColor(dc, RGB(245, 245, 245));
				PaintMapper(dc, st);
				SelectObject(dc, old);
			}
			EndPaint(h, &ps);
			return 0;
		}

		case WM_SIZE:
			InvalidateRect(h, NULL, TRUE);
			return 0;
	}

	return DefWindowProc(h, msg, wp, lp);
}

void MapperPanelRegisterClass(HINSTANCE hInst)
{
	static bool done = false;
	if (done) return;
	done = true;

	WNDCLASSEX wc = {};
	wc.cbSize        = sizeof(wc);
	wc.style         = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc   = WndProc;
	wc.hInstance     = hInst;
	wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = NULL;
	wc.lpszClassName = kClassName;
	RegisterClassEx(&wc);
}

HWND MapperPanelCreate(HWND parent, DbgSystem sys, int id)
{
	return CreateWindowEx(WS_EX_CLIENTEDGE, kClassName, TEXT(""),
	                     WS_CHILD | WS_VISIBLE,
	                     0, 0, 0, 0,
	                     parent, (HMENU)(LONG_PTR)id, GetModuleHandle(NULL), (LPVOID)(intptr_t)sys);
}

void MapperPanelRefresh(HWND h)
{
	if (h && IsWindow(h))
		InvalidateRect(h, NULL, FALSE);
}
