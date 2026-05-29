#include "CDisasmPanel.h"
#include "CDebuggerSnes.h"
#include "CDebuggerGb.h"
#include "CDebuggerLabels.h"
#include "../snes9x.h"
#include "../memmap.h"
#include "../65c816.h"
#include "../sgb/sgb.h"
#include <commctrl.h>
#include <windowsx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <string.h>
#include <tchar.h>

static const TCHAR *kClassName = TEXT("S9xDebuggerDisasmPanel");

enum
{
	IDC_DISASM_LV = 7000,

	IDM_DISASM_TOGGLE_BP = 7100,
	IDM_DISASM_COPY_ADDR,
	IDM_DISASM_COPY_LINE,
	IDM_DISASM_MOVE_PC,
	IDM_DISASM_RUN_TO_LOCATION,
	IDM_DISASM_GO_TO_LOCATION,
	IDM_DISASM_GO_TO_PC,
	IDM_DISASM_GO_TO_BRANCH_TARGET,
	IDM_DISASM_RUN_TO_BRANCH_TARGET
};

enum { COL_MARGIN = 0, COL_FLOW = 1, COL_ADDR = 2, COL_BYTES = 3, COL_MNEM = 4, COL_OPERAND = 5 };

struct DisasmLine
{
	uint32_t pc;
	uint8_t  length;
	bool     is_sub_start;
	bool     is_label_row;
	bool     is_unmapped;
	uint32_t section_index;
};

struct UnmappedRegion
{
	uint32_t pc;
	uint32_t size;
	const char *label;
};

static uint8_t BpBankForPC(DbgSystem sys, uint32_t pc)
{
	if (sys == DbgSystem::Snes)
		return (uint8_t)((pc >> 16) & 0xFF);
	const uint16_t cpu = (uint16_t)(pc & 0xFFFFu);
	if (cpu >= 0x4000 && cpu < 0x8000)
		return (uint8_t)(S9xSGBGetCurrentRomBank() & 0xFFu);
	return 0;
}

static bool IsInUnmappedRegion(DbgSystem sys, uint32_t pc, UnmappedRegion *out)
{
	if (sys != DbgSystem::Gb) return false;

	const uint16_t cpu = (uint16_t)(pc & 0xFFFF);

	if (cpu >= 0xA000 && cpu < 0xC000 && GbBackend::CartSRAMSize() == 0)
	{
		out->pc    = 0xA000u;
		out->size  = 0x2000u;
		out->label = "unmapped (no cart RAM)";
		return true;
	}
	if (cpu >= 0xFEA0 && cpu < 0xFF00)
	{
		out->pc    = 0xFEA0u;
		out->size  = 0x0060u;
		out->label = "unmapped (prohibited)";
		return true;
	}
	return false;
}

static bool IsTargetOpcode_Snes(uint8_t op)
{
	return op == 0x20 || op == 0x22 || op == 0xFC
	    || op == 0x4C || op == 0x5C
	    || op == 0x82 || op == 0x80;
}

static bool IsTargetOpcode_Gb(uint8_t op)
{
	return op == 0xCD || op == 0xC4 || op == 0xCC || op == 0xD4 || op == 0xDC
	    || (op & 0xC7) == 0xC7
	    || op == 0xC3
	    || op == 0x18;
}

static bool IsTargetOpcode(DbgSystem sys, uint8_t op)
{
	return sys == DbgSystem::Snes ? IsTargetOpcode_Snes(op) : IsTargetOpcode_Gb(op);
}

static COLORREF MnemColor(DbgSystem sys, uint8_t op)
{
	if (sys == DbgSystem::Snes)
	{
		switch (op)
		{
			case 0x10: case 0x30: case 0x50: case 0x70:
			case 0x80: case 0x82: case 0x90:
			case 0xB0: case 0xD0: case 0xF0:
				return RGB(50, 100, 180);
			case 0x4C: case 0x5C: case 0x6C: case 0xDC:
			case 0x20: case 0x22: case 0x7C: case 0xFC:
				return RGB(170, 60, 60);
			case 0x60: case 0x6B: case 0x40:
				return RGB(140, 30, 30);
			case 0x48: case 0x68: case 0xDA: case 0xFA:
			case 0x5A: case 0x7A: case 0x8B: case 0xAB:
			case 0x0B: case 0x2B: case 0x4B:
			case 0x08: case 0x28:
				return RGB(120, 60, 140);
		}
		return RGB(20, 20, 20);
	}
	if (op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38 ||
	    op == 0xC2 || op == 0xCA || op == 0xD2 || op == 0xDA)
		return RGB(50, 100, 180);
	if (op == 0x18 || op == 0xC3 || op == 0xE9)
		return RGB(170, 60, 60);
	if (op == 0xCD || op == 0xC4 || op == 0xCC || op == 0xD4 || op == 0xDC ||
	    (op & 0xC7) == 0xC7)
		return RGB(170, 60, 60);
	if (op == 0xC9 || op == 0xD9 ||
	    op == 0xC0 || op == 0xC8 || op == 0xD0 || op == 0xD8)
		return RGB(140, 30, 30);
	if (op == 0xC5 || op == 0xD5 || op == 0xE5 || op == 0xF5 ||
	    op == 0xC1 || op == 0xD1 || op == 0xE1 || op == 0xF1)
		return RGB(120, 60, 140);
	return RGB(20, 20, 20);
}

struct FlowArrow
{
	int src_idx;   // row of the branch instruction (tail)
	int dst_idx;   // row of the branch target (head/arrowhead)
	int top_idx;   // min(src,dst)
	int bot_idx;   // max(src,dst)
	int lane;
};

// Visual style keyed off a stable seed (the branch source address) so an
// arrow keeps the same color/style regardless of scroll position. Cycles
// color + pen style + weight to keep crossing arrows distinct. Dashed/dotted
// GDI pens require width 1, so weight-2 entries are solid.
static void ArrowStyle(uint32_t seed, COLORREF *color, int *pen_style, int *width)
{
	static const struct { COLORREF c; int s; int w; } kStyles[] = {
		{ RGB( 60, 160,  80), PS_DOT,   1 },
		{ RGB( 60,  90, 200), PS_SOLID, 2 },
		{ RGB(200,  60,  60), PS_DASH,  1 },
		{ RGB(150,  60, 160), PS_SOLID, 1 },
		{ RGB(210, 140,  40), PS_DOT,   1 },
		{ RGB( 40, 160, 160), PS_SOLID, 2 },
	};
	const int n = (int)(sizeof(kStyles)/sizeof(kStyles[0]));
	const uint32_t h = seed * 2654435761u;          // Knuth multiplicative hash
	const int i = (int)((h >> 24) % (uint32_t)n);
	*color = kStyles[i].c; *pen_style = kStyles[i].s; *width = kStyles[i].w;
}

struct DisasmPanelState
{
	DbgSystem    sys              = DbgSystem::None;
	HFONT        font             = nullptr;
	HWND         lv               = nullptr;
	uint32_t     view_start_pc    = 0;
	bool         view_initialized = false;
	DisasmLine  *lines            = nullptr;
	int          line_count       = 0;
	int          line_cap         = 0;
	uint32_t    *targets          = nullptr;
	int          targets_count    = 0;
	int          targets_cap      = 0;
	uint32_t    *entries          = nullptr;
	int          entries_count    = 0;
	int          entries_cap      = 0;
	uint32_t    *section_starts   = nullptr;
	int          sections_count   = 0;
	int          sections_cap     = 0;
	FlowArrow   *arrows           = nullptr;
	int          arrows_count     = 0;
	int          arrows_cap       = 0;
	uint32_t     rclick_pc        = 0;
	bool         rclick_valid     = false;
	bool         rclick_has_branch = false;
	uint32_t     rclick_branch_target = 0;
	uint32_t     shown_bps_version = 0;
	uint32_t     shown_pc          = 0xFFFFFFFFu;
	bool         shown_paused      = false;
	bool         walk_complete     = false;
};

static void EnsureTargetsCap(DisasmPanelState *st, int need)
{
	if (st->targets_cap >= need) return;
	int new_cap = st->targets_cap > 0 ? st->targets_cap : 128;
	while (new_cap < need) new_cap *= 2;
	uint32_t *new_arr = (uint32_t *)malloc(sizeof(uint32_t) * new_cap);
	if (st->targets && st->targets_count > 0)
		memcpy(new_arr, st->targets, sizeof(uint32_t) * st->targets_count);
	free(st->targets);
	st->targets     = new_arr;
	st->targets_cap = new_cap;
}

static bool IsKnownTarget(DisasmPanelState *st, uint32_t addr)
{
	int lo = 0, hi = st->targets_count - 1;
	while (lo <= hi)
	{
		int mid = (lo + hi) / 2;
		if (st->targets[mid] == addr) return true;
		if (st->targets[mid] < addr) lo = mid + 1;
		else                          hi = mid - 1;
	}
	return false;
}

static void AddKnownTarget(DisasmPanelState *st, uint32_t addr)
{
	int lo = 0, hi = st->targets_count;
	while (lo < hi)
	{
		int mid = (lo + hi) / 2;
		if (st->targets[mid] < addr) lo = mid + 1;
		else                          hi = mid;
	}
	if (lo < st->targets_count && st->targets[lo] == addr) return;
	EnsureTargetsCap(st, st->targets_count + 1);
	memmove(st->targets + lo + 1, st->targets + lo, sizeof(uint32_t) * (st->targets_count - lo));
	st->targets[lo] = addr;
	st->targets_count++;
}

