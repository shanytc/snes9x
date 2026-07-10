// ---------------------------------------------------------------------------
// Headless GB/GBC/SGB diagnostic harness for the snes9x `sgb/` core.
//
// Drives the real GB core (sgb/gb_*.cpp) with NO Windows / SNES dependencies,
// so you can reproduce and trace audio/CPU/timing bugs from the command line,
// in a loop, under a debugger, or in CI — without launching the GUI.
//
// It replicates the per-dot CPU/PPU/APU/timer interleaving that
// Emulator::RunCycles performs in sgb/sgb.cpp (interrupts run inside
// Cpu::Step, HDMA runs inside PpuStep), plus the run-mode boot register
// values from Emulator::Reset.
//
// What it surfaces:
//   * APU register-write trace  — every NRxx change with the PC that wrote it
//   * per-frame audio energy     — peak/avg of the mixed output (via ApuDrain),
//                                  so you can SEE whether a channel that is
//                                  "enabled" is actually producing sound
//   * PC histogram (final frame) — hang / busy-loop detector
//   * optional PC-range watcher  — count hits in a routine of interest
//   * run-mode boot divergence   — cgb vs sgb2 vs sgb vs dmg in one binary
//
// See README.md in this directory for the build command and worked examples
// (this is the tool used to find the Telefang digitized-voice duty bug).
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
#include <string>
#include <vector>
#include <map>
#include <algorithm>

using namespace SGB;

// --- Host hooks the GB core references; no-ops for the headless harness. -----
bool S9xSGBDebuggerBreakRequested() { return false; }
void S9xSGBOnPpuHBlank() {}
void S9xSGBOnPpuVBlank() {}
struct SLRec { int ly, scx, scy, stall, lcdc; };
static int g_slframe = -1;              // slframe=N : dump per-scanline scroll for frame N
static std::vector<SLRec> g_slrec;
void S9xSGBCaptureScanline(const unsigned char*);   // real body after `ppu` is declared

// --- Subsystems (same set Emulator::Impl owns). -----------------------------
static Cart   cart;
static Memory mem;
static Cpu    cpu;
static Ppu    ppu;
static Apu    apu;
static Timer  timer;
static Joypad joypad;

long g_sgb_resets = 0, g_sgb_rotations = 0;
std::vector<std::pair<int,int>> g_sgb_cmds;   // (frame, command id)
extern int g_cur_frame;
void S9xSGBOnJoyserWrite(unsigned char value)
{
    if (!joypad.sgb_active) return;
    static uint8_t prev = 0xFF;
    static bool    in_packet = false;
    static int     bits = 0;
    static uint8_t b0 = 0;
    const uint8_t sel = value & 0x30;
    if (sel == 0x00) { in_packet = true; bits = 0; b0 = 0; prev = value; ++g_sgb_resets; return; }
    if (in_packet)
    {
        if (sel == 0x10 || sel == 0x20)
        {
            if (bits < 8 && sel == 0x10) b0 |= static_cast<uint8_t>(1u << bits);
            if (bits == 7) g_sgb_cmds.push_back({g_cur_frame, b0 >> 3});
            if (++bits >= 128) in_packet = false;
        }
        prev = value;
        return;
    }
    if (!(prev & 0x20) && (value & 0x20))
    {
        joypad.sgb_index = static_cast<uint8_t>((joypad.sgb_index + 1) & 0x03);
        ++g_sgb_rotations;
    }
    prev = value;
}

// Per-scanline scroll recorder + SGB-ring model. CaptureScanline runs
// synchronously inside FinalizeScanline (mode 3 -> HBlank) — and, with the
// BGP back-spill re-capture, again from PpuWriteReg — so g_ring_fb holds
// exactly what the SGB BIOS's ICD2 lcd_ring would display (fbb= dumps it;
// diff vs fb= exposes corrections that miss the ring). slframe= records
// per-scanline scroll for one frame.
static uint8_t g_ring_fb[160 * 144];
void S9xSGBCaptureScanline(const unsigned char *pixels)
{
    if (ppu.ly < GB_SCREEN_HEIGHT)
        memcpy(&g_ring_fb[ppu.ly * 160], pixels, 160);
    if (g_cur_frame == g_slframe && ppu.ly < GB_SCREEN_HEIGHT)
        g_slrec.push_back({ (int)ppu.ly, (int)ppu.scx, (int)ppu.scy,
                            (int)ppu.mode3_sprite_stall, (int)ppu.lcdc });
}

static void DumpRing(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); return; }
    fprintf(f, "P6\n160 144\n255\n");
    for (int i = 0; i < 160 * 144; ++i)
    {
        unsigned char s = (unsigned char)((3 - (g_ring_fb[i] & 3)) * 85);
        unsigned char rgb[3] = { s, s, s };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("RingFB dumped -> %s\n", path);
}

// --- CPU trace hook state ---------------------------------------------------
static uint64_t g_instr = 0;
static std::map<uint16_t,uint64_t> g_pc_hist;     // sampled on the final frame
static bool     g_hist_on = false;
static uint16_t g_watch_lo = 0xFFFF, g_watch_hi = 0;  // optional PC-range watch
static uint64_t g_watch_hits = 0;

static int g_bgpw_frame = -1;     // bgpw=F: trace BGP writes + IRQ entries + HALTs that frame

