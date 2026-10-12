/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include <string.h>
#include "snes9x.h"
#include "upd96050.h"
#include "upd96050n.h"
#include "memmap.h"

// The boards' oscillators, as bsnes has them (Mesen runs the ST011 at 22 MHz).
#ifndef UPD96050_ST010_HZ
#define UPD96050_ST010_HZ	11000000
#endif
#ifndef UPD96050_ST011_HZ
#define UPD96050_ST011_HZ	15000000
#endif

enum
{
	F_OV0 = 0x01, F_OV1 = 0x02, F_Z = 0x04, F_C = 0x08, F_S0 = 0x10, F_S1 = 0x20
};

enum
{
	SR_RQM = 0x8000, SR_DRS = 0x1000, SR_DRC = 0x0400,
	SR_FIXED = 0x907c	// RQM, DRS and the unused bits: the DSP's own SR writes skip them
};

// Saved as-is, so the padding is explicit and the layout the same everywhere.
struct Regs
{
	uint16	stack[8];
	uint16	pc, rp, dp, sp;
	uint16	k, l, m, n;
	uint16	a, b, tr, trb;
	uint16	dr, sr, si, so;
	uint8	flaga, flagb;
	uint8	pad[6];
	uint64	line_base;		// master clock at the start of the current scanline
	uint64	synced;			// master clock the chip has been run up to
	uint64	executed;		// instructions run since reset
	uint64	target;			// instructions due by `synced`
	uint64	rem;			// what's left over of `target`, in master clocks * hz
};
static_assert(sizeof(Regs) == 96, "savestate layout");

// One program word, its fields pulled out once at load.
enum { D_OP, D_RT, D_JP, D_LD, D_PARK };
struct Dec
{
	uint8	kind, src, dst, alu;
	uint8	psel, accb, dpl, dphm;
	uint8	rpdec, pad;
	uint16	brch;
	uint16	imm;			// LD's immediate, or the jump's bank and address (bits 0-12)
};

static Dec		dec[16384];
static uint16	drom[2048];
static Regs		r;
static uint8	*ram = NULL;	// Memory.SRAM: 2048 words, low byte first
static uint32	hz = 15000000;
static bool8	loaded = FALSE;
static bool8	active = FALSE;
static int		chip = 0;
static const S9xUPD96050Native	*native = NULL;	// the native chip, or NULL for the firmware
static uint64	native_next = 0;	// master clock of the native chip's next counted access (~0: waiting on DR)

static inline uint32 ProgWord (const uint8 *image, uint32 i)
{
	return (image[i * 3] | (image[i * 3 + 1] << 8) | (image[i * 3 + 2] << 16));
}

#ifdef UPD96050_LAB
// The test lab's view: each RAM access the chip makes, at its instruction count.
void	(*S9xUPD96050LabRam) (char kind, uint64 at, uint32 word, uint16 v) = NULL;
#define LAB_RAM(kind, i, v)	do { if (S9xUPD96050LabRam) S9xUPD96050LabRam(kind, r.executed, (i) & 0x7ff, v); } while (0)
// how often each program word ran (an array of 16384)
uint64	*S9xUPD96050LabCover = NULL;
uint64	*S9xUPD96050LabTaken = NULL;	// conditional jumps taken, by their address
void	(*S9xUPD96050LabAt) (uint32 pc) = NULL;	// before each instruction
// and each CPU access: r/s/w DR read, SR read, DR write; m/n RAM read, write
void	(*S9xUPD96050LabBus) (char kind, uint64 at, uint32 address, uint8 v) = NULL;
#define LAB_BUS(kind, a, v)	do { if (S9xUPD96050LabBus) S9xUPD96050LabBus(kind, r.executed, a, v); } while (0)
#else
#define LAB_RAM(kind, i, v)	do { } while (0)
#define LAB_BUS(kind, a, v)	do { } while (0)
#endif

static inline uint16 RamRead (uint32 i)
{
	const uint8		*p = ram + ((i & 0x7ff) << 1);
	const uint16	v = (uint16) (p[0] | (p[1] << 8));
	LAB_RAM('R', i, v);
	return (v);
}