static void EnsureEntriesCap(DisasmPanelState *st, int need)
{
	if (st->entries_cap >= need) return;
	int new_cap = st->entries_cap > 0 ? st->entries_cap : 64;
	while (new_cap < need) new_cap *= 2;
	uint32_t *new_arr = (uint32_t *)malloc(sizeof(uint32_t) * new_cap);
	if (st->entries && st->entries_count > 0)
		memcpy(new_arr, st->entries, sizeof(uint32_t) * st->entries_count);
	free(st->entries);
	st->entries     = new_arr;
	st->entries_cap = new_cap;
}

static bool IsKnownEntry(DisasmPanelState *st, uint32_t addr)
{
	int lo = 0, hi = st->entries_count - 1;
	while (lo <= hi)
	{
		int mid = (lo + hi) / 2;
		if (st->entries[mid] == addr) return true;
		if (st->entries[mid] < addr) lo = mid + 1;
		else                          hi = mid - 1;
	}
	return false;
}

static void AddKnownEntry(DisasmPanelState *st, uint32_t addr)
{
	int lo = 0, hi = st->entries_count;
	while (lo < hi)
	{
		int mid = (lo + hi) / 2;
		if (st->entries[mid] < addr) lo = mid + 1;
		else                          hi = mid;
	}
	if (lo < st->entries_count && st->entries[lo] == addr) return;
	EnsureEntriesCap(st, st->entries_count + 1);
	memmove(st->entries + lo + 1, st->entries + lo, sizeof(uint32_t) * (st->entries_count - lo));
	st->entries[lo] = addr;
	st->entries_count++;
}

static void EnsureSectionsCap(DisasmPanelState *st, int need)
{
	if (st->sections_cap >= need) return;
	int new_cap = st->sections_cap > 0 ? st->sections_cap : 64;
	while (new_cap < need) new_cap *= 2;
	uint32_t *new_arr = (uint32_t *)malloc(sizeof(uint32_t) * new_cap);
	if (st->section_starts && st->sections_count > 0)
		memcpy(new_arr, st->section_starts, sizeof(uint32_t) * st->sections_count);
	free(st->section_starts);
	st->section_starts = new_arr;
	st->sections_cap   = new_cap;
}

static void AppendSectionStart(DisasmPanelState *st, uint32_t pc)
{
	EnsureSectionsCap(st, st->sections_count + 1);
	st->section_starts[st->sections_count++] = pc;
}

static bool IsCallOpcode(DbgSystem sys, uint8_t op)
{
	if (sys == DbgSystem::Snes)
		return op == 0x20 || op == 0x22 || op == 0xFC;
	return op == 0xCD || op == 0xC4 || op == 0xCC || op == 0xD4 || op == 0xDC
	    || (op & 0xC7) == 0xC7;
}

static bool IsPrologueOpcode(DbgSystem sys, uint8_t op)
{
	if (sys == DbgSystem::Snes)
		return op == 0x48 || op == 0x8B || op == 0x0B || op == 0x4B
		    || op == 0x08 || op == 0xDA || op == 0x5A;
	return op == 0xF5 || op == 0xC5 || op == 0xD5 || op == 0xE5;
}

static void SeedEntries(DisasmPanelState *st)
{
	if (st->sys == DbgSystem::Gb)
	{
		static const uint32_t gb_vectors[] = {
			0x0000, 0x0008, 0x0010, 0x0018, 0x0020, 0x0028, 0x0030, 0x0038,
			0x0040, 0x0048, 0x0050, 0x0058, 0x0060, 0x0100
		};
		for (uint32_t v : gb_vectors) AddKnownEntry(st, v);
		return;
	}

	static const uint32_t snes_vector_locs[] = {
		0x00FFE4, 0x00FFE6, 0x00FFE8, 0x00FFEA, 0x00FFEE,
		0x00FFF4, 0x00FFF8, 0x00FFFA, 0x00FFFC, 0x00FFFE
	};
	for (uint32_t loc : snes_vector_locs)
	{
		const uint8_t lo = SnesBackend::ReadByte(loc);
		const uint8_t hi = SnesBackend::ReadByte(loc + 1);
		const uint16_t ptr = (uint16_t)(lo | (hi << 8));
		if (ptr == 0x0000 || ptr == 0xFFFF) continue;
		AddKnownEntry(st, (uint32_t)ptr);
	}
}

static bool GetBranchTargetForPC(DbgSystem sys, uint32_t pc, uint32_t *out_target)
{
	if (sys == DbgSystem::Snes)
	{
		DisasmResult65816 r = {};
		SnesBackend::Disassemble(pc, &r);
		if (r.has_branch) { *out_target = r.branch_target; return true; }
	}
	else
	{
		DisasmResultGb r = {};
		GbBackend::Disassemble(pc, &r);
		if (r.has_branch) { *out_target = r.branch_target & 0xFFFFu; return true; }
	}
	return false;
}

static void EnsureCap(DisasmPanelState *st, int need)
{
	if (st->line_cap >= need) return;
	int new_cap = st->line_cap > 0 ? st->line_cap : 256;
	while (new_cap < need) new_cap *= 2;
	DisasmLine *new_arr = (DisasmLine *)malloc(sizeof(DisasmLine) * new_cap);
	if (st->lines && st->line_count > 0)
		memcpy(new_arr, st->lines, sizeof(DisasmLine) * st->line_count);
	free(st->lines);
	st->lines    = new_arr;
	st->line_cap = new_cap;
}

static void PushLine(DisasmPanelState *st, const DisasmLine &ln)
{
	EnsureCap(st, st->line_count + 1);
	st->lines[st->line_count++] = ln;
}

static void ClearLines(DisasmPanelState *st)
{
	st->line_count = 0;
}

static bool IsCtlFlowEnd(DbgSystem sys, uint8_t op)
{
	if (sys == DbgSystem::Snes)
		return op == 0x60 || op == 0x6B || op == 0x40;
	return op == 0xC9 || op == 0xD9;
}

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
	return GbBackend::CurrentDisplayPC();
}

static uint32_t AdvancePC(DbgSystem sys, uint32_t pc, uint8_t len)
{
	if (sys == DbgSystem::Snes)
		return (pc + len) & 0xFFFFFFu;

	const uint32_t cpu = pc & 0xFFFFu;
	const uint32_t new_cpu = cpu + len;
	if (new_cpu >= 0x10000u) return 0xFFFFu;
	return new_cpu;
}

static int DisasmOne(DbgSystem sys, uint32_t pc, char *line, size_t cap,
                     uint8_t *out_bytes, int *out_byte_count,
                     bool *out_has_eff = nullptr,
                     uint32_t *out_eff_addr = nullptr,
                     uint8_t *out_eff_val = nullptr,
                     bool *out_has_branch = nullptr,
                     uint32_t *out_branch_target = nullptr)
{
	if (out_has_eff)        *out_has_eff        = false;
	if (out_eff_addr)       *out_eff_addr       = 0;
	if (out_eff_val)        *out_eff_val        = 0;
	if (out_has_branch)     *out_has_branch     = false;
	if (out_branch_target)  *out_branch_target  = 0;

	if (sys == DbgSystem::Snes)
	{
		DisasmResult65816 r = {};
		uint8_t len = SnesBackend::Disassemble(pc, &r);
		_snprintf_s(line, cap, _TRUNCATE, "%-4s %s", r.mnemonic, r.operand);
		for (int i = 0; i < len && i < 4; i++) out_bytes[i] = r.bytes[i];
		*out_byte_count = len;
		if (out_has_eff)       *out_has_eff       = r.has_effective;
		if (out_eff_addr)      *out_eff_addr      = r.effective_addr;
		if (out_eff_val)       *out_eff_val       = r.effective_value;
		if (out_has_branch)    *out_has_branch    = r.has_branch;
		if (out_branch_target) *out_branch_target = r.branch_target;
		return len;
	}
	DisasmResultGb r = {};
	uint8_t len = GbBackend::Disassemble(pc, &r);
	_snprintf_s(line, cap, _TRUNCATE, "%-4s %s", r.mnemonic, r.operand);
	for (int i = 0; i < len && i < 3; i++) out_bytes[i] = r.bytes[i];
	*out_byte_count = len;
	if (out_has_eff)       *out_has_eff       = r.has_effective;
	if (out_eff_addr)      *out_eff_addr      = (uint32_t)r.effective_addr;
	if (out_eff_val)       *out_eff_val       = r.effective_value;
	if (out_has_branch)    *out_has_branch    = r.has_branch;
	if (out_branch_target) *out_branch_target = (uint32_t)r.branch_target;
	return len;
}

static void ResetLines(DisasmPanelState *st, uint32_t new_start_pc)
{
	st->view_start_pc  = new_start_pc;
	st->walk_complete  = false;
	ClearLines(st);
	st->targets_count  = 0;
	st->entries_count  = 0;
	st->sections_count = 0;
	SeedEntries(st);
	AppendSectionStart(st, new_start_pc);
	DisasmLine first{};
	first.pc            = new_start_pc;
	first.length        = 0;
	first.is_sub_start  = false;
	first.is_label_row  = false;
	first.section_index = 0;
	PushLine(st, first);
	ListView_SetItemCountEx(st->lv, 5000000, LVSICF_NOINVALIDATEALL);
	InvalidateRect(st->lv, NULL, FALSE);
}

static int LastInstrIdx(DisasmPanelState *st)
{
	for (int i = st->line_count - 1; i >= 0; i--)
		if (!st->lines[i].is_label_row) return i;
	return -1;
}

static uint32_t ComputeBranchTargetDisplay(DbgSystem sys, uint32_t /*caller_pc*/, uint32_t raw_branch)
{
	if (sys == DbgSystem::Snes)
		return raw_branch & 0xFFFFFF;
	return raw_branch & 0xFFFFu;
}

