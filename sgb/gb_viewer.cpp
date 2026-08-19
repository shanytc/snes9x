// Shared-memory viewer transport for instanced link sessions. See gb_viewer.h
// for the model. Windows-only; other platforms get inert stubs (the harness
// builds this file under Cygwin and never touches viewers).

#include "gb_viewer.h"
#include <string.h>
#include <stdio.h>

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {

constexpr uint32_t kMagic   = 0x47425657;  // 'GBVW'
constexpr uint32_t kFrameW  = 160;
constexpr uint32_t kFrameH  = 144;
// A viewer whose heartbeat stops (killed process) must not hold its last
// pad forever; ~2 seconds of host frames writes it off.
constexpr uint32_t kBeatTimeout = 120;

struct ViewSeat
{
	// Seqlock: master bumps to odd before writing pixels, even after.
	volatile uint32_t seq;
	volatile uint32_t pad;        // viewer -> master, SNES button mask
	volatile uint32_t beat;       // viewer bumps every pump
	volatile uint32_t attached;   // viewer sets on map, clears on clean exit
	uint16_t px[kFrameW * kFrameH];
};

struct ViewShm
{
	uint32_t magic;
	uint32_t players;
	volatile uint32_t alive;      // master holds 1 for the session's life
	ViewSeat seat[3];
};

HANDLE   g_host_map = NULL;
ViewShm *g_host     = nullptr;
int      g_host_players = 0;
uint32_t g_seen_beat[3];
uint32_t g_beat_age[3];

HANDLE   g_cli_map = NULL;
ViewShm *g_cli     = nullptr;
int      g_cli_seat = 0;         // 2..4

bool g_bg_input = false;         // the user's own preference, not the forced one

void MapName(char *out, size_t n, unsigned long pid)
{
	snprintf(out, n, "SuperSnes9x.GBView.%lu", pid);
}

// One keyboard feeds every window, so pads follow the focused instance.
bool ProcessHasFocus()
{
	HWND fg = GetForegroundWindow();
	if (!fg) return false;
	DWORD pid = 0;
	GetWindowThreadProcessId(fg, &pid);
	return pid == GetCurrentProcessId();
}

} // namespace

extern bool S9xSGBSplitCopySeatFrame(int player, uint16_t *dest);

bool S9xSGBViewerHostStart(int players)
{
	if (g_host) return true;
	if (players < 2 || players > 4) return false;

	char name[64];
	MapName(name, sizeof(name), GetCurrentProcessId());
	g_host_map = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
	                                0, sizeof(ViewShm), name);
	if (!g_host_map) return false;
	g_host = static_cast<ViewShm *>(
		MapViewOfFile(g_host_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ViewShm)));
	if (!g_host)
	{
		CloseHandle(g_host_map);
		g_host_map = NULL;
		return false;
	}
	memset(g_host, 0, sizeof(ViewShm));
	g_host->players = static_cast<uint32_t>(players);
	g_host->magic   = kMagic;
	g_host->alive   = 1;
	g_host_players  = players;
	for (int k = 0; k < 3; ++k) { g_seen_beat[k] = 0; g_beat_age[k] = 0; }
	return true;
}

void S9xSGBViewerHostStop(void)
{
	if (!g_host) return;
	g_host->alive = 0;
	UnmapViewOfFile(g_host);
	CloseHandle(g_host_map);
	g_host     = nullptr;
	g_host_map = NULL;
	g_host_players = 0;
}

bool S9xSGBViewerHostActive(void) { return g_host != nullptr; }

void S9xSGBViewerHostPush(void)
{
	if (!g_host) return;
	for (int k = 0; k < g_host_players - 1 && k < 3; ++k)
	{
		ViewSeat &s = g_host->seat[k];
		s.seq = s.seq + 1;               // odd: writer busy
		S9xSGBSplitCopySeatFrame(k + 2, const_cast<uint16_t *>(s.px));
		s.seq = s.seq + 1;               // even: frame published

		const uint32_t beat = s.beat;
		if (s.attached && beat == g_seen_beat[k])
		{
			if (++g_beat_age[k] == kBeatTimeout) s.pad = 0;
		}
		else
			g_beat_age[k] = 0;
		g_seen_beat[k] = beat;
	}
}

