#include "CDebugger.h"
#include "CDebuggerDlg.h"
#include "CDebuggerSnes.h"
#include "CDebuggerGb.h"
#include "CMemoryViewer.h"
#include "debugger_hook.h"
#include "../snes9x.h"
#include "../memmap.h"
#include "../65c816.h"
#include "../ppu.h"
#include "../sgb/sgb.h"

CDebugger gDebugger;

bool g_debugger_attached = false;
bool g_debugger_check_rw = false;

static bool BpsHaveAnyRW(const std::vector<DbgBreakpoint> &bps)
{
	for (const auto &bp : bps)
		if (bp.enabled && (bp.brk_read || bp.brk_write)) return true;
	return false;
}

HWND gSnesDebuggerHWND = NULL;
HWND gGbDebuggerHWND   = NULL;

void CDebugger::RecomputeAttached()
{
	g_debugger_attached = snes_attached_ || gb_attached_;
	g_debugger_check_rw = g_debugger_attached && BpsHaveAnyRW(bps_);
}

void CDebugger::AttachSnes(HWND dlg)
{
	snes_dlg_      = dlg;
	snes_attached_ = true;
	snes_free_run_ = true;
	snes_step_remaining_ = 0;
	snes_step_over_active_ = false;
	gSnesDebuggerHWND = dlg;
	RecomputeAttached();
}

void CDebugger::AttachGb(HWND dlg)
{
	gb_dlg_      = dlg;
	gb_attached_ = true;
	gb_free_run_ = true;
	gb_step_remaining_ = 0;
	gb_step_over_active_ = false;
	gGbDebuggerHWND = dlg;
	RecomputeAttached();
	S9xSGBSetDebuggerHook(&S9xDebuggerOnGbPreInstruction);
}

void CDebugger::DetachSnes()
{
	snes_attached_ = false;
	snes_dlg_      = NULL;
	gSnesDebuggerHWND = NULL;
	RecomputeAttached();
}

void CDebugger::DetachGb()
{
	gb_attached_ = false;
	gb_dlg_      = NULL;
	gGbDebuggerHWND = NULL;
	RecomputeAttached();
	S9xSGBSetDebuggerHook(nullptr);
}

void CDebugger::OnSnesPreInstruction()
{
	if (!snes_attached_)
		return;

	const uint8_t  bank = Registers.PB;
	const uint16_t addr = Registers.PCw;

	if (snes_skip_exec_bp_once_)
	{
		snes_skip_exec_bp_once_ = false;
	}
	else if (HasExecBreakpoint(DbgSystem::Snes, bank, addr))
	{
		HaltSnesNow();
		return;
	}

	if (snes_step_over_active_ && bank == snes_step_over_bank_ && addr == snes_step_over_addr_)
	{
		snes_step_over_active_ = false;
		HaltSnesNow();
		return;
	}

	if (snes_run_to_nmi_ && CPU.NMIPending)
	{
		snes_run_to_nmi_ = false;
		HaltSnesNow();
		return;
	}

	if (snes_run_to_irq_ && (CPU.IRQLine || CPU.IRQExternal))
	{
		snes_run_to_irq_ = false;
		HaltSnesNow();
		return;
	}

	if (snes_run_to_scanline_armed_ && (int)CPU.V_Counter == snes_run_to_scanline_target_)
	{
		snes_run_to_scanline_armed_ = false;
		HaltSnesNow();
		return;
	}

	if (snes_step_one_scanline_ && (int)CPU.V_Counter != snes_step_one_scanline_start_)
	{
		snes_step_one_scanline_ = false;
		HaltSnesNow();
		return;
	}

	if (snes_frame_step_armed_ && IPPU.TotalEmulatedFrames != snes_frame_step_start_)
	{
		snes_frame_step_armed_ = false;
		HaltSnesNow();
		return;
	}

	if (snes_break_in_armed_ && (uint64_t)CPU.Cycles >= snes_break_in_target_)
	{
		snes_break_in_armed_ = false;
		HaltSnesNow();
		return;
	}

	if (!snes_free_run_)
	{
		if (snes_step_remaining_ <= 0)
		{
			HaltSnesNow();
			return;
		}
		snes_step_remaining_--;
	}
}