static inline void RamWrite (uint32 i, uint16 v)
{
	uint8	*p = ram + ((i & 0x7ff) << 1);
	p[0] = (uint8) v;
	p[1] = (uint8) (v >> 8);
	LAB_RAM('W', i, v);
}

bool8 S9xUPD96050IsFirmware (const uint8 *image, uint32 size)
{
	if (!image || size != UPD96050_FIRMWARE_SIZE)
		return (FALSE);

	// Nearly every jump word should be a branch the chip knows; a byte-swapped dump won't be.
	uint32	jumps = 0, known = 0;
	for (uint32 i = 0; i < 16384; i++)
	{
		const uint32	op = ProgWord(image, i);
		if ((op >> 22) != 2)
			continue;
		const uint32	brch = (op >> 13) & 0x1ff;
		jumps++;
		if (brch == 0x000 || brch == 0x100 || brch == 0x101 || brch == 0x140 || brch == 0x141 ||
			(brch >= 0x080 && brch <= 0x0be && (brch < 0x0b0 ? !(brch & 1) : true)))
			known++;
	}

	return (jumps > 0 && known * 10 >= jumps * 9);
}

static void Decode (const uint8 *image)
{
	for (uint32 i = 0; i < 16384; i++)
	{
		const uint32	op = ProgWord(image, i);
		Dec				d;
		memset(&d, 0, sizeof(d));

		switch (op >> 22)
		{
			case 0:
			case 1:
				d.kind = (op >> 22) ? D_RT : D_OP;
				d.src = (op >> 4) & 15;
				d.dst = op & 15;
				d.alu = (op >> 16) & 15;
				d.psel = (op >> 20) & 3;
				d.accb = (op >> 15) & 1;
				d.dpl = (op >> 13) & 3;
				d.dphm = (op >> 9) & 15;
				d.rpdec = (op >> 8) & 1;
				break;

			case 2:
			{
				d.kind = D_JP;
				d.brch = (op >> 13) & 0x1ff;
				d.imm = (uint16) (((op & 3) << 11) | ((op >> 2) & 0x7ff));
				// A one-instruction loop on RQM can't end until the CPU touches DR.
				const uint32	target = ((i + 1) & 0x2000) | d.imm;
				if ((d.brch == 0x0bc || d.brch == 0x0be) && target == i)
					d.kind = D_PARK;
				break;
			}

			default:
				d.kind = D_LD;
				d.dst = op & 15;
				d.imm = (uint16) ((op >> 6) & 0xffff);
				break;
		}
		dec[i] = d;
	}
}

bool8 S9xUPD96050Load (const uint8 *image, uint32 size, int which)
{
	loaded = active = FALSE;
	native = NULL;
	if (!S9xUPD96050IsFirmware(image, size) || !Memory.SRAM)
		return (FALSE);

	chip = which;
	hz = (which == UPD96050_ST010) ? UPD96050_ST010_HZ : UPD96050_ST011_HZ;
	ram = Memory.SRAM;
	Decode(image);
	for (uint32 i = 0; i < 2048; i++)
		drom[i] = (uint16) (image[0xc000 + i * 2] | (image[0xc000 + i * 2 + 1] << 8));

	loaded = TRUE;
	S9xUPD96050Reset();
	return (TRUE);
}

bool8 S9xUPD96050LoadNative (int which)
{
	loaded = active = FALSE;
	native = (which == UPD96050_ST010) ? &S9xST010Native : (which == UPD96050_ST011) ? &S9xST011Native : NULL;
	if (!native || !Memory.SRAM)
		return (FALSE);

	chip = which;
	hz = (which == UPD96050_ST010) ? UPD96050_ST010_HZ : UPD96050_ST011_HZ;
	ram = Memory.SRAM;
	native->attach(&r.dr, &r.sr, ram);
	loaded = TRUE;
	S9xUPD96050Reset();
	return (TRUE);
}

void S9xUPD96050Unload (void)
{
	loaded = active = FALSE;
	native = NULL;
	native_next = 0;
}

bool8 S9xUPD96050IsNative (void)
{
	return (native != NULL);
}

bool8 S9xUPD96050Loaded (void)
{
	return (loaded);
}

bool8 S9xUPD96050Active (void)
{
	return (active);
}

