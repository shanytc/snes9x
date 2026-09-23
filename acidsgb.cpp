/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "snes9x.h"
#include "memmap.h"
#include "apu/apu.h"
#include "gfx.h"
#include "cheats.h"
#include "biosmanager.h"
#include "acidsgb.h"
#include "sgb/sgb.h"
#include "sgb/acid.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

bool8 S9xAcidSgbChild = FALSE;

namespace {

// The record stream, on a private copy of stdout: the CRT's stdout is sent to
// the null device so core messages can't land in the middle of a frame.
#ifdef _WIN32
HANDLE g_out = INVALID_HANDLE_VALUE;
#else
int g_out = -1;
#endif
bool g_out_ok = true;

void TakeStdout()
{
#ifdef _WIN32
	DuplicateHandle(GetCurrentProcess(), GetStdHandle(STD_OUTPUT_HANDLE),
	                GetCurrentProcess(), &g_out, 0, FALSE, DUPLICATE_SAME_ACCESS);
	freopen("NUL", "w", stdout);
#else
	g_out = dup(1);
	freopen("/dev/null", "w", stdout);
#endif
}

// A failed write means the parent stopped listening; the run is over.
void Emit(const void *data, size_t n)
{
	const char *p = static_cast<const char *>(data);
	while (g_out_ok && n)
	{
#ifdef _WIN32
		DWORD wrote = 0;
		if (!WriteFile(g_out, p, (DWORD) n, &wrote, NULL) || !wrote)
			g_out_ok = false;
#else
		const ssize_t wrote = write(g_out, p, n);
		if (wrote <= 0)
			g_out_ok = false;
#endif
		else
		{
			p += wrote;
			n -= (size_t) wrote;
		}
	}
}

int Fail(const std::string &msg)
{
	const uint8 tag = AcidTests::kChildError;
	const uint16 len = (uint16) msg.size();
	const uint8 hdr[3] = { tag, (uint8) (len & 0xFF), (uint8) (len >> 8) };
	Emit(hdr, sizeof hdr);
	Emit(msg.data(), len);
	return 1;
}

void OnSerial(void *, uint8_t b)
{
	const uint8 rec[2] = { AcidTests::kChildSerial, b };
	Emit(rec, sizeof rec);
}

} // anonymous

int S9xAcidSgbChildMain(int argc, char **argv)
{
	S9xAcidSgbChild = TRUE;
#ifdef _WIN32
	// A crash must end the run, not park it behind an invisible error box.
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
	TakeStdout();

	if (argc < 8 || strcmp(argv[1], AcidTests::kSgbChildFlag) != 0)
		return Fail("usage: <exe> -acidsgbchild <1|2> <rom> <bios> <boot|-> <nrx> <frames>");
	const int         model  = atoi(argv[2]) == 2 ? 2 : 1;
	const char       *rom    = argv[3];
	const char       *bios   = argv[4];
	const char       *boot   = argv[5];
	const bool        nrx    = atoi(argv[6]) != 0;
	const int         frames = atoi(argv[7]);

	// What the emulator's Game Boy Model menu and BIOS Manager set for a
	// pinned Super Game Boy; everything else stays at its zero default.
	Settings.FrameTimePAL      = 20000;
	Settings.FrameTimeNTSC     = 16667;
	Settings.SixteenBitSound   = TRUE;
	Settings.Stereo            = TRUE;
	Settings.SoundPlaybackRate = 48000;
	Settings.SoundInputRate    = 31950;
	Settings.Transparency      = TRUE;
	Settings.HDMATimingHack    = 100;
	Settings.BlockInvalidVRAMAccessMaster = TRUE;
	Settings.MaxSpriteTilesPerLine = 34;
	// Front ends set these from config; zero under ALLOW_CPU_OVERCLOCK stops the clock.
	Settings.OneClockCycle     = 6;
	Settings.OneSlowClockCycle = 8;
	Settings.TwoClockCycles    = 12;
	Settings.SuperFXClockMultiplier = 100;
	Settings.SuperFX           = TRUE;
	Settings.StopEmulation     = TRUE;
	Settings.GBBootPolicy       = (uint8) (model == 2 ? S9X_GBBOOT_SGB2 : S9X_GBBOOT_SGB);
	Settings.SGB_BIOSPreference = (uint8) model;
	Settings.GB_BIOSEnabled     = TRUE;
	Settings.GBSuppressNRxGlitches = nrx ? TRUE : FALSE;
	S9xSetBiosPath(model == 2 ? S9X_BIOS_SGB2 : S9X_BIOS_SGB1, bios);
	if (strcmp(boot, "-") != 0)
		S9xSetBiosPath(model == 2 ? S9X_BIOS_SGB2_BOOT : S9X_BIOS_SGB1_BOOT, boot);

	if (!Memory.Init() || !S9xInitAPU())
		return Fail("SNES core init failed");
	S9xInitSound(0);
	S9xSetSoundMute(TRUE);
	if (!S9xGraphicsInit())
		return Fail("SNES graphics init failed");

	if (!Memory.LoadROM(rom))
		return Fail(std::string("cannot load ") + rom);
	if (!Settings.SGB_BIOSModeActive)
		return Fail(std::string("the Super Game Boy BIOS did not take the cart: ") + bios);

	S9xDeleteCheats();
	Settings.StopEmulation = FALSE;
	S9xReset();
	SGB::Instance().SetSerialSink(&OnSerial, nullptr);

	uint8 rec[2 + AcidTests::kShotWidth * AcidTests::kShotHeight];
	rec[0] = AcidTests::kChildFrame;
	for (int i = 0; i < frames && g_out_ok; ++i)
	{
		S9xMainLoop();
		rec[1] = S9xSGBBootHandoffCaptured() ? 0 : AcidTests::kChildBooting;
		const SGB::FrameBuffer &fb = SGB::Instance().GetFrameBuffer();
		for (int k = 0; k < AcidTests::kShotWidth * AcidTests::kShotHeight; ++k)
			rec[2 + k] = fb.pixels ? (uint8) (fb.pixels[k] & 3) : 0;
		Emit(rec, sizeof rec);
	}
	return 0;
}
