#ifndef CDEBUGGER_H
#define CDEBUGGER_H

#include <windows.h>
#include <stdint.h>
#include <vector>

enum class DbgSystem { None, Snes, Gb };

struct DbgBreakpoint
{
	DbgSystem system;
	bool      enabled;
	uint8_t   bank;
	uint16_t  address;
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

	void OnSnesPreInstruction();
	void OnGbPreInstruction(uint16_t pc, uint8_t opcode);

	void Run();
	void Pause();
	void StepIn(DbgSystem sys);
	void StepOver(DbgSystem sys);
	void StepOut(DbgSystem sys);
	void FrameStep();
	void ResetMachine(DbgSystem sys);

	void AddExecBreakpoint(DbgSystem sys, uint8_t bank, uint16_t addr);
	void RemoveExecBreakpoint(DbgSystem sys, uint8_t bank, uint16_t addr);
	void ToggleExecBreakpoint(DbgSystem sys, uint8_t bank, uint16_t addr);
	bool HasExecBreakpoint(DbgSystem sys, uint8_t bank, uint16_t addr) const;
	const std::vector<DbgBreakpoint> &Breakpoints() const { return bps_; }

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

	std::vector<DbgBreakpoint> bps_;
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