int S9xUPD96050Chip (void)
{
	return (loaded ? chip : 0);
}

void S9xUPD96050Reset (void)
{
	if (!loaded)
		return;
	memset(&r, 0, sizeof(r));
	native_next = 0;
	if (native)
		native->reset();
	active = TRUE;
}

void S9xUPD96050Suspend (void)
{
	active = FALSE;
}

// The uPD7725's ALU, which the uPD96050 shares: S0, Z and S1 without branches. S1
// follows S0 until an overflow is outstanding (OV1), then holds its sign.
static inline uint32 SignFlags (uint16 v, uint8 flag)
{
	uint32	s0 = v >> 15;
	uint32	ov1 = (flag >> 1) & 1;
	uint32	s1 = s0 ^ ((s0 ^ (flag >> 5)) & ov1 & 1);
	return ((s0 << 4) | ((uint32) (v == 0) << 2) | (s1 << 5));
}

// ADD/SUB and friends: carry or borrow, OV0, and OV1, where a second overflow
// cancels the first if it lands on the other side.
static inline uint8 ArithFlags (uint16 q, uint16 p, uint32 res, uint8 flag, bool sub)
{
	uint16	v = (uint16) res;
	uint32	f = SignFlags(v, flag);
	uint32	s0 = (f >> 4) & 1, s1 = (f >> 5) & 1;
	uint32	ov0 = (sub ? ((q ^ v) & (q ^ p)) : ((q ^ v) & (p ^ v))) >> 15;
	uint32	ov1_in = (flag >> 1) & 1;
	uint32	t = 1 ^ (ov1_in & (s1 ^ s0));
	uint32	ov1 = ov1_in ^ (ov0 & (t ^ ov1_in));
	return ((uint8) (f | ((uint32) (res > 0xffff) << 3) | ov0 | (ov1 << 1)));
}

// ADC/SBB and SHL1 take their carry in from the other accumulator's flags.
static inline void Alu (uint32 op, uint16 &acc, uint8 &flag, uint16 p, bool cin)
{
	uint16	q = acc;
	uint32	res;

	switch (op)
	{
		case  1: acc = q | p; flag = (uint8) SignFlags(acc, flag); return;
		case  2: acc = q & p; flag = (uint8) SignFlags(acc, flag); return;
		case  3: acc = q ^ p; flag = (uint8) SignFlags(acc, flag); return;
		case  4: res = q - p; flag = ArithFlags(q, p, res, flag, true); break;
		case  5: res = q + p; flag = ArithFlags(q, p, res, flag, false); break;
		case  6: res = q - p - cin; flag = ArithFlags(q, p, res, flag, true); break;
		case  7: res = q + p + cin; flag = ArithFlags(q, p, res, flag, false); break;
		case  8: res = q - 1; flag = ArithFlags(q, 1, res, flag, true); break;
		case  9: res = q + 1; flag = ArithFlags(q, 1, res, flag, false); break;
		case 10: acc = (uint16) ~q; flag = (uint8) SignFlags(acc, flag); return;
		case 11: acc = (q >> 1) | (q & 0x8000); flag = (uint8) (SignFlags(acc, flag) | ((q & 1) << 3)); return;
		case 12: acc = (uint16) ((q << 1) | cin); flag = (uint8) (SignFlags(acc, flag) | ((q >> 15) << 3)); return;
		case 13: acc = (uint16) ((q << 2) | 3); flag = (uint8) SignFlags(acc, flag); return;
		case 14: acc = (uint16) ((q << 4) | 15); flag = (uint8) SignFlags(acc, flag); return;
		default: acc = (uint16) ((q << 8) | (q >> 8)); flag = (uint8) SignFlags(acc, flag); return;
	}

	acc = (uint16) res;
}

