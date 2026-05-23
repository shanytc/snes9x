#include "CDisasmPanel.h"
#include "CDebuggerSnes.h"
#include "CDebuggerGb.h"
#include <stdio.h>
#include <tchar.h>

static const TCHAR *kClassName = TEXT("S9xDebuggerDisasmPanel");
static const int    kMarginPx  = 22;
static const int    kRowH      = 16;

struct DisasmPanelState
{
	DbgSystem sys;
	HFONT     font;
	int       scroll_offset_rows;
	bool      follow_pc;
};

static DisasmPanelState *GetState(HWND h)
{
	return (DisasmPanelState *)GetWindowLongPtr(h, GWLP_USERDATA);
}

static uint32_t GetCurrentPC(DbgSystem sys)
{
	if (sys == DbgSystem::Snes)
	{
		SnesStatus s = {};
		SnesBackend::GetStatus(&s);
		return ((uint32_t)s.prog_bank << 16) | s.PC;
	}
	GbStatus s = {};
	GbBackend::GetStatus(&s);
	return s.pc;
}

static int DisasmOne(DbgSystem sys, uint32_t pc, char *line, size_t cap, uint8_t *out_bytes, int *out_byte_count)
{
	if (sys == DbgSystem::Snes)
	{
		DisasmResult65816 r = {};
		uint8_t len = SnesBackend::Disassemble(pc, &r);
		_snprintf_s(line, cap, _TRUNCATE, "%02X:%04X  %-4s %s",
		            (unsigned)(pc >> 16) & 0xFF, (unsigned)(pc & 0xFFFF),
		            r.mnemonic, r.operand);
		for (int i = 0; i < len && i < 4; i++) out_bytes[i] = r.bytes[i];
		*out_byte_count = len;
		return len;
	}
	DisasmResultGb r = {};
	uint8_t len = GbBackend::Disassemble((uint16_t)pc, &r);
	_snprintf_s(line, cap, _TRUNCATE, "%04X  %-4s %s",
	            (unsigned)(pc & 0xFFFF), r.mnemonic, r.operand);
	for (int i = 0; i < len && i < 3; i++) out_bytes[i] = r.bytes[i];
	*out_byte_count = len;
	return len;
}

