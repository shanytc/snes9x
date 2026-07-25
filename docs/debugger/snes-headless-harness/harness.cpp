#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "libretro.h"
#include "snes9x.h"
#include "memmap.h"
#include "cpuexec.h"
#include "ppu.h"
#include "dma.h"
#include "apu/apu.h"
#include "sfcbox.h"

struct PressEvent { int frame; uint16_t mask; int hold; };
struct PathEvent  { int frame; std::string path; };
struct PokeEvent  { int frame; uint32_t addr; uint8_t val; };
struct EvJmpEvent { int frame; uint8_t sel; uint32_t addr; };

static std::vector<PressEvent> g_press;
static std::vector<PokeEvent>  g_pokes;
static std::vector<EvJmpEvent> g_evjmps;
static std::vector<int>        g_resets;   // frames to call retro_reset (soft reset)
static std::vector<PathEvent>  g_dumps;
static std::vector<PathEvent>  g_cgdumps;
static std::vector<PathEvent>  g_saves;
static std::vector<int>        g_osddumps;   // frames to dump the SFC-Box OSD grid
static std::vector<int>        g_coins;      // frames to insert a coin (SFC-Box)
static std::vector<PokeEvent>  g_keysw;      // frames to turn the SFC-Box keyswitch (addr = position)
#if defined(HARNESS_TRACE_HOOKS)
extern int g_trace_cgram;
extern int g_trace_reg;
#else
int g_trace_cgram;
int g_trace_reg;
#endif
static std::string g_load;
static std::vector<PathEvent> g_loadats;   // mid-run state loads (rewind-style)
static std::vector<PathEvent> g_loadroms;  // mid-run ROM swaps (GUI File->Open)
static int  g_traceLo  = -1, g_traceHi = -1;
static int  g_regLo    = -1, g_regHi   = -1;
static int  g_frames   = 600;
static int  g_english  = 0;   // Settings.SFCBoxOSDEnglish override
static int  g_apuspd   = -1;
static int  g_logEvery = 60;
static int  g_frame    = 0;
static uint16_t g_buttons = 0;

static std::map<std::string, std::string> g_opts;
static const void *g_fb = nullptr;
static unsigned g_fbw = 0, g_fbh = 0;
static size_t   g_fbpitch = 0;
static enum retro_pixel_format g_pixfmt = RETRO_PIXEL_FORMAT_0RGB1555;

static bool env_cb(unsigned cmd, void *data)
{
    switch (cmd)
    {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        g_pixfmt = *(const enum retro_pixel_format *)data;
        return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = true;
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE:
    {
        retro_variable *var = (retro_variable *)data;
        auto it = g_opts.find(var->key ? var->key : "");
        if (it == g_opts.end())
            return false;
        var->value = it->second.c_str();
        return true;
    }
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    {
        // BIOS lookups (SGB/BS-X/SFC-Box) go through this. Honor the
        // conventional BIOS/ folder in the working directory, overridable
        // via SNES9X_SYSTEM_DIR. The core additionally probes the ROM's
        // own directory and its BIOS/ siblings, so this is just the
        // explicit front door.
        static const char *sysdir = nullptr;
        if (!sysdir)
        {
            const char *env = getenv("SNES9X_SYSTEM_DIR");
            sysdir = (env && *env) ? strdup(env) : "BIOS";
        }
        *(const char **)data = sysdir;
        return true;
    }
    default:
        return false;
    }
}

static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{
    if (data)
    {
        g_fb = data;
        g_fbw = w;
        g_fbh = h;
        g_fbpitch = pitch;
    }
}

static void audio_sample_cb(int16_t, int16_t) {}
static size_t audio_batch_cb(const int16_t *, size_t frames) { return frames; }
static void input_poll_cb() {}

static int16_t input_state_cb(unsigned port, unsigned device, unsigned, unsigned id)
{
    if (port != 0 || (device & 0xFF) != RETRO_DEVICE_JOYPAD || id > 15)
        return 0;
    return (g_buttons >> id) & 1;
}

