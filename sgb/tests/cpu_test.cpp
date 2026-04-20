/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Standalone SGB core test harness.
//
// Drives the SGB subsystem headlessly with nothing linked from the
// snes9x host. Use to verify Blargg test suites, dmg-acid2 renderer
// output, and commercial-game boot behavior independently of the
// larger build.
//
// Build:
//   cd sgb/tests && make
//
// Run:
//   ./sgb_test rom.gb [frames=1800] [mode=sgb|sgb2|dmg] [mul=1.0]
//
// Outputs:
//   stdout       serial port bytes. Blargg ROMs write their pass/fail text here.
//   sgb_out.ppm  final GB framebuffer dumped as PPM for visual diff
//                against dmg-acid2 / commercial-game reference shots.
//   sgb_out.rgb  final 256 × 224 composite (border + GB + mask) as packed
//                BGR555 uint16 LE — useful for SGB-specific visual checks.
//   stderr       run summary + diagnostics.
//
// Exit codes:
//   0  Ran to completion.
//   1  Argument / ROM load error.
//   2  Blargg-style "Failed" detected in serial output.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

#include "../sgb.h"
#include "../sgb_state.h"
#include "../gb_memory.h"

namespace {

std::string g_serial;

void OnSerial(uint8_t b)
{
	g_serial.push_back(static_cast<char>(b));
	fputc(b, stdout);
	fflush(stdout);
}

// Output the 160x144 indexed GB framebuffer as grayscale PPM so it can
// be diffed pixel-exactly against dmg-acid2 reference images.
bool DumpFramebufferPpm(const char *path, const uint8_t *fb, int w, int h)
{
	FILE *f = fopen(path, "wb");
	if (!f) return false;

	fprintf(f, "P6\n%d %d\n255\n", w, h);
	static const uint8_t shade_rgb[4] = { 255, 170, 85, 0 };
	for (int i = 0; i < w * h; ++i)
	{
		const uint8_t s = shade_rgb[fb[i] & 3];
		fputc(s, f);
		fputc(s, f);
		fputc(s, f);
	}
	fclose(f);
	return true;
}

// Dump the full 256 × 224 composite (border + GB + mask) as raw
// BGR555 LE. Viewer/comparison is the caller's problem.
bool DumpCompositeRaw(const char *path)
{
	std::vector<uint16_t> buf(256 * 224);
	S9xSGBBlitScreen(buf.data(), 256);

	FILE *f = fopen(path, "wb");
	if (!f) return false;
	const size_t n = buf.size() * sizeof(uint16_t);
	const size_t w = fwrite(buf.data(), 1, n, f);
	fclose(f);
	return w == n;
}

uint8_t ParseRunMode(const char *s)
{
	if (!s) return 1;
	if (std::strcmp(s, "dmg")  == 0) return 0;
	if (std::strcmp(s, "sgb2") == 0) return 2;
	return 1;
}

} // anonymous

int main(int argc, char **argv)
{
	if (argc < 2)
	{
		fprintf(stderr,
			"Usage: %s <rom.gb> [frames=1800] [mode=sgb|sgb2|dmg] [mul=1.0]\n",
			argv[0]);
		return 1;
	}

	const char *rom_path   = argv[1];
	const int   frames     = (argc >= 3) ? std::atoi(argv[2])       : 1800;
	const uint8_t mode     = ParseRunMode(argc >= 4 ? argv[3] : "sgb");
	const float   mul      = (argc >= 5) ? std::strtof(argv[4], nullptr) : 1.0f;

	if (!S9xSGBInit())
	{
		fprintf(stderr, "SGB init failed\n");
		return 1;
	}
	if (!S9xSGBLoadROM(rom_path))
	{
		fprintf(stderr, "Failed to load '%s'\n", rom_path);
		return 1;
	}
	S9xSGBSetRunMode(mode);
	S9xSGBSetClockMultiplier(mul);
	S9xSGBSetAudioRate(32000);
	SGB::SetSerialCallback(&OnSerial);

	fprintf(stderr,
		"rom=%s frames=%d mode=%d mul=%.2f\n",
		rom_path, frames, mode, static_cast<double>(mul));

	for (int i = 0; i < frames; ++i)
		S9xSGBRunFrame();

	// Dumps.
	const SGB::FrameBuffer &fb = SGB::Instance().GetFrameBuffer();
	DumpFramebufferPpm("sgb_out.ppm", fb.pixels,
	                   static_cast<int>(fb.width),
	                   static_cast<int>(fb.height));
	DumpCompositeRaw  ("sgb_out.rgb");

	// Blargg pass/fail heuristic.
	int exit_code = 0;
	const bool said_passed = g_serial.find("Passed") != std::string::npos;
	const bool said_failed = g_serial.find("Failed") != std::string::npos;
	if (said_failed) exit_code = 2;

	fprintf(stderr,
		"\n--- summary ---\n"
		"frames      = %d\n"
		"serial      = %zu bytes\n"
		"passed?     = %s\n"
		"failed?     = %s\n"
		"sgb_out.ppm = %dx%d indexed\n"
		"sgb_out.rgb = 256x224 BGR555 LE\n",
		frames, g_serial.size(),
		said_passed ? "yes" : "no",
		said_failed ? "yes" : "no",
		fb.width, fb.height);

	S9xSGBDeinit();
	return exit_code;
}
