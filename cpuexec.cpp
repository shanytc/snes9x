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
#include "ppu.h"
#include "gfx.h"

int32	S9xRefreshClocks = 0;
#include "sgb/sgb.h"
#include "sfcbox.h"
#include "superdisc.h"
#include "rp2040cart.h"
#include "upd7725.h"
#include "hg51b.h"
#include "nss.h"
#include "voicekun.h"
#include "xband.h"
#ifdef DEBUGGER
#include "debug.h"
#include "missing.h"
#include "getset.h"
#endif

// XBAND debug: capture the very first BRK/COP executed inside the
// XBAND firmware bank, so the deadlock handler can show us the panic
// trigger site instead of just the eventual STP.
uint8  XBandFirstBrkOp   = 0;
uint32 XBandFirstBrkPC   = 0;
uint16 XBandFirstBrkS    = 0;
bool   XBandFirstBrkSeen = false;

// XBAND debug: live PC sampler. Each opcode fetch increments a hit
// counter so we can see which addresses the firmware is actually
// executing right now (especially useful when the screen is blank but
// the CPU is running).
//   XBandPCBank[256] - one bucket per program bank ($00..$FF)
//   XBandPCD0Sub[16] - 4KB sub-buckets inside bank $D0 (the firmware
//                      ROM bank where most code lives)
//   XBandPCBucketTotal - total opcode fetches sampled
uint64 XBandPCBank[256] = {0};
uint64 XBandPCD0Sub[16] = {0};
uint64 XBandPCD5Sub[16] = {0};
uint64 XBandPCBucketTotal = 0;

// XBAND debug: per-bank counter for reads of the SNES cart-header
// region $XX:$FFB0-$FFDF. Lets us tell whether the BIOS ever actually
// reads the bytes we patched at $00:$FFB0 in Map_XBandMultiCartHiROMMap.
// Bumped from S9xGetByte when Settings.XBAND is set.
uint64 XBandHdrReadBank[256] = {0};
uint64 XBandHdrReadTotal = 0;

static inline void S9xReschedule (void);