static int button_id(const std::string &name)
{
    if (name == "A") return RETRO_DEVICE_ID_JOYPAD_A;
    if (name == "B") return RETRO_DEVICE_ID_JOYPAD_B;
    if (name == "X") return RETRO_DEVICE_ID_JOYPAD_X;
    if (name == "Y") return RETRO_DEVICE_ID_JOYPAD_Y;
    if (name == "START")  return RETRO_DEVICE_ID_JOYPAD_START;
    if (name == "SELECT") return RETRO_DEVICE_ID_JOYPAD_SELECT;
    if (name == "U") return RETRO_DEVICE_ID_JOYPAD_UP;
    if (name == "D") return RETRO_DEVICE_ID_JOYPAD_DOWN;
    if (name == "L") return RETRO_DEVICE_ID_JOYPAD_LEFT;
    if (name == "R") return RETRO_DEVICE_ID_JOYPAD_RIGHT;
    if (name == "LB") return RETRO_DEVICE_ID_JOYPAD_L;
    if (name == "RB") return RETRO_DEVICE_ID_JOYPAD_R;
    fprintf(stderr, "unknown button '%s'\n", name.c_str());
    exit(1);
}

static std::vector<std::string> split(const std::string &s, char sep)
{
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size())
    {
        size_t end = s.find(sep, start);
        if (end == std::string::npos) end = s.size();
        out.push_back(s.substr(start, end - start));
        start = end + 1;
    }
    return out;
}

static void parse_press(const std::string &arg)
{
    for (const std::string &item : split(arg, ','))
    {
        if (item.empty()) continue;
        std::vector<std::string> parts = split(item, ':');
        PressEvent ev;
        ev.frame = atoi(parts[0].c_str());
        ev.hold  = parts.size() > 2 ? atoi(parts[2].c_str()) : 8;
        ev.mask  = 0;
        for (const std::string &b : split(parts[1], '+'))
            ev.mask |= 1u << button_id(b);
        g_press.push_back(ev);
    }
}

static void parse_pokes(const std::string &arg)
{
    for (const std::string &item : split(arg, ','))
    {
        if (item.empty()) continue;
        std::vector<std::string> parts = split(item, ':');
        if (parts.size() < 3) { fprintf(stderr, "bad poke '%s'\n", item.c_str()); exit(1); }
        PokeEvent ev;
        ev.frame = atoi(parts[0].c_str());
        ev.addr  = (uint32_t)strtoul(parts[1].c_str(), nullptr, 16);
        ev.val   = (uint8_t)strtoul(parts[2].c_str(), nullptr, 16);
        g_pokes.push_back(ev);
    }
}

static void parse_evjmps(const std::string &arg)
{
    for (const std::string &item : split(arg, ','))
    {
        if (item.empty()) continue;
        std::vector<std::string> parts = split(item, ':');
        if (parts.size() < 3) { fprintf(stderr, "bad evjmp '%s'\n", item.c_str()); exit(1); }
        EvJmpEvent ev;
        ev.frame = atoi(parts[0].c_str());
        ev.sel   = (uint8_t)strtoul(parts[1].c_str(), nullptr, 16);
        ev.addr  = (uint32_t)strtoul(parts[2].c_str(), nullptr, 16);
        g_evjmps.push_back(ev);
    }
}

static void parse_paths(const std::string &arg, std::vector<PathEvent> &out)
{
    for (const std::string &item : split(arg, ','))
    {
        if (item.empty()) continue;
        size_t colon = item.find(':');
        PathEvent ev;
        ev.frame = atoi(item.substr(0, colon).c_str());
        ev.path  = item.substr(colon + 1);
        out.push_back(ev);
    }
}

