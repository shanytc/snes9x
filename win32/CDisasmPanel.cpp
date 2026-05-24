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

enum { COL_MARGIN = 0, COL_ADDR = 1, COL_BYTES = 2, COL_CODE = 3 };

struct DisasmLine
{
	uint32_t pc;
	uint8_t  length;
	bool     is_sub_start;
	bool     is_label_row;
	uint32_t section_index;
};

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
	uint32_t     rclick_pc        = 0;
	bool         rclick_valid     = false;
	bool         rclick_has_branch = false;
	uint32_t     rclick_branch_target = 0;
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
		if (r.has_branch)
		{
			const uint8_t bank = (uint8_t)((pc >> 16) & 0xFF);
			const uint16_t tgt = r.branch_target;
			uint8_t tgt_bank = bank;
			if (tgt < 0x4000)      tgt_bank = 0;
			*out_target = ((uint32_t)tgt_bank << 16) | tgt;
			return true;
		}
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
		return op == 0x60 || op == 0x6B || op == 0x40
		    || op == 0x4C || op == 0x5C
		    || op == 0x82 || op == 0x80;
	return op == 0xC9 || op == 0xD9
	    || op == 0xC3 || op == 0xE9
	    || op == 0x18;
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
		return (pc & 0xFF0000) | ((uint16_t)(pc + len) & 0xFFFF);

	uint8_t  bank = (uint8_t)((pc >> 16) & 0xFF);
	uint16_t cpu  = (uint16_t)(pc & 0xFFFF);
	cpu = (uint16_t)(cpu + len);

	if (bank == 0)
	{
		if (cpu >= 0x4000)
		{
			const uint8_t  total_banks = (uint8_t)GbBackend::TotalROMBanks();
			if (total_banks >= 2) bank = 1;
			cpu = (uint16_t)(0x4000 + (cpu - 0x4000));
		}
	}
	else
	{
		if (cpu >= 0x8000)
		{
			const uint8_t total_banks = (uint8_t)GbBackend::TotalROMBanks();
			const uint8_t next_bank = (uint8_t)(bank + 1);
			if (total_banks == 0 || next_bank < total_banks)
			{
				bank = next_bank;
				cpu  = (uint16_t)(0x4000 + (cpu - 0x8000));
			}
			else
			{
				cpu = 0x7FFF;
			}
		}
	}
	return ((uint32_t)bank << 16) | cpu;
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
	st->view_start_pc = new_start_pc;
	ClearLines(st);
	st->targets_count = 0;
	DisasmLine first{};
	first.pc            = new_start_pc;
	first.length        = 0;
	first.is_sub_start  = false;
	first.is_label_row  = false;
	first.section_index = 0;
	PushLine(st, first);
	ListView_SetItemCountEx(st->lv, 100000, LVSICF_NOINVALIDATEALL);
	InvalidateRect(st->lv, NULL, FALSE);
}

static int LastInstrIdx(DisasmPanelState *st)
{
	for (int i = st->line_count - 1; i >= 0; i--)
		if (!st->lines[i].is_label_row) return i;
	return -1;
}

static uint32_t ComputeBranchTargetDisplay(DbgSystem sys, uint32_t caller_pc, uint32_t raw_branch)
{
	if (sys == DbgSystem::Snes)
		return raw_branch & 0xFFFFFF;
	const uint16_t tgt_cpu     = (uint16_t)(raw_branch & 0xFFFF);
	const uint8_t  caller_bank = (uint8_t)((caller_pc >> 16) & 0xFF);
	uint8_t tgt_bank;
	if      (tgt_cpu < 0x4000) tgt_bank = 0;
	else if (tgt_cpu < 0x8000) tgt_bank = caller_bank;
	else                       tgt_bank = 0;
	return ((uint32_t)tgt_bank << 16) | tgt_cpu;
}

