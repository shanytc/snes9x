// ---------------------------------------------------------------------------
// GB/GBC regression harness — deterministic framebuffer-signature dumper.
//
// Runs a raw GB/GBC ROM headless for N frames and prints a stable signature
// (FNV-1a hash of the rendered framebuffer at every frame, plus checkpoint
// hashes for localization). Compare the signature of every ROM before and
// after a core change to catch graphics/timing regressions.
//
// The CPU/PPU/timer/APU interleave here is copied verbatim from
// Emulator::RunCycles in sgb/sgb.cpp (per-dot PPU step, CPU catch-up, the
// CGB double-speed ds_extra accumulator) so a regression seen here reflects
// the real Win32 build. Keep this loop in sync with sgb.cpp::RunCycles.
//
// Build (from repo root):
//   g++ -O2 -std=c++17 -Isgb docs/debugger/gb-headless-harness/gb_regress.cpp \
//     sgb/gb_apu.cpp sgb/gb_cart.cpp sgb/gb_cpu.cpp sgb/gb_joypad.cpp \
//     sgb/gb_mbc.cpp sgb/gb_memory.cpp sgb/gb_ops.cpp sgb/gb_ops_cb.cpp \
//     sgb/gb_ppu.cpp sgb/gb_timer.cpp -o /tmp/gb_regress
//
// Usage:  gb_regress <rom.gb|.gbc> [frames=1500]
// Output: <rom>\t<mode>\tframes=<N>\tsig=<hex>\tck=<h1,h2,h3,h4,h5>
// ---------------------------------------------------------------------------
#include "gb_cart.h"
#include "gb_cpu.h"
#include "gb_ppu.h"
#include "gb_apu.h"
#include "gb_timer.h"
#include "gb_joypad.h"
#include "gb_memory.h"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
using namespace SGB;

bool S9xSGBDebuggerBreakRequested() { return false; }
void S9xSGBOnJoyserWrite(unsigned char) {}
void S9xSGBOnPpuHBlank() {}
void S9xSGBOnPpuVBlank() {}
void S9xSGBCaptureScanline(const unsigned char*) {}

static Cart   cart;  static Memory mem; static Cpu cpu; static Ppu ppu;
static Apu    apu;   static Timer  timer; static Joypad joypad;

// Per-dot PPU model, copied from Emulator::RunCycles in sgb/sgb.cpp: the PPU
// advances one LCD dot at a time and the CPU steps only when it has
// kMaxOpcodeTCycles of headroom, so STAT/LYC/HBlank IRQs are serviced before
// the PPU starts the next mode 3. ds_extra carries the CGB double-speed CPU
// lead (stays 0 when double-speed never engages, so DMG is byte-identical).
static void RunCycles(int32_t tcycles)
{
    const int64_t target_t = ppu.t_cycles + tcycles;
    // Persistent across calls (mirrors Emulator::Impl::ds_extra/apu_ds_rem) —
    // re-deriving per call forgives CPU lag at every slice boundary, bleeding
    // ~2% CPU throughput in double-speed (Demotronic raster desync).
    static int64_t ds_extra = 0;
    static int32_t apu_rem  = 0;
    while (ppu.t_cycles < target_t)
    {
        PpuStep(ppu, mem, 1);
        if (mem.double_speed) ds_extra += 1;
        while (cpu.State().t_cycles + kMaxOpcodeTCycles <= ppu.t_cycles + ds_extra)
        {
            const int64_t pre_t = cpu.State().t_cycles;
            cpu.Step(mem);
            int32_t consumed = static_cast<int32_t>(cpu.State().t_cycles - pre_t);
            if (consumed <= 0) consumed = 4;
            TimerStep(timer, mem, consumed);
            int32_t apu_in = consumed;
            if (mem.double_speed) { apu_rem += consumed; apu_in = apu_rem >> 1; apu_rem &= 1; }
            ApuStep(apu, apu_in);
        }
    }
}

// FNV-1a 64-bit, folded into the running signature.
static inline void fnv(uint64_t &h, uint64_t v)
{
    h ^= v; h *= 1099511628211ull;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s rom [frames]\n", argv[0]); return 2; }
    const char *rom_path = argv[1];
    const int   frames   = argc > 2 ? atoi(argv[2]) : 1500;

    FILE *f = fopen(rom_path, "rb");
    if (!f) { fprintf(stderr, "open fail %s\n", rom_path); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> rom(sz);
    if (fread(rom.data(), 1, sz, f) != (size_t)sz) { return 1; } fclose(f);
    if (!CartLoad(cart, rom.data(), sz, rom_path)) { fprintf(stderr, "CartLoad fail\n"); return 1; }

    // Mode: CGB flag $80/$C0 -> CGB; otherwise DMG (exercises the GB core either way).
    const uint8_t cgb_flag = cart.header.cgb_flag;
    const bool    cgb      = (cgb_flag == 0x80 || cgb_flag == 0xC0);

    cpu.Reset(); MemReset(mem, cgb); PpuReset(ppu); ApuReset(apu);
    TimerReset(timer); JoypadReset(joypad); MbcReset(cart.mbc);
    mem.ppu = &ppu; mem.apu = &apu; mem.timer = &timer; mem.joypad = &joypad; mem.cart = &cart;
    CpuState &cs = cpu.State();
    if (cgb) { cs.r.af = 0x1180; cs.r.bc = 0x0000; cs.r.de = 0xFF56; cs.r.hl = 0x000D; }
    // else: cpu.Reset() leaves DMG post-boot register state.
    ppu.cgb = cgb;
    ApuSetClockHz(apu, 4194304);
    ApuSetOutputRate(apu, 48000);

    static int16_t buf[16384 * 2];
    uint64_t sig = 1469598103934665603ull;     // running signature over every frame
    uint64_t ck[5] = {0,0,0,0,0};              // checkpoint hashes for localization
    const int cp_at[5] = { frames/5, 2*frames/5, 3*frames/5, 4*frames/5, frames-1 };

    for (int fr = 0; fr < frames; ++fr)
    {
        JoypadSet(joypad, mem, 0);             // deterministic: no input
        ppu.frame_ready = false;
        int32_t remaining = 70224;
        while (remaining > 0 && !ppu.frame_ready) { int32_t c = remaining < 456 ? remaining : 456; RunCycles(c); remaining -= c; }
        int32_t safety = 70224;
        while (!ppu.frame_ready && safety > 0) { RunCycles(456); safety -= 456; }
        ApuDrain(apu, buf, 16384);

        // Hash the rendered frame (CGB colour buffer, or DMG shade buffer).
        uint64_t fh = 1469598103934665603ull;
        if (cgb) for (int i = 0; i < GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT; ++i) fnv(fh, ppu.color_fb[i]);
        else     for (int i = 0; i < GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT; ++i) fnv(fh, ppu.framebuffer[i]);
        fnv(sig, fh);
        for (int k = 0; k < 5; ++k) if (fr == cp_at[k]) ck[k] = fh;
    }

    // Basename for readable output.
    const char *base = rom_path; for (const char *p = rom_path; *p; ++p) if (*p=='/'||*p=='\\') base = p+1;
    printf("%s\tmode=%s\tframes=%d\tsig=%016llx\tck=%016llx,%016llx,%016llx,%016llx,%016llx\n",
           base, cgb ? "cgb" : "dmg", frames, (unsigned long long)sig,
           (unsigned long long)ck[0],(unsigned long long)ck[1],(unsigned long long)ck[2],
           (unsigned long long)ck[3],(unsigned long long)ck[4]);
    return 0;
}