static void EnsureLineCached(DisasmPanelState *st, int index)
{
	if (st->line_count == 0) return;
	if (st->walk_complete) return;
	while (st->line_count <= index)
	{
		const int last = LastInstrIdx(st);
		if (last < 0) break;

		const uint32_t back_pc  = st->lines[last].pc;
		const uint32_t back_sec = st->lines[last].section_index;

		uint32_t next_pc;
		uint32_t next_sec;
		bool     ends_section;

		if (st->lines[last].is_unmapped)
		{
			UnmappedRegion ur{};
			IsInUnmappedRegion(st->sys, back_pc, &ur);
			next_pc      = back_pc + ur.size;
			next_sec     = back_sec + 1;
			ends_section = true;
			if (next_pc == back_pc) { st->walk_complete = true; break; }
		}
		else
		{
			uint8_t back_length = st->lines[last].length;
			char ln[160]; uint8_t bytes[4]; int byte_count;
			bool has_branch = false; uint32_t raw_branch = 0;
			int len = DisasmOne(st->sys, back_pc, ln, sizeof(ln), bytes, &byte_count,
			                    nullptr, nullptr, nullptr,
			                    &has_branch, &raw_branch);
			if (back_length == 0)
			{
				back_length = (uint8_t)len;
				st->lines[last].length = back_length;
			}

			if (has_branch && IsTargetOpcode(st->sys, bytes[0]))
			{
				const uint32_t tgt_display = ComputeBranchTargetDisplay(st->sys, back_pc, raw_branch);
				AddKnownTarget(st, tgt_display);
				if (IsCallOpcode(st->sys, bytes[0]))
					AddKnownEntry(st, tgt_display);
			}

			ends_section = IsCtlFlowEnd(st->sys, bytes[0]);
			next_pc      = AdvancePC(st->sys, back_pc, back_length);
			if (next_pc == back_pc) { st->walk_complete = true; break; }
			next_sec     = ends_section ? (back_sec + 1) : back_sec;

			if (ends_section)
			{
				char pln[160]; uint8_t pb[4]; int pc_count;
				DisasmOne(st->sys, next_pc, pln, sizeof(pln), pb, &pc_count);
				if (IsPrologueOpcode(st->sys, pb[0]))
					AddKnownEntry(st, next_pc);
			}
		}

		UnmappedRegion ur{};
		if (IsInUnmappedRegion(st->sys, next_pc, &ur))
		{
			DisasmLine umrow{};
			umrow.pc            = ur.pc;
			umrow.length        = 0;
			umrow.is_sub_start  = true;
			umrow.is_label_row  = false;
			umrow.is_unmapped   = true;
			umrow.section_index = next_sec;
			PushLine(st, umrow);
			AppendSectionStart(st, ur.pc);
			continue;
		}

		if (IsKnownTarget(st, next_pc))
		{
			const bool already_labeled = st->line_count > 0
			    && st->lines[st->line_count - 1].is_label_row
			    && st->lines[st->line_count - 1].pc == next_pc;
			if (!already_labeled)
			{
				DisasmLine label{};
				label.pc            = next_pc;
				label.length        = 0;
				label.is_sub_start  = false;
				label.is_label_row  = true;
				label.section_index = next_sec;
				PushLine(st, label);
			}
		}

		DisasmLine next{};
		next.pc            = next_pc;
		next.length        = 0;
		next.is_sub_start  = ends_section;
		next.is_label_row  = false;
		next.section_index = next_sec;
		PushLine(st, next);
		if (ends_section) AppendSectionStart(st, next_pc);
	}

	if (st->walk_complete && st->lv)
		ListView_SetItemCountEx(st->lv, st->line_count, LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
}

static int FindIndexForPC(DisasmPanelState *st, uint32_t pc)
{
	for (int i = 0; i < st->line_count; i++)
	{
		if (st->lines[i].is_label_row) continue;
		if (st->lines[i].pc == pc) return i;
		if (st->lines[i].is_unmapped)
		{
			UnmappedRegion ur{};
			if (IsInUnmappedRegion(st->sys, st->lines[i].pc, &ur)
			    && pc >= ur.pc && pc < ur.pc + ur.size)
				return i;
		}
	}
	return -1;
}

static int FindIndexForPCInRange(DisasmPanelState *st, uint32_t pc, int lo, int hi)
{
	if (lo < 0) lo = 0;
	if (hi > st->line_count) hi = st->line_count;
	for (int i = lo; i < hi; i++)
	{
		if (st->lines[i].is_label_row || st->lines[i].is_unmapped) continue;
		if (st->lines[i].pc == pc) return i;
	}
	return -1;
}

static bool IsConditionalBranch(DbgSystem sys, uint8_t op)
{
	if (sys == DbgSystem::Snes)
		return op == 0x10 || op == 0x30 || op == 0x50 || op == 0x70
		    || op == 0x90 || op == 0xB0 || op == 0xD0 || op == 0xF0;
	return op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38
	    || op == 0xC2 || op == 0xCA || op == 0xD2 || op == 0xDA
	    || op == 0xC4 || op == 0xCC || op == 0xD4 || op == 0xDC;
}

static bool IsIndirectBranch(DbgSystem sys, uint8_t op)
{
	if (sys == DbgSystem::Snes)
		return op == 0x6C || op == 0x7C || op == 0xDC || op == 0xFC;
	return op == 0xE9;
}

// A branch whose static target we want to draw a flow arrow for: relative
// branches, absolute jumps, and calls with a known immediate target. Excludes
// returns, indirect jumps, and RST vectors.
static bool GetFlowBranch(DisasmPanelState *st, uint32_t pc, uint32_t *out_target, bool *out_cond)
{
	char ln[160]; uint8_t bytes[4]; int byte_count;
	bool has_branch = false; uint32_t raw_branch = 0;
	DisasmOne(st->sys, pc, ln, sizeof(ln), bytes, &byte_count,
	          nullptr, nullptr, nullptr, &has_branch, &raw_branch);
	if (!has_branch) return false;

	const uint8_t op = bytes[0];
	if (IsIndirectBranch(st->sys, op)) return false;
	if (st->sys == DbgSystem::Gb && (op & 0xC7) == 0xC7) return false; // RST

	*out_target = ComputeBranchTargetDisplay(st->sys, pc, raw_branch);
	*out_cond   = IsConditionalBranch(st->sys, op);
	return true;
}

static void EnsureArrowsCap(DisasmPanelState *st, int need)
{
	if (st->arrows_cap >= need) return;
	int new_cap = st->arrows_cap > 0 ? st->arrows_cap : 64;
	while (new_cap < need) new_cap *= 2;
	FlowArrow *new_arr = (FlowArrow *)malloc(sizeof(FlowArrow) * new_cap);
	if (st->arrows && st->arrows_count > 0)
		memcpy(new_arr, st->arrows, sizeof(FlowArrow) * st->arrows_count);
	free(st->arrows);
	st->arrows     = new_arr;
	st->arrows_cap = new_cap;
}

static void BuildFlowArrows(DisasmPanelState *st)
{
	st->arrows_count = 0;
	if (!st->lv || st->line_count <= 0) return;

	const int top      = ListView_GetTopIndex(st->lv);
	const int per_page = ListView_GetCountPerPage(st->lv);
	if (per_page <= 0) return;

	const int vis_lo = top;
	int vis_hi = top + per_page;
	if (vis_hi > st->line_count - 1) vis_hi = st->line_count - 1;

	// Scan beyond the viewport so an arrow with one endpoint off-screen is
	// still found. An arrow is drawn when AT LEAST ONE endpoint is visible;
	// the off-screen end simply runs to the viewport edge (no cap). Arrows
	// with both endpoints off-screen are dropped (no bare verticals).
	const int EXTRA = 256;
	int scan_lo = vis_lo - EXTRA; if (scan_lo < 0) scan_lo = 0;
	int scan_hi = vis_hi + EXTRA; if (scan_hi > st->line_count) scan_hi = st->line_count;

	for (int i = scan_lo; i < scan_hi; i++)
	{
		const DisasmLine &ln = st->lines[i];
		if (ln.is_label_row || ln.is_unmapped) continue;

		uint32_t tgt; bool cond;
		if (!GetFlowBranch(st, ln.pc, &tgt, &cond)) continue;

		const int dst = FindIndexForPCInRange(st, tgt, scan_lo, scan_hi);
		if (dst < 0 || dst == i) continue;

		const bool src_vis = (i   >= vis_lo && i   <= vis_hi);
		const bool dst_vis = (dst >= vis_lo && dst <= vis_hi);
		if (!src_vis && !dst_vis) continue;

		const int a_top = i < dst ? i : dst;
		const int a_bot = i < dst ? dst : i;

		EnsureArrowsCap(st, st->arrows_count + 1);
		FlowArrow &fa = st->arrows[st->arrows_count++];
		fa.src_idx = i;
		fa.dst_idx = dst;
		fa.top_idx = a_top;
		fa.bot_idx = a_bot;
		fa.lane    = 0;
	}

	// Shorter spans first so tight loops nest into the inner lanes.
	for (int i = 1; i < st->arrows_count; i++)
	{
		FlowArrow key = st->arrows[i];
		const int key_span = key.bot_idx - key.top_idx;
		int j = i - 1;
		while (j >= 0 && (st->arrows[j].bot_idx - st->arrows[j].top_idx) > key_span)
		{
			st->arrows[j + 1] = st->arrows[j];
			j--;
		}
		st->arrows[j + 1] = key;
	}

	const int MAXLANES = 6;
	for (int i = 0; i < st->arrows_count; i++)
	{
		int lane = 0;
		bool clash = true;
		while (clash && lane < MAXLANES)
		{
			clash = false;
			for (int j = 0; j < i; j++)
			{
				if (st->arrows[j].lane != lane) continue;
				if (st->arrows[i].top_idx <= st->arrows[j].bot_idx
				 && st->arrows[j].top_idx <= st->arrows[i].bot_idx)
				{ clash = true; break; }
			}
			if (clash) lane++;
		}
		if (lane >= MAXLANES) lane = MAXLANES - 1;
		st->arrows[i].lane = lane;
	}
}

static int FindOrExtendForPC(DisasmPanelState *st, uint32_t pc, int max_rows);

static uint32_t FindGoodStartAddr(DbgSystem sys, uint32_t target_pc, int lookback_bytes)
{
	const uint16_t target_lo = (uint16_t)(target_pc & 0xFFFF);
	if (target_lo < (uint16_t)lookback_bytes)
		return target_pc;

	const uint32_t bank_mask = (sys == DbgSystem::Snes) ? 0xFF0000u : 0u;

	for (int back = lookback_bytes; back >= 1; back--)
	{
		const uint16_t start_lo = (uint16_t)(target_lo - back);
		const uint32_t start    = (target_pc & bank_mask) | start_lo;

		uint32_t pc = start;
		bool aligned = false;
		for (int i = 0; i < 96; i++)
		{
			if (pc == target_pc) { aligned = true; break; }
			const uint16_t pc_lo = (uint16_t)(pc & 0xFFFF);
			if (pc_lo > target_lo) break;
			char ln[160]; uint8_t b[4]; int c;
			int len = DisasmOne(sys, pc, ln, sizeof(ln), b, &c);
			pc = AdvancePC(sys, pc, (uint8_t)len);
		}
		if (aligned) return start;
	}
	return target_pc;
}

static void BuildLabelRowName(DisasmPanelState *st, uint32_t pc, char *out, size_t cap)
{
	const char *label = LookupLabel(st->sys, pc);
	if (label)
	{
		_snprintf_s(out, cap, _TRUNCATE, "%s:", label);
		return;
	}
	if (st->sys == DbgSystem::Snes)
		_snprintf_s(out, cap, _TRUNCATE, "sub_%06X:", (unsigned)(pc & 0xFFFFFFu));
	else
		_snprintf_s(out, cap, _TRUNCATE, "sub_%04X:", (unsigned)(pc & 0xFFFFu));
}

struct OperandText
{
	char text[224];
	int  suffix_offset;
};

static void BuildOperandText(DisasmPanelState *st, uint32_t row_pc, OperandText *out_buf)
{
	char line[160]; uint8_t bytes[4]; int byte_count;
	bool has_eff = false; uint32_t eff_addr = 0; uint8_t eff_val = 0;
	DisasmOne(st->sys, row_pc, line, sizeof(line), bytes, &byte_count,
	          &has_eff, &eff_addr, &eff_val);

	const char *operand_start = strchr(line, ' ');
	if (operand_start)
		while (*operand_start == ' ') operand_start++;
	else
		operand_start = "";

	uint32_t lookup_addr = 0;
	bool     have_addr   = false;
	const char *p = operand_start;
	while (*p)
	{
		if (*p == '$' && (p == operand_start || p[-1] != '#'))
		{
			const char *digits = p + 1;
			uint32_t val = 0;
			int n = 0;
			while (n < 6)
			{
				const char c = *digits;
				int d;
				if (c >= '0' && c <= '9')      d = c - '0';
				else if (c >= 'a' && c <= 'f') d = 10 + c - 'a';
				else if (c >= 'A' && c <= 'F') d = 10 + c - 'A';
				else break;
				val = (val << 4) | (uint32_t)d;
				digits++;
				n++;
			}
			if (n >= 4)
			{
				lookup_addr = val;
				have_addr   = true;
				break;
			}
		}
		p++;
	}

	const char *label = NULL;
	if (has_eff)
		label = LookupLabel(st->sys, eff_addr);
	else if (have_addr)
		label = LookupLabel(st->sys, lookup_addr);

	char suffix[96] = "";
	if (has_eff)
	{
		if (label)
			_snprintf_s(suffix, 96, _TRUNCATE, " [%s] = $%02X", label, eff_val);
		else if (st->sys == DbgSystem::Snes)
			_snprintf_s(suffix, 96, _TRUNCATE, " [$%06X] = $%02X", eff_addr & 0xFFFFFF, eff_val);
		else
			_snprintf_s(suffix, 96, _TRUNCATE, " [$%04X] = $%02X", eff_addr & 0xFFFF, eff_val);
	}
	else if (label)
	{
		_snprintf_s(suffix, 96, _TRUNCATE, " [%s]", label);
	}

	out_buf->suffix_offset = suffix[0] ? (int)strlen(operand_start) : -1;
	_snprintf_s(out_buf->text, sizeof(out_buf->text), _TRUNCATE, "%s%s", operand_start, suffix);
}

static void OnGetDispInfo(DisasmPanelState *st, NMLVDISPINFOA *di)
{
	const int idx = di->item.iItem;
	EnsureLineCached(st, idx);

	if (idx < 0 || idx >= st->line_count) return;

	DisasmLine &ln = st->lines[idx];

	if (ln.is_label_row)
	{
		if (di->item.mask & LVIF_TEXT)
			di->item.pszText[0] = 0;
		return;
	}

	if (ln.is_unmapped)
	{
		if (di->item.mask & LVIF_TEXT)
		{
			char *out = di->item.pszText;
			const int cap = di->item.cchTextMax;
			UnmappedRegion ur{};
			IsInUnmappedRegion(st->sys, ln.pc, &ur);
			const uint32_t end_addr = ur.pc + ur.size - 1;
			switch (di->item.iSubItem)
			{
				case COL_MARGIN:
				case COL_FLOW:
					out[0] = 0;
					break;
				case COL_ADDR:
					_snprintf_s(out, cap, _TRUNCATE, "%04x-%04x",
					            (unsigned)(ur.pc & 0xFFFF),
					            (unsigned)(end_addr & 0xFFFF));
					break;
				case COL_BYTES:
					out[0] = 0;
					break;
				case COL_MNEM:
					out[0] = 0;
					break;
				case COL_OPERAND:
					_snprintf_s(out, cap, _TRUNCATE, "%s", ur.label ? ur.label : "unmapped");
					break;
			}
		}
		return;
	}

	if (ln.length == 0)
	{
		char buf[160]; uint8_t b[4]; int c;
		int len = DisasmOne(st->sys, ln.pc, buf, sizeof(buf), b, &c);
		ln.length = (uint8_t)len;
	}

	char line[160]; uint8_t bytes[4]; int byte_count;
	bool     has_eff  = false;
	uint32_t eff_addr = 0;
	uint8_t  eff_val  = 0;
	DisasmOne(st->sys, ln.pc, line, sizeof(line), bytes, &byte_count,
	          &has_eff, &eff_addr, &eff_val);

	if (di->item.mask & LVIF_TEXT)
	{
		char *out = di->item.pszText;
		const int cap = di->item.cchTextMax;
		switch (di->item.iSubItem)
		{
			case COL_MARGIN:
			case COL_FLOW:
				out[0] = 0;
				break;
			case COL_ADDR:
				if (st->sys == DbgSystem::Snes)
					_snprintf_s(out, cap, _TRUNCATE, "%06X", (unsigned)(ln.pc & 0xFFFFFFu));
				else
					_snprintf_s(out, cap, _TRUNCATE, "%04X", (unsigned)(ln.pc & 0xFFFFu));
				break;
			case COL_BYTES:
				if (byte_count == 1)      _snprintf_s(out, cap, _TRUNCATE, "%02X", bytes[0]);
				else if (byte_count == 2) _snprintf_s(out, cap, _TRUNCATE, "%02X %02X", bytes[0], bytes[1]);
				else if (byte_count == 3) _snprintf_s(out, cap, _TRUNCATE, "%02X %02X %02X", bytes[0], bytes[1], bytes[2]);
				else                      _snprintf_s(out, cap, _TRUNCATE, "%02X %02X %02X %02X", bytes[0], bytes[1], bytes[2], bytes[3]);
				break;
			case COL_MNEM:
			{
				const char *sp = strchr(line, ' ');
				int mlen = sp ? (int)(sp - line) : (int)strlen(line);
				if (mlen >= cap) mlen = cap - 1;
				memcpy(out, line, mlen);
				out[mlen] = 0;
				break;
			}
			case COL_OPERAND:
			{
				OperandText ot;
				BuildOperandText(st, ln.pc, &ot);
				_snprintf_s(out, cap, _TRUNCATE, "%s", ot.text);
				break;
			}
		}
	}
}

static bool RowHasExecBp(DisasmPanelState *st, const DisasmLine &row)
{
	if (row.is_label_row || row.is_unmapped) return false;
	const uint8_t  bank = BpBankForPC(st->sys, row.pc);
	const uint16_t addr = (uint16_t)(row.pc & 0xFFFF);
	return gDebugger.HasExecBreakpoint(st->sys, bank, addr);
}

static LRESULT OnCustomDraw(DisasmPanelState *st, NMLVCUSTOMDRAW *cd)
{
	switch (cd->nmcd.dwDrawStage)
	{
		case CDDS_PREPAINT:
			BuildFlowArrows(st);
			return CDRF_NOTIFYITEMDRAW;

		case CDDS_ITEMPREPAINT:
		{
			const int idx = (int)cd->nmcd.dwItemSpec;
			if (idx < st->line_count)
			{
				const DisasmLine &row = st->lines[idx];
				const bool in_func = (int)row.section_index < st->sections_count
				    && IsKnownEntry(st, st->section_starts[row.section_index]);
				if (row.is_label_row)
				{
					cd->clrTextBk = in_func ? RGB(255, 240, 240) : RGB(255, 255, 255);
					cd->clrText   = RGB(0, 96, 0);
				}
				else if (row.is_unmapped)
				{
					cd->clrTextBk = RGB(240, 240, 240);
					cd->clrText   = RGB(110, 110, 110);
				}
				else
				{
					const uint32_t cur_pc = GetCurrentPC(st->sys);
					if (RowHasExecBp(st, row))
					{
						cd->clrTextBk = RGB(200, 40, 40);
						cd->clrText   = RGB(255, 255, 255);
					}
					else if (Settings.Paused && row.pc == cur_pc)
					{
						cd->clrTextBk = RGB(255, 230, 90);
						cd->clrText   = RGB(0, 0, 0);
					}
					else if (in_func)
					{
						cd->clrTextBk = RGB(255, 240, 240);
					}
					else
					{
						cd->clrTextBk = RGB(255, 255, 255);
					}
				}
			}
			return CDRF_NOTIFYPOSTPAINT | CDRF_NOTIFYSUBITEMDRAW | CDRF_NEWFONT;
		}

		case CDDS_ITEMPREPAINT | CDDS_SUBITEM:
		{
			const int idx = (int)cd->nmcd.dwItemSpec;
			const int sub = cd->iSubItem;
			if (idx < st->line_count)
			{
				const DisasmLine &row = st->lines[idx];
				const bool in_func = (int)row.section_index < st->sections_count
				    && IsKnownEntry(st, st->section_starts[row.section_index]);
				if (row.is_label_row)
				{
					cd->clrTextBk = in_func ? RGB(255, 240, 240) : RGB(255, 255, 255);
					cd->clrText   = RGB(0, 96, 0);
				}
				else if (row.is_unmapped)
				{
					cd->clrTextBk = RGB(240, 240, 240);
					cd->clrText   = RGB(110, 110, 110);
				}
				else
				{
					const uint32_t cur_pc = GetCurrentPC(st->sys);
					const bool bp_col = RowHasExecBp(st, row)
					    && sub != COL_MARGIN && sub != COL_FLOW;
					if (bp_col)
					{
						cd->clrTextBk = RGB(200, 40, 40);
						cd->clrText   = RGB(255, 255, 255);
						cd->nmcd.uItemState &= ~(CDIS_SELECTED | CDIS_FOCUS);
					}
					else if (Settings.Paused && row.pc == cur_pc)
					{
						cd->clrTextBk = RGB(255, 230, 90);
						cd->clrText   = RGB(0, 0, 0);
					}
					else if (in_func)
					{
						cd->clrTextBk = RGB(255, 240, 240);
						cd->clrText   = RGB(20, 20, 20);
					}
					else
					{
						cd->clrTextBk = RGB(255, 255, 255);
						cd->clrText   = RGB(20, 20, 20);
					}
					if (!bp_col && sub == COL_MNEM)
					{
						char ln[160]; uint8_t b[4]; int c;
						DisasmOne(st->sys, row.pc, ln, sizeof(ln), b, &c);
						cd->clrText = MnemColor(st->sys, b[0]);
					}
					if (sub == COL_OPERAND)
						return CDRF_SKIPDEFAULT;
				}
				if (sub == COL_MARGIN || sub == COL_FLOW)
					cd->nmcd.uItemState &= ~(CDIS_SELECTED | CDIS_FOCUS);
			}
			return CDRF_NEWFONT;
		}

		case CDDS_ITEMPOSTPAINT:
		{
			const int idx = (int)cd->nmcd.dwItemSpec;
			if (idx >= st->line_count) return CDRF_DODEFAULT;

			// Code-flow arrow gutter — drawn for every row type so vertical
			// lanes stay continuous across label / unmapped rows.
			if (st->arrows_count > 0)
			{
				RECT fr = {};
				ListView_GetSubItemRect(st->lv, idx, COL_FLOW, LVIR_BOUNDS, &fr);
				const int cy         = (fr.top + fr.bottom) / 2;
				const int flow_right = fr.right - 1;
				const int LANE_W     = 9;
				const int HRUN       = 16;   // horizontal run from innermost lane to the arrowhead
				HDC dc = cd->nmcd.hdc;

				for (int ai = 0; ai < st->arrows_count; ai++)
				{
					const FlowArrow &a = st->arrows[ai];
					if (idx < a.top_idx || idx > a.bot_idx) continue;

					int x = flow_right - HRUN - a.lane * LANE_W;
					if (x < fr.left + 2) x = fr.left + 2;

					COLORREF col; int ps; int w;
					ArrowStyle(st->lines[a.src_idx].pc, &col, &ps, &w);

					HPEN pen  = CreatePen(ps, w, col);
					HPEN oldP = (HPEN)SelectObject(dc, pen);

					const int vy_top = (idx == a.top_idx) ? cy : fr.top;
					const int vy_bot = (idx == a.bot_idx) ? cy : fr.bottom;
					MoveToEx(dc, x, vy_top, NULL);
					LineTo(dc, x, vy_bot);

					if (idx == a.src_idx)        // tail — plain connector to the address
					{
						MoveToEx(dc, x, cy, NULL);
						LineTo(dc, flow_right, cy);
					}
					if (idx == a.dst_idx)        // head — connector + filled arrowhead
					{
						MoveToEx(dc, x, cy, NULL);
						LineTo(dc, flow_right, cy);
					}

					SelectObject(dc, oldP);
					DeleteObject(pen);

					if (idx == a.dst_idx)
					{
						POINT tri[3] = {
							{ flow_right - 6, cy - 4 },
							{ flow_right,     cy     },
							{ flow_right - 6, cy + 4 }
						};
						HBRUSH fill = CreateSolidBrush(col);
						HPEN   edge = CreatePen(PS_SOLID, 1, col);
						HBRUSH oldB = (HBRUSH)SelectObject(dc, fill);
						HPEN   oldE = (HPEN)SelectObject(dc, edge);
						Polygon(dc, tri, 3);
						SelectObject(dc, oldE);
						SelectObject(dc, oldB);
						DeleteObject(edge);
						DeleteObject(fill);
					}
				}
			}

			if (st->lines[idx].is_label_row)
			{
				RECT lr = {};
				ListView_GetItemRect(st->lv, idx, &lr, LVIR_BOUNDS);
				RECT ar = {};
				ListView_GetSubItemRect(st->lv, idx, COL_ADDR, LVIR_BOUNDS, &ar);

				char name[96];
				BuildLabelRowName(st, st->lines[idx].pc, name, sizeof(name));

				HDC dc = cd->nmcd.hdc;
				const int prev_bk = SetBkMode(dc, TRANSPARENT);
				SetTextColor(dc, RGB(0, 96, 0));
				TEXTMETRICA tm; GetTextMetricsA(dc, &tm);
				const int y = lr.top + ((lr.bottom - lr.top) - tm.tmHeight) / 2;
				const int x = ar.left + 4;
				RECT clip = { ar.left, lr.top, lr.right, lr.bottom };
				ExtTextOutA(dc, x, y, ETO_CLIPPED, &clip, name, (UINT)strlen(name), NULL);
				SetBkMode(dc, prev_bk);
				return CDRF_DODEFAULT;
			}

			if (st->lines[idx].is_unmapped)
				return CDRF_DODEFAULT;

			RECT row_rect = {};
			ListView_GetItemRect(st->lv, idx, &row_rect, LVIR_BOUNDS);
			const int margin_w = ListView_GetColumnWidth(st->lv, COL_MARGIN);
			RECT mr = { row_rect.left, row_rect.top,
			            row_rect.left + margin_w, row_rect.bottom };

			const uint32_t row_pc = st->lines[idx].pc;
			const uint32_t cur_pc = GetCurrentPC(st->sys);

			HDC dc = cd->nmcd.hdc;

			{
				RECT op_rect = {};
				ListView_GetSubItemRect(st->lv, idx, COL_OPERAND, LVIR_BOUNDS, &op_rect);

				const bool in_func = (int)st->lines[idx].section_index < st->sections_count
				    && IsKnownEntry(st, st->section_starts[st->lines[idx].section_index]);
				const bool is_pc       = Settings.Paused && row_pc == cur_pc;
				const bool is_selected = (ListView_GetItemState(st->lv, idx, LVIS_SELECTED) & LVIS_SELECTED) != 0;
				const bool has_bp      = RowHasExecBp(st, st->lines[idx]);

				COLORREF bg_color;
				if      (has_bp)      bg_color = RGB(200, 40, 40);
				else if (is_selected) bg_color = GetSysColor(COLOR_HIGHLIGHT);
				else if (is_pc)       bg_color = RGB(255, 230, 90);
				else if (in_func)     bg_color = RGB(255, 240, 240);
				else                  bg_color = RGB(255, 255, 255);

				HBRUSH bg = CreateSolidBrush(bg_color);
				FillRect(dc, &op_rect, bg);
				DeleteObject(bg);

				OperandText ot;
				BuildOperandText(st, row_pc, &ot);

				const int prev_bk_mode = SetBkMode(dc, TRANSPARENT);

				TEXTMETRICA tm; GetTextMetricsA(dc, &tm);
				int y = op_rect.top + ((op_rect.bottom - op_rect.top) - tm.tmHeight) / 2;
				int x = op_rect.left + 4;

				const int total_len = (int)strlen(ot.text);
				const int pre_end   = (ot.suffix_offset >= 0) ? ot.suffix_offset : total_len;

				auto DrawSpan = [&](const char *s, int n, COLORREF c) {
					if (n <= 0) return;
					SetTextColor(dc, c);
					ExtTextOutA(dc, x, y, ETO_CLIPPED, &op_rect, s, n, NULL);
					SIZE sz; GetTextExtentPoint32A(dc, s, n, &sz);
					x += sz.cx;
				};

				if (has_bp)
				{
					DrawSpan(ot.text, total_len, RGB(255, 255, 255));
				}
				else if (is_selected)
				{
					DrawSpan(ot.text, total_len, GetSysColor(COLOR_HIGHLIGHTTEXT));
				}
				else if (is_pc)
				{
					DrawSpan(ot.text, total_len, RGB(0, 0, 0));
				}
				else
				{
					int i = 0;
					while (i < pre_end)
					{
						if (ot.text[i] == '$')
						{
							int j = i + 1;
							while (j < pre_end)
							{
								const char c = ot.text[j];
								const bool is_hex = (c >= '0' && c <= '9')
								    || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
								if (!is_hex) break;
								j++;
							}
							DrawSpan(&ot.text[i], j - i, RGB(40, 80, 200));
							i = j;
						}
						else
						{
							int j = i;
							while (j < pre_end && ot.text[j] != '$') j++;
							DrawSpan(&ot.text[i], j - i, RGB(20, 20, 20));
							i = j;
						}
					}
					if (ot.suffix_offset >= 0)
						DrawSpan(&ot.text[ot.suffix_offset],
						         total_len - ot.suffix_offset, RGB(110, 110, 110));
				}

				SetBkMode(dc, prev_bk_mode);
			}

			if (st->lines[idx].is_sub_start)
			{
				HPEN sep = CreatePen(PS_SOLID, 1, RGB(180, 180, 180));
				HPEN oldP = (HPEN)SelectObject(dc, sep);
				MoveToEx(dc, row_rect.left, row_rect.top, NULL);
				LineTo(dc, row_rect.right, row_rect.top);
				SelectObject(dc, oldP);
				DeleteObject(sep);
			}

			const uint8_t  row_bank = BpBankForPC(st->sys, row_pc);
			const uint16_t row_addr = (uint16_t)(row_pc & 0xFFFF);
			const bool has_bp = gDebugger.HasExecBreakpoint(st->sys, row_bank, row_addr);

			const int cx = (mr.left + mr.right) / 2;
			const int cy = (mr.top + mr.bottom) / 2;
			const int r  = 5;

			if (has_bp)
			{
				HBRUSH fill = CreateSolidBrush(RGB(220, 30, 30));
				HPEN   edge = CreatePen(PS_SOLID, 1, RGB(120, 0, 0));
				HBRUSH oldB = (HBRUSH)SelectObject(dc, fill);
				HPEN   oldP = (HPEN)SelectObject(dc, edge);
				Ellipse(dc, cx - r, cy - r, cx + r, cy + r);
				SelectObject(dc, oldP);
				SelectObject(dc, oldB);
				DeleteObject(edge);
				DeleteObject(fill);
			}
			if (Settings.Paused && row_pc == cur_pc)
			{
				HBRUSH yel  = CreateSolidBrush(RGB(220, 200, 0));
				HPEN   edge = CreatePen(PS_SOLID, 1, RGB(140, 110, 0));
				HBRUSH oldB = (HBRUSH)SelectObject(dc, yel);
				HPEN   oldP = (HPEN)SelectObject(dc, edge);
				POINT arrow[3] = {
					{ cx - r, cy - r },
					{ cx + r, cy     },
					{ cx - r, cy + r }
				};
				Polygon(dc, arrow, 3);
				SelectObject(dc, oldP);
				SelectObject(dc, oldB);
				DeleteObject(edge);
				DeleteObject(yel);
			}
			return CDRF_DODEFAULT;
		}
	}
	return CDRF_DODEFAULT;
}

static HMENU BuildContextMenu(uint32_t pc, DbgSystem sys, bool has_branch, uint32_t branch_target)
{
	char buf[64];
	HMENU m = CreatePopupMenu();

	if (has_branch)
	{
		if (sys == DbgSystem::Snes)
			_snprintf_s(buf, 64, _TRUNCATE, "Go to Branch Target ($%06X)",
			            (unsigned)(branch_target & 0xFFFFFFu));
		else
			_snprintf_s(buf, 64, _TRUNCATE, "Go to Branch Target ($%04X)",
			            (unsigned)(branch_target & 0xFFFFu));
		AppendMenuA(m, MF_STRING, IDM_DISASM_GO_TO_BRANCH_TARGET, buf);

		if (sys == DbgSystem::Snes)
			_snprintf_s(buf, 64, _TRUNCATE, "Run to Branch Target ($%06X)",
			            (unsigned)(branch_target & 0xFFFFFFu));
		else
			_snprintf_s(buf, 64, _TRUNCATE, "Run to Branch Target ($%04X)",
			            (unsigned)(branch_target & 0xFFFFu));
		AppendMenuA(m, MF_STRING, IDM_DISASM_RUN_TO_BRANCH_TARGET, buf);

		AppendMenuA(m, MF_SEPARATOR, 0, NULL);
	}

	if (sys == DbgSystem::Snes)
		_snprintf_s(buf, 64, _TRUNCATE, "Toggle Breakpoint ($%06X)\tF9",
		            (unsigned)(pc & 0xFFFFFFu));
	else
		_snprintf_s(buf, 64, _TRUNCATE, "Toggle Breakpoint ($%04X)\tF9",
		            (unsigned)(pc & 0xFFFFu));
	AppendMenuA(m, MF_STRING, IDM_DISASM_TOGGLE_BP, buf);

	AppendMenuA(m, MF_SEPARATOR, 0, NULL);
	AppendMenuA(m, MF_STRING, IDM_DISASM_COPY_ADDR, "Copy Address");
	AppendMenuA(m, MF_STRING, IDM_DISASM_COPY_LINE, "Copy Selected\tCtrl+C");
	AppendMenuA(m, MF_SEPARATOR, 0, NULL);

	if (sys == DbgSystem::Snes)
		_snprintf_s(buf, 64, _TRUNCATE, "Move Program Counter ($%06X)",
		            (unsigned)(pc & 0xFFFFFFu));
	else
		_snprintf_s(buf, 64, _TRUNCATE, "Move Program Counter ($%04X)",
		            (unsigned)(pc & 0xFFFFu));
	AppendMenuA(m, MF_STRING, IDM_DISASM_MOVE_PC, buf);

	if (sys == DbgSystem::Snes)
		_snprintf_s(buf, 64, _TRUNCATE, "Run to Location ($%06X)\tCtrl+F11",
		            (unsigned)(pc & 0xFFFFFFu));
	else
		_snprintf_s(buf, 64, _TRUNCATE, "Run to Location ($%04X)\tCtrl+F11",
		            (unsigned)(pc & 0xFFFFu));
	AppendMenuA(m, MF_STRING, IDM_DISASM_RUN_TO_LOCATION, buf);

	if (sys == DbgSystem::Snes)
		_snprintf_s(buf, 64, _TRUNCATE, "Go to Location ($%06X)",
		            (unsigned)(pc & 0xFFFFFFu));
	else
		_snprintf_s(buf, 64, _TRUNCATE, "Go to Location ($%04X)",
		            (unsigned)(pc & 0xFFFFu));
	AppendMenuA(m, MF_STRING, IDM_DISASM_GO_TO_LOCATION, buf);

	AppendMenuA(m, MF_SEPARATOR, 0, NULL);
	AppendMenuA(m, MF_STRING, IDM_DISASM_GO_TO_PC, "Go to current PC");

	return m;
}

static void CopySelectionToClipboard(HWND parent, DisasmPanelState *st)
{
	std::string out;
	int idx = -1;
	while ((idx = ListView_GetNextItem(st->lv, idx, LVNI_SELECTED)) != -1)
	{
		EnsureLineCached(st, idx);
		if (idx >= st->line_count) break;
		const DisasmLine &ln = st->lines[idx];

		if (ln.is_label_row || ln.is_unmapped) continue;

		char line[160]; uint8_t bytes[4]; int byte_count;
		DisasmOne(st->sys, ln.pc, line, sizeof(line), bytes, &byte_count);

		char addr[16];
		if (st->sys == DbgSystem::Snes)
			_snprintf_s(addr, 16, _TRUNCATE, "%06X", (unsigned)(ln.pc & 0xFFFFFFu));
		else
			_snprintf_s(addr, 16, _TRUNCATE, "%04X", (unsigned)(ln.pc & 0xFFFFu));

		char bytestr[16] = "";
		for (int b = 0; b < byte_count && b < 4; b++)
			_snprintf_s(bytestr + b * 3, 16 - b * 3, _TRUNCATE, b ? " %02X" : "%02X", bytes[b]);

		OperandText ot;
		BuildOperandText(st, ln.pc, &ot);
		const char *suffix = ot.suffix_offset >= 0 ? ot.text + ot.suffix_offset : "";

		char row[320];
		_snprintf_s(row, sizeof(row), _TRUNCATE, "%s  %s  %s%s\r\n", addr, bytestr, line, suffix);
		out += row;
	}
	if (out.empty()) return;

	if (!OpenClipboard(parent)) return;
	EmptyClipboard();
	const size_t n = out.size();
	HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n + 1);
	if (g)
	{
		char *p = (char *)GlobalLock(g);
		memcpy(p, out.c_str(), n + 1);
		GlobalUnlock(g);
		SetClipboardData(CF_TEXT, g);
	}
	CloseClipboard();
}