// pctrace=F: per-instruction PC/reg/bank dump for frames F..F+1 —
// diff vs a SameBoy exec-callback trace to pin interrupt-timing divergences.
int g_pctrace_frame = -1;
static void Trace(uint16_t pc, uint8_t op, const CpuState &state)
{
    ++g_instr;
    if (g_hist_on) g_pc_hist[pc]++;
    if (pc >= g_watch_lo && pc <= g_watch_hi) ++g_watch_hits;
    if (g_pctrace_frame >= 0 && g_cur_frame >= g_pctrace_frame && g_cur_frame <= g_pctrace_frame + 1)
        fprintf(stderr, "P %04X a=%02X bc=%04X de=%04X hl=%04X | f%d ly=%d m=%d bank=%d\n",
                pc, state.r.a, state.r.bc, state.r.de, state.r.hl, g_cur_frame,
                (int)ppu.ly, (int)ppu.mode, mem.cart ? (int)mem.cart->mbc.rom_bank : -1);
    if (g_cur_frame == g_bgpw_frame)
    {
        if (pc == 0x40 || pc == 0x48 || pc == 0x50 || pc == 0x58)
            printf("IRQVEC %02X ly=%3d mode=%d mclk=%3d\n",
                   pc, ppu.ly, (int)ppu.mode, ppu.mode_clock);
        if (op == 0x76)
            printf("HALT   @%04X ly=%3d mode=%d mclk=%3d\n",
                   pc, ppu.ly, (int)ppu.mode, ppu.mode_clock);
    }
}

// --- Scripted input + framebuffer dumps --------------------------------------
// press=10:START,300:D,360:A   hold each button combo (B+A ok) for 8 frames
// fb=300:/tmp/f300.ppm         dump the screen as PPM after that frame runs
struct Press { int frame; uint8_t mask; };
static std::vector<Press> g_press;
static std::vector<std::pair<int,std::string>> g_fbdump;
static std::vector<std::pair<int,std::string>> g_fbldump;
static std::vector<std::pair<int,std::string>> g_fbbdump;   // fbb=F:path — SGB-ring view
static std::vector<std::pair<int,std::string>> g_bgmapdump; // bgmap=F:path — full 256x256 BG plane
static std::vector<std::pair<int,std::string>> g_winmapdump;// winmap=F:path — full 256x256 window plane
static std::string g_wav_path;                 // wav=/tmp/out.wav
static std::vector<int16_t> g_wav;             // accumulated stereo samples
static uint8_t g_mute51_keep = 0xFF;           // mute=124 -> clear those CHx bits in NR51
static int g_trig3_lo = -1, g_trig3_hi = -1;   // trig3=LO:HI -> dump CH3 trigger state
static int g_apuw_lo = 0, g_apuw_hi = 0x7fffffff; // apuw=LO:HI -> log APU writes only in window
int g_cur_frame = 0;
static double g_hostpace = 0.0;                // hostpace=60.0988 -> drain like the
                                               // win32 host: 48000/HZ frames per GB
                                               // frame, surplus pins the ring (drops)
static bool   g_drc = false;                   // drc -> mirror sgb.cpp's rate control
static double g_drc_integ = 0.0;
static double g_drc_corr  = 0.0;

static uint8_t BtnMask(const std::string &n)
{
    if (n=="A") return GB_A;         if (n=="B") return GB_B;
    if (n=="START") return GB_START; if (n=="SELECT") return GB_SELECT;
    if (n=="U") return GB_UP;        if (n=="D") return GB_DOWN;
    if (n=="L") return GB_LEFT;      if (n=="R") return GB_RIGHT;
    fprintf(stderr, "unknown button '%s'\n", n.c_str());
    exit(2);
}

static void ParsePress(const char *s)
{
    for (const char *p = s; *p; )
    {
        int fr = atoi(p);
        const char *colon = strchr(p, ':');
        if (!colon) break;
        const char *end = strchr(colon, ',');
        std::string btns(colon + 1, end ? end - colon - 1 : strlen(colon + 1));
        uint8_t mask = 0;
        size_t pos = 0;
        while (pos != std::string::npos)
        {
            size_t plus = btns.find('+', pos);
            mask |= BtnMask(btns.substr(pos, plus == std::string::npos ? plus : plus - pos));
            pos = plus == std::string::npos ? plus : plus + 1;
        }
        g_press.push_back({fr, mask});
        p = end ? end + 1 : "";
    }
}

static void ParseFbDump(const char *s, std::vector<std::pair<int,std::string>> &out)
{
    for (const char *p = s; *p; )
    {
        int fr = atoi(p);
        const char *colon = strchr(p, ':');
        if (!colon) break;
        const char *end = strchr(colon, ',');
        out.push_back({fr, std::string(colon + 1, end ? end - colon - 1 : strlen(colon + 1))});
        p = end ? end + 1 : "";
    }
}

