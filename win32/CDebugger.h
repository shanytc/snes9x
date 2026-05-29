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

struct DbgCallFrame
{
	uint32_t target      = 0;   // routine entry address (full)
	uint32_t return_addr = 0;   // address execution returns to (full)
	uint32_t caller_pc   = 0;   // address of the call instruction (full)
	uint16_t sp_at_call  = 0;   // stack pointer right after the call pushed
	uint8_t  caller_bank = 0;   // GB ROM bank active at the call site
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
	void RunToWramExec(DbgSystem sys);
	void RunToRamDisable(DbgSystem sys);
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

	const std::vector<DbgCallFrame> &CallStack(DbgSystem sys) const;
	uint32_t CallStackVersion() const { return cs_version_; }

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

	bool     snes_frame_step_armed_     = false;
	uint32_t snes_frame_step_start_     = 0;
	bool     gb_frame_step_armed_       = false;
	uint64_t gb_frame_step_start_t_     = 0;

	bool     gb_step_one_scanline_armed_  = false;
	uint64_t gb_step_one_scanline_start_t_ = 0;

	// One-shot: halt the instant the GB fetches an instruction from outside
	// ROM (VRAM/SRAM/WRAM/echo/OAM/IO, i.e. PC in [0x8000,0xFF7F)). Catches
	// runaway jumps into RAM — the classic "game derailed into its data and
	// is executing garbage" failure. HRAM ($FF80-) is excluded since OAM-DMA
	// wait routines legitimately run there.
	bool gb_run_to_wram_exec_   = false;

	// One-shot: halt on a GB cart write that DISABLES RAM — a store to
	// $0000-$1FFF whose low nibble != $A (MBC RAM-enable register). Catches
	// the exact instant cart RAM is turned off (e.g. a runaway write that
	// strands a game executing from SRAM), skipping the many $0A re-enables.
	bool gb_run_to_ram_disable_ = false;

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

	std::vector<DbgCallFrame> snes_cs_;
	std::vector<DbgCallFrame> gb_cs_;
	uint32_t cs_version_ = 0;

	bool     cs_snes_valid_   = false;
	uint32_t cs_snes_prev_pc_ = 0;
	uint16_t cs_snes_prev_sp_ = 0;
	uint8_t  cs_snes_prev_op_ = 0;

	bool     cs_gb_valid_     = false;
	uint16_t cs_gb_prev_pc_   = 0;
	uint16_t cs_gb_prev_sp_   = 0;
	uint8_t  cs_gb_prev_op_   = 0;
	uint8_t  cs_gb_prev_bank_ = 0;

	void TrackSnesCallStack(uint8_t bank, uint16_t addr, uint16_t sp, uint8_t op);
	void TrackGbCallStack(uint16_t pc, uint16_t sp, uint8_t op, uint8_t bank);
	// Best-effort GB call-stack reconstruction by scanning RAM upward from SP:
	// a 16-bit value V is treated as a frame when mem[V-3] is a CALL opcode or
	// mem[V-1] is an RST. Stateless — works even when the debugger is opened
	// mid-run with no prior live tracking. Rebuilt at every GB halt.
	void UnwindGbCallStack();
	void ClearCallStacks();

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
// lParam = display address to center the disassembly view on.
#define WM_USER_DEBUGGER_GOTO    (WM_USER + 0x202)

#endif