static inline void Move (uint16 idb, uint32 dst)
{
	switch (dst)
	{
		case  0: break;
		case  1: r.a = idb; break;
		case  2: r.b = idb; break;
		case  3: r.tr = idb; break;
		case  4: r.dp = idb & 0x7ff; break;
		case  5: r.rp = idb & 0x7ff; break;
		case  6: r.dr = idb; r.sr |= SR_RQM; break;
		case  7: r.sr = (r.sr & SR_FIXED) | (idb & ~SR_FIXED); break;
		case  8:
		case  9: r.so = idb; break;
		case 10: r.k = idb; break;
		case 11: r.k = idb; r.l = drom[r.rp]; break;				// KLR
		case 12: r.l = idb; r.k = RamRead(r.dp | 0x40); break;		// KLM
		case 13: r.l = idb; break;
		case 14: r.trb = idb; break;
		default: RamWrite(r.dp, idb); break;
	}
}

static inline uint16 Source (uint32 src)
{
	switch (src)
	{
		case  0: return (r.trb);
		case  1: return (r.a);
		case  2: return (r.b);
		case  3: return (r.tr);
		case  4: return (r.dp);
		case  5: return (r.rp);
		case  6: return (drom[r.rp]);
		case  7: return (0x8000 - ((r.flaga & F_S1) ? 1 : 0));	// SGN: the saturation value
		case  8: r.sr |= SR_RQM; return (r.dr);
		case  9: return (r.dr);			// DRNF: read without requesting more
		case 10: return (r.sr);
		case 11:
		case 12: return (r.si);
		case 13: return (r.k);
		case 14: return (r.l);
		default: return (RamRead(r.dp));
	}
}

static inline void ExecOP (const Dec &d)
{
	const uint16	idb = Source(d.src);

	if (d.alu)
	{
		// DEC and up don't take P, so they don't read it
		uint16	p = 0;
		if (d.alu < 8)
			switch (d.psel)
			{
				case 0:  p = RamRead(r.dp); break;
				case 1:  p = idb; break;
				case 2:  p = r.m; break;
				default: p = r.n; break;
			}

		if (d.accb)
			Alu(d.alu, r.b, r.flagb, p, (r.flaga & F_C) != 0);
		else
			Alu(d.alu, r.a, r.flaga, p, (r.flagb & F_C) != 0);
	}

	Move(idb, d.dst);

	// A move into DP or RP takes precedence over that pointer's modifier. Like the
	// uPD7725, a DP modifier keeps only DP's low byte.
	if (d.dst != 4)
	{
		switch (d.dpl)
		{
			case 1: r.dp = (r.dp & 0xf0) | ((r.dp + 1) & 0x0f); break;
			case 2: r.dp = (r.dp & 0xf0) | ((r.dp - 1) & 0x0f); break;
			case 3: r.dp &= 0xf0; break;
		}
		r.dp ^= d.dphm << 4;
	}

	if (d.rpdec && d.dst != 5)
		r.rp = (r.rp - 1) & 0x7ff;
}

static inline bool FlagTest (uint32 brch)
{
	static const uint8	flag_bits[6] = { F_C, F_Z, F_OV0, F_OV1, F_S0, F_S1 };
	const uint32		sel = (brch - 0x080) >> 2;
	const uint8			f = (sel & 1) ? r.flagb : r.flaga;
	const bool			set = (f & flag_bits[sel >> 1]) != 0;
	return ((brch & 2) ? set : !set);
}

static inline void ExecJP (const Dec &d)
{
	const uint16	jp = (r.pc & 0x2000) | d.imm;
	const uint32	brch = d.brch;
	bool			take;

	if (brch >= 0x080 && brch <= 0x0af)
		take = !(brch & 1) && FlagTest(brch);
	else switch (brch)
	{
		case 0x000: r.pc = r.so & 0x3fff; return;			// JMPSO
		case 0x0b0: take = (r.dp & 0x0f) == 0x00; break;
		case 0x0b1: take = (r.dp & 0x0f) != 0x00; break;
		case 0x0b2: take = (r.dp & 0x0f) == 0x0f; break;
		case 0x0b3: take = (r.dp & 0x0f) != 0x0f; break;
		case 0x0b4:											// JNSIAK, JNSOAK: the serial
		case 0x0b8: take = true; break;						// port is never wired up
		case 0x0bc: take = !(r.sr & SR_RQM); break;
		case 0x0be: take = (r.sr & SR_RQM) != 0; break;
		case 0x100: r.pc = jp & 0x1fff; return;				// LJMP
		case 0x101: r.pc = jp | 0x2000; return;				// HJMP
		case 0x140:											// LCALL
		case 0x141:											// HCALL
			r.stack[r.sp] = r.pc;
			r.sp = (r.sp + 1) & 7;
			r.pc = (brch & 1) ? (jp | 0x2000) : (jp & 0x1fff);
			return;
		default: take = false; break;
	}

	if (take)
	{
#ifdef UPD96050_LAB
		if (S9xUPD96050LabTaken)
			S9xUPD96050LabTaken[(r.pc - 1) & 0x3fff]++;
#endif
		r.pc = jp;
	}
}