static void WriteWav(const char *path, const std::vector<int16_t> &pcm, uint32_t rate)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); return; }
    const uint32_t data_bytes = (uint32_t)(pcm.size() * sizeof(int16_t));
    const uint32_t riff = 36 + data_bytes, byte_rate = rate * 4;
    const uint16_t fmt = 1, ch = 2, align = 4, bits = 16;
    fwrite("RIFF",1,4,f); fwrite(&riff,4,1,f); fwrite("WAVEfmt ",1,8,f);
    const uint32_t fmt_size = 16;
    fwrite(&fmt_size,4,1,f); fwrite(&fmt,2,1,f); fwrite(&ch,2,1,f);
    fwrite(&rate,4,1,f); fwrite(&byte_rate,4,1,f); fwrite(&align,2,1,f); fwrite(&bits,2,1,f);
    fwrite("data",1,4,f); fwrite(&data_bytes,4,1,f);
    fwrite(pcm.data(),1,data_bytes,f);
    fclose(f);
    printf("WAV dumped -> %s (%u samples)\n", path, (unsigned)(pcm.size()/2));
}

static void DumpLayer(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); return; }
    fprintf(f, "P5\n%d %d\n255\n", GB_SCREEN_WIDTH, GB_SCREEN_HEIGHT);
    for (int i = 0; i < GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT; ++i)
        fputc(ppu.layer[i] * 100, f);
    fclose(f);
    printf("Layer dumped -> %s\n", path);
}

static void DumpFb(const char *path, bool cgb)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); return; }
    fprintf(f, "P6\n%d %d\n255\n", GB_SCREEN_WIDTH, GB_SCREEN_HEIGHT);
    for (int i = 0; i < GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT; ++i)
    {
        uint8_t rgb[3];
        if (cgb)
        {
            const uint16_t v = ppu.color_fb[i];
            rgb[0] = (uint8_t)(((v      ) & 31) << 3);
            rgb[1] = (uint8_t)(((v >>  5) & 31) << 3);
            rgb[2] = (uint8_t)(((v >> 10) & 31) << 3);
        }
        else
        {
            const uint8_t shade = (uint8_t)((3 - (ppu.framebuffer[i] & 3)) * 85);
            rgb[0] = rgb[1] = rgb[2] = shade;
        }
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("FB dumped -> %s\n", path);
}

// Dump the full 256x256 BG (or window) tilemap plane so off-screen tiles are
// visible in context. Marks the on-screen viewport with a mid-gray box.
static void DumpPlane(const char *path, bool window)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); return; }
    const uint16_t map_base = window ? ((ppu.lcdc & 0x40) ? 0x1C00 : 0x1800)
                                     : ((ppu.lcdc & 0x08) ? 0x1C00 : 0x1800);
    const bool un = (ppu.lcdc & 0x10) != 0;
    static uint8_t plane[256 * 256];
    for (int py = 0; py < 256; ++py)
        for (int px = 0; px < 256; ++px)
        {
            const int tr = py >> 3, fy = py & 7, tc = px >> 3, fx = px & 7;
            const uint8_t n = ppu.vram[map_base + tr * 32 + tc];
            uint16_t ta = un ? (uint16_t)(n * 16) : (uint16_t)(0x1000 + (int8_t)n * 16);
            ta += fy * 2;
            const uint8_t lo = ppu.vram[ta], hi = ppu.vram[ta + 1];
            const uint8_t bit = 7 - fx;
            const uint8_t ci = (uint8_t)((((hi >> bit) & 1) << 1) | ((lo >> bit) & 1));
            plane[py * 256 + px] = (uint8_t)(ppu.bgp >> (ci * 2)) & 3;
        }
    fprintf(f, "P6\n256 256\n255\n");
    for (int py = 0; py < 256; ++py)
        for (int px = 0; px < 256; ++px)
        {
            uint8_t s = (uint8_t)((3 - plane[py * 256 + px]) * 85);
            // Viewport outline: BG uses scx/scy; window origin is (0,0) mapped to (wx-7, wy).
            bool edge = false;
            if (!window)
            {
                int vx = (px - ppu.scx) & 255, vy = (py - ppu.scy) & 255;
                edge = ((vx == 0 || vx == 159) && vy < 144) || ((vy == 0 || vy == 143) && vx < 160);
            }
            uint8_t rgb[3] = { s, s, s };
            if (edge) { rgb[0] = 255; rgb[1] = 0; rgb[2] = 0; }
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
    printf("Plane(%s) dumped -> %s  (map_base=%04X scx=%d scy=%d wx=%d wy=%d lcdc=%02X)\n",
           window ? "WIN" : "BG", path, map_base, ppu.scx, ppu.scy, ppu.wx, ppu.wy, ppu.lcdc);
    if (!window)
    {
        printf("  BG tilemap indices rows 0..11, cols 24..31:\n");
        for (int tr = 0; tr < 12; ++tr)
        {
            printf("   r%2d:", tr);
            for (int tc = 24; tc < 32; ++tc)
                printf(" %02X", ppu.vram[map_base + tr * 32 + tc]);
            printf("\n");
        }
    }
}

// --- APU register shadow, for change detection ------------------------------
struct ApuShadow {
    uint8_t nr10,nr11,nr12,nr13,nr14, nr21,nr22,nr23,nr24;
    uint8_t nr30,nr31,nr32,nr33,nr34, nr41,nr42,nr43,nr44;
    uint8_t nr50,nr51,nr52;
};
static ApuShadow prev{};
static long g_apu_writes_logged = 0;
static const long APU_WRITE_LOG_CAP = 400;

