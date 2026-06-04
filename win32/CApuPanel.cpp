#include "CApuPanel.h"
#include "../sgb/sgb.h"
#include <stdio.h>
#include <string.h>
#include <tchar.h>

// Live Game Boy APU view. Refreshed once per frame from DebuggerDlgRefresh, so
// it animates while the game runs — built to trace digitized-voice playback
// (Bart Simpson's "Eat my shorts", etc.) where a sample audibly cuts out half
// way. The CH3 (wave) row is highlighted since that's the usual voice channel;
// watch its EN/LEN columns and the lenOff/dacOff counters to see whether the
// channel is being disabled mid-sample, and wrWrites / triggers to see whether
// the game has simply stopped feeding it.

static const TCHAR *kClassName = TEXT("S9xDebuggerApuPanel");

struct ApuPanelState
{
	DbgSystem sys;
	HFONT     font;
	uint32_t  prev_nr50_writes; // for the per-frame NR50 (PCM) rate
	uint32_t  nr50_per_frame;   // computed once per frame in ApuPanelRefresh
	uint32_t  prev_pcm_silent;  // for the per-frame silent-PCM rate
	uint32_t  silent_per_frame; // NR50 writes/frame that hit a zero carrier
};

static ApuPanelState *GetState(HWND h)
{
	return (ApuPanelState *)GetWindowLongPtr(h, GWLP_USERDATA);
}

static const COLORREF kInk    = RGB(20, 20, 20);
static const COLORREF kGreen  = RGB(0, 140, 0);
static const COLORREF kRed    = RGB(200, 0, 0);
static const COLORREF kGrey   = RGB(140, 140, 140);
static const COLORREF kBg     = RGB(245, 245, 245);
static const COLORREF kRowHi  = RGB(225, 235, 250);

// One left-aligned line of plain text; advances y by the line height.
static void Line(HDC dc, int x, int &y, const char *s)
{
	SetTextColor(dc, kInk);
	TextOutA(dc, x, y, s, (int)strlen(s));
	y += 18;
}

// Text at a character-grid column (fixed-pitch font), with an explicit colour.
static void Cell(HDC dc, int x0, int charW, int col, int y, const char *s, COLORREF c)
{
	SetTextColor(dc, c);
	TextOutA(dc, x0 + col * charW, y, s, (int)strlen(s));
}

static const char *Ch3VolText(uint8_t code)
{
	switch (code & 3)
	{
		case 0:  return "mute";
		case 1:  return "100%";
		case 2:  return "50%";
		default: return "25%";
	}
}

// Expected timer-interrupt (= NR50-PCM sample) rate in Hz from TAC/TMA.
// Returns 0 when the timer is disabled.
static int TimerRateHz(uint8_t tac, uint8_t tma)
{
	if (!(tac & 0x04)) return 0;
	static const int inc[4] = { 4096, 262144, 65536, 16384 };
	const int period = 256 - (int)tma;     // TIMA counts TMA..0xFF, then overflows
	if (period <= 0) return 0;
	return inc[tac & 0x03] / period;
}

