#ifndef CDEBUGGER_H
#define CDEBUGGER_H

#include <windows.h>
#include <stdint.h>
#include <vector>

enum class DbgSystem { None, Snes, Gb };

enum DbgMemoryType
{
	DBG_MEM_CPU       = 0,
	DBG_MEM_PRG_ROM   = 1,
	DBG_MEM_WORK_RAM  = 2,
	DBG_MEM_VIDEO_RAM = 3,
	DBG_MEM_OAM       = 4,
	DBG_MEM_CG_RAM    = 5,
	DBG_MEM_REGISTER  = 6,
	DBG_MEM_SRAM      = 7,
	DBG_MEM_BOOT_ROM  = 8,
	DBG_MEM_HIGH_RAM  = 9
};

struct DbgBreakpoint
{
	DbgSystem system        = DbgSystem::None;
	bool      enabled       = true;
	bool      brk_execute   = true;
	bool      brk_read      = false;
	bool      brk_write     = false;
	bool      mark_event    = false;
	bool      break_exec    = true;
	int       memory_type   = DBG_MEM_CPU;
	uint8_t   bank          = 0;
	uint16_t  address       = 0;
	char      condition[64] = {};
};

class CDebugger
{
public:
	void AttachSnes(HWND dlg);
	void AttachGb(HWND dlg);
	void DetachSnes();
	void DetachGb();

	bool IsSnesAttached() const { return snes_attached_; }
	bool IsGbAttached()   const { return gb_attached_; }

	bool IsGbBreakPending() const { return gb_break_pending_; }

	void OnSnesPreInstruction();
	void OnGbPreInstruction(uint16_t pc, uint8_t opcode);

	void OnSnesMemAccess(uint32_t addr24, uint8_t value, bool is_write);
	void OnGbMemAccess(uint16_t addr, uint8_t value, bool is_write);
	bool HasRWBreakpoint(DbgSystem sys, uint8_t bank, uint16_t addr, bool is_write) const;

	void Run();
	void Pause(DbgSystem requested_by = DbgSystem::None);
	void OnEmulatorReset();
	void StepIn(DbgSystem sys);
	void StepOver(DbgSystem sys);
	void StepOut(DbgSystem sys);
	void FrameStep();
	void StepOneScanline(DbgSystem sys);
	void RunToNmi(DbgSystem sys);
	void RunToIrq(DbgSystem sys);
	void RunToScanline(DbgSystem sys, int target_v);
	void BreakIn(DbgSystem sys, uint64_t cycles_from_now);
	void ResetMachine(DbgSystem sys);
	void ReloadRom();

	void AddExecBreakpoint(DbgSystem sys, uint8_t bank, uint16_t addr);
	void RemoveExecBreakpoint(DbgSystem sys, uint8_t bank, uint16_t addr);
	void ToggleExecBreakpoint(DbgSystem sys, uint8_t bank, uint16_t addr);
	bool HasExecBreakpoint(DbgSystem sys, uint8_t bank, uint16_t addr) const;
	const std::vector<DbgBreakpoint> &Breakpoints() const { return bps_; }

	void   AddBreakpoint(const DbgBreakpoint &bp);
	void   UpdateBreakpoint(size_t index, const DbgBreakpoint &bp);
	void   RemoveBreakpointAt(size_t index);
	void   ToggleBreakpointEnabledAt(size_t index);
	size_t BreakpointCount() const { return bps_.size(); }
	const  DbgBreakpoint *GetBreakpoint(size_t index) const;
	uint32_t BreakpointsVersion() const { return bps_version_; }

	HWND SnesDlg() const { return snes_dlg_; }
	HWND GbDlg()   const { return gb_dlg_; }

private:
	void HaltSnesNow();
	void HaltGbNow();
	void RefreshSnes();
	void RefreshGb();
	void RecomputeAttached();

	bool snes_attached_ = false;
	bool gb_attached_   = false;
	HWND snes_dlg_      = NULL;
	HWND gb_dlg_        = NULL;

	bool snes_free_run_ = true;
	bool gb_free_run_   = true;
	int  snes_step_remaining_ = 0;
	int  gb_step_remaining_   = 0;

	bool     snes_step_over_active_ = false;
	uint8_t  snes_step_over_bank_   = 0;
	uint16_t snes_step_over_addr_   = 0;

	bool     gb_step_over_active_ = false;
	uint16_t gb_step_over_addr_   = 0;

	bool snes_frame_step_armed_ = false;
	bool gb_frame_step_armed_   = false;

	bool gb_break_pending_      = false;

	bool snes_skip_exec_bp_once_ = false;
	bool gb_skip_exec_bp_once_   = false;

	bool     snes_run_to_nmi_           = false;
	bool     snes_run_to_irq_           = false;
	bool     snes_run_to_scanline_armed_ = false;
	int      snes_run_to_scanline_target_ = 0;
	bool     snes_step_one_scanline_    = false;
	int      snes_step_one_scanline_start_ = -1;
	bool     snes_break_in_armed_       = false;
	uint64_t snes_break_in_target_      = 0;

	std::vector<DbgBreakpoint> bps_;
	uint32_t bps_version_ = 0;

	void OnBpsChanged();
};

extern CDebugger gDebugger;

void WinShowSnesDebuggerDialog();
void WinShowGbDebuggerDialog();

extern HWND gSnesDebuggerHWND;
extern HWND gGbDebuggerHWND;

void S9xDebuggerRefreshAll();

#define WM_USER_DEBUGGER_REFRESH (WM_USER + 0x200)
#define WM_USER_DEBUGGER_HALT    (WM_USER + 0x201)

#endif