static void OnPaint(HWND h, DisasmPanelState *st)
{
	PAINTSTRUCT ps;
	HDC dc = BeginPaint(h, &ps);
	RECT rc; GetClientRect(h, &rc);

	HBRUSH bgBrush = CreateSolidBrush(RGB(255, 255, 255));
	FillRect(dc, &rc, bgBrush);
	DeleteObject(bgBrush);

	HFONT old = (HFONT)SelectObject(dc, st->font);
	SetBkMode(dc, TRANSPARENT);

	const uint32_t cur_pc = GetCurrentPC(st->sys);
	uint32_t pc = cur_pc;

	const int rows = (rc.bottom - rc.top) / kRowH;
	const int pc_row = rows / 3;

	if (st->sys == DbgSystem::Snes)
		pc = (pc & 0xFF0000) | ((uint16_t)(pc - (pc_row * 2)) & 0xFFFF);
	else
		pc = (uint16_t)(pc - pc_row);

	for (int i = 0; i < rows; i++)
	{
		int y = i * kRowH;

		if (pc == cur_pc)
		{
			HBRUSH hb = CreateSolidBrush(RGB(255, 255, 180));
			RECT row = { 0, y, rc.right, y + kRowH };
			FillRect(dc, &row, hb);
			DeleteObject(hb);

			HBRUSH yel = CreateSolidBrush(RGB(220, 200, 0));
			POINT arrow[3] = {
				{ 4,            y + 3 },
				{ kMarginPx - 6, y + kRowH / 2 },
				{ 4,            y + kRowH - 3 }
			};
			HRGN hr = CreatePolygonRgn(arrow, 3, ALTERNATE);
			FillRgn(dc, hr, yel);
			DeleteObject(hr);
			DeleteObject(yel);
		}

		bool has_bp = false;
		if (st->sys == DbgSystem::Snes)
			has_bp = gDebugger.HasExecBreakpoint(DbgSystem::Snes, (uint8_t)(pc >> 16), (uint16_t)(pc & 0xFFFF));
		else
			has_bp = gDebugger.HasExecBreakpoint(DbgSystem::Gb, 0, (uint16_t)pc);

		if (has_bp)
		{
			HBRUSH red = CreateSolidBrush(RGB(200, 0, 0));
			RECT dot = { 6, y + 4, 6 + 10, y + 4 + 10 };
			FillRect(dc, &dot, red);
			DeleteObject(red);
		}

		char line[160];
		uint8_t bytes[4];
		int byte_count = 0;
		int len = DisasmOne(st->sys, pc, line, sizeof(line), bytes, &byte_count);

		char hex[16];
		if (byte_count == 1) _snprintf_s(hex, 16, _TRUNCATE, "%02X      ", bytes[0]);
		else if (byte_count == 2) _snprintf_s(hex, 16, _TRUNCATE, "%02X %02X   ", bytes[0], bytes[1]);
		else if (byte_count == 3) _snprintf_s(hex, 16, _TRUNCATE, "%02X %02X %02X", bytes[0], bytes[1], bytes[2]);
		else _snprintf_s(hex, 16, _TRUNCATE, "%02X %02X %02X %02X", bytes[0], bytes[1], bytes[2], bytes[3]);

		char full[200];
		_snprintf_s(full, 200, _TRUNCATE, "  %s   %s", hex, line);

		SetTextColor(dc, RGB(20, 20, 20));
		TextOutA(dc, kMarginPx, y + 1, full, (int)strlen(full));

		if (st->sys == DbgSystem::Snes)
		{
			const uint16_t lo = (uint16_t)(pc & 0xFFFF) + (uint16_t)len;
			pc = (pc & 0xFF0000) | lo;
		}
		else
		{
			pc = (uint16_t)((uint16_t)pc + (uint16_t)len);
		}
	}

	HPEN pen = CreatePen(PS_SOLID, 1, RGB(200, 200, 200));
	HPEN oldPen = (HPEN)SelectObject(dc, pen);
	MoveToEx(dc, kMarginPx - 1, 0, NULL);
	LineTo(dc, kMarginPx - 1, rc.bottom);
	SelectObject(dc, oldPen);
	DeleteObject(pen);

	SelectObject(dc, old);
	EndPaint(h, &ps);
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg)
	{
		case WM_NCCREATE:
		{
			CREATESTRUCT *cs = (CREATESTRUCT *)lp;
			DisasmPanelState *st = new DisasmPanelState();
			memset(st, 0, sizeof(*st));
			st->sys = (DbgSystem)(intptr_t)cs->lpCreateParams;
			st->font = CreateFont(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
			                       DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
			                       ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, TEXT("Consolas"));
			st->follow_pc = true;
			SetWindowLongPtr(h, GWLP_USERDATA, (LONG_PTR)st);
			break;
		}

		case WM_DESTROY:
		{
			DisasmPanelState *st = GetState(h);
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
			DisasmPanelState *st = GetState(h);
			if (st) OnPaint(h, st);
			return 0;
		}

		case WM_SIZE:
			InvalidateRect(h, NULL, FALSE);
			return 0;

		case WM_LBUTTONDOWN:
		{
			DisasmPanelState *st = GetState(h);
			if (!st) return 0;
			const int x = LOWORD(lp);
			const int y = HIWORD(lp);
			if (x < kMarginPx)
			{
				RECT rc; GetClientRect(h, &rc);
				const int rows = (rc.bottom - rc.top) / kRowH;
				const int pc_row = rows / 3;
				const int row = y / kRowH;

				const uint32_t cur_pc = GetCurrentPC(st->sys);
				uint32_t pc = cur_pc;
				if (st->sys == DbgSystem::Snes)
					pc = (pc & 0xFF0000) | ((uint16_t)(pc - (pc_row * 2)) & 0xFFFF);
				else
					pc = (uint16_t)(pc - pc_row);

				for (int i = 0; i < row; i++)
				{
					char line[160]; uint8_t b[4]; int c;
					int len = DisasmOne(st->sys, pc, line, sizeof(line), b, &c);
					if (st->sys == DbgSystem::Snes)
						pc = (pc & 0xFF0000) | (((uint16_t)(pc & 0xFFFF)) + (uint16_t)len);
					else
						pc = (uint16_t)((uint16_t)pc + (uint16_t)len);
				}

				const uint8_t  bank = (st->sys == DbgSystem::Snes) ? (uint8_t)(pc >> 16) : (uint8_t)0;
				const uint16_t addr = (uint16_t)(pc & 0xFFFF);
				gDebugger.ToggleExecBreakpoint(st->sys, bank, addr);
				InvalidateRect(h, NULL, FALSE);
			}
			SetFocus(h);
			return 0;
		}
	}

	return DefWindowProc(h, msg, wp, lp);
}

void DisasmPanelRegisterClass(HINSTANCE hInst)
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

HWND DisasmPanelCreate(HWND parent, DbgSystem sys, int id)
{
	return CreateWindowEx(WS_EX_CLIENTEDGE, kClassName, TEXT(""),
	                     WS_CHILD | WS_VISIBLE,
	                     0, 0, 0, 0,
	                     parent, (HMENU)(LONG_PTR)id, GetModuleHandle(NULL), (LPVOID)(intptr_t)sys);
}

void DisasmPanelRefresh(HWND h)
{
	if (h && IsWindow(h))
		InvalidateRect(h, NULL, FALSE);
}
