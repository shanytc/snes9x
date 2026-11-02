#include "snes9x_imgui.h"
#include "snes9x_imgui_noto.h"
#include "imgui.h"

#include <cstdint>
#include <string>
#include <array>
#include <vector>
#include <algorithm>

#include "snes9x.h"
#include "port.h"

#ifdef RETROACHIEVEMENTS_SUPPORT
void RA_RenderOverlay(int width, int height);
#endif
#include "controls.h"
#include "movie.h"
#include "gfx.h"
#include "ppu.h"
#include "memmap.h"
#include "cheats.h"

namespace
{
    S9xImGuiInitInfo settings;
} // anonymous

// One row of the pressed-keys overlay: the port and its device, then the
// device's parts, lit while held.
struct PressedKeysRow
{
    std::string label;
    std::vector<std::pair<std::string, bool>> parts;
    bool keys = false; // joypad keys sit in fixed cells, so the box doesn't jitter
};

static std::string PortLabel(int port, const char *device)
{
    std::string label = "#" + std::to_string(port + 1);
    if (device)
        label += std::string(" ") + device;
    return label;
}

static std::string Coordinates(int x, int y)
{
    char string[32];
    snprintf(string, sizeof(string), "(%03d,%03d)", x, y);
    return string;
}

static void AddJoypadRow(std::vector<PressedKeysRow> &rows, std::string label, int pad)
{
    const std::array<const char *, 15> keynames =
        { " ", " ", " ", "R", "L", "X", "A", "→", "←", "↓", "↑", "St", "Sel", "Y", "B" };
    const std::array<int, 12> keyorder =
        { 10, 9, 8, 7, 6, 14, 13, 5, 4, 3, 11, 12 }; // < ^ > v   A B Y X  L R  S s

    PressedKeysRow row;
    row.label = label;
    row.keys = true;
    uint16 buttons = MovieGetJoypad(pad);
    for (int j : keyorder)
        row.parts.push_back({ keynames[j], (buttons & (1 << (j + 1))) != 0 });
    rows.push_back(row);
}

// What each port holds, as overlay rows: Joypad #1 in port 1 and a Sunsoft
// Pachinko in port 2 read "#1 <keys>" and "#2 Sunsoft Pachinko Power 48% ...".
static std::vector<PressedKeysRow> PressedKeysRows()
{
    std::vector<PressedKeysRow> rows;
    enum controllers controller;
    int8_t ids[4];

    for (int port = 0; port < 2; port++)
    {
        S9xGetController(port, &controller, &ids[0], &ids[1], &ids[2], &ids[3]);

        switch (controller)
        {
        case CTL_JOYPAD:
            AddJoypadRow(rows, PortLabel(port, nullptr), ids[0]);
            break;

        case CTL_MP5:
            for (int n = 0; n < 4; n++)
                if (ids[n] != -1)
                    AddJoypadRow(rows, PortLabel(port, ("Multitap " + std::to_string(n + 1)).c_str()), ids[n]);
            break;

        case CTL_MOUSE: {
            uint8 buf[MOUSE_DATA_SIZE];
            if (!MovieGetMouse(port, buf))
                break;
            uint8 buttons = buf[4];
            rows.push_back({ PortLabel(port, "SNES Mouse"),
                             { { Coordinates((int16)READ_WORD(buf), (int16)READ_WORD(buf + 2)), true },
                               { "L", (buttons & 0x40) != 0 },
                               { "R", (buttons & 0x80) != 0 } } });
            break;
        }

        case CTL_SUPERSCOPE: {
            uint8 buf[SCOPE_DATA_SIZE];
            if (!MovieGetScope(port, buf))
                break;
            uint8 buttons = buf[4];
            rows.push_back({ PortLabel(port, "Super Scope"),
                             { { Coordinates((int16)READ_WORD(buf), (int16)READ_WORD(buf + 2)), true },
                               { "Fire", (buttons & 0x80) != 0 },
                               { "Cursor", (buttons & 0x40) != 0 },
                               { "Turbo", (buttons & 0x20) != 0 },
                               { "Pause", (buttons & 0x10) != 0 } } });
            break;
        }

        // One Justifier or two; the second's buttons sit a bit lower
        case CTL_JUSTIFIER: {
            uint8 buf[JUSTIFIER_DATA_SIZE];
            if (!MovieGetJustifier(port, buf))
                break;
            const int guns = ids[0] == 1 ? 2 : 1;
            uint8 buttons = buf[8];
            for (int k = 0; k < guns; k++)
                rows.push_back({ PortLabel(port, guns == 1 ? "Konami Justifier" : k == 0 ? "Justifier 1" : "Justifier 2"),
                                 { { Coordinates((int16)READ_WORD(buf + 2 * k), (int16)READ_WORD(buf + 4 + 2 * k)), true },
                                   { "Trigger", (buttons & (0x80 >> k)) != 0 },
                                   { "Start", (buttons & (0x20 >> k)) != 0 } } });
            break;
        }

        case CTL_MACSRIFLE: {
            uint8 buf[MACSRIFLE_DATA_SIZE];
            if (!MovieGetMacsRifle(port, buf))
                break;
            rows.push_back({ PortLabel(port, "M.A.C.S. Rifle"),
                             { { Coordinates((int16)READ_WORD(buf), (int16)READ_WORD(buf + 2)), true },
                               { "Trigger", (buf[4] & 0x01) != 0 } } });
            break;
        }

        // Power as the settings' gauge shows it: the dial's 0-127 as a percentage
        case CTL_PACHINKO: {
            uint8 buf[PACHINKO_DATA_SIZE];
            if (!MovieGetPachinko(port, buf))
                break;
            uint8 buttons = buf[1];
            rows.push_back({ PortLabel(port, "Sunsoft Pachinko"),
                             { { "Power " + std::to_string((buf[0] * 100 + 63) / 127) + "%", true },
                               { "Stronger", (buttons & 0x02) != 0 },
                               { "Weaker", (buttons & 0x04) != 0 },
                               { "Stop", (buttons & 0x01) != 0 } } });
            break;
        }

        default:
            break;
        }
    }

    return rows;
}