static void write_ppm(const std::string &path)
{
    if (!g_fb)
    {
        printf("fb dump f%d: no frame rendered yet\n", g_frame);
        return;
    }
    FILE *fp = fopen(path.c_str(), "wb");
    if (!fp)
    {
        fprintf(stderr, "cannot write %s\n", path.c_str());
        return;
    }
    fprintf(fp, "P6\n%u %u\n255\n", g_fbw, g_fbh);
    const uint8_t *rows = (const uint8_t *)g_fb;
    for (unsigned y = 0; y < g_fbh; y++)
    {
        const uint16_t *px = (const uint16_t *)(rows + y * g_fbpitch);
        for (unsigned x = 0; x < g_fbw; x++)
        {
            uint16_t p = px[x];
            uint8_t r, g, b;
            if (g_pixfmt == RETRO_PIXEL_FORMAT_RGB565)
            {
                r = (p >> 11) & 31; g = (p >> 5) & 63; b = p & 31;
                uint8_t rgb[3] = { (uint8_t)((r << 3) | (r >> 2)),
                                   (uint8_t)((g << 2) | (g >> 4)),
                                   (uint8_t)((b << 3) | (b >> 2)) };
                fwrite(rgb, 1, 3, fp);
            }
            else
            {
                r = (p >> 10) & 31; g = (p >> 5) & 31; b = p & 31;
                uint8_t rgb[3] = { (uint8_t)((r << 3) | (r >> 2)),
                                   (uint8_t)((g << 3) | (g >> 2)),
                                   (uint8_t)((b << 3) | (b >> 2)) };
                fwrite(rgb, 1, 3, fp);
            }
        }
    }
    fclose(fp);
    printf("fb dump f%d -> %s (%ux%u)\n", g_frame, path.c_str(), g_fbw, g_fbh);
}

static void dump_cgram(const std::string &path)
{
    FILE *fp = fopen(path.c_str(), "wb");
    if (!fp) { fprintf(stderr, "cannot write %s\n", path.c_str()); return; }
    fprintf(fp, "P6\n256 256\n255\n");
    for (int y = 0; y < 256; y++)
    {
        for (int x = 0; x < 256; x++)
        {
            int idx = (y / 16) * 16 + (x / 16);
            uint16_t c = PPU.CGDATA[idx];
            uint8_t rgb[3] = { (uint8_t)(((c      ) & 31) * 255 / 31),
                               (uint8_t)(((c >>  5) & 31) * 255 / 31),
                               (uint8_t)(((c >> 10) & 31) * 255 / 31) };
            fwrite(rgb, 1, 3, fp);
        }
    }
    fclose(fp);
    printf("cgram dump f%d -> %s\n", g_frame, path.c_str());
    for (int i = 0; i < 256; i += 16)
    {
        printf("  %02X:", i);
        for (int j = 0; j < 16; j++)
            printf(" %04X", PPU.CGDATA[i + j]);
        printf("\n");
    }
}

static void save_state(const std::string &path)
{
    size_t sz = retro_serialize_size();
    std::vector<uint8_t> buf(sz);
    if (!retro_serialize(buf.data(), sz))
    {
        fprintf(stderr, "serialize failed at f%d\n", g_frame);
        return;
    }
    FILE *fp = fopen(path.c_str(), "wb");
    if (!fp) { fprintf(stderr, "cannot write %s\n", path.c_str()); return; }
    fwrite(buf.data(), 1, sz, fp);
    fclose(fp);
    printf("state saved f%d -> %s (%zu bytes)\n", g_frame, path.c_str(), sz);
}

static void load_state(const std::string &path)
{
    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp) { fprintf(stderr, "cannot read %s\n", path.c_str()); exit(1); }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    std::vector<uint8_t> buf(sz);
    if (fread(buf.data(), 1, sz, fp) != (size_t)sz) { fclose(fp); exit(1); }
    fclose(fp);
    if (!retro_unserialize(buf.data(), sz))
    {
        fprintf(stderr, "unserialize failed (%s)\n", path.c_str());
        exit(1);
    }
    printf("state loaded <- %s\n", path.c_str());
}