void CDebugger::OnGbPreInstruction(uint16_t pc, uint8_t opcode)
{
	(void)opcode;
	if (!gb_attached_)
		return;

	uint8_t cur_bank = 0;
	if (pc >= 0x4000 && pc < 0x8000)
		cur_bank = (uint8_t)(S9xSGBGetCurrentRomBank() & 0xFF);

	if (gb_skip_exec_bp_once_)
	{
		gb_skip_exec_bp_once_ = false;
	}
	else if (HasExecBreakpoint(DbgSystem::Gb, cur_bank, pc))
	{
		HaltGbNow();
		return;
	}

	if (gb_break_pending_)
	{
		HaltGbNow();
		return;
	}

	if (gb_step_over_active_ && pc == gb_step_over_addr_)
	{
		gb_step_over_active_ = false;
		HaltGbNow();
		return;
	}

	if (gb_step_one_scanline_armed_)
	{
		const uint64_t now_t = S9xSGBGetGbTCycles();
		if (now_t - gb_step_one_scanline_start_t_ >= 456)
		{
			gb_step_one_scanline_armed_ = false;
			HaltGbNow();
			return;
		}
	}

	if (gb_frame_step_armed_)
	{
		const uint64_t now_t = S9xSGBGetGbTCycles();
		if (now_t - gb_frame_step_start_t_ >= 70224)
		{
			gb_frame_step_armed_ = false;
			HaltGbNow();
			return;
		}
	}

	if (!gb_free_run_)
	{
		if (gb_step_remaining_ <= 0)
		{
			HaltGbNow();
			return;
		}
		gb_step_remaining_--;
	}
}

void CDebugger::HaltSnesNow()
{
	Settings.Paused = true;
	CPU.Flags |= SCAN_KEYS_FLAG;
	snes_free_run_ = false;
	snes_step_remaining_ = 0;
	RefreshSnes();
}

void CDebugger::HaltGbNow()
{
	gb_break_pending_ = false;
	Settings.Paused = true;
	CPU.Flags |= SCAN_KEYS_FLAG;
	gb_free_run_ = false;
	gb_step_remaining_ = 0;
	S9xSGBRequestDebuggerBreak();
	RefreshGb();
}

void CDebugger::Run()
{
	snes_free_run_ = true;
	gb_free_run_   = true;
	snes_step_over_active_ = false;
	gb_step_over_active_   = false;
	snes_frame_step_armed_ = false;
	gb_frame_step_armed_   = false;
	gb_step_one_scanline_armed_ = false;
	gb_break_pending_      = false;
	snes_run_to_nmi_ = false;
	snes_run_to_irq_ = false;
	snes_run_to_scanline_armed_ = false;
	snes_step_one_scanline_ = false;
	snes_break_in_armed_ = false;
	snes_skip_exec_bp_once_ = true;
	gb_skip_exec_bp_once_   = true;
	S9xSGBClearDebuggerBreak();
	Settings.Paused = false;
}

void CDebugger::Pause(DbgSystem requested_by)
{
	if (requested_by == DbgSystem::Gb && gb_attached_)
	{
		gb_break_pending_ = true;
		gb_free_run_      = false;
		RefreshGb();
		return;
	}

	Settings.Paused = true;
	snes_free_run_ = false;
	gb_free_run_   = false;
	RefreshSnes();
	RefreshGb();
}

void CDebugger::StepIn(DbgSystem sys)
{
	if (sys == DbgSystem::Snes)
	{
		snes_free_run_ = false;
		snes_step_remaining_ = 1;
		gb_free_run_ = true;
		snes_skip_exec_bp_once_ = true;
	}
	else if (sys == DbgSystem::Gb)
	{
		gb_free_run_ = false;
		gb_step_remaining_ = 1;
		snes_free_run_ = true;
		gb_skip_exec_bp_once_ = true;
	}
	S9xSGBClearDebuggerBreak();
	Settings.Paused = false;
}

void CDebugger::StepOver(DbgSystem sys)
{
	if (sys == DbgSystem::Snes)
	{
		const uint8_t  bank = Registers.PB;
		const uint16_t pc   = Registers.PCw;
		const uint32_t pc24 = ((uint32_t)bank << 16) | pc;
		const uint8_t  op   = SnesBackend::ReadByte(pc24);

		int len = 0;
		switch (op)
		{
			case 0x20: len = 3; break;  // JSR abs
			case 0x22: len = 4; break;  // JSL long
			case 0xFC: len = 3; break;  // JSR (a,X)
			default:   len = 0; break;
		}

		if (len > 0)
		{
			snes_step_over_active_ = true;
			snes_step_over_bank_   = bank;
			snes_step_over_addr_   = (uint16_t)(pc + len);
			snes_free_run_         = true;
			gb_free_run_           = true;
			snes_skip_exec_bp_once_ = true;
			S9xSGBClearDebuggerBreak();
			Settings.Paused        = false;
			return;
		}
	}
	else if (sys == DbgSystem::Gb)
	{
		GbStatus gs = {};
		GbBackend::GetStatus(&gs);
		const uint16_t pc = gs.pc;
		const uint8_t  op = GbBackend::ReadByte(pc);

		int len = 0;
		if (op == 0xCD || op == 0xC4 || op == 0xCC || op == 0xD4 || op == 0xDC)
			len = 3;
		else if ((op & 0xC7) == 0xC7)
			len = 1;

		if (len > 0)
		{
			gb_step_over_active_ = true;
			gb_step_over_addr_   = (uint16_t)(pc + len);
			snes_free_run_       = true;
			gb_free_run_         = true;
			gb_skip_exec_bp_once_ = true;
			S9xSGBClearDebuggerBreak();
			Settings.Paused      = false;
			return;
		}
	}

	StepIn(sys);
}