static void PaintApu(HDC dc, ApuPanelState *st, int charW)
{
	const int x = 8;
	int y = 8;

	if (st->sys != DbgSystem::Gb)
	{
		Line(dc, x, y, "APU panel: Game Boy audio only.");
		Line(dc, x, y, "(SNES audio is the SPC700 / S-DSP.)");
		return;
	}

	SgbApuInfo a = {};
	if (!S9xSGBGetApuState(&a))
	{
		Line(dc, x, y, "No Game Boy cartridge loaded.");
		return;
	}

	char buf[128];

	// Master row — NR52 power gates the whole chip.
	const char *mlbl = "APU master: ";
	SetTextColor(dc, kInk);
	TextOutA(dc, x, y, mlbl, (int)strlen(mlbl));
	Cell(dc, x, charW, (int)strlen(mlbl), y, a.master_enabled ? "ON" : "OFF",
	     a.master_enabled ? kGreen : kRed);
	_snprintf_s(buf, sizeof(buf), _TRUNCATE, "   NR50 $%02X  NR51 $%02X  step %u",
	            a.nr50, a.nr51, a.frame_seq_step);
	Cell(dc, x, charW, (int)strlen(mlbl) + 4, y, buf, kInk);
	y += 18;

	_snprintf_s(buf, sizeof(buf), _TRUNCATE, "Output: %d Hz   queued %u frames",
	            a.output_rate, a.queued_samples);
	Line(dc, x, y, buf);

	y += 6;

	// Column header for the per-channel table.
	Cell(dc, x, charW, 0,  y, "Ch",   kInk);
	Cell(dc, x, charW, 6,  y, "EN",   kInk);
	Cell(dc, x, charW, 11, y, "DAC",  kInk);
	Cell(dc, x, charW, 16, y, "LEN",  kInk);
	Cell(dc, x, charW, 24, y, "freq", kInk);
	Cell(dc, x, charW, 32, y, "vol",  kInk);
	Cell(dc, x, charW, 38, y, "trig", kInk);
	y += 18;

	const char *names[4] = { "CH1", "CH2", "CH3", "CH4" };
	for (int i = 0; i < 4; ++i)
	{
		const SgbApuChannelInfo &c = a.ch[i];

		// Highlight the wave channel (CH3) — the usual digitized-voice path.
		if (i == 2)
		{
			RECT hr = { x - 4, y - 1, x + charW * 46, y + 17 };
			HBRUSH hb = CreateSolidBrush(kRowHi);
			FillRect(dc, &hr, hb);
			DeleteObject(hb);
			SetBkColor(dc, kRowHi);
		}

		Cell(dc, x, charW, 0, y, names[i], kInk);
		Cell(dc, x, charW, 6,  y, c.enabled ? "on" : "--",
		     c.enabled ? kGreen : kGrey);
		Cell(dc, x, charW, 11, y, c.dac_enabled ? "on" : "off",
		     c.dac_enabled ? kGreen : kRed);

		if (c.length_enabled)
		{
			_snprintf_s(buf, sizeof(buf), _TRUNCATE, "%u", c.length);
			// Red when a gated length is counting down to silence.
			Cell(dc, x, charW, 16, y, buf, c.enabled ? kInk : kRed);
		}
		else
		{
			Cell(dc, x, charW, 16, y, "off", kGrey);
		}

		if (i == 3)
			_snprintf_s(buf, sizeof(buf), _TRUNCATE, "--");
		else
			_snprintf_s(buf, sizeof(buf), _TRUNCATE, "$%03X", c.freq);
		Cell(dc, x, charW, 24, y, buf, kInk);

		if (i == 2)
			Cell(dc, x, charW, 32, y, Ch3VolText((uint8_t)c.volume), kInk);
		else
		{
			_snprintf_s(buf, sizeof(buf), _TRUNCATE, "%u", c.volume);
			Cell(dc, x, charW, 32, y, buf, kInk);
		}

		_snprintf_s(buf, sizeof(buf), _TRUNCATE, "%u", c.trigger_count);
		Cell(dc, x, charW, 38, y, buf, kInk);

		if (i == 2) SetBkColor(dc, kBg);
		y += 18;
	}

	y += 6;

	// --- PCM-via-NR50 voice meter ---------------------------------------
	// This game streams digitized speech by writing NR50 once per timer IRQ
	// (CH4 = DC carrier). nr50/f is therefore the live PCM sample rate: it
	// should hold ~rate/60 through a whole voice line. A drop to 0 mid-line
	// is the sample being cut short — the "you only hear half" bug.
	const int rate = TimerRateHz(a.timer_tac, a.timer_tma);
	const char *plbl = "PCM NR50:  ";
	SetTextColor(dc, kInk);
	TextOutA(dc, x, y, plbl, (int)strlen(plbl));
	_snprintf_s(buf, sizeof(buf), _TRUNCATE, "%u/f", st->nr50_per_frame);
	// Green while actively streaming a voice, grey when idle.
	Cell(dc, x, charW, (int)strlen(plbl), y, buf,
	     st->nr50_per_frame > 30 ? kGreen : kGrey);
	_snprintf_s(buf, sizeof(buf), _TRUNCATE, "   silent ");
	Cell(dc, x, charW, (int)strlen(plbl) + 12, y, buf, kInk);
	_snprintf_s(buf, sizeof(buf), _TRUNCATE, "%u/f", st->silent_per_frame);
	// Red when PCM samples are landing on a dead carrier — the "half voice" bug.
	Cell(dc, x, charW, (int)strlen(plbl) + 22, y, buf,
	     st->silent_per_frame > 0 ? kRed : kGreen);
	y += 18;

	_snprintf_s(buf, sizeof(buf), _TRUNCATE,
	            "Timer: TAC $%02X TMA $%02X TIMA $%02X",
	            a.timer_tac, a.timer_tma, a.timer_tima);
	Line(dc, x, y, buf);
	if (rate > 0)
	{
		_snprintf_s(buf, sizeof(buf), _TRUNCATE,
		            "  ~%d Hz PCM   expect ~%d NR50/f", rate, rate / 60);
		Line(dc, x, y, buf);
	}

	y += 6;

	// CH3 detail — position, volume, and the disable/feed counters.
	_snprintf_s(buf, sizeof(buf), _TRUNCATE,
	            "CH3  pos %u/32   vol %s   wrWrites %u",
	            a.wave_pos, Ch3VolText(a.ch3_volume_code), a.wave_ram_writes);
	Line(dc, x, y, buf);

	const char *dlbl = "     lenOff ";
	SetTextColor(dc, kInk);
	TextOutA(dc, x, y, dlbl, (int)strlen(dlbl));
	_snprintf_s(buf, sizeof(buf), _TRUNCATE, "%u", a.ch3_len_disable);
	Cell(dc, x, charW, (int)strlen(dlbl), y, buf, a.ch3_len_disable ? kRed : kGreen);
	const int after = (int)strlen(dlbl) + (int)strlen(buf);
	Cell(dc, x, charW, after, y, "   dacOff ", kInk);
	_snprintf_s(buf, sizeof(buf), _TRUNCATE, "%u", a.ch3_dac_disable);
	Cell(dc, x, charW, after + 10, y, buf, kInk);
	y += 18;

	// Wave RAM (32 4-bit samples). Split across two lines of 8 bytes.
	char line[64];
	int n = 0;
	for (int i = 0; i < 8; ++i)
		n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, "%02X ", a.wave_ram[i]);
	_snprintf_s(buf, sizeof(buf), _TRUNCATE, "wave: %s", line);
	Line(dc, x, y, buf);

	n = 0;
	for (int i = 8; i < 16; ++i)
		n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, "%02X ", a.wave_ram[i]);
	_snprintf_s(buf, sizeof(buf), _TRUNCATE, "      %s", line);
	Line(dc, x, y, buf);
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg)
	{
		case WM_NCCREATE:
		{
			CREATESTRUCT *cs = (CREATESTRUCT *)lp;
			ApuPanelState *st = new ApuPanelState();
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
			ApuPanelState *st = GetState(h);
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
			ApuPanelState *st = GetState(h);
			PAINTSTRUCT ps;
			HDC dc = BeginPaint(h, &ps);

			RECT rc; GetClientRect(h, &rc);
			HBRUSH bg = CreateSolidBrush(kBg);
			FillRect(dc, &rc, bg);
			DeleteObject(bg);

			if (st)
			{
				HFONT old = (HFONT)SelectObject(dc, st->font);
				SetBkMode(dc, OPAQUE);
				SetBkColor(dc, kBg);
				SIZE sz = {};
				GetTextExtentPoint32A(dc, "0", 1, &sz);
				const int charW = sz.cx > 0 ? sz.cx : 8;
				PaintApu(dc, st, charW);
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

void ApuPanelRegisterClass(HINSTANCE hInst)
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

HWND ApuPanelCreate(HWND parent, DbgSystem sys, int id)
{
	return CreateWindowEx(WS_EX_CLIENTEDGE, kClassName, TEXT(""),
	                     WS_CHILD | WS_VISIBLE,
	                     0, 0, 0, 0,
	                     parent, (HMENU)(LONG_PTR)id, GetModuleHandle(NULL), (LPVOID)(intptr_t)sys);
}

void ApuPanelRefresh(HWND h)
{
	if (!h || !IsWindow(h)) return;

	// Sample the NR50 (PCM) write counter once per frame here — refresh is
	// driven from the per-frame DebuggerDlgRefresh, so the delta is a true
	// per-frame rate. Doing it in WM_PAINT would corrupt on extra repaints.
	ApuPanelState *st = GetState(h);
	if (st)
	{
		SgbApuInfo a = {};
		if (S9xSGBGetApuState(&a))
		{
			st->nr50_per_frame   = a.nr50_writes - st->prev_nr50_writes;
			st->prev_nr50_writes = a.nr50_writes;
			st->silent_per_frame = a.pcm_silent - st->prev_pcm_silent;
			st->prev_pcm_silent  = a.pcm_silent;
		}
	}

	InvalidateRect(h, NULL, FALSE);
}