// Dump the raw (pre-translation) SFC-Box OSD text grid: per non-blank row,
// the cell bytes as hex plus a best-effort ASCII rendering ('.' = non-ASCII).
static void dump_osd()
{
    printf("--- OSD dump f%d (rows with content only) ---\n", g_frame);
    for (int y = 0; y < SFCBOX_OSD_H; y++)
    {
        const uint8 *ch = SFCBox.OSD.VRAMChar[y];
        const uint8 *at = SFCBox.OSD.VRAMColor[y];
        bool blank = true;
        for (int x = 0; x < SFCBOX_OSD_W; x++)
            if (ch[x] != 0xff && ch[x] != 0x20) { blank = false; break; }
        if (blank)
            continue;
        printf("  row %2d ch:", y);
        for (int x = 0; x < SFCBOX_OSD_W; x++) printf(" %02X", ch[x]);
        printf("\n         at:");
        for (int x = 0; x < SFCBOX_OSD_W; x++) printf(" %02X", at[x]);
        printf("\n         as: ");
        for (int x = 0; x < SFCBOX_OSD_W; x++)
        {
            uint8 c = ch[x];
            putchar((c >= 0x20 && c < 0x7f) ? c : (c == 0xff ? ' ' : '.'));
        }
        printf("\n");
    }
    fflush(stdout);
}