void CDebugger::StepOut(DbgSystem sys)
{
	StepIn(sys);
}

void CDebugger::FrameStep()
{
	Run();
	if (snes_attached_)
	{
		snes_frame_step_armed_ = true;
		snes_frame_step_start_ = IPPU.TotalEmulatedFrames;
	}
	if (gb_attached_)
	{
		gb_frame_step_armed_   = true;
		gb_frame_step_start_t_ = S9xSGBGetGbTCycles();
	}
}

void CDebugger::StepOneScanline(DbgSystem sys)
{
	if (sys == DbgSystem::Snes)
	{
		snes_step_one_scanline_       = true;
		snes_step_one_scanline_start_ = (int)CPU.V_Counter;
		snes_free_run_                = true;
		gb_free_run_                  = true;
		snes_skip_exec_bp_once_       = true;
		S9xSGBClearDebuggerBreak();
		Settings.Paused               = false;
		return;
	}
	if (sys == DbgSystem::Gb)
	{
		gb_step_one_scanline_armed_   = true;
		gb_step_one_scanline_start_t_ = S9xSGBGetGbTCycles();
		snes_free_run_                = true;
		gb_free_run_                  = true;
		gb_skip_exec_bp_once_         = true;
		S9xSGBClearDebuggerBreak();
		Settings.Paused               = false;
		return;
	}
	StepIn(sys);
}

void CDebugger::RunToNmi(DbgSystem sys)
{
	if (sys != DbgSystem::Snes) return;
	snes_run_to_nmi_ = true;
	snes_free_run_   = true;
	gb_free_run_     = true;
	S9xSGBClearDebuggerBreak();
	Settings.Paused  = false;
}

void CDebugger::RunToIrq(DbgSystem sys)
{
	if (sys != DbgSystem::Snes) return;
	snes_run_to_irq_ = true;
	snes_free_run_   = true;
	gb_free_run_     = true;
	S9xSGBClearDebuggerBreak();
	Settings.Paused  = false;
}

void CDebugger::RunToScanline(DbgSystem sys, int target_v)
{
	if (sys != DbgSystem::Snes) return;
	snes_run_to_scanline_armed_  = true;
	snes_run_to_scanline_target_ = target_v;
	snes_free_run_               = true;
	gb_free_run_                 = true;
	S9xSGBClearDebuggerBreak();
	Settings.Paused              = false;
}

void CDebugger::BreakIn(DbgSystem sys, uint64_t cycles_from_now)
{
	if (sys != DbgSystem::Snes)
	{
		StepIn(sys);
		return;
	}
	snes_break_in_armed_  = true;
	snes_break_in_target_ = (uint64_t)CPU.Cycles + cycles_from_now;
	snes_free_run_        = true;
	gb_free_run_          = true;
	S9xSGBClearDebuggerBreak();
	Settings.Paused       = false;
}

void CDebugger::ResetMachine(DbgSystem sys)
{
	(void)sys;
	S9xReset();
}

void CDebugger::ReloadRom()
{
	S9xReset();
}

