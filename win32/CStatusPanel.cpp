#include "CStatusPanel.h"
#include "CDebuggerSnes.h"
#include "CDebuggerGb.h"
#include <stdio.h>
#include <tchar.h>

static const TCHAR *kClassName = TEXT("S9xDebuggerStatusPanel");

struct StatusPanelState
{
	DbgSystem sys;
	HFONT     font;
};

static StatusPanelState *GetState(HWND h)
{
	return (StatusPanelState *)GetWindowLongPtr(h, GWLP_USERDATA);
}

static void DrawReg(HDC dc, int x, int y, const char *name, uint32_t value, int width_hex)
{
	char buf[32];
	if (width_hex == 6)      _snprintf_s(buf, 32, _TRUNCATE, "%s:%06X", name, value);
	else if (width_hex == 4) _snprintf_s(buf, 32, _TRUNCATE, "%s:%04X", name, value);
	else                     _snprintf_s(buf, 32, _TRUNCATE, "%s:%02X",   name, value);
	TextOutA(dc, x, y, buf, (int)strlen(buf));
}

static void DrawCheck(HDC dc, int x, int y, const char *label, bool on)
{
	RECT box = { x, y + 2, x + 12, y + 14 };
	HBRUSH br = (HBRUSH)GetStockObject(WHITE_BRUSH);
	FillRect(dc, &box, br);
	HPEN pen = CreatePen(PS_SOLID, 1, RGB(120, 120, 120));
	HPEN oldp = (HPEN)SelectObject(dc, pen);
	HBRUSH oldb = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
	Rectangle(dc, box.left, box.top, box.right, box.bottom);
	SelectObject(dc, oldb);
	if (on)
	{
		MoveToEx(dc, box.left + 2, box.top + 6, NULL);
		LineTo(dc, box.left + 5, box.top + 9);
		LineTo(dc, box.right - 2, box.top + 2);
	}
	SelectObject(dc, oldp);
	DeleteObject(pen);
	TextOutA(dc, x + 16, y, label, (int)strlen(label));
}

static void PaintSnes(HDC dc, HWND h)
{
	SnesStatus s = {};
	SnesBackend::GetStatus(&s);

	int y = 8;
	int x = 8;
	const int colW = 90;

	SetTextColor(dc, RGB(20, 20, 20));

	DrawReg(dc, x + colW*0, y, "A",  s.A,  4);
	DrawReg(dc, x + colW*1, y, "X",  s.X,  4);
	DrawReg(dc, x + colW*2, y, "Y",  s.Y,  4);
	DrawReg(dc, x + colW*3, y, "PC", ((uint32_t)s.prog_bank << 16) | s.PC, 6);
	y += 18;

	DrawReg(dc, x + colW*0, y, "D",  s.D,  4);
	DrawReg(dc, x + colW*1, y, "DB", s.data_bank, 2);
	DrawReg(dc, x + colW*2, y, "SP", s.S,  4);
	DrawReg(dc, x + colW*3, y, "P",  s.P,  2);
	y += 24;

	const char *flags[8] = {"Carry","Zero","Interrupt","Decimal","Index","Memory","Overflow","Negative"};
	const uint8_t masks[8] = {0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80};
	for (int i = 0; i < 4; i++)
		DrawCheck(dc, x + colW*i, y, flags[i], (s.P & masks[i]) != 0);
	y += 18;
	for (int i = 4; i < 8; i++)
		DrawCheck(dc, x + colW*(i-4), y, flags[i], (s.P & masks[i]) != 0);
	y += 24;

	DrawCheck(dc, x, y, "Emulation", s.emulation);
	y += 24;

	char buf[64];
	_snprintf_s(buf, 64, _TRUNCATE, "Cycle: %u", s.cycles);
	TextOutA(dc, x, y, buf, (int)strlen(buf));
	y += 18;
	_snprintf_s(buf, 64, _TRUNCATE, "Scanline: %u", s.v_counter);
	TextOutA(dc, x, y, buf, (int)strlen(buf));
	y += 18;
	_snprintf_s(buf, 64, _TRUNCATE, "VRAM Addr: $%04X", s.vram_addr);
	TextOutA(dc, x, y, buf, (int)strlen(buf));
	y += 18;
	_snprintf_s(buf, 64, _TRUNCATE, "OAM Addr: $%04X", s.oam_addr);
	TextOutA(dc, x, y, buf, (int)strlen(buf));
	y += 18;
	_snprintf_s(buf, 64, _TRUNCATE, "CGRAM Addr: $%04X", s.cgram_addr);
	TextOutA(dc, x, y, buf, (int)strlen(buf));
	y += 18;
	_snprintf_s(buf, 64, _TRUNCATE, "Frame: %u", s.frame);
	TextOutA(dc, x, y, buf, (int)strlen(buf));
}

