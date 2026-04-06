/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "snes9x.h"
#include "memmap.h"
#include "cpuops.h"
#include "dma.h"
#include "apu/apu.h"
#include "fxemu.h"
#include "snapshot.h"
#include "movie.h"
#include "xband.h"
#ifdef DEBUGGER
#include "debug.h"
#include "missing.h"
#endif

// XBAND debug: capture the very first BRK/COP executed inside the
// XBAND firmware bank, so the deadlock handler can show us the panic
// trigger site instead of just the eventual STP.
uint8  XBandFirstBrkOp   = 0;
uint32 XBandFirstBrkPC   = 0;
uint16 XBandFirstBrkS    = 0;
bool   XBandFirstBrkSeen = false;

static inline void S9xReschedule (void);

void S9xMainLoop (void)
{
	#define CHECK_FOR_IRQ_CHANGE() \
	if (Timings.IRQFlagChanging) \
	{ \
		if (Timings.IRQFlagChanging & IRQ_TRIGGER_NMI) \
		{ \
			CPU.NMIPending = TRUE; \
			Timings.NMITriggerPos = CPU.Cycles + 6; \
		} \
		if (Timings.IRQFlagChanging & IRQ_CLEAR_FLAG) \
			ClearIRQ(); \
		else if (Timings.IRQFlagChanging & IRQ_SET_FLAG) \
			SetIRQ(); \
		Timings.IRQFlagChanging = IRQ_NONE; \
	}

	if (CPU.Flags & SCAN_KEYS_FLAG)
	{
		CPU.Flags &= ~SCAN_KEYS_FLAG;
		S9xMovieUpdate();
	}

	for (;;)
	{
		if (CPU.NMIPending)
		{
			#ifdef DEBUGGER
			if (Settings.TraceHCEvent)
			    S9xTraceFormattedMessage ("Comparing %d to %d\n", Timings.NMITriggerPos, CPU.Cycles);
			#endif
			if (Timings.NMITriggerPos <= CPU.Cycles)
			{
				CPU.NMIPending = FALSE;
				Timings.NMITriggerPos = 0xffff;
				if (CPU.WaitingForInterrupt)
				{
					CPU.WaitingForInterrupt = FALSE;
					Registers.PCw++;
					CPU.Cycles += TWO_CYCLES + ONE_DOT_CYCLE / 2;
					while (CPU.Cycles >= CPU.NextEvent)
						S9xDoHEventProcessing();
				}

				CHECK_FOR_IRQ_CHANGE();
				S9xOpcode_NMI();
			}
		}

		if (CPU.Cycles >= Timings.NextIRQTimer)
		{
			#ifdef DEBUGGER
			S9xTraceMessage ("Timer triggered\n");
			#endif

			S9xUpdateIRQPositions(false);
			CPU.IRQLine = TRUE;
		}

		if (CPU.IRQLine || CPU.IRQExternal)
		{
			if (CPU.WaitingForInterrupt)
			{
				CPU.WaitingForInterrupt = FALSE;
				Registers.PCw++;
				CPU.Cycles += TWO_CYCLES + ONE_DOT_CYCLE / 2;
				while (CPU.Cycles >= CPU.NextEvent)
					S9xDoHEventProcessing();
			}

			if (!CheckFlag(IRQ))
			{
				/* The flag pushed onto the stack is the new value */
				CHECK_FOR_IRQ_CHANGE();
				S9xOpcode_IRQ();
			}
		}

		/* Change IRQ flag for instructions that set it only on last cycle */
		CHECK_FOR_IRQ_CHANGE();

	#ifdef DEBUGGER
		if ((CPU.Flags & BREAK_FLAG) && !(CPU.Flags & SINGLE_STEP_FLAG))
		{
			for (int Break = 0; Break != 6; Break++)
			{
				if (S9xBreakpoint[Break].Enabled &&
					S9xBreakpoint[Break].Bank == Registers.PB &&
					S9xBreakpoint[Break].Address == Registers.PCw)
				{
					if (S9xBreakpoint[Break].Enabled == 2)
						S9xBreakpoint[Break].Enabled = TRUE;
					else
						CPU.Flags |= DEBUG_MODE_FLAG;
				}
			}
		}

		if (CPU.Flags & DEBUG_MODE_FLAG)
			break;

		if (CPU.Flags & TRACE_FLAG)
			S9xTrace();

		if (CPU.Flags & SINGLE_STEP_FLAG)
		{
			CPU.Flags &= ~SINGLE_STEP_FLAG;
			CPU.Flags |= DEBUG_MODE_FLAG;
		}
	#endif

		if (CPU.Flags & SCAN_KEYS_FLAG)
		{
			break;
		}

		uint8				Op;
		struct	SOpcodes	*Opcodes;

		if (CPU.PCBase)
		{
			Op = CPU.PCBase[Registers.PCw];
			CPU.Cycles += CPU.MemSpeed;
			Opcodes = ICPU.S9xOpcodes;

			// XBAND debug: trap the first BRK / COP / ABORT-style trap
			// instructions executed inside the firmware bank. Save PC,
			// stack contents and the surrounding code so the deadlock
			// dialog can show us where the firmware tripped its panic.
			extern uint8  XBandFirstBrkOp;
			extern uint32 XBandFirstBrkPC;
			extern uint16 XBandFirstBrkS;
			extern bool   XBandFirstBrkSeen;
			if (Settings.XBAND && !XBandFirstBrkSeen)
			{
				unsigned cpb = (unsigned)((Registers.PBPC >> 16) & 0xFF);
				if ((Op == 0x00 || Op == 0x02) && cpb >= 0xC0)
				{
					XBandFirstBrkOp   = Op;
					XBandFirstBrkPC   = Registers.PBPC & 0xffffff;
					XBandFirstBrkS    = Registers.S.W;
					XBandFirstBrkSeen = true;
				}
			}

			if (CPU.Cycles > 1000000)
			{
				Settings.StopEmulation = true;
				CPU.Flags |= HALTED_FLAG;
				{
					static char msg[32768];
					unsigned pb = (unsigned)((Registers.PBPC >> 16) & 0xFF);
					unsigned pc = (unsigned)(Registers.PCw & 0xFFFF);
					int pos = snprintf(msg, sizeof(msg),
						"CPU is deadlocked at PB:PC=%02X:%04X opcode=%02X\n"
						"A=%04X X=%04X Y=%04X S=%04X D=%04X DB=%02X\n\n",
						pb, pc, (unsigned)Op,
						(unsigned)Registers.A.W,
						(unsigned)Registers.X.W,
						(unsigned)Registers.Y.W,
						(unsigned)Registers.S.W,
						(unsigned)Registers.D.W,
						(unsigned)Registers.DB);

					// Show the first BRK/COP executed inside the XBAND
					// firmware. This is the panic trigger point — every-
					// thing afterward (BRK handler -> recovery -> STP)
					// is just consequence.
					if (XBandFirstBrkSeen)
					{
						pos += snprintf(msg + pos, sizeof(msg) - pos,
							"FIRST BRK/COP in XBAND ROM:\n"
							"  opcode=%02X at PB:PC=%02X:%04X (S=%04X)\n",
							(unsigned)XBandFirstBrkOp,
							(unsigned)((XBandFirstBrkPC >> 16) & 0xFF),
							(unsigned)(XBandFirstBrkPC & 0xFFFF),
							(unsigned)XBandFirstBrkS);

						if (Settings.XBAND)
							S9xXBandTraceSuppress(true);

						uint8  trap_pb = (uint8)((XBandFirstBrkPC >> 16) & 0xFF);
						uint16 trap_pc = (uint16)(XBandFirstBrkPC & 0xFFFF);
						for (int row = 0; row < 4; row++)
						{
							int row_start = -32 + row * 16;
							pos += snprintf(msg + pos, sizeof(msg) - pos,
								"  %02X:%04X: ",
								(unsigned)trap_pb,
								(unsigned)(((int)trap_pc + row_start) & 0xFFFF));
							for (int col = 0; col < 16; col++)
							{
								int ofs = row_start + col;
								uint32 a = ((uint32)trap_pb << 16) |
								           (((int)trap_pc + ofs) & 0xFFFF);
								uint8 b = S9xGetByte(a);
								bool mark = (ofs == 0);
								pos += snprintf(msg + pos, sizeof(msg) - pos,
									"%s%02X%s",
									mark ? "<" : "",
									(unsigned)b,
									mark ? ">" : " ");
							}
							pos += snprintf(msg + pos, sizeof(msg) - pos, "\n");
						}
						pos += snprintf(msg + pos, sizeof(msg) - pos, "\n");

						if (Settings.XBAND)
							S9xXBandTraceSuppress(false);
					}
					else
					{
						pos += snprintf(msg + pos, sizeof(msg) - pos,
							"NO BRK/COP seen in XBAND firmware bank.\n"
							"Panic was reached via different mechanism.\n\n");
					}

					if (Settings.XBAND)
						S9xXBandTraceSuppress(true);

					// Dump code around PC so we can see the panic handler
					// and what fell through to it. 128 bytes before, 32
					// after = 10 lines of 16 bytes.
					pos += snprintf(msg + pos, sizeof(msg) - pos,
						"Bytes around %02X:%04X:\n", pb, pc);
					int before = 128, after = 32;
					for (int row = 0; row < (before + after + 15) / 16; row++)
					{
						int row_start = -before + row * 16;
						pos += snprintf(msg + pos, sizeof(msg) - pos,
							"  %02X:%04X: ", pb,
							(unsigned)(((int)pc + row_start) & 0xFFFF));
						for (int col = 0; col < 16; col++)
						{
							int ofs = row_start + col;
							uint32 a = (Registers.PBPC & 0xff0000) |
							           (((int)pc + ofs) & 0xFFFF);
							uint8 b = S9xGetByte(a);
							pos += snprintf(msg + pos, sizeof(msg) - pos,
								"%s%02X%s",
								(ofs == 0) ? "[" : "",
								(unsigned)b,
								(ofs == 0) ? "]" : " ");
						}
						pos += snprintf(msg + pos, sizeof(msg) - pos, "\n");
					}

					// Dump the stack so we can see return addresses left
					// by whoever JSL'd/JSR'd into this panic handler.
					// 65816 stack grows down from S, so the most recent
					// pushes are at [S+1], [S+2], [S+3]...
					pos += snprintf(msg + pos, sizeof(msg) - pos,
						"\nStack around S=%04X (next pushed at S+1):\n",
						(unsigned)Registers.S.W);
					for (int row = 0; row < 4; row++)
					{
						uint16 sa = (uint16)(Registers.S.W + 1 + row * 16);
						pos += snprintf(msg + pos, sizeof(msg) - pos,
							"  00:%04X: ", (unsigned)sa);
						for (int col = 0; col < 16; col++)
						{
							uint8 b = S9xGetByte((uint16)(sa + col));
							pos += snprintf(msg + pos, sizeof(msg) - pos,
								"%02X ", (unsigned)b);
						}
						pos += snprintf(msg + pos, sizeof(msg) - pos, "\n");
					}
					pos += snprintf(msg + pos, sizeof(msg) - pos, "\n");

					// Scan the stack for plausible JSL return addresses.
					// Require PBR to be in the HiROM code region ($C0-$FF)
					// since that's where the XBAND firmware lives, and
					// dedupe so we don't dump the same address repeatedly.
					pos += snprintf(msg + pos, sizeof(msg) - pos,
						"Possible caller sites (derived from stack):\n");
					uint32 seen[8] = {0};
					int seen_count = 0;
					int found = 0;
					for (int off = 0; off < 48 && found < 4; off++)
					{
						uint16 a0 = (uint16)(Registers.S.W + 1 + off);
						uint8 pcl = S9xGetByte(a0);
						uint8 pch = S9xGetByte((uint16)(a0 + 1));
						uint8 pbr = S9xGetByte((uint16)(a0 + 2));

						// Must be a HiROM code bank.
						if (pbr < 0xC0 || pbr > 0xFF)
							continue;

						uint32 ret_addr = ((uint32)pbr << 16) |
						                  ((uint32)pch << 8)  | pcl;

						// Dedupe: if we've already reported this address,
						// skip. (JSL return addresses adjacent on the
						// stack often produce the same candidate from
						// slightly different offsets.)
						bool dup = false;
						for (int s = 0; s < seen_count; s++)
							if (seen[s] == ret_addr) { dup = true; break; }
						if (dup) continue;
						if (seen_count < 8)
							seen[seen_count++] = ret_addr;

						// Require the target byte to look like a valid
						// instruction continuation (not ROM pad / fill).
						uint8 target_byte = S9xGetByte(ret_addr);
						if (target_byte == 0x55 || target_byte == 0xFF)
							continue;

						pos += snprintf(msg + pos, sizeof(msg) - pos,
							"  stack[%04X..%04X] -> after JSL returns to %02X:%04X\n",
							(unsigned)a0, (unsigned)(a0 + 2),
							(unsigned)pbr, (unsigned)((pch << 8) | pcl));

						// Dump 48 bytes before and 16 after this return
						// address so we can see the JSL site and the
						// check that led to it.
						uint16 pc2 = (uint16)((pch << 8) | pcl);
						int b2 = 48, a2 = 16;
						for (int row = 0; row < (b2 + a2 + 15) / 16; row++)
						{
							int row_start = -b2 + row * 16;
							pos += snprintf(msg + pos, sizeof(msg) - pos,
								"    %02X:%04X: ", (unsigned)pbr,
								(unsigned)(((int)pc2 + row_start) & 0xFFFF));
							for (int col = 0; col < 16; col++)
							{
								int ofs = row_start + col;
								uint32 a = ((uint32)pbr << 16) |
								           (((int)pc2 + ofs) & 0xFFFF);
								uint8 b = S9xGetByte(a);
								pos += snprintf(msg + pos, sizeof(msg) - pos,
									"%s%02X%s",
									(ofs == 0) ? "<" : "",
									(unsigned)b,
									(ofs == 0) ? ">" : " ");
							}
							pos += snprintf(msg + pos, sizeof(msg) - pos, "\n");
						}
						found++;
					}
					pos += snprintf(msg + pos, sizeof(msg) - pos, "\n");

					// Scan the XBAND ROM for ANY references to the crash
					// handler entries: $D0:5000, $D0:500F, $D0:50DB,
					// $D0:50EC, $D0:5107, and the $D0:4E80 handler too.
					// Covers JSL/JML 4-byte absolute long and BRL in the
					// same bank.
					if (Settings.XBAND)
					{
						static const uint16 targets[] = {
							0x5000, 0x500F, 0x50DB, 0x50EC, 0x5107,
							0x4E80, 0x4E96,
						};
						const int num_targets = (int)(sizeof(targets)/sizeof(targets[0]));

						pos += snprintf(msg + pos, sizeof(msg) - pos,
							"References to crash handlers in XBAND ROM:\n");
						int refs = 0;

						// JSL/JML $D0:<target>
						for (uint32 off = 0; off + 4 < 0x100000 && refs < 12; off++)
						{
							uint8 b0 = Memory.ROM[off];
							if (b0 != 0x22 && b0 != 0x5C) continue;
							uint8 b1 = Memory.ROM[off+1];
							uint8 b2 = Memory.ROM[off+2];
							uint8 b3 = Memory.ROM[off+3];
							if (b3 != 0xD0) continue;
							uint16 tgt = (uint16)(b1 | (b2 << 8));
							for (int t = 0; t < num_targets; t++)
							{
								if (tgt == targets[t])
								{
									pos += snprintf(msg + pos, sizeof(msg) - pos,
										"  %s $D0:%04X at rom+%05X\n",
										(b0 == 0x22) ? "JSL" : "JML",
										(unsigned)tgt, (unsigned)off);
									refs++;
									break;
								}
							}
						}

						// BRL $<target> — scan bank $C0 (first 64KB of ROM).
						for (uint32 off = 0; off + 3 < 0x10000 && refs < 20; off++)
						{
							if (Memory.ROM[off] != 0x82) continue;
							int16 disp = (int16)(Memory.ROM[off+1] |
							                     (Memory.ROM[off+2] << 8));
							uint16 target = (uint16)(off + 3 + disp);
							for (int t = 0; t < num_targets; t++)
							{
								if (target == targets[t])
								{
									pos += snprintf(msg + pos, sizeof(msg) - pos,
										"  BRL $%04X from D0:%04X\n",
										(unsigned)target, (unsigned)off);
									refs++;
									break;
								}
							}
						}

						if (refs == 0)
							pos += snprintf(msg + pos, sizeof(msg) - pos,
								"  (no direct references found)\n");

						// Dump $D0:0000-$D0:00FF — firmware entry (reset
						// vector ultimately targets this).
						pos += snprintf(msg + pos, sizeof(msg) - pos,
							"\nFirmware entry at $D0:0000 (256 bytes):\n");
						for (int row = 0; row < 16; row++)
						{
							uint32 base = 0x0000 + row * 16;
							pos += snprintf(msg + pos, sizeof(msg) - pos,
								"  D0:%04X: ", (unsigned)base);
							for (int col = 0; col < 16; col++)
								pos += snprintf(msg + pos, sizeof(msg) - pos,
									"%02X ", (unsigned)Memory.ROM[base + col]);
							pos += snprintf(msg + pos, sizeof(msg) - pos, "\n");
						}

						// Dump $D0:5000-$D0:510F — code *before* the
						// recovery entry at $D0:5107 so we can see the
						// fall-through / branch that reaches it.
						pos += snprintf(msg + pos, sizeof(msg) - pos,
							"\nCode at $D0:5000-$D0:510F:\n");
						for (int row = 0; row < 17; row++)
						{
							uint32 base = 0x5000 + row * 16;
							pos += snprintf(msg + pos, sizeof(msg) - pos,
								"  D0:%04X: ", (unsigned)base);
							for (int col = 0; col < 16; col++)
							{
								uint8 b = Memory.ROM[base + col];
								bool mark = (base + col == 0x5107);
								pos += snprintf(msg + pos, sizeof(msg) - pos,
									"%s%02X%s",
									mark ? "<" : "",
									(unsigned)b,
									mark ? ">" : " ");
							}
							pos += snprintf(msg + pos, sizeof(msg) - pos, "\n");
						}

						// Dump the vector trampoline area ($00:$FF80-$FFDF)
						// where native COP/BRK/ABORT/NMI/IRQ land.
						pos += snprintf(msg + pos, sizeof(msg) - pos,
							"\nVector trampolines at rom+FF80..FFDF:\n");
						for (int row = 0; row < 6; row++)
						{
							uint32 base = 0xFF80 + row * 16;
							pos += snprintf(msg + pos, sizeof(msg) - pos,
								"  %04X: ", (unsigned)base);
							for (int col = 0; col < 16; col++)
								pos += snprintf(msg + pos, sizeof(msg) - pos,
									"%02X ", (unsigned)Memory.ROM[base + col]);
							pos += snprintf(msg + pos, sizeof(msg) - pos, "\n");
						}

						// Dump the SNES interrupt/reset vectors at the
						// end of bank 0 (ROM offsets $FFE0-$FFFF). Native
						// and emulation mode vectors live here.
						pos += snprintf(msg + pos, sizeof(msg) - pos,
							"\nInterrupt vectors at rom+FFE0..FFFF:\n");
						for (int row = 0; row < 2; row++)
						{
							uint32 base = 0xFFE0 + row * 16;
							pos += snprintf(msg + pos, sizeof(msg) - pos,
								"  %04X: ", (unsigned)base);
							for (int col = 0; col < 16; col++)
							{
								uint8 b = Memory.ROM[base + col];
								pos += snprintf(msg + pos, sizeof(msg) - pos,
									"%02X ", (unsigned)b);
							}
							pos += snprintf(msg + pos, sizeof(msg) - pos, "\n");
						}
						{
							uint16 reset_vec =
								Memory.ROM[0xFFFC] |
								(Memory.ROM[0xFFFD] << 8);
							uint16 nmi_vec =
								Memory.ROM[0xFFFA] |
								(Memory.ROM[0xFFFB] << 8);
							uint16 irq_vec =
								Memory.ROM[0xFFFE] |
								(Memory.ROM[0xFFFF] << 8);
							pos += snprintf(msg + pos, sizeof(msg) - pos,
								"  Decoded (emu): RESET=$00:%04X  NMI=$00:%04X  IRQ/BRK=$00:%04X\n",
								(unsigned)reset_vec,
								(unsigned)nmi_vec,
								(unsigned)irq_vec);
						}
						pos += snprintf(msg + pos, sizeof(msg) - pos, "\n");
					}

					// Dump 48 bytes of code around each unique caller-PC
					// seen in the XBAND MMIO trace. This lets us see the
					// instruction stream around every register access,
					// including the CMP / branch that followed a status
					// read. Much more informative than just dumping the
					// fixed locations we thought were important.
					if (Settings.XBAND)
					{
						uint32 pc_seen[32] = {0};
						int    pc_count    = 0;
						pos += snprintf(msg + pos, sizeof(msg) - pos,
							"Code around each unique MMIO caller PC:\n");
						for (int e = 0; e < XBAND_TRACE_SIZE; e++)
						{
							XBandTraceEntry entry;
							if (!S9xXBandGetTraceEntry(e, &entry))
								break;
							uint32 p = entry.caller_pc;
							bool dup = false;
							for (int k = 0; k < pc_count; k++)
								if (pc_seen[k] == p) { dup = true; break; }
							if (dup) continue;
							if (pc_count >= 32) break;
							pc_seen[pc_count++] = p;

							uint8 pcb = (uint8)((p >> 16) & 0xFF);
							uint16 plo = (uint16)(p & 0xFFFF);
							pos += snprintf(msg + pos, sizeof(msg) - pos,
								"  pc=%06X  (%s $%06X):\n",
								(unsigned)p,
								entry.is_write ? "W" : "R",
								(unsigned)(entry.address & 0xFFFFFF));
							for (int row = 0; row < 3; row++)
							{
								int row_start = -16 + row * 16;
								pos += snprintf(msg + pos, sizeof(msg) - pos,
									"    %02X:%04X: ",
									(unsigned)pcb,
									(unsigned)(((int)plo + row_start) & 0xFFFF));
								for (int col = 0; col < 16; col++)
								{
									int ofs = row_start + col;
									uint32 a = ((uint32)pcb << 16) |
									           (((int)plo + ofs) & 0xFFFF);
									uint8 b = S9xGetByte(a);
									bool mark = (ofs == 0);
									pos += snprintf(msg + pos, sizeof(msg) - pos,
										"%s%02X%s",
										mark ? "<" : "",
										(unsigned)b,
										mark ? ">" : " ");
								}
								pos += snprintf(msg + pos, sizeof(msg) - pos, "\n");
							}
						}
						pos += snprintf(msg + pos, sizeof(msg) - pos, "\n");
					}

					// Hardcoded dump of the XBAND "main" function at
					// $D0:4C55, plus scan for the first RTL / RTS / RTI
					// that would exit main (so we can see why/where
					// it's returning).
					if (Settings.XBAND)
					{
						pos += snprintf(msg + pos, sizeof(msg) - pos,
							"XBAND main at $D0:4C55 (256 bytes):\n");
						for (int row = 0; row < 16; row++)
						{
							uint32 base = 0xD04C55 + row * 16;
							pos += snprintf(msg + pos, sizeof(msg) - pos,
								"  %02X:%04X: ",
								(unsigned)((base >> 16) & 0xFF),
								(unsigned)(base & 0xFFFF));
							for (int col = 0; col < 16; col++)
							{
								uint32 a = base + col;
								uint8 b = S9xGetByte(a);
								bool is_target = (a == 0xD04C55);
								pos += snprintf(msg + pos, sizeof(msg) - pos,
									"%s%02X%s",
									is_target ? "<" : "",
									(unsigned)b,
									is_target ? ">" : " ");
							}
							pos += snprintf(msg + pos, sizeof(msg) - pos, "\n");
						}
						pos += snprintf(msg + pos, sizeof(msg) - pos, "\n");

						// Find the first RTL (6B) in the next ~1KB after
						// main's entry and dump 48 bytes of code around
						// it. That's the likely exit path we're hitting.
						uint32 exit_addr = 0;
						for (uint32 o = 0; o < 0x400; o++)
						{
							uint8 b = S9xGetByte(0xD04C55 + o);
							if (b == 0x6B) { exit_addr = 0xD04C55 + o; break; }
						}
						if (exit_addr)
						{
							pos += snprintf(msg + pos, sizeof(msg) - pos,
								"First RTL in main at $%06X:\n", exit_addr);
							for (int row = 0; row < 4; row++)
							{
								int row_start = -32 + row * 16;
								uint32 base = exit_addr + row_start;
								pos += snprintf(msg + pos, sizeof(msg) - pos,
									"  %02X:%04X: ",
									(unsigned)((base >> 16) & 0xFF),
									(unsigned)(base & 0xFFFF));
								for (int col = 0; col < 16; col++)
								{
									uint32 a = base + col;
									uint8 b = S9xGetByte(a);
									bool mark = (a == exit_addr);
									pos += snprintf(msg + pos, sizeof(msg) - pos,
										"%s%02X%s",
										mark ? "<" : "",
										(unsigned)b,
										mark ? ">" : " ");
								}
								pos += snprintf(msg + pos, sizeof(msg) - pos, "\n");
							}
							pos += snprintf(msg + pos, sizeof(msg) - pos, "\n");
						}
					}

					if (Settings.XBAND)
						S9xXBandTraceSuppress(false);

					if (Settings.XBAND && pos > 0 && (size_t)pos < sizeof(msg))
						S9xXBandDumpTrace(msg + pos, sizeof(msg) - pos);
					S9xMessage(S9X_FATAL_ERROR, 0, msg);
				}
				return;
			}
		}
		else
		{
			Op = S9xGetByte(Registers.PBPC);
			OpenBus = Op;
			Opcodes = S9xOpcodesSlow;
		}

		if ((Registers.PCw & MEMMAP_MASK) + ICPU.S9xOpLengths[Op] >= MEMMAP_BLOCK_SIZE)
		{
			uint8	*oldPCBase = CPU.PCBase;

			CPU.PCBase = S9xGetBasePointer(ICPU.ShiftedPB + ((uint16) (Registers.PCw + 4)));
			if (oldPCBase != CPU.PCBase || (Registers.PCw & ~MEMMAP_MASK) == (0xffff & ~MEMMAP_MASK))
				Opcodes = S9xOpcodesSlow;
		}

		Registers.PCw++;
		(*Opcodes[Op].S9xOpcode)();

		if (Settings.SA1)
			S9xSA1MainLoop();
	}

	S9xPackStatus();
}