static ApuShadow Snap()
{
    ApuShadow s;
    s.nr10=apu.ch1.nrx0; s.nr11=apu.ch1.nrx1; s.nr12=apu.ch1.nrx2; s.nr13=apu.ch1.nrx3; s.nr14=apu.ch1.nrx4;
    s.nr21=apu.ch2.nrx1; s.nr22=apu.ch2.nrx2; s.nr23=apu.ch2.nrx3; s.nr24=apu.ch2.nrx4;
    s.nr30=apu.ch3.nr30; s.nr31=apu.ch3.nr31; s.nr32=apu.ch3.nr32; s.nr33=apu.ch3.nr33; s.nr34=apu.ch3.nr34;
    s.nr41=apu.ch4.nr41; s.nr42=apu.ch4.nr42; s.nr43=apu.ch4.nr43; s.nr44=apu.ch4.nr44;
    s.nr50=apu.nr50; s.nr51=apu.nr51; s.nr52=apu.master_enabled?0x80:0;
    return s;
}

// --- One Emulator::RunCycles slice (per-dot interleave, double-speed aware) --
static void RunCycles(int32_t tcycles, bool trace_apu)
{
    CpuState &cs = cpu.State();
    int64_t target_t = ppu.t_cycles + tcycles;
    // Persistent double-speed CPU budget ledger, mirroring Emulator::Impl::
    // ds_extra / apu_ds_rem — re-deriving per call forgives CPU lag at every
    // slice boundary and desyncs cycle-counted raster loops (Demotronic).
    static int64_t ds_extra_acc = -1;
    static int32_t apu_rem_acc  = 0;
    if (ds_extra_acc < 0)
    {
        ds_extra_acc = cs.t_cycles - ppu.t_cycles;
        if (ds_extra_acc < 0) ds_extra_acc = 0;
    }
    int64_t &ds_extra = ds_extra_acc;
    int32_t &apu_rem  = apu_rem_acc;

    while (ppu.t_cycles < target_t)
    {
        PpuStep(ppu, mem, 1);                       // HDMA-on-HBlank lives here
        if (mem.double_speed) ds_extra += 1;

        while (cs.t_cycles + kMaxOpcodeTCycles <= ppu.t_cycles + ds_extra)
        {
            uint16_t prePC = cs.r.pc;
            int64_t  pre_t = cs.t_cycles;
            uint8_t  pre_pos3 = apu.ch3.pos;
            uint8_t  pre_bgp  = ppu.bgp;
            cpu.Step(mem);                           // interrupts serviced inside
            int32_t consumed = (int32_t)(cs.t_cycles - pre_t);
            if (consumed <= 0) consumed = 4;

            if (g_cur_frame == g_bgpw_frame && ppu.bgp != pre_bgp)
            {
                // arrival line-dot (mode2=0.., mode3=80.., mode0=252.. for stall=0)
                int arr = ppu.mode_clock;
                if      (ppu.mode == PpuMode::Transfer) arr += 80;
                else if (ppu.mode == PpuMode::HBlank)   arr += 80 + 172 + ppu.mode3_sprite_stall;
                const long long lag = (long long)(ppu.t_cycles - (pre_t + 4));
                printf("BGPW pc=%04X %02X->%02X ly=%3d mode=%d mclk=%3d arr=%3d lag=%2lld true=%3lld\n",
                       prePC, pre_bgp, ppu.bgp, ppu.ly, (int)ppu.mode, ppu.mode_clock,
                       arr, lag, (long long)(arr - lag));
                static std::map<uint16_t,bool> dumped;
                if (!dumped[prePC] && dumped.size() < 4)
                {
                    dumped[prePC] = true;
                    printf("  ROM around %04X:", prePC);
                    for (int i = -8; i < 40; ++i)
                        printf(" %02X", MemRead(mem, (uint16_t)(prePC + i)));
                    printf("\n");
                }
            }

            TimerStep(timer, mem, consumed);         // CPU clock domain (DIV doubles)
            int32_t apu_in = consumed;               // APU stays real-time
            if (mem.double_speed) { apu_rem += consumed; apu_in = apu_rem >> 1; apu_rem &= 1; }
            if (g_mute51_keep != 0xFF) apu.nr51 &= g_mute51_keep;
            ApuStep(apu, apu_in);

            static uint32_t last_tg3 = 0;
            static uint8_t  last_nr30 = 0;
            if (g_cur_frame >= g_trig3_lo && g_cur_frame < g_trig3_hi)
            {
                if (apu.ch3.nr30 != last_nr30)
                    printf("NR30 %02X->%02X t=%lld pos=%2d ftimer=%d\n",
                        last_nr30, apu.ch3.nr30, (long long)cs.t_cycles, apu.ch3.pos, apu.ch3.freq_timer);
                if (apu.dbg_trigger_count[2] != last_tg3)
                {
                    printf("TRIG3 #%u f%d t=%lld pc=%04X freq=%03X prepos=%2d buf=%X ram=",
                        apu.dbg_trigger_count[2], g_cur_frame, (long long)cs.t_cycles, prePC,
                        apu.ch3.freq, pre_pos3, apu.ch3.sample_buf);
                    for (int i = 0; i < 16; ++i) printf("%02X", apu.ch3.ram[i]);
                    printf("\n");
                }
            }
            last_nr30 = apu.ch3.nr30;
            last_tg3  = apu.dbg_trigger_count[2];

            if (trace_apu && g_apu_writes_logged < APU_WRITE_LOG_CAP
                && g_cur_frame >= g_apuw_lo && g_cur_frame < g_apuw_hi)
            {
                ApuShadow now = Snap();
                if (memcmp(&now, &prev, sizeof now) != 0)
                {
                    #define D(field,name) if(now.field!=prev.field){ if(g_apu_writes_logged<APU_WRITE_LOG_CAP){ \
                        printf("    APUW f%d pc=%04X %-4s %02X->%02X\n",g_cur_frame,prePC,name,prev.field,now.field); ++g_apu_writes_logged; } }
                    D(nr10,"NR10")D(nr11,"NR11")D(nr12,"NR12")D(nr13,"NR13")D(nr14,"NR14")
                    D(nr21,"NR21")D(nr22,"NR22")D(nr23,"NR23")D(nr24,"NR24")
                    D(nr30,"NR30")D(nr31,"NR31")D(nr32,"NR32")D(nr33,"NR33")D(nr34,"NR34")
                    D(nr41,"NR41")D(nr42,"NR42")D(nr43,"NR43")D(nr44,"NR44")
                    D(nr50,"NR50")D(nr51,"NR51")D(nr52,"NR52")
                    #undef D
                    prev = now;
                }
            }
            else
            {
                prev = Snap();
            }
        }
    }
}