static void PaintGb(HDC dc, HWND h)
{
	GbStatus s = {};
	GbBackend::GetStatus(&s);

	int y = 8;
	int x = 8;
	const int colW = 110;

	SetTextColor(dc, RGB(20, 20, 20));

	DrawReg(dc, x + colW*0, y, "A",  (s.af >> 8) & 0xFF, 2);
	DrawReg(dc, x + colW*1, y, "F",  s.af & 0xFF,        2);
	DrawReg(dc, x + colW*2, y, "PC", s.pc,               4);
	DrawReg(dc, x + colW*3, y, "SP", s.sp,               4);
	y += 18;

	DrawReg(dc, x + colW*0, y, "BC", s.bc, 4);
	DrawReg(dc, x + colW*1, y, "DE", s.de, 4);
	DrawReg(dc, x + colW*2, y, "HL", s.hl, 4);
	y += 24;

	const uint8_t f = s.af & 0xFF;
	DrawCheck(dc, x + colW*0, y, "Z (Zero)",  (f & 0x80) != 0);
	DrawCheck(dc, x + colW*1, y, "N (Sub)",   (f & 0x40) != 0);
	DrawCheck(dc, x + colW*2, y, "H (Half)",  (f & 0x20) != 0);
	DrawCheck(dc, x + colW*3, y, "C (Carry)", (f & 0x10) != 0);
	y += 24;

	DrawCheck(dc, x + colW*0, y, "IME",     s.ime != 0);
	DrawCheck(dc, x + colW*1, y, "HALT",    s.halted != 0);
	DrawCheck(dc, x + colW*2, y, "STOP",    s.stopped != 0);
	y += 24;

	char buf[64];
	_snprintf_s(buf, 64, _TRUNCATE, "IE: $%02X    IF: $%02X", s.ie, s.if_);
	TextOutA(dc, x, y, buf, (int)strlen(buf));
	y += 18;

	_snprintf_s(buf, 64, _TRUNCATE, "T-Cycles: %llu", (unsigned long long)s.t_cycles);
	TextOutA(dc, x, y, buf, (int)strlen(buf));
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg)
	{
		case WM_NCCREATE:
		{
			CREATESTRUCT *cs = (CREATESTRUCT *)lp;
			StatusPanelState *st = new StatusPanelState();
			memset(st, 0, sizeof(*st));
			st->sys = (DbgSystem)(intptr_t)cs->lpCreateParams;
			st->font = CreateFont(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
			                       DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
			                       ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, TEXT("Consolas"));
			SetWindowLongPtr(h, GWLP_USERDATA, (LONG_PTR)st);
			break;
		}

		case WM_DESTROY:
		{
			StatusPanelState *st = GetState(h);
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
			StatusPanelState *st = GetState(h);
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
				if (st->sys == DbgSystem::Snes) PaintSnes(dc, h);
				else                            PaintGb(dc, h);
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

void StatusPanelRegisterClass(HINSTANCE hInst)
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

HWND StatusPanelCreate(HWND parent, DbgSystem sys, int id)
{
	return CreateWindowEx(WS_EX_CLIENTEDGE, kClassName, TEXT(""),
	                     WS_CHILD | WS_VISIBLE,
	                     0, 0, 0, 0,
	                     parent, (HMENU)(LONG_PTR)id, GetModuleHandle(NULL), (LPVOID)(intptr_t)sys);
}

void StatusPanelRefresh(HWND h)
{
	if (h && IsWindow(h))
		InvalidateRect(h, NULL, FALSE);
}