void CDebugger::OnEmulatorReset()
{
	snes_free_run_         = true;
	gb_free_run_           = true;
	snes_step_remaining_   = 0;
	gb_step_remaining_     = 0;
	snes_step_over_active_ = false;
	gb_step_over_active_   = false;
	snes_frame_step_armed_ = false;
	snes_frame_step_start_ = 0;
	gb_frame_step_armed_   = false;
	gb_frame_step_start_t_ = 0;
	gb_step_one_scanline_armed_   = false;
	gb_step_one_scanline_start_t_ = 0;
	gb_break_pending_      = false;
	snes_run_to_nmi_       = false;
	snes_run_to_irq_       = false;
	snes_run_to_scanline_armed_ = false;
	snes_step_one_scanline_     = false;
	snes_step_one_scanline_start_ = -1;
	snes_break_in_armed_   = false;
	snes_skip_exec_bp_once_ = false;
	gb_skip_exec_bp_once_   = false;
	S9xSGBClearDebuggerBreak();
	if (snes_dlg_) DebuggerDlgInvalidateCache(snes_dlg_);
	if (gb_dlg_)   DebuggerDlgInvalidateCache(gb_dlg_);
	MemoryViewerRefreshAll();
	RefreshSnes();
	RefreshGb();
}

void S9xDebuggerOnEmulatorReset(void)
{
	gDebugger.OnEmulatorReset();
}

static int InferMemoryType(DbgSystem sys, uint8_t bank, uint16_t addr)
{
	if (sys == DbgSystem::Gb)
	{
		if (addr < 0x8000) return DBG_MEM_PRG_ROM;
		if (addr < 0xA000) return DBG_MEM_VIDEO_RAM;
		if (addr < 0xC000) return DBG_MEM_SRAM;
		if (addr < 0xFE00) return DBG_MEM_WORK_RAM;
		if (addr < 0xFEA0) return DBG_MEM_OAM;
		if (addr < 0xFF00) return DBG_MEM_CPU;
		if (addr < 0xFF80) return DBG_MEM_REGISTER;
		if (addr < 0xFFFF) return DBG_MEM_HIGH_RAM;
		return DBG_MEM_REGISTER;
	}
	if ((addr >= 0x2100 && addr < 0x2200) || (addr >= 0x4200 && addr < 0x4400))
		return DBG_MEM_REGISTER;
	if (bank == 0x7E || bank == 0x7F)
		return DBG_MEM_WORK_RAM;
	if (addr < 0x2000 && ((bank < 0x40) || (bank >= 0x80 && bank < 0xC0)))
		return DBG_MEM_WORK_RAM;
	if (bank >= 0xC0 || (addr >= 0x8000 && (bank < 0x40 || (bank >= 0x80 && bank < 0xC0))))
		return DBG_MEM_PRG_ROM;
	if (bank >= 0x40 && bank < 0x7E)
		return DBG_MEM_PRG_ROM;
	return DBG_MEM_CPU;
}

void CDebugger::AddExecBreakpoint(DbgSystem sys, uint8_t bank, uint16_t addr)
{
	if (HasExecBreakpoint(sys, bank, addr))
		return;
	DbgBreakpoint bp{};
	bp.system      = sys;
	bp.enabled     = true;
	bp.brk_execute = true;
	bp.memory_type = InferMemoryType(sys, bank, addr);
	bp.bank        = bank;
	bp.address     = addr;
	bps_.push_back(bp);
	OnBpsChanged();
}

void CDebugger::RemoveExecBreakpoint(DbgSystem sys, uint8_t bank, uint16_t addr)
{
	for (auto it = bps_.begin(); it != bps_.end(); ++it)
	{
		if (it->system == sys && it->bank == bank && it->address == addr && it->brk_execute)
		{
			bps_.erase(it);
			OnBpsChanged();
			return;
		}
	}
}

void CDebugger::ToggleExecBreakpoint(DbgSystem sys, uint8_t bank, uint16_t addr)
{
	if (HasExecBreakpoint(sys, bank, addr))
		RemoveExecBreakpoint(sys, bank, addr);
	else
		AddExecBreakpoint(sys, bank, addr);
}

bool CDebugger::HasExecBreakpoint(DbgSystem sys, uint8_t bank, uint16_t addr) const
{
	for (const auto &bp : bps_)
	{
		if (!bp.enabled) continue;
		if (!bp.brk_execute) continue;
		if (bp.system != sys) continue;
		if (sys == DbgSystem::Snes && bp.bank != bank) continue;
		if (sys == DbgSystem::Gb && addr < 0x8000 && bp.bank != bank) continue;
		if (bp.address == addr)
			return true;
	}
	return false;
}

void CDebugger::AddBreakpoint(const DbgBreakpoint &bp)
{
	bps_.push_back(bp);
	OnBpsChanged();
}

void CDebugger::UpdateBreakpoint(size_t index, const DbgBreakpoint &bp)
{
	if (index >= bps_.size()) return;
	bps_[index] = bp;
	OnBpsChanged();
}