static void log_frame()
{
    printf("[f%5d] PC=%02X:%04X mode=%d INIDISP=%02X TM=%02X TS=%02X CGWSEL=%02X CGADSUB=%02X HDMAEN=%02X %ux%u\n",
           g_frame,
           (unsigned)Registers.PB, (unsigned)Registers.PCw,
           Memory.FillRAM[0x2105] & 7,
           Memory.FillRAM[0x2100],
           Memory.FillRAM[0x212C], Memory.FillRAM[0x212D],
           Memory.FillRAM[0x2130], Memory.FillRAM[0x2131],
           Memory.FillRAM[0x420C],
           g_fbw, g_fbh);
    for (int c = 0; c < 8; c++)
    {
        if (!(Memory.FillRAM[0x420C] & (1 << c)))
            continue;
        printf("         hdma%d -> $21%02X mode=%d %s tbl=%02X:%04X cur=%04X ind=%02X:%04X\n",
               c, DMA[c].BAddress, DMA[c].TransferMode,
               DMA[c].HDMAIndirectAddressing ? "ind" : "dir",
               DMA[c].ABank, DMA[c].AAddress, DMA[c].Address,
               DMA[c].IndirectBank, DMA[c].IndirectAddress);
    }
    fflush(stdout);
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr,
            "usage: %s <rom.sfc> [frames=N] [log=N] [press=F:BTN[+BTN][:HOLD],...]\n"
            "          [fb=F:path.ppm,...] [save=F:path,...] [load=path]\n"
            "buttons: A B X Y START SELECT U D L R LB RB\n", argv[0]);
        return 1;
    }

    const char *rompath = argv[1];
    for (int i = 2; i < argc; i++)
    {
        std::string a = argv[i];
        if      (a.rfind("frames=", 0) == 0) g_frames = atoi(a.c_str() + 7);
        else if (a.rfind("log=", 0) == 0)    g_logEvery = atoi(a.c_str() + 4);
        else if (a.rfind("press=", 0) == 0)  parse_press(a.substr(6));
        else if (a.rfind("fb=", 0) == 0)     parse_paths(a.substr(3), g_dumps);
        else if (a.rfind("cgram=", 0) == 0)  parse_paths(a.substr(6), g_cgdumps);
        else if (a.rfind("save=", 0) == 0)   parse_paths(a.substr(5), g_saves);
        else if (a.rfind("load=", 0) == 0)   g_load = a.substr(5);
        else if (a.rfind("loadat=", 0) == 0) parse_paths(a.substr(7), g_loadats);
        else if (a.rfind("loadrom=", 0) == 0) parse_paths(a.substr(8), g_loadroms);
        else if (a.rfind("apuspd=", 0) == 0) g_apuspd = atoi(a.c_str() + 7);
        else if (a.rfind("poke=", 0) == 0)   parse_pokes(a.substr(5));
        else if (a.rfind("evjmp=", 0) == 0)  parse_evjmps(a.substr(6));
        else if (a.rfind("reset=", 0) == 0)  g_resets.push_back(atoi(a.c_str() + 6));
        else if (a.rfind("english=", 0) == 0) g_english = atoi(a.c_str() + 8);
        else if (a.rfind("coin=", 0) == 0)
        {
            for (const std::string &f : split(a.substr(5), ','))
                if (!f.empty()) g_coins.push_back(atoi(f.c_str()));
        }
        else if (a.rfind("keysw=", 0) == 0)
        {
            for (const std::string &item : split(a.substr(6), ','))
            {
                if (item.empty()) continue;
                std::vector<std::string> parts = split(item, ':');
                PokeEvent ev;
                ev.frame = atoi(parts[0].c_str());
                ev.addr  = (uint32_t)atoi(parts[1].c_str());   // port-80h bit index
                ev.val   = 0;
                g_keysw.push_back(ev);
            }
        }
        else if (a.rfind("osd=", 0) == 0)
        {
            for (const std::string &f : split(a.substr(4), ','))
                if (!f.empty()) g_osddumps.push_back(atoi(f.c_str()));
        }
        else if (a.rfind("reg=", 0) == 0)
        {
            std::vector<std::string> lohi = split(a.substr(4), ':');
            g_regLo = atoi(lohi[0].c_str());
            g_regHi = lohi.size() > 1 ? atoi(lohi[1].c_str()) : g_regLo + 1;
        }
        else if (a.rfind("trace=", 0) == 0)
        {
            std::vector<std::string> lohi = split(a.substr(6), ':');
            g_traceLo = atoi(lohi[0].c_str());
            g_traceHi = lohi.size() > 1 ? atoi(lohi[1].c_str()) : g_traceLo + 1;
        }
        else if (a.rfind("opt=", 0) == 0)
        {
            for (const std::string &kv : split(a.substr(4), ','))
            {
                size_t colon = kv.find(':');
                if (colon != std::string::npos)
                    g_opts[kv.substr(0, colon)] = kv.substr(colon + 1);
            }
        }
        else if (a.find_first_not_of("0123456789") == std::string::npos)
                 g_frames = atoi(a.c_str());
        else { fprintf(stderr, "unknown arg '%s'\n", a.c_str()); return 1; }
    }

    FILE *fp = fopen(rompath, "rb");
    if (!fp) { fprintf(stderr, "cannot open %s\n", rompath); return 1; }
    fseek(fp, 0, SEEK_END);
    long romsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    std::vector<uint8_t> rom(romsize);
    if (fread(rom.data(), 1, romsize, fp) != (size_t)romsize) { fclose(fp); return 1; }
    fclose(fp);

    retro_set_environment(env_cb);
    retro_set_video_refresh(video_cb);
    retro_set_audio_sample(audio_sample_cb);
    retro_set_audio_sample_batch(audio_batch_cb);
    retro_set_input_poll(input_poll_cb);
    retro_set_input_state(input_state_cb);
    retro_init();

    // .zip goes path-only so Memory.LoadROM runs the same FileLoader/unzip
    // route the win32 GUI uses (needs a -DUNZIP_SUPPORT core build).
    const size_t rplen = strlen(rompath);
    const bool is_zip = rplen > 4 && !strcasecmp(rompath + rplen - 4, ".zip");
    retro_game_info gi = { rompath, is_zip ? nullptr : rom.data(),
                           is_zip ? 0 : (size_t)romsize, nullptr };
    if (!retro_load_game(&gi))
    {
        fprintf(stderr, "retro_load_game failed\n");
        return 1;
    }
    printf("loaded %s (%ld bytes) '%s'\n", rompath, romsize, Memory.ROMName);

    if (g_apuspd >= 0)
    {
        Timings.APUSpeedup = g_apuspd;
        S9xAPUTimingSetSpeedup(g_apuspd);
        printf("APU speedup override: %d\n", g_apuspd);
    }

    if (!g_load.empty())
        load_state(g_load);

    if (g_english)
    {
        Settings.SFCBoxOSDEnglish = TRUE;
        printf("SFC-Box OSD language: English\n");
    }

    for (g_frame = 0; g_frame < g_frames; g_frame++)
    {
        g_buttons = 0;
        for (const PressEvent &ev : g_press)
            if (g_frame >= ev.frame && g_frame < ev.frame + ev.hold)
                g_buttons |= ev.mask;

        for (const PokeEvent &ev : g_pokes)
            if (ev.frame == g_frame)
            {
                Memory.RAM[ev.addr & 0x1FFFF] = ev.val;
                printf("poke f%d: $7E:%04X <- %02X\n", g_frame, ev.addr & 0xFFFF, ev.val);
            }

        for (const EvJmpEvent &ev : g_evjmps)
            if (ev.frame == g_frame)
            {
                if (ev.sel != 0xFF)
                    S9xSetEvent(ev.sel, 0x206000);
                Registers.PL |= 0x30;
                S9xUnpackStatus();
                S9xSetPCBase(ev.addr & 0xFFFFFF);
                printf("evjmp f%d: select <- %02X, PC <- %06X\n", g_frame, ev.sel, ev.addr);
            }

        for (int rf : g_resets)
            if (rf == g_frame)
            {
                retro_reset();
                printf("soft reset at f%d\n", g_frame);
            }

        for (int cf : g_coins)
            if (cf == g_frame)
            {
                S9xSFCBoxInsertCoin();
                printf("coin inserted at f%d\n", g_frame);
            }

        for (const PokeEvent &ev : g_keysw)
            if (ev.frame == g_frame)
            {
                SFCBox.Keyswitch = (uint8)ev.addr;
                printf("keyswitch -> %u at f%d\n", ev.addr, g_frame);
            }

        for (const PathEvent &ev : g_loadats)
            if (ev.frame == g_frame)
            {
                load_state(ev.path);
                printf("mid-run state load f%d <- %s\n", g_frame, ev.path.c_str());
            }

        // Mid-run cart swap through Memory.LoadROM — the exact path the
        // win32 GUI's File->Open takes over a live session.
        for (const PathEvent &ev : g_loadroms)
            if (ev.frame == g_frame)
                printf("mid-run LoadROM f%d <- %s : %s\n", g_frame, ev.path.c_str(),
                       Memory.LoadROM(ev.path.c_str()) ? "ok" : "FAILED");

        g_trace_cgram = (g_frame >= g_traceLo && g_frame < g_traceHi);
        g_trace_reg   = (g_frame >= g_regLo && g_frame < g_regHi);
        if (g_trace_cgram || g_trace_reg)
            printf("--- trace frame %d ---\n", g_frame);

        retro_run();

        bool interesting = g_frame < 3 || g_frame == g_frames - 1 ||
                           (g_logEvery > 0 && g_frame % g_logEvery == 0);
        for (const PathEvent &ev : g_dumps)
            if (ev.frame == g_frame) { write_ppm(ev.path); interesting = true; }
        for (const PathEvent &ev : g_cgdumps)
            if (ev.frame == g_frame) { dump_cgram(ev.path); interesting = true; }
        for (const PathEvent &ev : g_saves)
            if (ev.frame == g_frame) save_state(ev.path);
        for (int of : g_osddumps)
            if (of == g_frame) { dump_osd(); interesting = true; }
        if (interesting)
            log_frame();
    }

    retro_deinit();
    return 0;
}