// Up to `target`, or until the chip parks on a wait the CPU has to end.
static void Run (void)
{
	if (native)
	{
		if (r.executed < r.target)
		{
			const uint64	owed = native->run(r.target - r.executed);
			const uint64	master = Settings.PAL ? 21281370 : 21477273;
			r.executed = r.target;
			// the first clock its owed instructions have all run by (one owed: the next sync anyway)
			native_next = (owed > 1) ? r.synced + (owed * master - r.rem + hz - 1) / hz : owed ? 0 : ~(uint64) 0;
		}
		return;
	}

	while (r.executed < r.target)
	{
		const Dec	&d = dec[r.pc];
		r.executed++;
#ifdef UPD96050_LAB
		if (S9xUPD96050LabCover)
			S9xUPD96050LabCover[r.pc]++;
		if (S9xUPD96050LabAt)
			S9xUPD96050LabAt(r.pc);
#endif

		if (d.kind == D_PARK && ((r.sr & SR_RQM) ? d.brch == 0x0be : d.brch == 0x0bc))
		{
			r.executed = r.target;
			break;
		}

		r.pc = (r.pc + 1) & 0x3fff;

		switch (d.kind)
		{
			case D_OP:
				ExecOP(d);
				break;
			case D_RT:
				ExecOP(d);
				r.sp = (r.sp - 1) & 7;
				r.pc = r.stack[r.sp] & 0x3fff;
				break;
			case D_LD:
				Move(d.imm, d.dst);
				break;
			default:	// D_JP, and a D_PARK that falls through
				ExecJP(d);
				break;
		}

		// The multiplier runs every cycle on whatever K and L hold.
		const int32	prod = (int16) r.k * (int16) r.l;
		r.m = (uint16) (prod >> 15);
		r.n = (uint16) (prod << 1);
	}
}

// The chip's clock up to master clock `t`, and the instructions due by then run.
static void SyncTo (int32 cycles)
{
	const uint64	t = r.line_base + (uint64) (int64) cycles;
	if (t <= r.synced)
		return;
	// A native chip can't change what the CPU sees before its next counted access (a waiting one still
	// syncs about once a second, so the clock sums below can't overflow).
	if (t < native_next && t - r.synced < ((uint64) 1 << 24))
		return;

	const uint64	master = Settings.PAL ? 21281370 : 21477273;
	const uint64	acc = (t - r.synced) * hz + r.rem;
	r.synced = t;
	r.target += acc / master;
	r.rem = acc % master;
	Run();
}

// The firmware's idle loops aren't all one-instruction waits it can park on, so the chip
// runs to each line's end: a long gap between accesses isn't one burst of catch-up.
void S9xUPD96050EndScanline (void)
{
	SyncTo(Timings.H_Max);
	r.line_base += Timings.H_Max;
}

static uint8 ReadBus (uint32 address)
{
	if (address & 0x080000)
		return (ram[address & 0xfff]);
	if (address & 1)
		return ((uint8) (r.sr >> 8));

	if (r.sr & SR_DRC)
	{
		r.sr &= ~SR_RQM;
		return ((uint8) r.dr);
	}
	if (r.sr & SR_DRS)
	{
		r.sr &= ~(SR_RQM | SR_DRS);
		return ((uint8) (r.dr >> 8));
	}
	r.sr |= SR_DRS;
	return ((uint8) r.dr);
}

