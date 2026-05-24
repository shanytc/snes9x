#include "CDebugger.h"
#include "CDebuggerDlg.h"
#include "CDebuggerSnes.h"
#include "CDebuggerGb.h"
#include "debugger_hook.h"
#include "../snes9x.h"
#include "../memmap.h"
#include "../65c816.h"
#include "../sgb/sgb.h"

CDebugger gDebugger;

bool g_debugger_attached = false;

HWND gSnesDebuggerHWND = NULL;
HWND gGbDebuggerHWND   = NULL;

void CDebugger::RecomputeAttached()
{
	g_debugger_attached = snes_attached_ || gb_attached_;
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

	if (HasExecBreakpoint(DbgSystem::Snes, bank, addr))
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

	if (HasExecBreakpoint(DbgSystem::Gb, 0, pc))
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
	Settings.Paused = true;
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
	snes_run_to_nmi_ = false;
	snes_run_to_irq_ = false;
	snes_run_to_scanline_armed_ = false;
	snes_step_one_scanline_ = false;
	snes_break_in_armed_ = false;
	S9xSGBClearDebuggerBreak();
	Settings.Paused = false;
}

void CDebugger::Pause()
{
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
	}
	else if (sys == DbgSystem::Gb)
	{
		gb_free_run_ = false;
		gb_step_remaining_ = 1;
		snes_free_run_ = true;
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
	snes_frame_step_armed_ = true;
	gb_frame_step_armed_   = true;
	Run();
}

void CDebugger::StepOneScanline(DbgSystem sys)
{
	if (sys != DbgSystem::Snes)
	{
		StepIn(sys);
		return;
	}
	snes_step_one_scanline_       = true;
	snes_step_one_scanline_start_ = (int)CPU.V_Counter;
	snes_free_run_                = true;
	gb_free_run_                  = true;
	S9xSGBClearDebuggerBreak();
	Settings.Paused               = false;
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

void CDebugger::AddExecBreakpoint(DbgSystem sys, uint8_t bank, uint16_t addr)
{
	if (HasExecBreakpoint(sys, bank, addr))
		return;
	DbgBreakpoint bp{};
	bp.system  = sys;
	bp.enabled = true;
	bp.bank    = bank;
	bp.address = addr;
	bps_.push_back(bp);
}

void CDebugger::RemoveExecBreakpoint(DbgSystem sys, uint8_t bank, uint16_t addr)
{
	for (auto it = bps_.begin(); it != bps_.end(); ++it)
	{
		if (it->system == sys && it->bank == bank && it->address == addr)
		{
			bps_.erase(it);
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
		if (bp.system != sys) continue;
		if (sys == DbgSystem::Snes && bp.bank != bank) continue;
		if (bp.address == addr)
			return true;
	}
	return false;
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