static void ImGui_DrawPressedKeys(int spacing)
{
    std::vector<PressedKeysRow> rows = PressedKeysRows();
    if (rows.empty())
        return;

    auto draw_list = ImGui::GetForegroundDrawList();
    const float cell_width = ImGui::CalcTextSize("→ ").x;
    const float space_width = ImGui::CalcTextSize(" ").x;

    // The labels share a column so the keys line up, and every box is as wide as the widest row
    float label_width = 0, parts_width = 0;
    for (auto &row : rows)
    {
        label_width = std::max(label_width, ImGui::CalcTextSize(row.label.c_str()).x);
        float width = 0;
        for (auto &part : row.parts)
            width += row.keys ? cell_width : ImGui::CalcTextSize(part.first.c_str()).x + space_width;
        parts_width = std::max(parts_width, width);
    }

    int box_width = 2 * spacing + label_width + space_width + parts_width;
    int box_height = 2 * spacing + ImGui::CalcTextSize("#").y;
    int x = (ImGui::GetIO().DisplaySize.x - box_width) / 2;
    int y = ImGui::GetIO().DisplaySize.y - (spacing + box_height) * (int)rows.size();

    for (auto &row : rows)
    {
        draw_list->AddRectFilled(ImVec2(x, y),
                                 ImVec2(x + box_width, y + box_height),
                                 settings.box_color,
                                 spacing / 2.0f);

        float text_x = x + spacing;
        draw_list->AddText(ImVec2(text_x, y + spacing), settings.text_color, row.label.c_str());
        text_x += label_width + space_width;

        for (auto &part : row.parts)
        {
            draw_list->AddText(ImVec2(text_x, y + spacing), part.second ? settings.text_color : settings.inactive_text_color, part.first.c_str());
            text_x += row.keys ? cell_width : ImGui::CalcTextSize(part.first.c_str()).x + space_width;
        }

        y += spacing + box_height;
    }
}

static void ImGui_GetWatchesText(std::string& osd_text)
{
    for (unsigned int i = 0; i < sizeof(watches) / sizeof(watches[0]); i++)
    {
        if (!watches[i].on)
            break;

        int32	displayNumber = 0;
        char	buf[64];

        for (int r = 0; r < watches[i].size; r++)
            displayNumber += (Cheat.CWatchRAM[(watches[i].address - 0x7E0000) + r]) << (8 * r);

        if (watches[i].format == 1)
            sprintf(buf, "%s,%du = %u", watches[i].desc, watches[i].size, (unsigned int)displayNumber);
        else
            if (watches[i].format == 3)
                sprintf(buf, "%s,%dx = %X", watches[i].desc, watches[i].size, (unsigned int)displayNumber);
            else // signed
            {
                if (watches[i].size == 1)
                    displayNumber = (int32)((int8)displayNumber);
                else if (watches[i].size == 2)
                    displayNumber = (int32)((int16)displayNumber);
                else if (watches[i].size == 3)
                    if (displayNumber >= 8388608)
                        displayNumber -= 16777216;

                sprintf(buf, "%s,%ds = %d", watches[i].desc, watches[i].size, (int)displayNumber);
            }

        osd_text += buf;
        osd_text += '\n';
    }
}

