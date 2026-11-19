/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _CPUEXEC_H_
#define _CPUEXEC_H_

#include "ppu.h"
#ifdef DEBUGGER
#include "debug.h"
#endif

struct SOpcodes
{
	void (*S9xOpcode) (void);
};

struct SICPU
{
	struct SOpcodes	*S9xOpcodes;
	uint8	*S9xOpLengths;
	uint8	_Carry;
	uint8	_Zero;
	uint8	_Negative;
	uint8	_Overflow;
	uint32	ShiftedPB;
	uint32	ShiftedDB;
	uint32	Frame;
	uint32	FrameAdvanceCount;
};

extern S9X_MACHINE struct SICPU		ICPU;
extern S9X_MACHINE int32	S9xRefreshClocks;	// running total of DRAM refresh stalls

extern struct SOpcodes	S9xOpcodesE1[256];
extern struct SOpcodes	S9xOpcodesM1X1[256];
extern struct SOpcodes	S9xOpcodesM1X0[256];
extern struct SOpcodes	S9xOpcodesM0X1[256];
extern struct SOpcodes	S9xOpcodesM0X0[256];
extern struct SOpcodes	S9xOpcodesSlow[256];
extern uint8			S9xOpLengthsM1X1[256];
extern uint8			S9xOpLengthsM1X0[256];
extern uint8			S9xOpLengthsM0X1[256];
extern uint8			S9xOpLengthsM0X0[256];

void S9xMainLoop (void);
void S9xReset (void);
void S9xSoftReset (void);
void S9xSGBCaptureSoftResetCheckpoint (void);
void S9xSGBInvalidateSoftResetCheckpoint (void);
void S9xDoHEventProcessing (void);
void S9xRunPendingHDMA (int32 busLen);
void S9xCPUAddBusCyclesSlow (int32 n);
void S9xCPUBusCycleSlow (int32 busLen);
int32 S9xLastBusStart (void);
void S9xSettleLastBus (int32 shift);

// A chip's record of the CPU waiting on its status: the last read, and the turn of the wait loop at the
// sites seen to repeat one (by PB:PC, the E flag and the FastROM speed, which can change a turn's length).
struct SPollSkip
{
	uint32	pbpc;
	int32	cycles, next, period;
	uint64	line;
	uint32	site[8];
	int32	turn[8];
};
void S9xResetPollSkip (SPollSkip &p);
// At a status read of a wait `loop` (the chip checked its code) that can't change for `room` cycles: the CPU
// moved on by whole turns, short of the next event or IRQ timer; the cycles moved. `line`: the chip's line base.
int32 S9xSkipPollTurns (SPollSkip &p, uint64 line, bool loop, int64 room);

#ifndef INT32_MIN
#define INT32_MIN	(-2147483647 - 1)
#endif

// Length of the next bus cycle when n fetch/internal clocks remain.
static inline int32 S9xBusCycleLen (int32 n, int32 memSpeed)
{
	if (n % ONE_CYCLE == 0 && (n % memSpeed != 0 || memSpeed == ONE_CYCLE))
		return (ONE_CYCLE);
	return ((n >= memSpeed) ? memSpeed : n);
}

// Bus cycles ending before FastBusEnd need no event or HDMA work. A pending
// HDMA forces every bus cycle through the slow path.
static alwaysinline void S9xUpdateFastBusEnd (void)
{
	CPU.FastBusEnd = CPU.HDMAEdge ? INT32_MIN : CPU.NextEvent;
}

// A CPU bus cycle of busLen clocks starts: remembered for IRQ sampling, and a
// triggered HDMA takes the bus at the second one (bsnes timing).
static alwaysinline void S9xCPUBusCycleStart (int32 busLen)
{
	CPU.LastBusStart = CPU.Cycles;
	if (CPU.HDMAEdge && !--CPU.HDMAEdge)
	{
		S9xUpdateFastBusEnd();
		S9xRunPendingHDMA(busLen);
	}
}

// One bus cycle of busLen clocks, as a memory access.
static alwaysinline void S9xCPUBusCycle (int32 busLen)
{
	if (CPU.Cycles + busLen < CPU.FastBusEnd)
	{
		CPU.LastBusStart = CPU.Cycles;
		CPU.Cycles += busLen;
	}
	else
		S9xCPUBusCycleSlow(busLen);
}

// Add fetch/internal cycles one bus cycle at a time so the hook sees each.
// On the fast path only the last cycle's start matters, and
// S9xLastBusStart() splits the run to find it when an IRQ needs it.
static alwaysinline void S9xCPUAddBusCycles (int32 n)
{
	if (CPU.Cycles + n < CPU.FastBusEnd)
	{
		CPU.LastRunStart = CPU.Cycles;
		CPU.LastRunShape = n | (CPU.MemSpeed << 8);
		CPU.Cycles += n;
	}
	else
		S9xCPUAddBusCyclesSlow(n);
}

static inline void S9xUnpackStatus (void)
{
	ICPU._Zero = (Registers.PL & Zero) == 0;
	ICPU._Negative = (Registers.PL & Negative);
	ICPU._Carry = (Registers.PL & Carry);
	ICPU._Overflow = (Registers.PL & Overflow) >> 6;
}

static inline void S9xPackStatus (void)
{
	Registers.PL &= ~(Zero | Negative | Carry | Overflow);
	Registers.PL |= ICPU._Carry | ((ICPU._Zero == 0) << 1) | (ICPU._Negative & 0x80) | (ICPU._Overflow << 6);
}

static inline void S9xFixCycles (void)
{
	if (CheckEmulation())
	{
		ICPU.S9xOpcodes = S9xOpcodesE1;
		ICPU.S9xOpLengths = S9xOpLengthsM1X1;
	}
	else
	if (CheckMemory())
	{
		if (CheckIndex())
		{
			ICPU.S9xOpcodes = S9xOpcodesM1X1;
			ICPU.S9xOpLengths = S9xOpLengthsM1X1;
		}
		else
		{
			ICPU.S9xOpcodes = S9xOpcodesM1X0;
			ICPU.S9xOpLengths = S9xOpLengthsM1X0;
		}
	}
	else
	{
		if (CheckIndex())
		{
			ICPU.S9xOpcodes = S9xOpcodesM0X1;
			ICPU.S9xOpLengths = S9xOpLengthsM0X1;
		}
		else
		{
			ICPU.S9xOpcodes = S9xOpcodesM0X0;
			ICPU.S9xOpLengths = S9xOpLengthsM0X0;
		}
	}
}

#endif