static void Usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s <rom.gb|.gbc> [frames=1500] [mode=cgb|sgb2|sgb|dmg]\n"
        "          [input|noinput] [watchLoHex watchHiHex]\n"
        "  rom      raw GB/GBC ROM (unzip first)\n"
        "  frames   SNES-frames to run (~60/sec)\n"
        "  mode     boot register state: cgb (A=11), sgb2 (A=FF), sgb (A=01), dmg\n"
        "  input    pulse START then A every 120 frames (advances menus);\n"
        "           noinput leaves the pad idle (default)\n"
        "  watch    optional PC range to count hits in (e.g. 38F2 3925)\n"
        "  press=F:BTN[+BTN],...   scripted input: hold combo 8 frames at frame F\n"
        "           (A B START SELECT U D L R); overrides input/noinput\n"
        "  fb=F:path.ppm,...       dump screen as PPM after frame F\n"
        "  fbl=F:path.pgm,...      dump per-pixel layer map (0=BG 100=WIN 200=OBJ) after frame F\n"
        "  wav=path.wav            dump mixed audio (48kHz stereo S16) to a WAV\n"
        "  mute=NN..               silence channels 1-4 in NR51 (e.g. mute=124 solos CH3)\n"
        "  trig3=LO:HI             print CH3 state + wave RAM at each trigger in frames [LO,HI)\n"
        "  apuw=LO:HI              log frame-tagged APU register writes + per-frame CH3 state in [LO,HI)\n"
        "  hostpace=FPS            drain only 48000/FPS samples per frame like the real host\n"
        "                          (surplus pins the ring and drops, shortfall starves)\n"
        "  drc                     enable the sgb.cpp dynamic-rate-control mirror\n", argv0);
}