static void WriteDR (uint8 byte)
{
	if (r.sr & SR_DRC)
	{
		r.sr &= ~SR_RQM;
		r.dr = (r.dr & 0xff00) | byte;
	}
	else if (r.sr & SR_DRS)
	{
		r.sr &= ~(SR_RQM | SR_DRS);
		r.dr = (uint16) ((byte << 8) | (r.dr & 0x00ff));
	}
	else
	{
		r.sr |= SR_DRS;
		r.dr = (r.dr & 0xff00) | byte;
	}
}

uint8 S9xUPD96050Read (uint32 address, int32 speed)
{
	if (speed < 0)
		return ((address & 0x080000) ? ram[address & 0xfff] : (address & 1) ? (uint8) (r.sr >> 8) : (uint8) r.dr);
	// a DR read is a handshake: a native chip waiting on it runs up to it, then acts on it from its next sync
	const bool	data = !(address & 0x080001);
	if (data)
		native_next = 0;
	SyncTo(CPU.Cycles + speed - 1);
	const uint8	v = ReadBus(address);
	if (data)
		native_next = 0;
	LAB_BUS((address & 0x080000) ? 'm' : (address & 1) ? 's' : 'r', address, v);
	return (v);
}

void S9xUPD96050Write (uint32 address, uint8 byte, int32 speed)
{
	if (address & 0x080000)
	{
		if (speed >= 0)
			SyncTo(CPU.Cycles + speed);
		ram[address & 0xfff] = byte;
		LAB_BUS('n', address, byte);
		return;
	}

	// The CPU can't write SR.
	if (speed < 0 || (address & 1))
		return;
	native_next = 0;
	SyncTo(CPU.Cycles + speed);
	LAB_BUS('w', address, byte);
	WriteDR(byte);
	native_next = 0;
}

// The data RAM is SRAM, which the snapshot already carries; a native chip saves its
// program's place after the registers.
uint32 S9xUPD96050StateSize (void)
{
	return (sizeof(r) + (native ? native->state_size() : 0));
}

void S9xUPD96050StateSave (uint8 *buf)
{
	memcpy(buf, &r, sizeof(r));
	if (native)
		native->state_save(buf + sizeof(r));
}

bool8 S9xUPD96050StateLoad (const uint8 *buf, uint32 size)
{
	if (!loaded || size < sizeof(r))
		return (FALSE);
	memcpy(&r, buf, sizeof(r));
	r.pc &= 0x3fff;
	r.sp &= 7;
	r.rp &= 0x7ff;
	r.dp &= 0x7ff;

	const uint32	extra = size - (uint32) sizeof(r);
	if (native ? !native->state_load(buf + sizeof(r), extra) : extra != 0)
	{
		// from the other kind of chip, firmware or native: it picks up at its command wait
		if (native)
			native->idle();
		else
		{
			r.pc = 0x0002;
			r.sp = 0;
		}
		S9xMessage(S9X_WARNING, S9X_FREEZE_FILE_INFO, "This state's Seta chip ran from its firmware, or without it; carrying on from its command wait.");
	}
	native_next = 0;
	active = TRUE;
	return (TRUE);
}

#ifdef UPD96050_LAB
// The CPU's accesses without the clock (the lab steps the chip itself).
uint8 S9xUPD96050LabRead (uint32 address)
{
	return (ReadBus(address));
}

void S9xUPD96050LabWrite (uint32 address, uint8 byte)
{
	if (address & 0x080000)
		ram[address & 0xfff] = byte;
	else if (!(address & 1))
		WriteDR(byte);
}

uint8 S9xUPD96050LabReadDR (void)
{
	return (ReadBus(0x600000));
}

// The lab drives the chip by instruction counts instead of the CPU's clock.
void S9xUPD96050LabStep (uint64 n)
{
	r.target = r.executed + n;
	Run();
}

uint64 S9xUPD96050LabExecuted (void)
{
	return (r.executed);
}

uint32 S9xUPD96050LabPC (void)
{
	return (r.pc);
}

// DR and SR as they stand, without a read's effects
void S9xUPD96050LabRegs (uint16 *dr, uint16 *sr)
{
	*dr = r.dr;
	*sr = r.sr;
}

// TR and TRB, which the ST011's commands leave to the next
void S9xUPD96050LabTR (uint16 *tr, uint16 *trb)
{
	*tr = r.tr;
	*trb = r.trb;
}
#endif