static inline void S9xReschedule (void)
{
	switch (CPU.WhichEvent)
	{
		case HC_HBLANK_START_EVENT:
			CPU.WhichEvent = HC_HDMA_START_EVENT;
			CPU.NextEvent  = Timings.HDMAStart;
			break;

		case HC_HDMA_START_EVENT:
			CPU.WhichEvent = HC_HCOUNTER_MAX_EVENT;
			CPU.NextEvent  = Timings.H_Max;
			break;

		case HC_HCOUNTER_MAX_EVENT:
			CPU.WhichEvent = HC_HDMA_INIT_EVENT;
			CPU.NextEvent  = Timings.HDMAInit;
			break;

		case HC_HDMA_INIT_EVENT:
			CPU.WhichEvent = HC_RENDER_EVENT;
			CPU.NextEvent  = Timings.RenderPos;
			break;

		case HC_RENDER_EVENT:
			CPU.WhichEvent = HC_WRAM_REFRESH_EVENT;
			CPU.NextEvent  = Timings.WRAMRefreshPos;
			break;

		case HC_WRAM_REFRESH_EVENT:
			CPU.WhichEvent = HC_HBLANK_START_EVENT;
			CPU.NextEvent  = Timings.HBlankStart;
			break;
	}
}

void S9xDoHEventProcessing (void)
{
#ifdef DEBUGGER
	static char	eventname[7][32] =
	{
		"",
		"HC_HBLANK_START_EVENT",
		"HC_HDMA_START_EVENT  ",
		"HC_HCOUNTER_MAX_EVENT",
		"HC_HDMA_INIT_EVENT   ",
		"HC_RENDER_EVENT      ",
		"HC_WRAM_REFRESH_EVENT"
	};
#endif

#ifdef DEBUGGER
	if (Settings.TraceHCEvent)
		S9xTraceFormattedMessage("--- HC event processing  (%s)  expected HC:%04d  executed HC:%04d VC:%04d",
			eventname[CPU.WhichEvent], CPU.NextEvent, CPU.Cycles, CPU.V_Counter);
#endif

	switch (CPU.WhichEvent)
	{
		case HC_HBLANK_START_EVENT:
			S9xReschedule();
			break;

		case HC_HDMA_START_EVENT:
			S9xReschedule();

			if (PPU.HDMA && CPU.V_Counter <= PPU.ScreenHeight)
			{
			#ifdef DEBUGGER
				S9xTraceFormattedMessage("*** HDMA Transfer HC:%04d, Channel:%02x", CPU.Cycles, PPU.HDMA);
			#endif
				PPU.HDMA = S9xDoHDMA(PPU.HDMA);
			}

			break;

		case HC_HCOUNTER_MAX_EVENT:
			if (Settings.SuperFX)
			{
				if (!SuperFX.oneLineDone)
					S9xSuperFXExec();
				SuperFX.oneLineDone = FALSE;
			}

			S9xAPUEndScanline();
			CPU.Cycles -= Timings.H_Max;
			if (Timings.NMITriggerPos != 0xffff)
				Timings.NMITriggerPos -= Timings.H_Max;
			if (Timings.NextIRQTimer != 0x0fffffff)
				Timings.NextIRQTimer -= Timings.H_Max;
			S9xAPUSetReferenceTime(CPU.Cycles);

			if (Settings.SA1)
				SA1.Cycles -= Timings.H_Max * 3;

			CPU.V_Counter++;
			if (CPU.V_Counter >= Timings.V_Max)	// V ranges from 0 to Timings.V_Max - 1
			{
				CPU.V_Counter = 0;

				// From byuu:
				// [NTSC]
				// interlace mode has 525 scanlines: 263 on the even frame, and 262 on the odd.
				// non-interlace mode has 524 scanlines: 262 scanlines on both even and odd frames.
				// [PAL] <PAL info is unverified on hardware>
				// interlace mode has 625 scanlines: 313 on the even frame, and 312 on the odd.
				// non-interlace mode has 624 scanlines: 312 scanlines on both even and odd frames.
				if (IPPU.Interlace && S9xInterlaceField())
					Timings.V_Max = Timings.V_Max_Master + 1;	// 263 (NTSC), 313?(PAL)
				else
					Timings.V_Max = Timings.V_Max_Master;		// 262 (NTSC), 312?(PAL)

				Memory.FillRAM[0x213F] ^= 0x80;
				PPU.RangeTimeOver = 0;

				// FIXME: reading $4210 will wait 2 cycles, then perform reading, then wait 4 more cycles.
				Memory.FillRAM[0x4210] = Model->_5A22;

				ICPU.Frame++;
				PPU.HVBeamCounterLatched = 0;

				// Shuttle modem bytes between the XBAND socket and the
				// UART FIFOs once per frame.
				if (Settings.XBAND)
					S9xXBandPoll();
			}

			// From byuu:
			// In non-interlace mode, there are 341 dots per scanline, and 262 scanlines per frame.
			// On odd frames, scanline 240 is one dot short.
			// In interlace mode, there are always 341 dots per scanline. Even frames have 263 scanlines,
			// and odd frames have 262 scanlines.
			// Interlace mode scanline 240 on odd frames is not missing a dot.
			if (CPU.V_Counter == 240 && !IPPU.Interlace && S9xInterlaceField())	// V=240
				Timings.H_Max = Timings.H_Max_Master - ONE_DOT_CYCLE;	// HC=1360
			else
				Timings.H_Max = Timings.H_Max_Master;					// HC=1364

			if (Model->_5A22 == 2)
			{
				if (CPU.V_Counter != 240 || IPPU.Interlace || !S9xInterlaceField())	// V=240
				{
					if (Timings.WRAMRefreshPos == SNES_WRAM_REFRESH_HC_v2 - ONE_DOT_CYCLE)	// HC=534
						Timings.WRAMRefreshPos = SNES_WRAM_REFRESH_HC_v2;					// HC=538
					else
						Timings.WRAMRefreshPos = SNES_WRAM_REFRESH_HC_v2 - ONE_DOT_CYCLE;	// HC=534
				}
			}
			else
				Timings.WRAMRefreshPos = SNES_WRAM_REFRESH_HC_v1;

			if (CPU.V_Counter == PPU.ScreenHeight + FIRST_VISIBLE_LINE)	// VBlank starts from V=225(240).
			{
				S9xEndScreenRefresh();
				#ifdef DEBUGGER
					if (!(CPU.Flags & FRAME_ADVANCE_FLAG))
				#endif
				{
					S9xSyncSpeed();
				}

				CPU.Flags |= SCAN_KEYS_FLAG;

				PPU.HDMA = 0;
				// Bits 7 and 6 of $4212 are computed when read in S9xGetPPU.
			#ifdef DEBUGGER
				missing.dma_this_frame = 0;
			#endif
				IPPU.MaxBrightness = PPU.Brightness;
				PPU.ForcedBlanking = (Memory.FillRAM[0x2100] >> 7) & 1;

				if (!PPU.ForcedBlanking)
				{
					PPU.OAMAddr = PPU.SavedOAMAddr;

					uint8	tmp = 0;

					if (PPU.OAMPriorityRotation)
						tmp = (PPU.OAMAddr & 0xFE) >> 1;
					if ((PPU.OAMFlip & 1) || PPU.FirstSprite != tmp)
					{
						PPU.FirstSprite = tmp;
						IPPU.OBJChanged = TRUE;
					}

					PPU.OAMFlip = 0;
				}

				// FIXME: writing to $4210 will wait 6 cycles.
				Memory.FillRAM[0x4210] = 0x80 | Model->_5A22;
				if (Memory.FillRAM[0x4200] & 0x80)
				{
#ifdef DEBUGGER
					if (Settings.TraceHCEvent)
					    S9xTraceFormattedMessage ("NMI Scheduled for next scanline.");
#endif
					// FIXME: triggered at HC=6, checked just before the final CPU cycle,
					// then, when to call S9xOpcode_NMI()?
					CPU.NMIPending = TRUE;
					Timings.NMITriggerPos = 6 + 6;
				}

			}

			if (CPU.V_Counter == PPU.ScreenHeight + 3)	// FIXME: not true
			{
				if (Memory.FillRAM[0x4200] & 1)
					S9xDoAutoJoypad();
			}

			if (CPU.V_Counter == FIRST_VISIBLE_LINE)	// V=1
				S9xStartScreenRefresh();

			S9xReschedule();

			break;

		case HC_HDMA_INIT_EVENT:
			S9xReschedule();

			if (CPU.V_Counter == 0)
			{
			#ifdef DEBUGGER
				S9xTraceFormattedMessage("*** HDMA Init     HC:%04d, Channel:%02x", CPU.Cycles, PPU.HDMA);
			#endif
				S9xStartHDMA();
			}

			break;

		case HC_RENDER_EVENT:
			if (CPU.V_Counter >= FIRST_VISIBLE_LINE && CPU.V_Counter <= PPU.ScreenHeight)
				RenderLine((uint8) (CPU.V_Counter - FIRST_VISIBLE_LINE));

			S9xReschedule();

			break;

		case HC_WRAM_REFRESH_EVENT:
		#ifdef DEBUGGER
			S9xTraceFormattedMessage("*** WRAM Refresh  HC:%04d", CPU.Cycles);
		#endif

			CPU.Cycles += SNES_WRAM_REFRESH_CYCLES;

			S9xReschedule();

			break;
	}

#ifdef DEBUGGER
	if (Settings.TraceHCEvent)
		S9xTraceFormattedMessage("--- HC event rescheduled (%s)  expected HC:%04d  current  HC:%04d",
			eventname[CPU.WhichEvent], CPU.NextEvent, CPU.Cycles);
#endif
}