static void ImGui_DrawTextOverlay(const char *text,
                                  int x, int y,
                                  int padding,
                                  ImGui::DrawTextAlignment halign = ImGui::DrawTextAlignment::BEGIN,
                                  ImGui::DrawTextAlignment valign = ImGui::DrawTextAlignment::BEGIN,
                                  int wrap_at = 0)
{
    auto text_size = ImGui::CalcTextSize(text, nullptr, false, wrap_at);
    auto box_size = ImVec2(text_size.x + padding * 2, text_size.y + padding * 2);
    auto draw_list = ImGui::GetForegroundDrawList();
    if (halign == ImGui::DrawTextAlignment::END)
        x = x - box_size.x;
    else if (halign == ImGui::DrawTextAlignment::CENTER)
        x = x - box_size.x / 2;
    if (valign == ImGui::DrawTextAlignment::END)
        y = y - box_size.y;

    draw_list->AddRectFilled(ImVec2(x, y),
                             ImVec2(x + box_size.x, y + box_size.y),
                             settings.box_color,
                             settings.spacing / 2.0f);

    draw_list->AddText(nullptr, 0.0f, ImVec2(x + padding, y + padding), settings.text_color, text, nullptr, wrap_at);
}

static std::string sjis_to_utf8(std::string in)
{
    std::string out;
    for (const auto &i : in)
    {
        unsigned char c = i;
        if (c > 160 && c < 192)
		{
            out += "\357\275";
			out += c;
		}
        else if (c >= 192)
        {
            out += "\357\276";
            c -= 0x40;
			out += c;
        }
        else
            out += c;
    }

    return out;
}