uint16_t S9xSGBViewerHostPad(int player)
{
	const int k = player - 2;
	if (!g_host || k < 0 || k >= 3) return 0;
	const ViewSeat &s = g_host->seat[k];
	if (!s.attached || g_beat_age[k] >= kBeatTimeout) return 0;
	return static_cast<uint16_t>(s.pad);
}

bool S9xSGBViewerHostLocalPads(void)
{
	return !g_host || g_bg_input || ProcessHasFocus();
}

void S9xSGBViewerSetBackgroundInput(bool on)
{
	g_bg_input = on;
}

int S9xSGBViewerHostViewerCount(void)
{
	if (!g_host) return 0;
	int n = 0;
	for (int k = 0; k < g_host_players - 1 && k < 3; ++k)
		if (g_host->seat[k].attached && g_beat_age[k] < kBeatTimeout) ++n;
	return n;
}

bool S9xSGBViewerClientStart(unsigned long master_pid, int seat, int players)
{
	if (g_cli) return true;
	if (seat < 2 || seat > 4) return false;
	(void)players;

	char name[64];
	MapName(name, sizeof(name), master_pid);
	g_cli_map = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
	if (!g_cli_map) return false;
	g_cli = static_cast<ViewShm *>(
		MapViewOfFile(g_cli_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ViewShm)));
	if (!g_cli || g_cli->magic != kMagic)
	{
		if (g_cli) UnmapViewOfFile(g_cli);
		CloseHandle(g_cli_map);
		g_cli     = nullptr;
		g_cli_map = NULL;
		return false;
	}
	g_cli_seat = seat;
	g_cli->seat[seat - 2].attached = 1;
	return true;
}

void S9xSGBViewerClientStop(void)
{
	if (!g_cli) return;
	g_cli->seat[g_cli_seat - 2].attached = 0;
	UnmapViewOfFile(g_cli);
	CloseHandle(g_cli_map);
	g_cli     = nullptr;
	g_cli_map = NULL;
}

bool S9xSGBViewerClientActive(void) { return g_cli != nullptr; }

bool S9xSGBViewerClientAlive(void)  { return g_cli && g_cli->alive; }

bool S9xSGBViewerClientBlit(uint16_t *dest, uint32_t pitch_pixels)
{
	if (!g_cli || !dest) return false;
	const ViewSeat &s = g_cli->seat[g_cli_seat - 2];
	if (s.seq == 0) return false;

	// Seqlock read; a torn frame is retried, a persistent writer race
	// (master mid-teardown) falls out after a few spins.
	for (int tries = 0; tries < 8; ++tries)
	{
		const uint32_t before = s.seq;
		if (before & 1) continue;
		for (uint32_t y = 0; y < kFrameH; ++y)
			memcpy(dest + y * pitch_pixels,
			       const_cast<const uint16_t *>(s.px) + y * kFrameW,
			       kFrameW * sizeof(uint16_t));
		if (s.seq == before) return true;
	}
	return false;
}

void S9xSGBViewerClientSetPad(uint16_t snes_pad_mask)
{
	if (!g_cli) return;
	ViewSeat &s = g_cli->seat[g_cli_seat - 2];
	s.pad  = (g_bg_input || ProcessHasFocus()) ? snes_pad_mask : 0;
	s.beat = s.beat + 1;
}

#else  // !_WIN32 — inert stubs for the portable builds.

bool S9xSGBViewerHostStart(int) { return false; }
void S9xSGBViewerHostStop(void) {}
bool S9xSGBViewerHostActive(void) { return false; }
void S9xSGBViewerHostPush(void) {}
uint16_t S9xSGBViewerHostPad(int) { return 0; }
bool S9xSGBViewerHostLocalPads(void) { return true; }
void S9xSGBViewerSetBackgroundInput(bool) {}
int S9xSGBViewerHostViewerCount(void) { return 0; }
bool S9xSGBViewerClientStart(unsigned long, int, int) { return false; }
void S9xSGBViewerClientStop(void) {}
bool S9xSGBViewerClientActive(void) { return false; }
bool S9xSGBViewerClientAlive(void) { return false; }
bool S9xSGBViewerClientBlit(uint16_t *, uint32_t) { return false; }
void S9xSGBViewerClientSetPad(uint16_t) {}

#endif