static void OnContextMenuCommand(HWND parent, DisasmPanelState *st, int cmd)
{
	if (!st->rclick_valid && cmd != IDM_DISASM_COPY_LINE && cmd != IDM_DISASM_GO_TO_PC)
		return;
	const uint32_t pc = st->rclick_pc;
	const uint8_t  bank = BpBankForPC(st->sys, pc);
	const uint16_t addr = (uint16_t)(pc & 0xFFFF);

	switch (cmd)
	{
		case IDM_DISASM_TOGGLE_BP:
		{
			gDebugger.ToggleExecBreakpoint(st->sys, bank, addr);
			InvalidateRect(st->lv, NULL, FALSE);
			HWND root = GetAncestor(parent, GA_ROOT);
			if (root) PostMessage(root, WM_USER_DEBUGGER_REFRESH, 0, 0);
			break;
		}
		case IDM_DISASM_COPY_ADDR:
		{
			char buf[16];
			if (st->sys == DbgSystem::Snes)
				_snprintf_s(buf, 16, _TRUNCATE, "%06X", (unsigned)(pc & 0xFFFFFFu));
			else
				_snprintf_s(buf, 16, _TRUNCATE, "%04X", addr);
			if (OpenClipboard(parent))
			{
				EmptyClipboard();
				HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, strlen(buf) + 1);
				if (g)
				{
					char *p = (char *)GlobalLock(g);
					strcpy(p, buf);
					GlobalUnlock(g);
					SetClipboardData(CF_TEXT, g);
				}
				CloseClipboard();
			}
			break;
		}
		case IDM_DISASM_COPY_LINE:
			CopySelectionToClipboard(parent, st);
			break;
		case IDM_DISASM_MOVE_PC:
			if (st->sys == DbgSystem::Snes)
			{
				Registers.PB  = bank;
				Registers.PCw = addr;
				S9xDebuggerRefreshAll();
			}
			break;
		case IDM_DISASM_RUN_TO_LOCATION:
			gDebugger.AddExecBreakpoint(st->sys, bank, addr);
			gDebugger.Run();
			break;
		case IDM_DISASM_GO_TO_LOCATION:
		{
			const int idx = FindOrExtendForPC(st, pc, 2000000);
			if (idx >= 0) ListView_EnsureVisible(st->lv, idx, FALSE);
			else if (st->sys == DbgSystem::Snes) { ResetLines(st, pc); st->view_initialized = true; }
			break;
		}
		case IDM_DISASM_GO_TO_PC:
		{
			const uint32_t cur = GetCurrentPC(st->sys);
			const int idx = FindOrExtendForPC(st, cur, 2000000);
			if (idx >= 0) ListView_EnsureVisible(st->lv, idx, FALSE);
			else if (st->sys == DbgSystem::Snes) { ResetLines(st, cur); st->view_initialized = true; }
			break;
		}
		case IDM_DISASM_GO_TO_BRANCH_TARGET:
			if (st->rclick_has_branch)
			{
				const int idx = FindOrExtendForPC(st, st->rclick_branch_target, 2000000);
				if (idx >= 0) ListView_EnsureVisible(st->lv, idx, FALSE);
				else if (st->sys == DbgSystem::Snes) { ResetLines(st, st->rclick_branch_target); st->view_initialized = true; }
			}
			break;
		case IDM_DISASM_RUN_TO_BRANCH_TARGET:
			if (st->rclick_has_branch)
			{
				const uint8_t  bb = BpBankForPC(st->sys, st->rclick_branch_target);
				const uint16_t aa = (uint16_t)(st->rclick_branch_target & 0xFFFF);
				gDebugger.AddExecBreakpoint(st->sys, bb, aa);
				gDebugger.Run();
			}
			break;
	}
}