static void EnsureLineCached(DisasmPanelState *st, int index)
{
	if (st->line_count == 0) return;
	while (st->line_count <= index)
	{
		const int last = LastInstrIdx(st);
		if (last < 0) break;

		const uint32_t back_pc  = st->lines[last].pc;
		uint8_t        back_length = st->lines[last].length;
		const uint32_t back_sec = st->lines[last].section_index;

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
		}

		const bool ends_section = IsCtlFlowEnd(st->sys, bytes[0]);
		const uint32_t next_pc  = AdvancePC(st->sys, back_pc, back_length);
		const uint32_t next_sec = ends_section ? (back_sec + 1) : back_sec;

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
	}
}

static int FindIndexForPC(DisasmPanelState *st, uint32_t pc)
{
	for (int i = 0; i < st->line_count; i++)
		if (st->lines[i].pc == pc && !st->lines[i].is_label_row) return i;
	return -1;
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

static void OnGetDispInfo(DisasmPanelState *st, NMLVDISPINFOA *di)
{
	const int idx = di->item.iItem;
	EnsureLineCached(st, idx);

	if (idx < 0 || idx >= st->line_count) return;

	DisasmLine &ln = st->lines[idx];

	if (ln.is_label_row)
	{
		if (di->item.mask & LVIF_TEXT)
		{
			char *out = di->item.pszText;
			const int cap = di->item.cchTextMax;
			if (di->item.iSubItem == COL_CODE)
				_snprintf_s(out, cap, _TRUNCATE, "$%02x%04x:",
				            (unsigned)(ln.pc >> 16) & 0xFF,
				            (unsigned)(ln.pc & 0xFFFF));
			else
				out[0] = 0;
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
				out[0] = 0;
				break;
			case COL_ADDR:
				_snprintf_s(out, cap, _TRUNCATE, "%02x:%04x",
				            (unsigned)(ln.pc >> 16) & 0xFF,
				            (unsigned)(ln.pc & 0xFFFF));
				break;
			case COL_BYTES:
				if (byte_count == 1)      _snprintf_s(out, cap, _TRUNCATE, "%02X", bytes[0]);
				else if (byte_count == 2) _snprintf_s(out, cap, _TRUNCATE, "%02X %02X", bytes[0], bytes[1]);
				else if (byte_count == 3) _snprintf_s(out, cap, _TRUNCATE, "%02X %02X %02X", bytes[0], bytes[1], bytes[2]);
				else                      _snprintf_s(out, cap, _TRUNCATE, "%02X %02X %02X %02X", bytes[0], bytes[1], bytes[2], bytes[3]);
				break;
			case COL_CODE:
			{
				uint32_t lookup_addr = 0;
				bool     have_addr   = false;
				const char *p = line;
				while (*p)
				{
					if (*p == '$' && (p == line || p[-1] != '#'))
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

				char suffix[64] = "";
				if (has_eff)
				{
					if (label)
						_snprintf_s(suffix, 64, _TRUNCATE, " [%s] = $%02X", label, eff_val);
					else if (st->sys == DbgSystem::Snes)
						_snprintf_s(suffix, 64, _TRUNCATE, " [$%06X] = $%02X", eff_addr & 0xFFFFFF, eff_val);
					else
						_snprintf_s(suffix, 64, _TRUNCATE, " [$%04X] = $%02X", eff_addr & 0xFFFF, eff_val);
				}
				else if (label)
				{
					_snprintf_s(suffix, 64, _TRUNCATE, " [%s]", label);
				}

				_snprintf_s(out, cap, _TRUNCATE, "%s%s", line, suffix);
				break;
			}
		}
	}
}

static LRESULT OnCustomDraw(DisasmPanelState *st, NMLVCUSTOMDRAW *cd)
{
	switch (cd->nmcd.dwDrawStage)
	{
		case CDDS_PREPAINT:
			return CDRF_NOTIFYITEMDRAW;

		case CDDS_ITEMPREPAINT:
		{
			const int idx = (int)cd->nmcd.dwItemSpec;
			if (idx < st->line_count)
			{
				const DisasmLine &row = st->lines[idx];
				if (row.is_label_row)
				{
					cd->clrTextBk = (row.section_index & 1) ? RGB(255, 240, 240) : RGB(255, 255, 255);
					cd->clrText   = RGB(0, 96, 0);
				}
				else
				{
					const uint32_t cur_pc = GetCurrentPC(st->sys);
					if (row.pc == cur_pc)
					{
						cd->clrTextBk = RGB(255, 255, 180);
						cd->clrText   = RGB(0, 0, 0);
					}
					else if (row.section_index & 1)
					{
						cd->clrTextBk = RGB(255, 240, 240);
					}
					else
					{
						cd->clrTextBk = RGB(255, 255, 255);
					}
				}
			}
			return CDRF_NOTIFYPOSTPAINT | CDRF_NEWFONT;
		}

		case CDDS_ITEMPOSTPAINT:
		{
			const int idx = (int)cd->nmcd.dwItemSpec;
			if (idx >= st->line_count) return CDRF_DODEFAULT;

			if (st->lines[idx].is_label_row)
				return CDRF_DODEFAULT;

			RECT row_rect = {};
			ListView_GetItemRect(st->lv, idx, &row_rect, LVIR_BOUNDS);
			const int margin_w = ListView_GetColumnWidth(st->lv, COL_MARGIN);
			RECT mr = { row_rect.left, row_rect.top,
			            row_rect.left + margin_w, row_rect.bottom };

			const uint32_t row_pc = st->lines[idx].pc;
			const uint32_t cur_pc = GetCurrentPC(st->sys);

			HDC dc = cd->nmcd.hdc;

			if (st->lines[idx].is_sub_start)
			{
				HPEN sep = CreatePen(PS_SOLID, 1, RGB(180, 180, 180));
				HPEN oldP = (HPEN)SelectObject(dc, sep);
				MoveToEx(dc, row_rect.left, row_rect.top, NULL);
				LineTo(dc, row_rect.right, row_rect.top);
				SelectObject(dc, oldP);
				DeleteObject(sep);
			}

			bool has_bp = false;
			if (st->sys == DbgSystem::Snes)
				has_bp = gDebugger.HasExecBreakpoint(DbgSystem::Snes,
				                                     (uint8_t)(row_pc >> 16),
				                                     (uint16_t)(row_pc & 0xFFFF));
			else
				has_bp = gDebugger.HasExecBreakpoint(DbgSystem::Gb, 0, (uint16_t)row_pc);

			if (has_bp)
			{
				HBRUSH red = CreateSolidBrush(RGB(200, 0, 0));
				RECT dot = { mr.left + 4, mr.top + 3, mr.left + 14, mr.top + 13 };
				FillRect(dc, &dot, red);
				DeleteObject(red);
			}
			if (row_pc == cur_pc)
			{
				HBRUSH yel = CreateSolidBrush(RGB(220, 200, 0));
				const int cy = (mr.top + mr.bottom) / 2;
				POINT arrow[3] = {
					{ mr.left + 14, mr.top + 2 },
					{ mr.right - 3, cy },
					{ mr.left + 14, mr.bottom - 2 }
				};
				HRGN hr = CreatePolygonRgn(arrow, 3, ALTERNATE);
				FillRgn(dc, hr, yel);
				DeleteObject(hr);
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
			_snprintf_s(buf, 64, _TRUNCATE, "Go to Branch Target ($%02X:%04X)",
			            (unsigned)(branch_target >> 16) & 0xFF, (unsigned)(branch_target & 0xFFFF));
		else
			_snprintf_s(buf, 64, _TRUNCATE, "Go to Branch Target ($%04X)",
			            (unsigned)(branch_target & 0xFFFF));
		AppendMenuA(m, MF_STRING, IDM_DISASM_GO_TO_BRANCH_TARGET, buf);

		if (sys == DbgSystem::Snes)
			_snprintf_s(buf, 64, _TRUNCATE, "Run to Branch Target ($%02X:%04X)",
			            (unsigned)(branch_target >> 16) & 0xFF, (unsigned)(branch_target & 0xFFFF));
		else
			_snprintf_s(buf, 64, _TRUNCATE, "Run to Branch Target ($%04X)",
			            (unsigned)(branch_target & 0xFFFF));
		AppendMenuA(m, MF_STRING, IDM_DISASM_RUN_TO_BRANCH_TARGET, buf);

		AppendMenuA(m, MF_SEPARATOR, 0, NULL);
	}

	if (sys == DbgSystem::Snes)
		_snprintf_s(buf, 64, _TRUNCATE, "Toggle Breakpoint ($%02X:%04X)\tF9",
		            (unsigned)(pc >> 16) & 0xFF, (unsigned)(pc & 0xFFFF));
	else
		_snprintf_s(buf, 64, _TRUNCATE, "Toggle Breakpoint ($%04X)\tF9",
		            (unsigned)(pc & 0xFFFF));
	AppendMenuA(m, MF_STRING, IDM_DISASM_TOGGLE_BP, buf);

	AppendMenuA(m, MF_SEPARATOR, 0, NULL);
	AppendMenuA(m, MF_STRING, IDM_DISASM_COPY_ADDR, "Copy Address");
	AppendMenuA(m, MF_STRING, IDM_DISASM_COPY_LINE, "Copy Selected\tCtrl+C");
	AppendMenuA(m, MF_SEPARATOR, 0, NULL);

	if (sys == DbgSystem::Snes)
		_snprintf_s(buf, 64, _TRUNCATE, "Move Program Counter ($%02X:%04X)",
		            (unsigned)(pc >> 16) & 0xFF, (unsigned)(pc & 0xFFFF));
	else
		_snprintf_s(buf, 64, _TRUNCATE, "Move Program Counter ($%04X)",
		            (unsigned)(pc & 0xFFFF));
	AppendMenuA(m, MF_STRING, IDM_DISASM_MOVE_PC, buf);

	if (sys == DbgSystem::Snes)
		_snprintf_s(buf, 64, _TRUNCATE, "Run to Location ($%02X:%04X)\tCtrl+F11",
		            (unsigned)(pc >> 16) & 0xFF, (unsigned)(pc & 0xFFFF));
	else
		_snprintf_s(buf, 64, _TRUNCATE, "Run to Location ($%04X)\tCtrl+F11",
		            (unsigned)(pc & 0xFFFF));
	AppendMenuA(m, MF_STRING, IDM_DISASM_RUN_TO_LOCATION, buf);

	if (sys == DbgSystem::Snes)
		_snprintf_s(buf, 64, _TRUNCATE, "Go to Location ($%02X:%04X)",
		            (unsigned)(pc >> 16) & 0xFF, (unsigned)(pc & 0xFFFF));
	else
		_snprintf_s(buf, 64, _TRUNCATE, "Go to Location ($%04X)",
		            (unsigned)(pc & 0xFFFF));
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
		char line[160]; uint8_t bytes[4]; int byte_count;
		DisasmOne(st->sys, ln.pc, line, sizeof(line), bytes, &byte_count);

		char addr[16];
		if (st->sys == DbgSystem::Snes)
			_snprintf_s(addr, 16, _TRUNCATE, "%02X:%04X",
			            (unsigned)(ln.pc >> 16) & 0xFF, (unsigned)(ln.pc & 0xFFFF));
		else
			_snprintf_s(addr, 16, _TRUNCATE, "%04X", (unsigned)(ln.pc & 0xFFFF));

		char row[256];
		_snprintf_s(row, 256, _TRUNCATE, "%s  %s\r\n", addr, line);
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
	const uint8_t  bank = (st->sys == DbgSystem::Snes) ? (uint8_t)(pc >> 16) : (uint8_t)0;
	const uint16_t addr = (uint16_t)(pc & 0xFFFF);

	switch (cmd)
	{
		case IDM_DISASM_TOGGLE_BP:
			gDebugger.ToggleExecBreakpoint(st->sys, bank, addr);
			InvalidateRect(st->lv, NULL, FALSE);
			break;
		case IDM_DISASM_COPY_ADDR:
		{
			char buf[16];
			if (st->sys == DbgSystem::Snes)
				_snprintf_s(buf, 16, _TRUNCATE, "%02X:%04X", bank, addr);
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
			const int idx = FindOrExtendForPC(st, pc, 200000);
			if (idx >= 0) ListView_EnsureVisible(st->lv, idx, FALSE);
			else          ResetLines(st, pc);
			break;
		}
		case IDM_DISASM_GO_TO_PC:
		{
			const uint32_t cur = GetCurrentPC(st->sys);
			const int idx = FindOrExtendForPC(st, cur, 200000);
			if (idx >= 0) ListView_EnsureVisible(st->lv, idx, FALSE);
			else { ResetLines(st, cur); st->view_initialized = true; }
			break;
		}
		case IDM_DISASM_GO_TO_BRANCH_TARGET:
			if (st->rclick_has_branch)
			{
				const int idx = FindOrExtendForPC(st, st->rclick_branch_target, 200000);
				if (idx >= 0) ListView_EnsureVisible(st->lv, idx, FALSE);
				else          ResetLines(st, st->rclick_branch_target);
			}
			break;
		case IDM_DISASM_RUN_TO_BRANCH_TARGET:
			if (st->rclick_has_branch)
			{
				const uint8_t  bb = (st->sys == DbgSystem::Snes) ? (uint8_t)(st->rclick_branch_target >> 16) : (uint8_t)0;
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

	const int pc_idx = FindOrExtendForPC(st, cur_pc, 80000);
	if (pc_idx < 0) return;

	const int per_page = ListView_GetCountPerPage(st->lv);
	const int top      = ListView_GetTopIndex(st->lv);
	const int bottom   = top + per_page;

	if (pc_idx < top || pc_idx > bottom)
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

			LVCOLUMNA col = {};
			col.mask = LVCF_TEXT | LVCF_WIDTH;
			col.pszText = (LPSTR)" ";        col.cx = 32;  SendMessageA(st->lv, LVM_INSERTCOLUMNA, COL_MARGIN, (LPARAM)&col);
			col.pszText = (LPSTR)"Address";  col.cx = 70;  SendMessageA(st->lv, LVM_INSERTCOLUMNA, COL_ADDR,   (LPARAM)&col);
			col.pszText = (LPSTR)"Bytes";    col.cx = 95;  SendMessageA(st->lv, LVM_INSERTCOLUMNA, COL_BYTES,  (LPARAM)&col);
			col.pszText = (LPSTR)"Code";     col.cx = 320; SendMessageA(st->lv, LVM_INSERTCOLUMNA, COL_CODE,   (LPARAM)&col);

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
				delete st;
			}
			return 0;
		}

		case WM_SIZE:
		{
			DisasmPanelState *st = GetState(h);
			if (st && st->lv)
			{
				RECT rc; GetClientRect(h, &rc);
				MoveWindow(st->lv, 0, 0, rc.right, rc.bottom, TRUE);
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
					    && !st->lines[act->iItem].is_label_row)
					{
						const uint32_t pc = st->lines[act->iItem].pc;
						const uint8_t  b  = (st->sys == DbgSystem::Snes) ? (uint8_t)(pc >> 16) : (uint8_t)0;
						const uint16_t a  = (uint16_t)(pc & 0xFFFF);
						gDebugger.ToggleExecBreakpoint(st->sys, b, a);
						InvalidateRect(st->lv, NULL, FALSE);
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

void DisasmPanelRefresh(HWND h)
{
	DisasmPanelState *st = GetState(h);
	if (!st || !st->lv) return;

	EnsurePCVisible(st);
	InvalidateRect(st->lv, NULL, FALSE);
}