void CDebugger::RemoveBreakpointAt(size_t index)
{
	if (index >= bps_.size()) return;
	bps_.erase(bps_.begin() + index);
	OnBpsChanged();
}

void CDebugger::ToggleBreakpointEnabledAt(size_t index)
{
	if (index >= bps_.size()) return;
	bps_[index].enabled = !bps_[index].enabled;
	OnBpsChanged();
}

const DbgBreakpoint *CDebugger::GetBreakpoint(size_t index) const
{
	if (index >= bps_.size()) return nullptr;
	return &bps_[index];
}

void CDebugger::OnBpsChanged()
{
	bps_version_++;
	g_debugger_check_rw = g_debugger_attached && BpsHaveAnyRW(bps_);
}

bool CDebugger::HasRWBreakpoint(DbgSystem sys, uint8_t bank, uint16_t addr, bool is_write) const
{
	for (const auto &bp : bps_)
	{
		if (!bp.enabled) continue;
		if (bp.system != sys) continue;
		if (is_write) { if (!bp.brk_write) continue; }
		else          { if (!bp.brk_read)  continue; }
		if (sys == DbgSystem::Snes && bp.bank != bank) continue;
		if (sys == DbgSystem::Gb && addr < 0x8000 && bp.bank != bank) continue;
		if (bp.address == addr) return true;
	}
	return false;
}

void CDebugger::OnSnesMemAccess(uint32_t addr24, uint8_t value, bool is_write)
{
	(void)value;
	if (!snes_attached_) return;
	const uint8_t  bank = (uint8_t)((addr24 >> 16) & 0xFF);
	const uint16_t addr = (uint16_t)(addr24 & 0xFFFF);
	if (HasRWBreakpoint(DbgSystem::Snes, bank, addr, is_write))
		HaltSnesNow();
}

void CDebugger::OnGbMemAccess(uint16_t addr, uint8_t value, bool is_write)
{
	(void)value;
	if (!gb_attached_) return;
	uint8_t cur_bank = 0;
	if (addr >= 0x4000 && addr < 0x8000)
		cur_bank = (uint8_t)(S9xSGBGetCurrentRomBank() & 0xFF);
	if (HasRWBreakpoint(DbgSystem::Gb, cur_bank, addr, is_write))
		HaltGbNow();
}

void S9xDebuggerOnSnesMemAccess(uint32_t addr24, uint8_t value, bool is_write)
{
	gDebugger.OnSnesMemAccess(addr24, value, is_write);
}

void S9xDebuggerOnGbMemAccess(uint16_t addr, uint8_t value, bool is_write)
{
	gDebugger.OnGbMemAccess(addr, value, is_write);
}

void CDebugger::RefreshSnes()
{
	if (snes_dlg_)
		PostMessage(snes_dlg_, WM_USER_DEBUGGER_REFRESH, 0, 0);
}

void CDebugger::RefreshGb()
{
	if (gb_dlg_)
		PostMessage(gb_dlg_, WM_USER_DEBUGGER_REFRESH, 0, 0);
}

void S9xDebuggerOnSnesPreInstruction(void)
{
	gDebugger.OnSnesPreInstruction();
}

void S9xDebuggerOnGbPreInstruction(uint16_t pc, uint8_t opcode)
{
	gDebugger.OnGbPreInstruction(pc, opcode);
}

void S9xDebuggerRefreshAll()
{
	if (gSnesDebuggerHWND)
		PostMessage(gSnesDebuggerHWND, WM_USER_DEBUGGER_REFRESH, 0, 0);
	if (gGbDebuggerHWND)
		PostMessage(gGbDebuggerHWND, WM_USER_DEBUGGER_REFRESH, 0, 0);
	MemoryViewerRefreshAll();
}

void WinShowSnesDebuggerDialog()
{
	DebuggerDlgGlobalInit(GetModuleHandle(NULL));
	if (gSnesDebuggerHWND && IsWindow(gSnesDebuggerHWND))
	{
		ShowWindow(gSnesDebuggerHWND, SW_SHOW);
		SetForegroundWindow(gSnesDebuggerHWND);
		return;
	}
	DebuggerDlgCreate(DbgSystem::Snes);
}

void WinShowGbDebuggerDialog()
{
	DebuggerDlgGlobalInit(GetModuleHandle(NULL));
	if (gGbDebuggerHWND && IsWindow(gGbDebuggerHWND))
	{
		ShowWindow(gGbDebuggerHWND, SW_SHOW);
		SetForegroundWindow(gGbDebuggerHWND);
		return;
	}
	DebuggerDlgCreate(DbgSystem::Gb);
}