static void OnRightClick(HWND parent, DisasmPanelState *st)
{
	LVHITTESTINFO hti = {};
	DWORD pos = GetMessagePos();
	POINT pt = { GET_X_LPARAM(pos), GET_Y_LPARAM(pos) };
	hti.pt = pt;
	ScreenToClient(st->lv, &hti.pt);
	int idx = ListView_HitTest(st->lv, &hti);

	if (idx >= 0 && idx < st->line_count)
	{
		st->rclick_pc    = st->lines[idx].pc;
		st->rclick_valid = true;
		st->rclick_has_branch = GetBranchTargetForPC(st->sys, st->rclick_pc,
		                                              &st->rclick_branch_target);

		if (ListView_GetItemState(st->lv, idx, LVIS_SELECTED) == 0)
		{
			ListView_SetItemState(st->lv, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
			ListView_SetItemState(st->lv, idx, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
		}
	}
	else
	{
		st->rclick_valid      = false;
		st->rclick_pc         = 0;
		st->rclick_has_branch = false;
	}

	HMENU menu = BuildContextMenu(st->rclick_pc, st->sys,
	                              st->rclick_has_branch, st->rclick_branch_target);
	int cmd = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
	                              pt.x, pt.y, 0, parent, NULL);
	DestroyMenu(menu);
	if (cmd) OnContextMenuCommand(parent, st, cmd);
}

static uint32_t BankStart(DbgSystem sys, uint32_t pc)
{
	if (sys == DbgSystem::Snes)
		return pc & 0xFF0000;
	return 0;
}

static int FindOrExtendForPC(DisasmPanelState *st, uint32_t pc, int max_rows)
{
	int idx = FindIndexForPC(st, pc);
	if (idx >= 0) return idx;

	uint32_t prev_pc = st->line_count > 0 ? st->lines[st->line_count - 1].pc : 0;
	while (st->line_count < max_rows)
	{
		const int prev = st->line_count;
		EnsureLineCached(st, st->line_count);
		if (st->line_count <= prev) break;
		const uint32_t last_pc = st->lines[st->line_count - 1].pc;
		if (last_pc == pc) return st->line_count - 1;
		if (st->line_count > 1 && last_pc < prev_pc) break;
		if (last_pc > pc) break;
		prev_pc = last_pc;
	}
	return FindIndexForPC(st, pc);
}

static void EnsurePCVisible(DisasmPanelState *st)
{
	const uint32_t cur_pc = GetCurrentPC(st->sys);

	const bool need_reanchor = !st->view_initialized
	    || BankStart(st->sys, cur_pc) != BankStart(st->sys, st->view_start_pc);

	if (need_reanchor)
	{
		const uint32_t start = BankStart(st->sys, cur_pc);
		ResetLines(st, start);
		st->view_initialized = true;
	}

	int pc_idx = FindOrExtendForPC(st, cur_pc, 2000000);
	if (pc_idx < 0)
	{
		ResetLines(st, cur_pc);
		st->view_initialized = true;
		pc_idx = FindOrExtendForPC(st, cur_pc, 2000000);
		if (pc_idx < 0) return;
	}

	const int per_page = ListView_GetCountPerPage(st->lv);
	const int top      = ListView_GetTopIndex(st->lv);
	const int bottom   = top + per_page - 1;
	const int margin   = (per_page > 12) ? 3 : 1;

	if (pc_idx < top + margin || pc_idx > bottom - margin)
	{
		if (per_page > 6)
		{
			const int third = per_page / 3;
			int bottom_anchor = pc_idx + (per_page - 1 - third);
			if (bottom_anchor >= st->line_count) bottom_anchor = st->line_count - 1;
			ListView_EnsureVisible(st->lv, bottom_anchor, FALSE);
		}
		ListView_EnsureVisible(st->lv, pc_idx, FALSE);
	}
}

// The flow-arrow gutter spans rows, so the ListView's default scroll-blit
// smears it: it copies old gutter pixels to new positions and only repaints
// newly-exposed rows. Force a full client repaint after any scroll so every
// row's arrows are recomputed and redrawn cleanly.
static LRESULT CALLBACK DisasmLvSubclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                         UINT_PTR id, DWORD_PTR /*ref*/)
{
	switch (msg)
	{
		case WM_WINDOWPOSCHANGING:
		{
			WINDOWPOS *wpos = (WINDOWPOS *)lp;
			if (wpos && !(wpos->flags & SWP_NOSIZE))
				wpos->flags |= SWP_NOCOPYBITS;
			break;
		}
		case WM_VSCROLL:
		case WM_HSCROLL:
		case WM_MOUSEWHEEL:
		case WM_KEYDOWN:
		case WM_SIZE:
		case WM_WINDOWPOSCHANGED:
		{
			const LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
			InvalidateRect(hwnd, NULL, FALSE);
			return r;
		}
		case WM_NCDESTROY:
			RemoveWindowSubclass(hwnd, DisasmLvSubclass, id);
			break;
	}
	return DefSubclassProc(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg)
	{
		case WM_NCCREATE:
		{
			CREATESTRUCT *cs = (CREATESTRUCT *)lp;
			DisasmPanelState *st = new DisasmPanelState();
			st->sys = (DbgSystem)(intptr_t)cs->lpCreateParams;
			st->font = CreateFont(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
			                       DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
			                       ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, TEXT("Consolas"));
			EnsureCap(st, 256);
			SetWindowLongPtr(h, GWLP_USERDATA, (LONG_PTR)st);
			break;
		}

		case WM_CREATE:
		{
			DisasmPanelState *st = GetState(h);
			st->lv = CreateWindowEx(0, WC_LISTVIEW, TEXT(""),
			                       WS_CHILD | WS_VISIBLE | WS_TABSTOP
			                       | LVS_REPORT | LVS_OWNERDATA
			                       | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
			                       0, 0, 0, 0,
			                       h, (HMENU)(LONG_PTR)IDC_DISASM_LV,
			                       GetModuleHandle(NULL), NULL);
			ListView_SetUnicodeFormat(st->lv, FALSE);
			ListView_SetExtendedListViewStyle(st->lv,
			    LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
			SendMessage(st->lv, WM_SETFONT, (WPARAM)st->font, TRUE);
			SetWindowSubclass(st->lv, DisasmLvSubclass, 1, 0);

			LVCOLUMNA col = {};
			col.mask = LVCF_TEXT | LVCF_WIDTH;
			col.pszText = (LPSTR)" ";        col.cx = 32;  SendMessageA(st->lv, LVM_INSERTCOLUMNA, COL_MARGIN,  (LPARAM)&col);
			col.pszText = (LPSTR)"";         col.cx = 84;  SendMessageA(st->lv, LVM_INSERTCOLUMNA, COL_FLOW,    (LPARAM)&col);
			col.pszText = (LPSTR)"Address";  col.cx = 70;  SendMessageA(st->lv, LVM_INSERTCOLUMNA, COL_ADDR,    (LPARAM)&col);
			col.pszText = (LPSTR)"Opcodes";  col.cx = 95;  SendMessageA(st->lv, LVM_INSERTCOLUMNA, COL_BYTES,   (LPARAM)&col);
			col.pszText = (LPSTR)"Mnem";     col.cx = 48;  SendMessageA(st->lv, LVM_INSERTCOLUMNA, COL_MNEM,    (LPARAM)&col);
			col.pszText = (LPSTR)"Operand";  col.cx = 290; SendMessageA(st->lv, LVM_INSERTCOLUMNA, COL_OPERAND, (LPARAM)&col);

			ListView_SetItemCountEx(st->lv, 0, LVSICF_NOINVALIDATEALL);
			return 0;
		}

		case WM_DESTROY:
		{
			DisasmPanelState *st = GetState(h);
			if (st)
			{
				if (st->font) DeleteObject(st->font);
				free(st->lines);
				free(st->targets);
				free(st->entries);
				free(st->section_starts);
				free(st->arrows);
				delete st;
			}
			return 0;
		}

		case WM_WINDOWPOSCHANGING:
		{
			WINDOWPOS *wpos = (WINDOWPOS *)lp;
			if (wpos && !(wpos->flags & SWP_NOSIZE))
				wpos->flags |= SWP_NOCOPYBITS;
			break;
		}

		case WM_SIZE:
		{
			DisasmPanelState *st = GetState(h);
			if (st && st->lv)
			{
				RECT rc; GetClientRect(h, &rc);
				MoveWindow(st->lv, 0, 0, rc.right, rc.bottom, TRUE);
				InvalidateRect(st->lv, NULL, FALSE);
			}
			return 0;
		}

		case WM_NOTIFY:
		{
			NMHDR *hdr = (NMHDR *)lp;
			DisasmPanelState *st = GetState(h);
			if (!st || hdr->hwndFrom != st->lv) break;

			switch (hdr->code)
			{
				case LVN_GETDISPINFOA:
					OnGetDispInfo(st, (NMLVDISPINFOA *)lp);
					return 0;
				case NM_CUSTOMDRAW:
					return OnCustomDraw(st, (NMLVCUSTOMDRAW *)lp);
				case NM_RCLICK:
					OnRightClick(h, st);
					return 0;
				case NM_DBLCLK:
				{
					NMITEMACTIVATE *act = (NMITEMACTIVATE *)lp;
					if (act->iItem >= 0 && act->iItem < st->line_count
					    && !st->lines[act->iItem].is_label_row
					    && !st->lines[act->iItem].is_unmapped)
					{
						const uint32_t pc = st->lines[act->iItem].pc;
						const uint8_t  b  = BpBankForPC(st->sys, pc);
						const uint16_t a  = (uint16_t)(pc & 0xFFFF);
						gDebugger.ToggleExecBreakpoint(st->sys, b, a);
						InvalidateRect(st->lv, NULL, FALSE);
						HWND root = GetAncestor(h, GA_ROOT);
						if (root) PostMessage(root, WM_USER_DEBUGGER_REFRESH, 0, 0);
					}
					return 0;
				}
			}
			break;
		}
	}

	return DefWindowProc(h, msg, wp, lp);
}

void DisasmPanelRegisterClass(HINSTANCE hInst)
{
	static bool done = false;
	if (done) return;
	done = true;

	INITCOMMONCONTROLSEX icc = {};
	icc.dwSize = sizeof(icc);
	icc.dwICC  = ICC_LISTVIEW_CLASSES;
	InitCommonControlsEx(&icc);

	WNDCLASSEX wc = {};
	wc.cbSize        = sizeof(wc);
	wc.style         = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc   = WndProc;
	wc.hInstance     = hInst;
	wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wc.lpszClassName = kClassName;
	RegisterClassEx(&wc);
}

HWND DisasmPanelCreate(HWND parent, DbgSystem sys, int id)
{
	return CreateWindowEx(WS_EX_CLIENTEDGE, kClassName, TEXT(""),
	                     WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
	                     0, 0, 0, 0,
	                     parent, (HMENU)(LONG_PTR)id, GetModuleHandle(NULL), (LPVOID)(intptr_t)sys);
}

void DisasmPanelInvalidateCache(HWND h)
{
	DisasmPanelState *st = GetState(h);
	if (!st) return;
	st->view_initialized = false;
	st->view_start_pc    = 0;
	st->shown_pc         = 0xFFFFFFFFu;
	st->shown_paused     = false;
	st->shown_bps_version = 0;
	st->line_count       = 0;
	st->targets_count    = 0;
	st->entries_count    = 0;
	st->sections_count   = 0;
	st->arrows_count     = 0;
	st->walk_complete    = false;
	if (st->lv)
	{
		ListView_SetItemCountEx(st->lv, 0, LVSICF_NOINVALIDATEALL);
		InvalidateRect(st->lv, NULL, FALSE);
	}
}

void DisasmPanelGotoAddress(HWND h, uint32_t addr)
{
	DisasmPanelState *st = GetState(h);
	if (!st || !st->lv) return;

	const uint32_t start = BankStart(st->sys, addr);
	if (!st->view_initialized ||
	    BankStart(st->sys, st->view_start_pc) != start)
	{
		ResetLines(st, start);
		st->view_initialized = true;
	}

	int idx = FindOrExtendForPC(st, addr, 2000000);
	if (idx < 0)
	{
		ResetLines(st, addr);
		st->view_initialized = true;
		idx = FindOrExtendForPC(st, addr, 2000000);
		if (idx < 0) return;
	}

	const int per_page = ListView_GetCountPerPage(st->lv);
	if (per_page > 6)
	{
		const int third = per_page / 3;
		int bottom_anchor = idx + (per_page - 1 - third);
		if (bottom_anchor >= st->line_count) bottom_anchor = st->line_count - 1;
		ListView_EnsureVisible(st->lv, bottom_anchor, FALSE);
	}
	ListView_EnsureVisible(st->lv, idx, FALSE);
	ListView_SetItemState(st->lv, idx, LVIS_SELECTED | LVIS_FOCUSED,
	                      LVIS_SELECTED | LVIS_FOCUSED);
	InvalidateRect(st->lv, NULL, FALSE);
	UpdateWindow(st->lv);
}

void DisasmPanelRefresh(HWND h)
{
	DisasmPanelState *st = GetState(h);
	if (!st || !st->lv) return;

	const uint32_t cur_pc          = GetCurrentPC(st->sys);
	const uint32_t cur_bps_version = gDebugger.BreakpointsVersion();
	const bool     bps_changed     = st->shown_bps_version != cur_bps_version;
	const bool     pc_changed      = st->shown_pc          != cur_pc;
	const bool     first_run       = !st->view_initialized;
	const bool     just_paused     = !st->shown_paused &&  Settings.Paused;
	const bool     just_unpaused   =  st->shown_paused && !Settings.Paused;

	if (first_run || just_paused || (Settings.Paused && pc_changed))
		EnsurePCVisible(st);

	if (first_run || Settings.Paused || just_unpaused || bps_changed)
	{
		InvalidateRect(st->lv, NULL, FALSE);
		UpdateWindow(st->lv);
	}

	{
		HWND parent = GetParent(h);
		while (parent && (GetWindowLongPtr(parent, GWL_STYLE) & WS_CHILD))
			parent = GetParent(parent);
		if (parent)
		{
			const int pc_idx_for_title = FindIndexForPC(st, cur_pc);
			const int top_for_title    = ListView_GetTopIndex(st->lv);
			const int per_page_title   = ListView_GetCountPerPage(st->lv);
			char title[200];
			_snprintf_s(title, sizeof(title), _TRUNCATE,
			            "%s Debugger [P:%d sp:%d jp:%d pend:%d pc:%06X idx:%d top:%d pp:%d lc:%d]",
			            st->sys == DbgSystem::Snes ? "SNES" : "GB",
			            Settings.Paused ? 1 : 0,
			            st->shown_paused ? 1 : 0,
			            just_paused ? 1 : 0,
			            gDebugger.IsGbBreakPending() ? 1 : 0,
			            (unsigned)cur_pc,
			            pc_idx_for_title,
			            top_for_title,
			            per_page_title,
			            st->line_count);
			SetWindowTextA(parent, title);
		}
	}

	st->shown_bps_version = cur_bps_version;
	st->shown_pc          = cur_pc;
	st->shown_paused      = Settings.Paused;
}