int main(int argc, char **argv)
{
    if (argc < 2) { Usage(argv[0]); return 2; }
    const char *rom_path = argv[1];
    int         frames   = argc>2 ? atoi(argv[2]) : 1500;
    const char *mode     = argc>3 ? argv[3] : "cgb";
    bool        noinput  = !(argc>4 && !strcmp(argv[4],"input"));
    int         softreset_frame = -1;
    if (argc>6) { g_watch_lo=(uint16_t)strtol(argv[5],0,16); g_watch_hi=(uint16_t)strtol(argv[6],0,16); }
    for (int i = 2; i < argc; ++i)
    {
        if (!strncmp(argv[i], "softreset=", 10)) softreset_frame = atoi(argv[i] + 10);
        if (!strncmp(argv[i], "slframe=", 8)) g_slframe = atoi(argv[i] + 8);
        if (!strncmp(argv[i], "bgpw=", 5)) g_bgpw_frame = atoi(argv[i] + 5);
        if (!strncmp(argv[i], "fbb=",   4)) ParseFbDump(argv[i] + 4, g_fbbdump);
        if (!strncmp(argv[i], "press=", 6)) ParsePress(argv[i] + 6);
        if (!strncmp(argv[i], "fb=",    3)) ParseFbDump(argv[i] + 3, g_fbdump);
        if (!strncmp(argv[i], "fbl=",   4)) ParseFbDump(argv[i] + 4, g_fbldump);
        if (!strncmp(argv[i], "pctrace=", 8)) g_pctrace_frame = atoi(argv[i]+8);
        if (!strncmp(argv[i], "bgmap=", 6)) ParseFbDump(argv[i] + 6, g_bgmapdump);
        if (!strncmp(argv[i], "winmap=",7)) ParseFbDump(argv[i] + 7, g_winmapdump);
        if (!strncmp(argv[i], "wav=",   4)) g_wav_path = argv[i] + 4;
        if (!strncmp(argv[i], "trig3=", 6)) sscanf(argv[i] + 6, "%d:%d", &g_trig3_lo, &g_trig3_hi);
        if (!strncmp(argv[i], "apuw=",  5)) sscanf(argv[i] + 5, "%d:%d", &g_apuw_lo, &g_apuw_hi);
        if (!strncmp(argv[i], "hostpace=", 9)) g_hostpace = atof(argv[i] + 9);
        if (!strcmp(argv[i], "drc")) g_drc = true;
        if (!strncmp(argv[i], "mute=",  5))
            for (const char *m = argv[i] + 5; *m; ++m)
                if (*m >= '1' && *m <= '4')
                {
                    const int ch = *m - '1';
                    g_mute51_keep &= (uint8_t)~((1u << ch) | (1u << (ch + 4)));
                }
    }

    FILE *f = fopen(rom_path,"rb");
    if (!f) { fprintf(stderr,"cannot open %s\n", rom_path); return 1; }
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    std::vector<uint8_t> rom(sz);
    if (fread(rom.data(),1,sz,f)!=(size_t)sz) { fprintf(stderr,"short read\n"); return 1; }
    fclose(f);

    if (!CartLoad(cart, rom.data(), sz, rom_path)) { fprintf(stderr,"CartLoad failed\n"); return 1; }
    printf("Loaded %s: cartType=0x%02X cgbFlag=0x%02X rom=%uB ram=%uB mbc=%d\n",
        rom_path, cart.header.cart_type, cart.header.cgb_flag,
        cart.header.rom_size, cart.header.ram_size, (int)cart.mbc.type);

    // Replicate Emulator::Reset() for a standalone cart (no boot ROM, no SGB BIOS).
    cpu.Reset(); MemReset(mem, !strcmp(mode,"cgb")); PpuReset(ppu); ApuReset(apu, !strcmp(mode,"cgb"), true);
    TimerReset(timer); JoypadReset(joypad); MbcReset(cart.mbc);
    mem.ppu=&ppu; mem.apu=&apu; mem.timer=&timer; mem.joypad=&joypad; mem.cart=&cart; mem.cpu = &cpu.State();

    bool cgb=false; int clk=4194304;
    CpuState &cs = cpu.State();
    if      (!strcmp(mode,"cgb"))  { cgb=true; cs.r.af=0x1180; cs.r.bc=0x0000; cs.r.de=0xFF56; cs.r.hl=0x000D; }
    else if (!strcmp(mode,"sgb2")) { cs.r.af=0xFF00; cs.r.bc=0x0014; cs.r.de=0x0000; cs.r.hl=0xC060; }
    else if (!strcmp(mode,"sgb"))  { cs.r.af=0x0100; cs.r.bc=0x0014; cs.r.de=0x0000; cs.r.hl=0xC060; clk=4295454; }
    else                           { /* dmg: keep cpu.Reset() DMG defaults */ }
    ppu.cgb = cgb;
    joypad.sgb_active = (!strcmp(mode,"sgb") || !strcmp(mode,"sgb2"));
    ApuSetClockHz(apu, clk);
    int g_orate = getenv("SGB_ORATE") ? atoi(getenv("SGB_ORATE")) : 48000;
    ApuSetOutputRate(apu, g_orate);
    Cpu::SetTraceHook(&Trace);

    auto SoftReset = [&]() {
        cpu.Reset(); MemReset(mem, cgb); PpuReset(ppu); ApuReset(apu, cgb, true);
        TimerReset(timer); JoypadReset(joypad); MbcReset(cart.mbc);
        mem.ppu=&ppu; mem.apu=&apu; mem.timer=&timer; mem.joypad=&joypad; mem.cart=&cart; mem.cpu = &cpu.State();
        CpuState &c = cpu.State();
        if      (!strcmp(mode,"cgb"))  { c.r.af=0x1180; c.r.bc=0x0000; c.r.de=0xFF56; c.r.hl=0x000D; }
        else if (!strcmp(mode,"sgb2")) { c.r.af=0xFF00; c.r.bc=0x0014; c.r.de=0x0000; c.r.hl=0xC060; }
        else if (!strcmp(mode,"sgb"))  { c.r.af=0x0100; c.r.bc=0x0014; c.r.de=0x0000; c.r.hl=0xC060; }
        ppu.cgb = cgb;
        ApuSetClockHz(apu, clk);
    };
    printf("MODE=%s cgb=%d clk=%d af=%04X frames=%d input=%s watch=%04X-%04X\n\n",
        mode, cgb, clk, cs.r.af, frames, noinput?"no":"yes", g_watch_lo, g_watch_hi);

    // Stats
    int first_ch[4] = {-1,-1,-1,-1};        // first frame each channel triggers
    int first_freq[2] = {-1,-1};            // first frame CH1/CH2 freq != 0
    int ds_frame=-1, stop_frame=-1;
    int64_t total_samples=0; int global_peak=0, frames_with_audio=0;
    uint64_t watch_prev=0;
    int ring_full_frames=0; double drain_carry=0.0;
    prev = Snap();
    static int16_t buf[16384*2];

    for (int fr=0; fr<frames; ++fr)
    {
        g_cur_frame = fr;
        if (fr == softreset_frame) { printf("[softreset @ frame %d]\n", fr); SoftReset(); }
        uint8_t mask = 0;
        if (!g_press.empty())
        {
            for (const Press &pr : g_press)
                if (fr >= pr.frame && fr < pr.frame + 8) mask |= pr.mask;
        }
        else if (!noinput) { int p=fr%120; if(p>=10&&p<16) mask=GB_START; else if(p>=40&&p<46) mask=GB_A; }
        JoypadSet(joypad, mem, mask);

        ppu.frame_ready=false;
        uint64_t instr_before=g_instr;
        g_hist_on = (fr==frames-1);

        int32_t remaining=70224;            // ~one frame of GB T-cycles
        while (remaining>0 && !ppu.frame_ready) { int32_t c=remaining<456?remaining:456; RunCycles(c,true); remaining-=c; }
        int32_t safety=70224;
        while (!ppu.frame_ready && safety>0) { RunCycles(456,false); safety-=456; }

        for (const auto &d : g_fbdump)
            if (d.first == fr) DumpFb(d.second.c_str(), ppu.cgb);
        for (const auto &d : g_fbldump)
            if (d.first == fr) DumpLayer(d.second.c_str());
        for (const auto &d : g_fbbdump)
            if (d.first == fr) DumpRing(d.second.c_str());
        for (const auto &d : g_bgmapdump)
            if (d.first == fr) DumpPlane(d.second.c_str(), false);
        for (const auto &d : g_winmapdump)
            if (d.first == fr) DumpPlane(d.second.c_str(), true);

        if (g_slframe == fr)
        {
            const uint16_t base = (ppu.lcdc & 0x08) ? 0x1C00 : 0x1800;
            printf("TILEMAP@%d lcdc=%02X bgp=%02X (rows6-15, cols0-9):\n", fr, ppu.lcdc, ppu.bgp);
            const bool un = (ppu.lcdc & 0x10) != 0;
            for (int tr = 6; tr < 16; ++tr)
            {
                printf(" tr%2d:", tr);
                for (int tc = 0; tc < 10; ++tc) printf(" %02X", ppu.vram[base + tr * 32 + tc]);
                // col-0 tile's 16 bytes of data
                const uint8_t n = ppu.vram[base + tr * 32 + 0];
                const uint16_t ta = un ? (uint16_t)(n * 16) : (uint16_t)(0x1000 + (int8_t)n * 16);
                printf("   | col0 tile %02X data:", n);
                for (int b = 0; b < 16; ++b) printf(" %02X", ppu.vram[ta + b]);
                printf("\n");
            }
        }

        // Audio energy this frame.
        {
            const uint32_t head=apu.sample_head, tail=apu.sample_tail;
            const uint32_t fill=(head>=tail)?(head-tail):(APU_SAMPLE_BUF_SIZE-tail+head);
            if (fill >= APU_SAMPLE_BUF_SIZE-2) ring_full_frames++;
            if (g_drc)
            {
                const double err = (double)fill / APU_SAMPLE_BUF_SIZE - 0.125;
                g_drc_integ += 5e-5 * err;
                if (g_drc_integ >  0.03) g_drc_integ =  0.03;
                if (g_drc_integ < -0.03) g_drc_integ = -0.03;
                g_drc_corr = 0.02 * err + g_drc_integ;
                if (g_drc_corr >  0.03) g_drc_corr =  0.03;
                if (g_drc_corr < -0.03) g_drc_corr = -0.03;
                ApuSetClockHz(apu, (int32_t)((double)clk * (1.0 + g_drc_corr) + 0.5));
            }
        }
        int32_t want = 16384;
        if (g_hostpace > 0.0)
        {
            drain_carry += 48000.0 / g_hostpace;
            want = (int32_t)drain_carry;
            drain_carry -= want;
        }
        int32_t got=ApuDrain(apu,buf,want); total_samples+=got;
        if (!g_wav_path.empty()) g_wav.insert(g_wav.end(), buf, buf + got*2);
        int32_t peak=0; int64_t energy=0;
        for(int i=0;i<got*2;i++){ int v=buf[i]; if(v<0)v=-v; if(v>peak)peak=v; energy+=v; }
        if(peak>global_peak) global_peak=peak;
        if(peak>0) frames_with_audio++;

        uint64_t watch_this=g_watch_hits-watch_prev; watch_prev=g_watch_hits;

        if (mem.double_speed && ds_frame<0) ds_frame=fr;
        if (cs.stopped && stop_frame<0) stop_frame=fr;
        for(int i=0;i<4;i++) if(apu.dbg_trigger_count[i]>0 && first_ch[i]<0) first_ch[i]=fr;
        if(apu.ch1.freq && first_freq[0]<0) first_freq[0]=fr;
        if(apu.ch2.freq && first_freq[1]<0) first_freq[1]=fr;

        bool in_apuw = (fr >= g_apuw_lo && fr < g_apuw_hi && g_apuw_hi != 0x7fffffff);
        if (in_apuw)
            printf("[f%5d] CH3 en=%d dac=%d len=%u lenEn=%d nr30=%02X nr32=%02X nr34=%02X freq=%03X pos=%2d tg3=%u | peak=%d\n",
                fr, apu.ch3.enabled, apu.ch3.dac_enabled, apu.ch3.length, apu.ch3.length_enabled,
                apu.ch3.nr30, apu.ch3.nr32, apu.ch3.nr34, apu.ch3.freq, apu.ch3.pos,
                apu.dbg_trigger_count[2], peak);
        bool notable = (peak>40) || (watch_this>0);
        if (fr<6 || fr%120==0 || fr==frames-1 || notable)
            printf("[f%5d] pc=%04X instr=%llu DS=%d stop=%d halt=%d ime=%d watch=%llu | "
                   "M=%d audPeak=%d audAvg=%lld | CH1(en%d f%03X v%2d) CH2(en%d f%03X v%2d) "
                   "CH3(en%d) CH4(en%d) tg=%u/%u/%u/%u\n",
                fr, cs.r.pc, (unsigned long long)(g_instr-instr_before),
                mem.double_speed, cs.stopped, cs.halted, cs.ime, (unsigned long long)watch_this,
                apu.master_enabled, peak, (long long)(got?energy/(got*2):0),
                apu.ch1.enabled,apu.ch1.freq,apu.ch1.env_volume,
                apu.ch2.enabled,apu.ch2.freq,apu.ch2.env_volume,
                apu.ch3.enabled,apu.ch4.enabled,
                apu.dbg_trigger_count[0],apu.dbg_trigger_count[1],apu.dbg_trigger_count[2],apu.dbg_trigger_count[3]);
    }

    if (!g_wav_path.empty()) WriteWav(g_wav_path.c_str(), g_wav, g_orate);

    if (g_slframe >= 0)
    {
        printf("\n=== per-scanline scroll @ frame %d  (stall = sprites*? + SCX&7) ===\n", g_slframe);
        int prev_s7 = -1, runs = 0;
        for (const SLRec &r : g_slrec)
        {
            printf("  ly%3d  scx=%3d (scx&7=%d)  scy=%3d  stall=%d  lcdc=%02X %s\n",
                   r.ly, r.scx, r.scx & 7, r.scy, r.stall, r.lcdc,
                   (r.lcdc & 1) ? "BGon" : "BGoff<<<");
            if ((r.scx & 7) != prev_s7) { ++runs; prev_s7 = r.scx & 7; }
        }
        printf("  --> distinct SCX&7 transitions across the frame: %d "
               "(high = per-scanline SCX-fine raster; feeds mode-3 penalty)\n", runs);
    }

    printf("\n=== SUMMARY ===\n");
    printf("double_speed: %s (frame %d)   STOP: %s (frame %d)\n",
        ds_frame>=0?"YES":"no", ds_frame, stop_frame>=0?"YES":"no", stop_frame);
    printf("first trigger: CH1 f%d  CH2 f%d  CH3 f%d  CH4 f%d\n",
        first_ch[0],first_ch[1],first_ch[2],first_ch[3]);
    printf("first non-zero freq: CH1 f%d  CH2 f%d\n", first_freq[0], first_freq[1]);
    printf("final trig counts: CH1=%u CH2=%u CH3=%u CH4=%u  waveWrites=%u nr50w=%u nr51w=%u\n",
        apu.dbg_trigger_count[0],apu.dbg_trigger_count[1],apu.dbg_trigger_count[2],apu.dbg_trigger_count[3],
        apu.dbg_wave_ram_writes, apu.dbg_nr50_writes, apu.dbg_nr51_writes);
    printf("AUDIO: totalSamples=%lld globalPeak=%d framesWithAudio=%d/%d  watchHits=%llu\n",
        (long long)total_samples, global_peak, frames_with_audio, frames, (unsigned long long)g_watch_hits);
    if (g_hostpace > 0.0 || g_drc)
        printf("HOSTPACE: ringFullFrames=%d/%d drcCorr=%+.5f\n", ring_full_frames, frames, g_drc_corr);

    extern long g_sgb_resets, g_sgb_rotations;
    extern std::vector<std::pair<int,int>> g_sgb_cmds;
    printf("SGB: packetResets=%ld joypadRotations=%ld cmds=%zu\n", g_sgb_resets, g_sgb_rotations, g_sgb_cmds.size());
    static const char *kCmd[0x20] = {
        "PAL01","PAL23","PAL03","PAL12","ATTR_BLK","ATTR_LIN","ATTR_DIV","ATTR_CHR",
        "SOUND","SOU_TRN","PAL_SET","PAL_TRN","ATRC_EN","TEST_EN","ICON_EN","DATA_SND",
        "DATA_TRN","MLT_REQ","JUMP","CHR_TRN","PCT_TRN","ATTR_TRN","ATTR_SET","MASK_EN",
        "OBJ_TRN","?","?","?","?","?","?","?"};
    for (auto &c : g_sgb_cmds)
        printf("  cmd f%-5d 0x%02X %s\n", c.first, c.second, c.second < 0x20 ? kCmd[c.second] : "?");

    printf("\nHottest PCs on final frame (distinct=%zu) — busy-loop / hang detector:\n", g_pc_hist.size());
    std::vector<std::pair<uint64_t,uint16_t>> v;
    for (auto &kv : g_pc_hist) v.push_back({kv.second, kv.first});
    std::sort(v.rbegin(), v.rend());
    for (size_t i=0;i<v.size() && i<12;i++) printf("  PC %04X : %llu\n", v[i].second, (unsigned long long)v[i].first);
    return 0;
}