bool S9xImGuiDraw(int width, int height)
{
    if (Memory.ROMFilename.empty())
        return false;

    if (!ImGui::GetCurrentContext())
        return false;

    ImGui::GetIO().DisplaySize.x = width;
    ImGui::GetIO().DisplaySize.y = height;
    ImGui::GetIO().DisplayFramebufferScale.x = 1.0;
    ImGui::GetIO().DisplayFramebufferScale.y = 1.0;
    ImGui::NewFrame();

    if (Settings.DisplayTime)
    {
        char string[256];

        time_t rawtime;
        struct tm *timeinfo;
        time(&rawtime);
        timeinfo = localtime(&rawtime);

        sprintf(string, "%02u:%02u", timeinfo->tm_hour, timeinfo->tm_min);
        ImGui_DrawTextOverlay(string,
                              width - settings.spacing,
                              height - settings.spacing,
                              settings.spacing,
                              ImGui::DrawTextAlignment::END,
                              ImGui::DrawTextAlignment::END);
    }

    if (Settings.DisplayFrameRate)
    {
        char string[256];
        static uint32 lastFrameCount = 0, calcFps = 0;
        static time_t lastTime = time(NULL);

        // Normalize emulation-throughput counters to logical game frames when
        // run-ahead is on (each iteration emulates RunAhead+1 SNES frames but
        // only one is displayed).
        uint32 runAheadMul = (Settings.RunAhead > 0) ? (uint32)Settings.RunAhead + 1 : 1;

        time_t currTime = time(NULL);
        if (lastTime != currTime)
        {
            if (lastFrameCount < IPPU.TotalEmulatedFrames)
            {
                calcFps = (IPPU.TotalEmulatedFrames - lastFrameCount) / (uint32)(currTime - lastTime);
            }
            lastTime = currTime;
            lastFrameCount = IPPU.TotalEmulatedFrames;
        }

        sprintf(string, "%u fps\n%02d/%02d", calcFps / runAheadMul, (int)(IPPU.DisplayedRenderedFrameCount * runAheadMul), (int)Memory.ROMFramesPerSecond);

        ImGui_DrawTextOverlay(string,
                              width - settings.spacing,
                              settings.spacing,
                              settings.spacing,
                              ImGui::DrawTextAlignment::END,
                              ImGui::DrawTextAlignment::BEGIN);
    }

    if (Settings.DisplayFrameNumber)
    {
        char string[32];
        sprintf(string, "#%u", (unsigned)IPPU.TotalEmulatedFrames);
        const float line_h = ImGui::CalcTextSize("0").y;
        const int gap = Settings.DisplayFrameRate
                        ? (int)(line_h * 2.0f + settings.spacing * 2 + 10)
                        : 0;
        ImGui_DrawTextOverlay(string,
                              width - settings.spacing,
                              settings.spacing + gap,
                              settings.spacing,
                              ImGui::DrawTextAlignment::END,
                              ImGui::DrawTextAlignment::BEGIN);
    }

    if (S9xEventTimerDisplay() == 1)
    {
        int secs = S9xPF94TimeRemaining();
        if (secs >= 0)
        {
            char string[16];
            sprintf(string, "%02d:%02d", secs / 60, secs % 60);
            const float line_h = ImGui::CalcTextSize("0").y;
            int gap = 0;
            if (Settings.DisplayFrameRate)
                gap += (int)(line_h * 2.0f + settings.spacing * 2 + 10);
            if (Settings.DisplayFrameNumber)
                gap += (int)(line_h + settings.spacing + 10);
            ImGui_DrawTextOverlay(string,
                                  width - settings.spacing,
                                  settings.spacing + gap,
                                  settings.spacing,
                                  ImGui::DrawTextAlignment::END,
                                  ImGui::DrawTextAlignment::BEGIN);
        }
    }

    if (Settings.DisplayPressedKeys)
    {
        ImGui_DrawPressedKeys(settings.spacing / 2);
    }

    if (Settings.DisplayIndicators)
    {
        if (Settings.Paused || Settings.ForcedPause)
        {
            ImGui_DrawTextOverlay("❚❚",
                                  settings.spacing,
                                  settings.spacing,
                                  settings.spacing);
        }
        else if (Settings.TurboMode)
        {
            ImGui_DrawTextOverlay("▶▶",
                                  settings.spacing,
                                  settings.spacing,
                                  settings.spacing);
        }
    }

    std::string utf8_message;
    if (Settings.DisplayWatchedAddresses)
    {
        ImGui_GetWatchesText(utf8_message);
    }

    if (!GFX.InfoString.empty())
    {
        utf8_message += sjis_to_utf8(GFX.InfoString);
    }

    if (Settings.DisplayMovieFrame && S9xMovieActive())
    {
        // move movie frame count into its own line if info message is active and not already a newline at end
        if (!utf8_message.empty() && utf8_message.back() != '\n')
        {
            utf8_message += '\n';
        }
        utf8_message += GFX.FrameDisplayString;
    }

    if (!utf8_message.empty())
    {
        ImGui_DrawTextOverlay(utf8_message.c_str(),
                              settings.spacing,
                              height - settings.spacing,
                              settings.spacing,
                              ImGui::DrawTextAlignment::BEGIN,
                              ImGui::DrawTextAlignment::END,
                              width - settings.spacing * 4);
    }

#ifdef RETROACHIEVEMENTS_SUPPORT
    RA_RenderOverlay(width, height);
#endif

    ImGui::Render();

    return true;
}

bool S9xImGuiRunning()
{
    if (ImGui::GetCurrentContext())
        return true;
    return false;
}

void S9xImGuiDeinit()
{
    if (S9xImGuiRunning())
        ImGui::DestroyContext();
}

S9xImGuiInitInfo S9xImGuiGetDefaults()
{
    return { 24, 10, 0x88000000, 0xffffffff, 0x44ffffff };
}

void S9xImGuiInit(S9xImGuiInitInfo *init_info)
{
    static ImVector<ImWchar> ranges;
    if (ImGui::GetCurrentContext())
        return;

    if (init_info)
    {
        ::settings = *init_info;
    }
    else
    {
        settings = S9xImGuiGetDefaults();
    }

    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouse;
    ImGui::StyleColorsLight();
    ImFontGlyphRangesBuilder builder;
    builder.Clear();
    builder.AddRanges(ImGui::GetIO().Fonts->GetGlyphRangesDefault());
    builder.AddRanges(ImGui::GetIO().Fonts->GetGlyphRangesJapanese());
    builder.AddText("←↑→↓▶❚");
    ranges.clear();
    builder.BuildRanges(&ranges);
    ImGui::GetIO().Fonts->AddFontFromMemoryCompressedTTF(imgui_noto_font_compressed_data, imgui_noto_font_compressed_size, settings.font_size, nullptr, ranges.Data);
}