void S9xMainLoop (void)
{
	// Super Game Boy mode — run the GB core for one frame and return.
	// The 65816 loop below is bypassed entirely; snes9x's frontends
	// call S9xMainLoop once per frame, so this satisfies the contract.
	if (Settings.SuperGameBoy)
	{
		if (CPU.Flags & SCAN_KEYS_FLAG)
		{
			CPU.Flags &= ~SCAN_KEYS_FLAG;
			S9xMovieUpdate();
		}

		IPPU.RenderThisFrame      = !Settings.InRunAhead;
		PPU.ScreenHeight          = SGB_GB_SCREEN_H;
		IPPU.RenderedScreenWidth  = SGB_GB_SCREEN_W;
		IPPU.RenderedScreenHeight = SGB_GB_SCREEN_H;

		// Pipe the SNES controller 0 bitmask into the GB joypad. P6d's
		// MLT_REQ handling reads from mlt_current_player; for now we
		// only wire the first controller.
		S9xSGBSetJoypad(MovieGetJoypad(0));

		// Push timing knobs each frame so UI changes take effect live.
		// Default GBClockMultiplier to 1.0 if it's been left at its
		// zero-initialized value (backwards-compat with older configs).
		const float mul = (Settings.GBClockMultiplier > 0.0f)
		                  ? Settings.GBClockMultiplier : 1.0f;
		S9xSGBSetClockMultiplier(mul);
		S9xSGBSetRunMode(Settings.GameBoyRunMode);
		S9xSGBSetNoSpriteLimit(Settings.GBNoSpriteLimit);
		// Match the GB APU's downsample target to the host playback rate
		// so the samples we drain need no further rate conversion. The
		// setter is idempotent, so calling every frame is cheap and
		// picks up sound-config changes (output device / rate switch).
		S9xSGBSetAudioRate(Settings.SoundPlaybackRate);

		// Hidden runahead frames advance GB state but skip blit/present:
		// double-presenting per displayed frame can clobber video on
		// some host backends.
		if (!Settings.InRunAhead)
			S9xStartScreenRefresh();
		S9xSGBRunFrame();
		if (!Settings.InRunAhead)
		{
			IPPU.RenderedScreenWidth  = SGB_GB_SCREEN_W;
			IPPU.RenderedScreenHeight = SGB_GB_SCREEN_H;
			S9xSGBBlitScreenGB(GFX.Screen, GFX.RealPPL);
			S9xEndScreenRefresh();
		}

		// Drive the host audio callback to drain GB samples into the
		// sound device. In standard SNES mode the SPC scanline path
		// fires this from S9xAPUEndScanline; in BIOS-less SGB mode
		// that path is bypassed so we trigger it ourselves once per
		// frame. The defensive cap in S9xSGBGetSampleCount keeps
		// ProcessSound's wait condition satisfiable so we don't pin
		// at 1 fps the way the prior naive hookup did.
		if (!Settings.InRunAhead)
			S9xLandSamples();

		if (!Settings.InRunAhead)
			S9xSyncSpeed();

		CPU.Flags |= SCAN_KEYS_FLAG;
		return;
	}

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

	// Reset the SGB sync anchor to the current SNES cycle at loop entry
	// and publish the scanline period for wrap-compensation. getset.h
	// calls S9xSGBSyncToSnesCycle on every ICD2 access; without a fresh
	// anchor the first delta would be huge (or bogus-negative if a
	// scanline wrap just happened) and misattribute past SNES cycles
	// to the GB core.
	if (Settings.SGB_BIOSModeActive)
	{
		S9xSGBSetHMax(Timings.H_Max);
		S9xSGBResetSyncAnchor(CPU.Cycles);
		// Same per-frame push the BIOS-less branch above does, so the Emulator
		// Hacks checkbox reaches the GB core under a running SGB BIOS too.
		S9xSGBSetNoSpriteLimit(Settings.GBNoSpriteLimit);
	}

	for (;;)
	{
		if (Settings.SFCBox)
		{
			// The KROM released the reset line: reboot the SNES side at an
			// instruction boundary (the mapping registers already point at
			// whatever the KROM selected).
			if (S9xSFCBoxPendingReset())
				S9xSFCBoxApplySNESReset();

			// Held in reset (or the box is powered off): skip opcode execution
			// but keep the H/V event machinery running so frame pacing advances.
			if (S9xSFCBoxSNESHeld() || S9xSFCBoxPoweredOff())
			{
				CPU.Cycles = CPU.NextEvent;
				while (CPU.Cycles >= CPU.NextEvent)
					S9xDoHEventProcessing();
				if (CPU.Flags & SCAN_KEYS_FLAG)
					break;
				continue;
			}
		}

		if (Settings.NSS)
		{
			// The supervisor pulled the reset line: the APU is on it too,
			// so whatever the last game was playing stops here.
			if (S9xNSSPendingAPUReset())
				S9xNSSApplyAPUReset();

			// The supervisor released the reset line: reboot the game side
			// at an instruction boundary.
			if (S9xNSSPendingReset())
				S9xNSSApplySNESReset();

			// Held in reset or halted: skip opcode execution but keep the
			// H/V events running so the Z80, APU and frame pacing advance.
			if (S9xNSSSNESHeld())
			{
				CPU.Cycles = CPU.NextEvent;
				while (CPU.Cycles >= CPU.NextEvent)
					S9xDoHEventProcessing();
				if (CPU.Flags & SCAN_KEYS_FLAG)
					break;
				continue;
			}
		}

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

			// /IRQ is sampled before an instruction's last bus cycle (bsnes):
			// a rise inside that cycle waits for the next instruction.
			if (S9xLastBusStart() < Timings.NextIRQTimer && !CPU.WaitingForInterrupt)
				CPU.IRQDeferOne = TRUE;
			S9xUpdateIRQPositions(false);
			CPU.IRQLine = TRUE;
		}

		if (CPU.IRQDeferOne)
			CPU.IRQDeferOne = FALSE;
		else
		if (CPU.IRQLine || CPU.IRQExternal)
		{
			if (CPU.WaitingForInterrupt)
			{
				CPU.WaitingForInterrupt = FALSE;
				// Upstream adds ONE_DOT_CYCLE here; SNES_IRQ_TRIGGER_CYCLES already
				// carries that poll delay, so adding it again wakes WAI a dot late.
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
		if (CPU.WaitingForInterrupt)
		{
			S9xCPUBusCycleStart(ONE_CYCLE);
			CPU.Cycles += ONE_CYCLE;
			while (CPU.Cycles >= CPU.NextEvent)
				S9xDoHEventProcessing();
		}
		else
		{
			// Olympic Summer Games (SGB Enhanced) workaround. The SGB BIOS's
			// JUMP packet handler at $00:C72B does SEI before JMP [$00B8] to
			// transfer control to user-uploaded code (Olympic's $7E:081B
			// handler). Per pandocs the JUMP target runs with IRQs disabled
			// intentionally. On real hardware Olympic re-enables IRQ later
			// via its cmd-09 (SOU_TRN) hook at $7E:0900 which calls sub_80C58D
			// (CLI when $02CA != 0). In our emulation the BIOS reaches the
			// V-counter-gated wait at $00:BA6A (LDA $22; BEQ $-04) before
			// the SOU_TRN packets get drained, so I=1 blocks the IRQ that
			// would write $22 and the wait hangs forever. Clearing I here
			// restores the invariant the wait requires; non-Olympic SGB
			// titles never reach $BA6A with I=1 so they are unaffected.
			if (Settings.SGB_BIOSModeActive &&
			    Registers.PB == 0x00 && Registers.PCw == 0xBA6A &&
			    CheckIRQ())
			{
				ClearIRQ();
			}

			// Voicer-kun: the game names a CD track, starts it, or ends the voice.
			if (Settings.VoiceKun)
			{
				if (VoiceKunHook.PlayPC && Registers.PBPC == VoiceKunHook.PlayPC)
				{
					uint32	s = Registers.S.W;
					int	arg = 0;
					if (!VoiceKunHook.ArmPC)
						arg = VoiceKunHook.ArgStackOff
							? (Memory.RAM[(s + VoiceKunHook.ArgStackOff) & 0x1ffff] |
							   (Memory.RAM[(s + VoiceKunHook.ArgStackOff + 1) & 0x1ffff] << 8))
							  + VoiceKunHook.TrackBias
							: (Registers.A.W & 0xff) + VoiceKunHook.TrackBias;
					S9xVoiceKunPlayTrack(arg);
				}
				else
				if (VoiceKunHook.ArmPC && Registers.PBPC == VoiceKunHook.ArmPC)
				{
					uint32	s = Registers.S.W;
					int	arg = Memory.RAM[(s + VoiceKunHook.ArmArgOff) & 0x1ffff] |
					          (Memory.RAM[(s + VoiceKunHook.ArmArgOff + 1) & 0x1ffff] << 8);
					S9xVoiceKunArmTrack(arg + VoiceKunHook.TrackBias);
				}
				else
				if (VoiceKunHook.StopPC && Registers.PBPC == VoiceKunHook.StopPC)
					S9xVoiceKunStop();
				else
				if (VoiceKunHook.IRCmdPC && Registers.PBPC == VoiceKunHook.IRCmdPC)
				{
					uint32	s = Registers.S.W;
					int	cmd = Memory.RAM[(s + VoiceKunHook.IRCmdArgOff) & 0x1ffff] |
					          (Memory.RAM[(s + VoiceKunHook.IRCmdArgOff + 1) & 0x1ffff] << 8);
					S9xVoiceKunDeckCommand(cmd);
				}

				// Voice counter: ticks once per voiced scene whether or not the
				// game prompts for the disc, so it catches scene changes the
				// prompt hooks miss (continuous play).
				int	vaddr = S9xVoiceKunVoiceIdAddr();
				if (vaddr)
					S9xVoiceKunPollVoiceId(Memory.RAM[vaddr & 0x1ffff]);
			}

			uint8				Op;
			struct	SOpcodes	*Opcodes;

			if (CPU.PCBase)
			{
				Op = CPU.PCBase[Registers.PCw];
				S9xCPUBusCycleStart(CPU.MemSpeed);
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
				extern uint64 XBandPCBank[256];
				extern uint64 XBandPCD0Sub[16];
				extern uint64 XBandPCD5Sub[16];
				extern uint64 XBandPCBucketTotal;
				if (Settings.XBAND)
				{
					unsigned cpb = (unsigned)((Registers.PBPC >> 16) & 0xFF);
					if (!XBandFirstBrkSeen)
					{
						if ((Op == 0x00 || Op == 0x02) && cpb >= 0xC0)
						{
							XBandFirstBrkOp   = Op;
							XBandFirstBrkPC   = Registers.PBPC & 0xffffff;
							XBandFirstBrkS    = Registers.S.W;
							XBandFirstBrkSeen = true;
						}
					}
					// Per-bank histogram (256 banks, 64KB each).
					XBandPCBank[cpb]++;
					// Detail histogram for bank $D0 — 16 buckets of 4KB
					// each, so we can see which $1000-block of the firmware
					// is hot. Same for bank $D5 (added once we discovered
					// the BIOS is also spending most of its time there).
					if (cpb == 0xD0)
						XBandPCD0Sub[(Registers.PCw >> 12) & 0xF]++;
					else if (cpb == 0xD5)
						XBandPCD5Sub[(Registers.PCw >> 12) & 0xF]++;
					XBandPCBucketTotal++;

					// kDispatcherVector trap. Every XBAND OS function call
					// goes through `JSL $E0:$0040`. When PBPC reaches that
					// address (start of dispatcher), read the JSL return
					// address from the stack to identify the caller, then
					// log it along with the function ID (X) and A. We
					// trigger only on the first instruction of the
					// dispatcher (PBPC == $00E00040 EXACTLY) so we don't
					// re-log on every instruction inside the dispatcher.
					if ((Registers.PBPC & 0x00FFFFFF) == 0x00E00040)
					{
						// JSL pushes 24-bit return address (PB, PCH, PCL).
						// On entry to the callee, S+1 = PCL, S+2 = PCH,
						// S+3 = PB. The address pushed is the address of
						// the LAST byte of the JSL operand (because RTL
						// reads back PB:PC then increments PC by 1).
						uint16 s = Registers.S.W;
						uint8 ra_lo  = S9xGetByte((uint32)((s + 1) & 0xFFFF));
						uint8 ra_mid = S9xGetByte((uint32)((s + 2) & 0xFFFF));
						uint8 ra_hi  = S9xGetByte((uint32)((s + 3) & 0xFFFF));
						uint32 caller =
							((uint32)ra_hi << 16) |
							((uint32)ra_mid << 8) |
							((uint32)ra_lo);
						S9xXBandLogDispatcherCall(caller,
							Registers.X.W, Registers.A.W);
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
		}

		if (Settings.SA1)
			S9xSA1MainLoop();

		// Per-SNES-opcode GB sync — instruction-level interleaving.
		// Mesen-equivalent granularity: every SNES opcode advances the
		// GB by the corresponding cycle delta, so $6000/$6002/$7800
		// reads from the BIOS see GB state that varies naturally as
		// SNES cycles tick. Without this fine-grained sync the BIOS
		// band counter ($0294) phase-locks to deterministic GB
		// scanlines, $0294 never reaches 18, the swap never fires,
		// the WRAM ping-pong buffers never fill, and BG3 char renders
		// empty. Costs ~50% wall fps in BIOS mode (was the reason
		// it was removed in 0762b725), but it's the only way to
		// match real-hardware-equivalent BIOS state-machine progress.
		// Only gates on BIOS-released so pre-release boot/handshake
		// stays cheap.
		if (Settings.SGB_BIOSModeActive && S9xSGBBIOSGBIsReleased())
			S9xSGBSyncToSnesCycle(CPU.Cycles);
	}

	// P2 — in BIOS mode the GB core is held in reset until the BIOS
	// writes the release bit to the ICD2 reset register ($6003 bit 7).
	// Matches real SGB hardware: the SNES boots first, brings up border
	// + palette, then unblocks the GB CPU. Until then we skip stepping
	// entirely so the SNES loop keeps its 60 fps budget.
	if (Settings.SGB_BIOSModeActive)
	{
		const bool released = S9xSGBBIOSGBIsReleased();

		if (released)
		{
			const float mul = (Settings.GBClockMultiplier > 0.0f)
			                  ? Settings.GBClockMultiplier : 1.0f;
			S9xSGBSetClockMultiplier(mul);
			S9xSGBSetRunMode(Settings.GameBoyRunMode);
			// The no-sprite-limit hack is offered for the BIOS-less GB/GBC
			// core only — the Hacks dialog hides its checkbox in BIOS mode,
			// so force it off here rather than leave a hidden box in effect.
			S9xSGBSetNoSpriteLimit(false);
			S9xSGBSetAudioRate(Settings.SoundPlaybackRate);
		}

	}

	// split any pending run now, while ONE_CYCLE is the one it was added under
	S9xSettleLastBus(0);

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
			// 5A22 v2: HDMA init is due at HC 12 + (clock & 7) at line start
			if (CPU.V_Counter == 0 && Timings.WRAMRefreshPos != SNES_WRAM_REFRESH_HC_v1)
				CPU.NextEvent = 12 + (SNES_WRAM_REFRESH_HC_v2 - Timings.WRAMRefreshPos);
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

	S9xUpdateFastBusEnd();
}

void S9xCPUBusCycleSlow (int32 busLen)
{
	S9xCPUBusCycleStart(busLen);
	CPU.Cycles += busLen;
	while (CPU.Cycles >= CPU.NextEvent)
		S9xDoHEventProcessing();
}

void S9xCPUAddBusCyclesSlow (int32 n)
{
	while (n > 0)
	{
		int32	c = S9xBusCycleLen(n, CPU.MemSpeed);
		S9xCPUBusCycleSlow(c);
		n -= c;
	}
}

// Start of the most recent bus cycle: a fast-path run newer than the last
// single bus cycle is split the way the slow path would have split it.
int32 S9xLastBusStart (void)
{
	if (CPU.LastRunStart <= CPU.LastBusStart)
		return (CPU.LastBusStart);

	int32	start = CPU.LastRunStart;
	int32	n = CPU.LastRunShape & 0xff, memSpeed = CPU.LastRunShape >> 8;
	while (n > 0)
	{
		int32	c = S9xBusCycleLen(n, memSpeed);
		if ((n -= c) > 0)
			start += c;
	}

	return (start);
}

// Fold any run into LastBusStart, moved by shift clocks.
void S9xSettleLastBus (int32 shift)
{
	CPU.LastBusStart = S9xLastBusStart() - shift;
	CPU.LastRunStart = CPU.LastBusStart - 1;
}

void S9xResetPollSkip (SPollSkip &p)
{
	memset(&p, 0xff, sizeof(p));
}

// A turn of such a wait changes nothing but the clock, so whole turns can pass at once. A turn is two
// reads apart, the same twice, within one event window; a gap that differs had something more in it.
int32 S9xSkipPollTurns (SPollSkip &p, uint64 line, bool loop, int64 room)
{
	if (ONE_CYCLE != 6 || SLOW_ONE_CYCLE != 8)
	{
		p.pbpc = ~0u;
		return (0);
	}

	const int32		c = CPU.Cycles;
	const uint32	pbpc = Registers.PBPC;
	const uint32	key = pbpc | (CheckEmulation() ? 0x80000000 : 0) | (CPU.FastROMSpeed == ONE_CYCLE ? 0x40000000 : 0);
	const int32		period = (pbpc == p.pbpc && line == p.line && CPU.NextEvent == p.next) ? c - p.cycles : 0;
	p.pbpc = pbpc;
	p.cycles = c;
	p.line = line;
	p.next = CPU.NextEvent;

	int	s = 0;
	while (s < 8 && p.site[s] != key)
		s++;
	if (period > 0 && period == p.period && s == 8)
	{
		memmove(p.site + 1, p.site, 7 * sizeof(p.site[0]));
		memmove(p.turn + 1, p.turn, 7 * sizeof(p.turn[0]));
		p.site[0] = key;
		p.turn[0] = period;
		s = 0;
	}
	p.period = period;
	if (s == 8 || (period > 0 && period != p.turn[s]) || p.turn[s] <= 0 || !loop)
		return (0);
	const int32	turn = p.turn[s];

	if (CPU.InDMAorHDMA || CPU.HDMAEdge || CPU.NMIPending || CPU.IRQDeferOne || Timings.IRQFlagChanging ||
		((CPU.IRQLine || CPU.IRQExternal) && !CheckFlag(IRQ)) || Settings.SA1)
		return (0);
#ifdef DEBUGGER
	if (CPU.Flags & (BREAK_FLAG | TRACE_FLAG | SINGLE_STEP_FLAG | DEBUG_MODE_FLAG))
		return (0);
#endif

	int64	lim = (int64) (CPU.NextEvent < Timings.NextIRQTimer ? CPU.NextEvent : Timings.NextIRQTimer) - 1 - c;
	if (room < lim)
		lim = room;
	if (lim < turn)
		return (0);

	const int32	skip = (int32) (lim / turn) * turn;
	CPU.Cycles += skip;
	CPU.LastBusStart += skip;
	CPU.LastRunStart += skip;
	p.cycles = CPU.Cycles;
	return (skip);
}

void S9xRunPendingHDMA (int32 busLen)
{
	if (PPU.HDMA && CPU.V_Counter <= PPU.ScreenHeight)
		PPU.HDMA = S9xDoHDMASynced(PPU.HDMA, busLen);
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
			S9xApplyMidLineEvents();
			S9xReschedule();
			break;

		case HC_HDMA_START_EVENT:
			S9xReschedule();

			if (PPU.HDMA && CPU.V_Counter <= PPU.ScreenHeight)
			{
			#ifdef DEBUGGER
				S9xTraceFormattedMessage("*** HDMA Transfer HC:%04d, Channel:%02x", CPU.Cycles, PPU.HDMA);
			#endif
				// HDMA takes the bus two CPU bus cycles later (bsnes); a DMA in
				// flight hands it over directly.
				if (CPU.InDMA || Model->_5A22 != 2)
					PPU.HDMA = S9xDoHDMA(PPU.HDMA);
				else
				{
					CPU.HDMAEdge = 2;
					S9xUpdateFastBusEnd();
				}
			}

			break;

		case HC_HCOUNTER_MAX_EVENT:
		{
			// no bus cycles reached the pending HDMA (CPU held): run it now
			if (CPU.HDMAEdge)
			{
				CPU.HDMAEdge = 0;
				S9xUpdateFastBusEnd();
				S9xRunPendingHDMA(ONE_CYCLE);
			}

			const int32	finishedLine = Timings.H_Max;

			if (Settings.SuperFX)
			{
				if (!SuperFX.oneLineDone)
					S9xSuperFXExec();
				SuperFX.oneLineDone = FALSE;
			}

			// Per-scanline GB sync in BIOS-released mode. Replaces the
			// old per-opcode hook for performance (millions of calls/sec
			// → ~262×60 = 15k/sec). ICD2-register accesses in getset.h
			// also call SyncToSnesCycle inline so $7800/$6002 reads
			// still see freshest GB state mid-opcode; this scanline-end
			// sync just guarantees forward progress on scanlines that
			// have no ICD2 traffic.
			if (Settings.SGB_BIOSModeActive && S9xSGBBIOSGBIsReleased())
				S9xSGBSyncToSnesCycle(CPU.Cycles);

			// SFC-Box: the supervisor HD64180 runs its slice of every
			// scanline whether or not the SNES itself is executing.
			if (Settings.SFCBox)
				S9xSFCBoxEndScanline();

			// NSS: likewise for its Z80, which owns the timer and the
			// reset line and so must keep running while the game is held.
			if (Settings.NSS)
				S9xNSSEndScanline();

			// Super Disc: mechacon replies and the drive's 75Hz sector clock.
			if (Settings.SuperDisc)
				S9xSuperDiscEndScanline();

			// RP2040 cart: the chip runs on between the SNES's accesses to it.
			if (Settings.RP2040Cart)
				S9xRP2040CartEndScanline();

			// DSP-n firmware and the Cx4: likewise, so a long gap isn't one burst of catch-up.
			if (S9xUPD7725Active())
				S9xUPD7725EndScanline();
			if (S9xHG51BActive())
				S9xHG51BEndScanline();

			S9xAPUEndScanline();
			CPU.Cycles -= Timings.H_Max;
			// Keep the SGB sync anchor continuous across the wrap so
			// multi-scanline deltas (DMA bursts) aren't discarded.
			if (Settings.SGB_BIOSModeActive)
				S9xSGBNotifyScanlineWrap(Timings.H_Max);
			if (Timings.NMITriggerPos != 0xffff)
				Timings.NMITriggerPos -= Timings.H_Max;
			if (Timings.NextIRQTimer != 0x0fffffff)
				Timings.NextIRQTimer -= Timings.H_Max;
			S9xSettleLastBus(Timings.H_Max);
			S9xAPUSetReferenceTime(CPU.Cycles);

			PPU.CentreXLatched = false;
			PPU.CentreYLatched = false;
			PPU.M7HOFSLatched = false;
			PPU.M7VOFSLatched = false;

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
				Timings.FrameInterlace = Memory.FillRAM[0x2133] & 1;
				if (Timings.FrameInterlace && S9xInterlaceField())
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
			if (CPU.V_Counter == 240 && !Timings.FrameInterlace && S9xInterlaceField())	// V=240
				Timings.H_Max = Timings.H_Max_Master - ONE_DOT_CYCLE;	// HC=1360
			else
				Timings.H_Max = Timings.H_Max_Master;					// HC=1364

			if (Model->_5A22 == 2)
			{
				// refresh = 530 + 8 - (clock & 7) at line start, so it flips after
				// a 1364-clock line and holds after the 1360-clock short one
				if (finishedLine & 4)
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
					if (!Settings.InRunAhead)
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
		}

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
			S9xRefreshClocks += SNES_WRAM_REFRESH_CYCLES;

			S9xReschedule();

			break;

		default:
			S9xMessage(S9X_FATAL_ERROR, 0, "Invalid H-event state.\n");
			abort();
			break;
	}

#ifdef DEBUGGER
	if (Settings.TraceHCEvent)
		S9xTraceFormattedMessage("--- HC event rescheduled (%s)  expected HC:%04d  current  HC:%04d",
			eventname[CPU.WhichEvent], CPU.NextEvent, CPU.Cycles);
#endif
}
