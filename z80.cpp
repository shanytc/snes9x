/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Zilog Z80 core for the Nintendo Super System supervisor board.
// See z80.h for scope and docs/nss.md for the board context.
//
// Dispatch is a switch over the primary opcode with an index-mode parameter
// for the DD/FD prefixes. Unlike the Z180 core in hd64180.cpp this one
// executes the undocumented encodings: IXH/IXL/IYH/IYL, SLL, the DDCB forms
// that copy their result into a register, and the ED holes as NOPs. The
// internal MEMPTR (WZ) register is maintained so BIT n,(HL) reports the
// hardware's flag bits 3/5. Cycle counts are accumulated at bus-access
// granularity (M1 4 / memory 3 / io 4 plus per-class internal cycles).

#include <string.h>
#include "port.h"
#include "z80.h"

struct SZ80			Z80;
struct SZ80Callbacks	Z80CB;

#define z	Z80

// Flag bits
#define FL_C	0x01
#define FL_N	0x02
#define FL_P	0x04	// parity/overflow
#define FL_X	0x08
#define FL_H	0x10
#define FL_Y	0x20
#define FL_Z	0x40
#define FL_S	0x80

static uint8	ParityTable[256];
static bool8	TablesBuilt = FALSE;

static void BuildTables (void)
{
	for (int i = 0; i < 256; i++)
	{
		int	bits = 0;
		for (int b = 0; b < 8; b++)
			bits += (i >> b) & 1;
		ParityTable[i] = (bits & 1) ? 0 : FL_P;
	}

	TablesBuilt = TRUE;
}

// ---------------------------------------------------------------------------
// Bus access

static inline uint8 RdMem (uint16 addr)
{
	z.Cycles += 3;
	return (Z80CB.MemRead(addr));
}

static inline void WrMem (uint16 addr, uint8 byte)
{
	z.Cycles += 3;
	Z80CB.MemWrite(addr, byte);
}

static inline uint8 Fetch (void)
{
	z.Cycles += 3;
	return (Z80CB.MemRead(z.PC++));
}

static inline uint8 FetchM1 (void)
{
	z.Cycles += 4;
	z.R = (z.R & 0x80) | ((z.R + 1) & 0x7f);
	return (Z80CB.MemRead(z.PC++));
}

static inline uint16 Fetch16 (void)
{
	uint16	lo = Fetch();
	return (lo | ((uint16) Fetch() << 8));
}

static inline uint16 RdMem16 (uint16 addr)
{
	uint16	lo = RdMem(addr);
	return (lo | ((uint16) RdMem((uint16) (addr + 1)) << 8));
}

static inline void WrMem16 (uint16 addr, uint16 word)
{
	WrMem(addr, (uint8) word);
	WrMem((uint16) (addr + 1), (uint8) (word >> 8));
}

static inline void Push16 (uint16 word)
{
	z.SP -= 2;
	WrMem((uint16) (z.SP + 1), (uint8) (word >> 8));
	WrMem(z.SP, (uint8) word);
}

static inline uint16 Pop16 (void)
{
	uint16	word = RdMem16(z.SP);
	z.SP += 2;
	return (word);
}

static inline uint8 RdIO (uint16 port)
{
	z.Cycles += 4;
	return (Z80CB.IORead(port));
}

static inline void WrIO (uint16 port, uint8 byte)
{
	z.Cycles += 4;
	Z80CB.IOWrite(port, byte);
}

// Register-pair helpers
static inline uint16 GetBC (void)	{ return (((uint16) z.B << 8) | z.C); }
static inline uint16 GetDE (void)	{ return (((uint16) z.D << 8) | z.E); }
static inline uint16 GetHL (void)	{ return (((uint16) z.H << 8) | z.L); }
static inline uint16 GetAF (void)	{ return (((uint16) z.A << 8) | z.F); }
static inline void SetBC (uint16 v)	{ z.B = v >> 8; z.C = (uint8) v; }
static inline void SetDE (uint16 v)	{ z.D = v >> 8; z.E = (uint8) v; }
static inline void SetHL (uint16 v)	{ z.H = v >> 8; z.L = (uint8) v; }
static inline void SetAF (uint16 v)	{ z.A = v >> 8; z.F = (uint8) v; }

// ixmode: 0 = HL, 1 = IX, 2 = IY (for the DD/FD prefixed forms)
static inline uint16 GetIdx (int ixmode)
{
	return (ixmode == 1 ? z.IX : ixmode == 2 ? z.IY : GetHL());
}

static inline void SetIdx (int ixmode, uint16 v)
{
	if (ixmode == 1)		z.IX = v;
	else if (ixmode == 2)	z.IY = v;
	else					SetHL(v);
}

// Effective address of the (HL) / (IX+d) / (IY+d) memory operand. The index
// forms fetch the displacement and spend five internal cycles on the add,
// and the result lands in MEMPTR.
static inline uint16 EffAddr (int ixmode)
{
	if (!ixmode)
		return (GetHL());

	int8	d = (int8) Fetch();
	z.Cycles += 5;
	z.WZ = (uint16) (GetIdx(ixmode) + d);
	return (z.WZ);
}

// ---------------------------------------------------------------------------
// ALU helpers

static inline void SetSZP (uint8 v)
{
	z.F = (z.F & FL_C) | (v & (FL_S | FL_X | FL_Y)) | (v ? 0 : FL_Z) | ParityTable[v];
}

static inline uint8 Inc8 (uint8 v)
{
	uint8	r = v + 1;
	z.F = (z.F & FL_C) | (r & (FL_S | FL_X | FL_Y)) | (r ? 0 : FL_Z) |
		  ((r & 0x0f) == 0 ? FL_H : 0) | (r == 0x80 ? FL_P : 0);
	return (r);
}

static inline uint8 Dec8 (uint8 v)
{
	uint8	r = v - 1;
	z.F = (z.F & FL_C) | FL_N | (r & (FL_S | FL_X | FL_Y)) | (r ? 0 : FL_Z) |
		  ((r & 0x0f) == 0x0f ? FL_H : 0) | (r == 0x7f ? FL_P : 0);
	return (r);
}

static inline void Add8 (uint8 v, uint8 carry)
{
	uint16	r = z.A + v + carry;
	uint8	res = (uint8) r;
	z.F = (res & (FL_S | FL_X | FL_Y)) | (res ? 0 : FL_Z) |
		  (((z.A ^ v ^ res) & 0x10) ? FL_H : 0) |
		  ((~(z.A ^ v) & (z.A ^ res) & 0x80) ? FL_P : 0) |
		  ((r & 0x100) ? FL_C : 0);
	z.A = res;
}

static inline void Sub8 (uint8 v, uint8 carry, bool8 store)
{
	uint16	r = z.A - v - carry;
	uint8	res = (uint8) r;
	z.F = FL_N | (res & (FL_S | FL_X | FL_Y)) | (res ? 0 : FL_Z) |
		  (((z.A ^ v ^ res) & 0x10) ? FL_H : 0) |
		  (((z.A ^ v) & (z.A ^ res) & 0x80) ? FL_P : 0) |
		  ((r & 0x100) ? FL_C : 0);
	if (store)
		z.A = res;
	else
		// CP leaves bits 3/5 from the operand, not the result.
		z.F = (z.F & ~(FL_X | FL_Y)) | (v & (FL_X | FL_Y));
}

static inline void And8 (uint8 v)	{ z.A &= v; SetSZP(z.A); z.F = (z.F & ~(FL_N | FL_C)) | FL_H; }
static inline void Or8 (uint8 v)	{ z.A |= v; SetSZP(z.A); z.F &= ~(FL_N | FL_C | FL_H); }
static inline void Xor8 (uint8 v)	{ z.A ^= v; SetSZP(z.A); z.F &= ~(FL_N | FL_C | FL_H); }

static inline uint16 Add16 (uint16 a, uint16 b)
{
	uint32	r = (uint32) a + b;
	z.WZ = (uint16) (a + 1);
	z.F = (z.F & (FL_S | FL_Z | FL_P)) |
		  (((a ^ b ^ r) & 0x1000) ? FL_H : 0) |
		  ((r & 0x10000) ? FL_C : 0) |
		  (((uint8) (r >> 8)) & (FL_X | FL_Y));
	z.Cycles += 7;
	return ((uint16) r);
}

static inline void Adc16 (uint16 v)
{
	uint16	hl = GetHL();
	uint32	r = (uint32) hl + v + (z.F & FL_C);
	uint16	res = (uint16) r;
	z.WZ = (uint16) (hl + 1);
	z.F = (((uint8) (res >> 8)) & (FL_S | FL_X | FL_Y)) | (res ? 0 : FL_Z) |
		  (((hl ^ v ^ res) & 0x1000) ? FL_H : 0) |
		  ((~(hl ^ v) & (hl ^ res) & 0x8000) ? FL_P : 0) |
		  ((r & 0x10000) ? FL_C : 0);
	SetHL(res);
	z.Cycles += 7;
}

static inline void Sbc16 (uint16 v)
{
	uint16	hl = GetHL();
	uint32	r = (uint32) hl - v - (z.F & FL_C);
	uint16	res = (uint16) r;
	z.WZ = (uint16) (hl + 1);
	z.F = FL_N | (((uint8) (res >> 8)) & (FL_S | FL_X | FL_Y)) | (res ? 0 : FL_Z) |
		  (((hl ^ v ^ res) & 0x1000) ? FL_H : 0) |
		  (((hl ^ v) & (hl ^ res) & 0x8000) ? FL_P : 0) |
		  ((r & 0x10000) ? FL_C : 0);
	SetHL(res);
	z.Cycles += 7;
}

static inline uint8 Rlc (uint8 v)	{ uint8 c = v >> 7; v = (v << 1) | c;            SetSZP(v); z.F = (z.F & ~(FL_H | FL_N | FL_C)) | (c ? FL_C : 0); return (v); }
static inline uint8 Rrc (uint8 v)	{ uint8 c = v & 1;  v = (v >> 1) | (c << 7);     SetSZP(v); z.F = (z.F & ~(FL_H | FL_N | FL_C)) | (c ? FL_C : 0); return (v); }
static inline uint8 Rl (uint8 v)	{ uint8 c = v >> 7; v = (v << 1) | (z.F & FL_C); SetSZP(v); z.F = (z.F & ~(FL_H | FL_N | FL_C)) | (c ? FL_C : 0); return (v); }
static inline uint8 Rr (uint8 v)	{ uint8 c = v & 1;  v = (v >> 1) | ((z.F & FL_C) << 7); SetSZP(v); z.F = (z.F & ~(FL_H | FL_N | FL_C)) | (c ? FL_C : 0); return (v); }
static inline uint8 Sla (uint8 v)	{ uint8 c = v >> 7; v <<= 1;                     SetSZP(v); z.F = (z.F & ~(FL_H | FL_N | FL_C)) | (c ? FL_C : 0); return (v); }
static inline uint8 Sra (uint8 v)	{ uint8 c = v & 1;  v = (v >> 1) | (v & 0x80);   SetSZP(v); z.F = (z.F & ~(FL_H | FL_N | FL_C)) | (c ? FL_C : 0); return (v); }
static inline uint8 Sll (uint8 v)	{ uint8 c = v >> 7; v = (v << 1) | 1;            SetSZP(v); z.F = (z.F & ~(FL_H | FL_N | FL_C)) | (c ? FL_C : 0); return (v); }
static inline uint8 Srl (uint8 v)	{ uint8 c = v & 1;  v >>= 1;                     SetSZP(v); z.F = (z.F & ~(FL_H | FL_N | FL_C)) | (c ? FL_C : 0); return (v); }

static inline void Daa (void)
{
	uint8	a = z.A;
	uint8	adjust = 0;
	uint8	carry = z.F & FL_C;

	if ((z.F & FL_H) || (a & 0x0f) > 9)
		adjust |= 0x06;
	if (carry || a > 0x99)
	{
		adjust |= 0x60;
		carry = FL_C;
	}

	uint8	r = (z.F & FL_N) ? a - adjust : a + adjust;
	z.F = (z.F & FL_N) | carry |
		  (((a ^ r) & 0x10) ? FL_H : 0) |
		  (r & (FL_S | FL_X | FL_Y)) | (r ? 0 : FL_Z) | ParityTable[r];
	z.A = r;
}

// ---------------------------------------------------------------------------
// Register operand decode: 0..7 = B C D E H L (mem) A. In index mode the H
// and L slots address the halves of IX/IY — but only for instructions that
// have no (IX+d) operand, so the memory forms pass ixmode 0 here.

static inline uint8 GetReg (int r, int ixmode)
{
	switch (r)
	{
		case 0: return (z.B);
		case 1: return (z.C);
		case 2: return (z.D);
		case 3: return (z.E);
		case 4: return (ixmode ? (uint8) (GetIdx(ixmode) >> 8) : z.H);
		case 5: return (ixmode ? (uint8) GetIdx(ixmode) : z.L);
		case 7: return (z.A);
	}
	return (0);
}

static inline void SetReg (int r, int ixmode, uint8 v)
{
	switch (r)
	{
		case 0: z.B = v;	break;
		case 1: z.C = v;	break;
		case 2: z.D = v;	break;
		case 3: z.E = v;	break;
		case 4:
			if (ixmode) SetIdx(ixmode, (uint16) ((GetIdx(ixmode) & 0x00ff) | ((uint16) v << 8)));
			else        z.H = v;
			break;
		case 5:
			if (ixmode) SetIdx(ixmode, (uint16) ((GetIdx(ixmode) & 0xff00) | v));
			else        z.L = v;
			break;
		case 7: z.A = v;	break;
	}
}

static inline bool8 CondMet (int cc)
{
	switch (cc)
	{
		case 0: return (!(z.F & FL_Z));		// NZ
		case 1: return ((z.F & FL_Z) != 0);	// Z
		case 2: return (!(z.F & FL_C));		// NC
		case 3: return ((z.F & FL_C) != 0);	// C
		case 4: return (!(z.F & FL_P));		// PO
		case 5: return ((z.F & FL_P) != 0);	// PE
		case 6: return (!(z.F & FL_S));		// P
		case 7: return ((z.F & FL_S) != 0);	// M
	}
	return (FALSE);
}

// ---------------------------------------------------------------------------
// CB block. In index mode the displacement is fetched BEFORE the sub-opcode
// (DD CB d op), the operand is always (IX+d), and the undocumented forms
// copy the result into the register the low three bits name.

static void ExecCB (int ixmode)
{
	uint16	ea = 0;
	uint8	op;

	if (ixmode)
	{
		int8	d = (int8) Fetch();
		ea = (uint16) (GetIdx(ixmode) + d);
		z.WZ = ea;
		op = Fetch();	// not an M1 cycle on DDCB
		z.Cycles += 2;
	}
	else
	{
		op = FetchM1();
		ea = GetHL();
	}

	const int	r = op & 0x07;
	const int	bit = (op >> 3) & 0x07;
	const bool8	mem = ixmode || (r == 6);
	uint8		v = mem ? RdMem(ea) : GetReg(r, 0);

	if (mem)
		z.Cycles += 1;

	switch (op >> 6)
	{
		case 0:	// rotates/shifts
			switch (bit)
			{
				case 0: v = Rlc(v);	break;
				case 1: v = Rrc(v);	break;
				case 2: v = Rl(v);	break;
				case 3: v = Rr(v);	break;
				case 4: v = Sla(v);	break;
				case 5: v = Sra(v);	break;
				case 6: v = Sll(v);	break;
				case 7: v = Srl(v);	break;
			}
			break;

		case 1:	// BIT b,r — bits 3/5 come from MEMPTR for the memory forms
		{
			uint8	t = v & (1 << bit);
			z.F = (z.F & FL_C) | FL_H | (t ? 0 : (FL_Z | FL_P)) | (t & FL_S) |
				  (mem ? (uint8) ((z.WZ >> 8) & (FL_X | FL_Y)) : (v & (FL_X | FL_Y)));
			return;
		}

		case 2:	v &= ~(1 << bit);	break;	// RES
		case 3:	v |= (1 << bit);	break;	// SET
	}

	if (mem)
	{
		WrMem(ea, v);
		if (ixmode && r != 6)	// undocumented: also lands in the named register
			SetReg(r, 0, v);
	}
	else
		SetReg(r, 0, v);
}

// ---------------------------------------------------------------------------
// ED block. The undefined entries are two-byte NOPs on real silicon.

static void ExecED (void)
{
	uint8	op = FetchM1();

	switch (op)
	{
		case 0x40: case 0x48: case 0x50: case 0x58:
		case 0x60: case 0x68: case 0x70: case 0x78:	// IN r,(C) (70h: flags only)
		{
			uint8	v = RdIO(GetBC());
			z.WZ = (uint16) (GetBC() + 1);
			if (op != 0x70)
				SetReg((op >> 3) & 7, 0, v);
			SetSZP(v);
			z.F &= ~(FL_H | FL_N);
			return;
		}

		case 0x41: case 0x49: case 0x51: case 0x59:
		case 0x61: case 0x69: case 0x79:			// OUT (C),r
			WrIO(GetBC(), GetReg((op >> 3) & 7, 0));
			z.WZ = (uint16) (GetBC() + 1);
			return;

		case 0x71:	// OUT (C),0 — undocumented
			WrIO(GetBC(), 0);
			z.WZ = (uint16) (GetBC() + 1);
			return;

		case 0x42:	Sbc16(GetBC());	return;
		case 0x52:	Sbc16(GetDE());	return;
		case 0x62:	Sbc16(GetHL());	return;
		case 0x72:	Sbc16(z.SP);	return;
		case 0x4a:	Adc16(GetBC());	return;
		case 0x5a:	Adc16(GetDE());	return;
		case 0x6a:	Adc16(GetHL());	return;
		case 0x7a:	Adc16(z.SP);	return;

		case 0x43: case 0x53: case 0x63: case 0x73:	// LD (nn),rp
		{
			uint16	addr = Fetch16();
			z.WZ = (uint16) (addr + 1);
			WrMem16(addr, op == 0x43 ? GetBC() : op == 0x53 ? GetDE() :
			              op == 0x63 ? GetHL() : z.SP);
			return;
		}

		case 0x4b: case 0x5b: case 0x6b: case 0x7b:	// LD rp,(nn)
		{
			uint16	addr = Fetch16();
			uint16	v = RdMem16(addr);
			z.WZ = (uint16) (addr + 1);
			if      (op == 0x4b) SetBC(v);
			else if (op == 0x5b) SetDE(v);
			else if (op == 0x6b) SetHL(v);
			else                 z.SP = v;
			return;
		}

		case 0x44: case 0x4c: case 0x54: case 0x5c:	// NEG (eight encodings)
		case 0x64: case 0x6c: case 0x74: case 0x7c:
		{
			uint8	a = z.A;
			z.A = 0;
			Sub8(a, 0, TRUE);
			return;
		}

		case 0x45: case 0x55: case 0x5d: case 0x65:	// RETN (and its aliases)
		case 0x6d: case 0x75: case 0x7d:
		case 0x4d:									// RETI
			z.IFF1 = z.IFF2;
			z.PC = Pop16();
			z.WZ = z.PC;
			return;

		case 0x46: case 0x4e: case 0x66: case 0x6e:	z.IM = 0;	return;
		case 0x56: case 0x76:						z.IM = 1;	return;
		case 0x5e: case 0x7e:						z.IM = 2;	return;

		case 0x47:	z.I = z.A;	z.Cycles += 1;	return;
		case 0x4f:	z.R = z.A;	z.Cycles += 1;	return;

		case 0x57:	// LD A,I
		case 0x5f:	// LD A,R
			z.A = (op == 0x57) ? z.I : z.R;
			z.F = (z.F & FL_C) | (z.A & (FL_S | FL_X | FL_Y)) | (z.A ? 0 : FL_Z) |
				  (z.IFF2 ? FL_P : 0);
			z.Cycles += 1;
			return;

		case 0x67:	// RRD
		{
			uint8	m = RdMem(GetHL());
			z.WZ = (uint16) (GetHL() + 1);
			WrMem(GetHL(), (uint8) ((m >> 4) | (z.A << 4)));
			z.A = (z.A & 0xf0) | (m & 0x0f);
			SetSZP(z.A);
			z.F &= ~(FL_H | FL_N);
			z.Cycles += 4;
			return;
		}

		case 0x6f:	// RLD
		{
			uint8	m = RdMem(GetHL());
			z.WZ = (uint16) (GetHL() + 1);
			WrMem(GetHL(), (uint8) ((m << 4) | (z.A & 0x0f)));
			z.A = (z.A & 0xf0) | (m >> 4);
			SetSZP(z.A);
			z.F &= ~(FL_H | FL_N);
			z.Cycles += 4;
			return;
		}

		case 0xa0: case 0xa8: case 0xb0: case 0xb8:	// LDI/LDD/LDIR/LDDR
		{
			const bool8	up = !(op & 0x08);
			const bool8	rep = (op & 0x10) != 0;
			const uint8	v = RdMem(GetHL());

			WrMem(GetDE(), v);
			SetHL(GetHL() + (up ? 1 : -1));
			SetDE(GetDE() + (up ? 1 : -1));
			SetBC(GetBC() - 1);

			const uint8	n = (uint8) (v + z.A);
			z.F = (z.F & (FL_S | FL_Z | FL_C)) | (GetBC() ? FL_P : 0) |
				  ((n & 0x02) ? FL_Y : 0) | ((n & 0x08) ? FL_X : 0);
			z.Cycles += 2;
			if (rep && GetBC())
			{
				z.PC -= 2;
				z.WZ = (uint16) (z.PC + 1);
				z.Cycles += 5;
			}
			return;
		}

		case 0xa1: case 0xa9: case 0xb1: case 0xb9:	// CPI/CPD/CPIR/CPDR
		{
			const bool8	up = !(op & 0x08);
			const bool8	rep = (op & 0x10) != 0;
			const uint8	v = RdMem(GetHL());
			uint8		r = z.A - v;

			SetHL(GetHL() + (up ? 1 : -1));
			SetBC(GetBC() - 1);
			z.WZ = (uint16) (z.WZ + (up ? 1 : -1));

			const uint8	h = ((z.A ^ v ^ r) & 0x10) ? FL_H : 0;
			const uint8	n = (uint8) (r - (h ? 1 : 0));
			z.F = (z.F & FL_C) | FL_N | (r & FL_S) | (r ? 0 : FL_Z) | h |
				  (GetBC() ? FL_P : 0) |
				  ((n & 0x02) ? FL_Y : 0) | ((n & 0x08) ? FL_X : 0);
			z.Cycles += 5;
			if (rep && GetBC() && r)
			{
				z.PC -= 2;
				z.WZ = (uint16) (z.PC + 1);
				z.Cycles += 5;
			}
			return;
		}

		case 0xa2: case 0xaa: case 0xb2: case 0xba:	// INI/IND/INIR/INDR
		{
			const bool8	up = !(op & 0x08);
			const bool8	rep = (op & 0x10) != 0;
			const uint8	v = RdIO(GetBC());

			z.WZ = (uint16) (GetBC() + (up ? 1 : -1));
			WrMem(GetHL(), v);
			SetHL(GetHL() + (up ? 1 : -1));
			z.B--;

			const uint16	t = (uint16) v + ((z.C + (up ? 1 : -1)) & 0xff);
			z.F = (z.B & (FL_S | FL_X | FL_Y)) | (z.B ? 0 : FL_Z) |
				  ((v & 0x80) ? FL_N : 0) | ((t & 0x100) ? (FL_H | FL_C) : 0) |
				  ParityTable[(uint8) ((t & 7) ^ z.B)];
			z.Cycles += 1;
			if (rep && z.B)
			{
				z.PC -= 2;
				z.Cycles += 5;
			}
			return;
		}

		case 0xa3: case 0xab: case 0xb3: case 0xbb:	// OUTI/OUTD/OTIR/OTDR
		{
			const bool8	up = !(op & 0x08);
			const bool8	rep = (op & 0x10) != 0;
			const uint8	v = RdMem(GetHL());

			z.B--;
			WrIO(GetBC(), v);
			SetHL(GetHL() + (up ? 1 : -1));
			z.WZ = (uint16) (GetBC() + (up ? 1 : -1));

			const uint16	t = (uint16) v + z.L;
			z.F = (z.B & (FL_S | FL_X | FL_Y)) | (z.B ? 0 : FL_Z) |
				  ((v & 0x80) ? FL_N : 0) | ((t & 0x100) ? (FL_H | FL_C) : 0) |
				  ParityTable[(uint8) ((t & 7) ^ z.B)];
			z.Cycles += 1;
			if (rep && z.B)
			{
				z.PC -= 2;
				z.Cycles += 5;
			}
			return;
		}
	}

	// Every other ED encoding is a two-byte NOP.
}

// ---------------------------------------------------------------------------
// Main opcode block, parameterized by index mode.

static void ExecOp (uint8 op, int ixmode)
{
	switch (op)
	{
		case 0x00:	return;	// NOP

		case 0x01:	SetBC(Fetch16());	return;
		case 0x11:	SetDE(Fetch16());	return;
		case 0x21:	SetIdx(ixmode, Fetch16());	return;
		case 0x31:	z.SP = Fetch16();	return;

		case 0x02:	WrMem(GetBC(), z.A);	z.WZ = (uint16) (((uint16) z.A << 8) | ((GetBC() + 1) & 0xff));	return;
		case 0x12:	WrMem(GetDE(), z.A);	z.WZ = (uint16) (((uint16) z.A << 8) | ((GetDE() + 1) & 0xff));	return;
		case 0x0a:	z.A = RdMem(GetBC());	z.WZ = (uint16) (GetBC() + 1);	return;
		case 0x1a:	z.A = RdMem(GetDE());	z.WZ = (uint16) (GetDE() + 1);	return;

		case 0x22:	// LD (nn),HL/IX
		{
			uint16	addr = Fetch16();
			z.WZ = (uint16) (addr + 1);
			WrMem16(addr, GetIdx(ixmode));
			return;
		}

		case 0x2a:	// LD HL/IX,(nn)
		{
			uint16	addr = Fetch16();
			z.WZ = (uint16) (addr + 1);
			SetIdx(ixmode, RdMem16(addr));
			return;
		}

		case 0x32:	// LD (nn),A
		{
			uint16	addr = Fetch16();
			WrMem(addr, z.A);
			z.WZ = (uint16) (((uint16) z.A << 8) | ((addr + 1) & 0xff));
			return;
		}

		case 0x3a:	// LD A,(nn)
		{
			uint16	addr = Fetch16();
			z.A = RdMem(addr);
			z.WZ = (uint16) (addr + 1);
			return;
		}

		case 0x03:	SetBC(GetBC() + 1);	z.Cycles += 2;	return;
		case 0x13:	SetDE(GetDE() + 1);	z.Cycles += 2;	return;
		case 0x23:	SetIdx(ixmode, GetIdx(ixmode) + 1);	z.Cycles += 2;	return;
		case 0x33:	z.SP++;	z.Cycles += 2;	return;
		case 0x0b:	SetBC(GetBC() - 1);	z.Cycles += 2;	return;
		case 0x1b:	SetDE(GetDE() - 1);	z.Cycles += 2;	return;
		case 0x2b:	SetIdx(ixmode, GetIdx(ixmode) - 1);	z.Cycles += 2;	return;
		case 0x3b:	z.SP--;	z.Cycles += 2;	return;

		case 0x04: case 0x0c: case 0x14: case 0x1c:
		case 0x24: case 0x2c: case 0x3c:	// INC r
			SetReg(op >> 3, ixmode, Inc8(GetReg(op >> 3, ixmode)));
			return;

		case 0x34:	// INC (HL)/(IX+d)
		{
			uint16	ea = EffAddr(ixmode);
			uint8	v = RdMem(ea);
			z.Cycles += 1;
			WrMem(ea, Inc8(v));
			return;
		}

		case 0x05: case 0x0d: case 0x15: case 0x1d:
		case 0x25: case 0x2d: case 0x3d:	// DEC r
			SetReg(op >> 3, ixmode, Dec8(GetReg(op >> 3, ixmode)));
			return;

		case 0x35:	// DEC (HL)/(IX+d)
		{
			uint16	ea = EffAddr(ixmode);
			uint8	v = RdMem(ea);
			z.Cycles += 1;
			WrMem(ea, Dec8(v));
			return;
		}

		case 0x06: case 0x0e: case 0x16: case 0x1e:
		case 0x26: case 0x2e: case 0x3e:	// LD r,n
			SetReg(op >> 3, ixmode, Fetch());
			return;

		case 0x36:	// LD (HL),n / LD (IX+d),n — the literal follows the displacement
			if (ixmode)
			{
				int8	d = (int8) Fetch();
				uint16	ea = (uint16) (GetIdx(ixmode) + d);
				uint8	n = Fetch();
				z.WZ = ea;
				z.Cycles += 2;
				WrMem(ea, n);
			}
			else
				WrMem(GetHL(), Fetch());
			return;

		case 0x07:	// RLCA
		{
			uint8	c = z.A >> 7;
			z.A = (z.A << 1) | c;
			z.F = (z.F & (FL_S | FL_Z | FL_P)) | (z.A & (FL_X | FL_Y)) | (c ? FL_C : 0);
			return;
		}

		case 0x0f:	// RRCA
		{
			uint8	c = z.A & 1;
			z.A = (z.A >> 1) | (c << 7);
			z.F = (z.F & (FL_S | FL_Z | FL_P)) | (z.A & (FL_X | FL_Y)) | (c ? FL_C : 0);
			return;
		}

		case 0x17:	// RLA
		{
			uint8	c = z.A >> 7;
			z.A = (z.A << 1) | (z.F & FL_C);
			z.F = (z.F & (FL_S | FL_Z | FL_P)) | (z.A & (FL_X | FL_Y)) | (c ? FL_C : 0);
			return;
		}

		case 0x1f:	// RRA
		{
			uint8	c = z.A & 1;
			z.A = (z.A >> 1) | ((z.F & FL_C) << 7);
			z.F = (z.F & (FL_S | FL_Z | FL_P)) | (z.A & (FL_X | FL_Y)) | (c ? FL_C : 0);
			return;
		}

		case 0x27:	Daa();	return;
		case 0x2f:	// CPL
			z.A = ~z.A;
			z.F = (z.F & (FL_S | FL_Z | FL_P | FL_C)) | FL_H | FL_N | (z.A & (FL_X | FL_Y));
			return;
		case 0x37:	// SCF
			z.F = (z.F & (FL_S | FL_Z | FL_P)) | FL_C | (z.A & (FL_X | FL_Y));
			return;
		case 0x3f:	// CCF
			z.F = ((z.F & (FL_S | FL_Z | FL_P | FL_C)) ^ FL_C) |
				  ((z.F & FL_C) ? FL_H : 0) | (z.A & (FL_X | FL_Y));
			return;

		case 0x08:	// EX AF,AF'
		{
			uint8	t;
			t = z.A; z.A = z.A2; z.A2 = t;
			t = z.F; z.F = z.F2; z.F2 = t;
			return;
		}

		case 0x09:	SetIdx(ixmode, Add16(GetIdx(ixmode), GetBC()));	return;
		case 0x19:	SetIdx(ixmode, Add16(GetIdx(ixmode), GetDE()));	return;
		case 0x29:	SetIdx(ixmode, Add16(GetIdx(ixmode), GetIdx(ixmode)));	return;
		case 0x39:	SetIdx(ixmode, Add16(GetIdx(ixmode), z.SP));	return;

		case 0x10:	// DJNZ
		{
			int8	d = (int8) Fetch();
			z.Cycles += 1;
			if (--z.B)
			{
				z.PC += d;
				z.WZ = z.PC;
				z.Cycles += 5;
			}
			return;
		}

		case 0x18:	// JR
		{
			int8	d = (int8) Fetch();
			z.PC += d;
			z.WZ = z.PC;
			z.Cycles += 5;
			return;
		}

		case 0x20: case 0x28: case 0x30: case 0x38:	// JR cc
		{
			int8	d = (int8) Fetch();
			if (CondMet((op >> 3) & 3))
			{
				z.PC += d;
				z.WZ = z.PC;
				z.Cycles += 5;
			}
			return;
		}

		case 0x76:	// HALT — PC stays past the opcode so the ISR's RET resumes after it
			z.Halted = TRUE;
			return;

		default:
			break;
	}

	// LD r,r' block (40h-7Fh, 76h handled above)
	if (op >= 0x40 && op <= 0x7f)
	{
		const int	dst = (op >> 3) & 7;
		const int	src = op & 7;

		if (src == 6)
			SetReg(dst, 0, RdMem(EffAddr(ixmode)));
		else if (dst == 6)
			WrMem(EffAddr(ixmode), GetReg(src, 0));
		else
			SetReg(dst, ixmode, GetReg(src, ixmode));
		return;
	}

	// ALU op block (80h-BFh)
	if (op >= 0x80 && op <= 0xbf)
	{
		const int	src = op & 7;
		const uint8	v = (src == 6) ? RdMem(EffAddr(ixmode)) : GetReg(src, ixmode);

		switch ((op >> 3) & 7)
		{
			case 0:	Add8(v, 0);	break;
			case 1:	Add8(v, z.F & FL_C);	break;
			case 2:	Sub8(v, 0, TRUE);	break;
			case 3:	Sub8(v, z.F & FL_C, TRUE);	break;
			case 4:	And8(v);	break;
			case 5:	Xor8(v);	break;
			case 6:	Or8(v);		break;
			case 7:	Sub8(v, 0, FALSE);	break;	// CP
		}
		return;
	}

	// C0h-FFh
	switch (op)
	{
		case 0xc0: case 0xc8: case 0xd0: case 0xd8:
		case 0xe0: case 0xe8: case 0xf0: case 0xf8:	// RET cc
			z.Cycles += 1;
			if (CondMet((op >> 3) & 7))
			{
				z.PC = Pop16();
				z.WZ = z.PC;
			}
			return;

		case 0xc9:	z.PC = Pop16();	z.WZ = z.PC;	return;

		case 0xc1:	SetBC(Pop16());	return;
		case 0xd1:	SetDE(Pop16());	return;
		case 0xe1:	SetIdx(ixmode, Pop16());	return;
		case 0xf1:	SetAF(Pop16());	return;

		case 0xc5:	z.Cycles += 1;	Push16(GetBC());	return;
		case 0xd5:	z.Cycles += 1;	Push16(GetDE());	return;
		case 0xe5:	z.Cycles += 1;	Push16(GetIdx(ixmode));	return;
		case 0xf5:	z.Cycles += 1;	Push16(GetAF());	return;

		case 0xc2: case 0xca: case 0xd2: case 0xda:
		case 0xe2: case 0xea: case 0xf2: case 0xfa:	// JP cc,nn
		{
			uint16	target = Fetch16();
			z.WZ = target;
			if (CondMet((op >> 3) & 7))
				z.PC = target;
			return;
		}

		case 0xc3:	z.PC = Fetch16();	z.WZ = z.PC;	return;
		case 0xe9:	z.PC = GetIdx(ixmode);	return;	// JP (HL)/(IX) — leaves MEMPTR alone

		case 0xc4: case 0xcc: case 0xd4: case 0xdc:
		case 0xe4: case 0xec: case 0xf4: case 0xfc:	// CALL cc,nn
		{
			uint16	target = Fetch16();
			z.WZ = target;
			if (CondMet((op >> 3) & 7))
			{
				z.Cycles += 1;
				Push16(z.PC);
				z.PC = target;
			}
			return;
		}

		case 0xcd:	// CALL nn
		{
			uint16	target = Fetch16();
			z.WZ = target;
			z.Cycles += 1;
			Push16(z.PC);
			z.PC = target;
			return;
		}

		case 0xc7: case 0xcf: case 0xd7: case 0xdf:
		case 0xe7: case 0xef: case 0xf7: case 0xff:	// RST p
			z.Cycles += 1;
			Push16(z.PC);
			z.PC = op & 0x38;
			z.WZ = z.PC;
			return;

		case 0xc6:	Add8(Fetch(), 0);	return;
		case 0xce:	Add8(Fetch(), z.F & FL_C);	return;
		case 0xd6:	Sub8(Fetch(), 0, TRUE);	return;
		case 0xde:	Sub8(Fetch(), z.F & FL_C, TRUE);	return;
		case 0xe6:	And8(Fetch());	return;
		case 0xee:	Xor8(Fetch());	return;
		case 0xf6:	Or8(Fetch());	return;
		case 0xfe:	Sub8(Fetch(), 0, FALSE);	return;

		case 0xd3:	// OUT (n),A
		{
			uint8	n = Fetch();
			WrIO((uint16) (((uint16) z.A << 8) | n), z.A);
			z.WZ = (uint16) (((uint16) z.A << 8) | ((n + 1) & 0xff));
			return;
		}

		case 0xdb:	// IN A,(n)
		{
			uint8	n = Fetch();
			uint16	port = (uint16) (((uint16) z.A << 8) | n);
			z.A = RdIO(port);
			z.WZ = (uint16) (port + 1);
			return;
		}

		case 0xd9:	// EXX
		{
			uint8	t;
			t = z.B; z.B = z.B2; z.B2 = t;
			t = z.C; z.C = z.C2; z.C2 = t;
			t = z.D; z.D = z.D2; z.D2 = t;
			t = z.E; z.E = z.E2; z.E2 = t;
			t = z.H; z.H = z.H2; z.H2 = t;
			t = z.L; z.L = z.L2; z.L2 = t;
			return;
		}

		case 0xe3:	// EX (SP),HL/IX
		{
			uint16	t = RdMem16(z.SP);
			WrMem16(z.SP, GetIdx(ixmode));
			SetIdx(ixmode, t);
			z.WZ = t;
			z.Cycles += 3;
			return;
		}

		case 0xeb:	// EX DE,HL (the prefixes do not reach this one)
		{
			uint16	t = GetDE();
			SetDE(GetHL());
			SetHL(t);
			return;
		}

		case 0xf3:	z.IFF1 = z.IFF2 = 0;	z.EIPending = 0;	return;	// DI
		case 0xfb:	// EI — the interrupt window opens after the NEXT instruction
			z.IFF1 = z.IFF2 = 1;
			z.EIPending = 1;
			return;

		case 0xf9:	z.SP = GetIdx(ixmode);	z.Cycles += 2;	return;
	}

	// Unreachable: CB/ED/DD/FD are consumed by the prefix loop in Step().
}

// ---------------------------------------------------------------------------
// Interrupts

static void Step (void)
{
	int	ixmode = 0;

	for (;;)
	{
		uint8	op = FetchM1();

		if (op == 0xdd)		{ ixmode = 1; continue; }
		if (op == 0xfd)		{ ixmode = 2; continue; }
		if (op == 0xcb)		{ ExecCB(ixmode); return; }
		if (op == 0xed)		{ ExecED(); return; }	// a pending DD/FD is discarded

		ExecOp(op, ixmode);
		return;
	}
}

static void TakeNMI (void)
{
	z.NMIPending = FALSE;
	z.Halted = FALSE;
	z.IFF2 = z.IFF1;
	z.IFF1 = 0;
	z.R = (z.R & 0x80) | ((z.R + 1) & 0x7f);
	Push16(z.PC);
	z.PC = 0x0066;
	z.WZ = z.PC;
	z.Cycles += 5;
}

static void TakeINT (void)
{
	z.Halted = FALSE;
	z.IFF1 = z.IFF2 = 0;
	z.R = (z.R & 0x80) | ((z.R + 1) & 0x7f);

	const uint8	bus = Z80CB.IntAck ? Z80CB.IntAck() : 0xff;

	switch (z.IM)
	{
		case 0:
			// Only the RST opcodes the hardware can place on the bus are
			// honoured; anything else behaves as the NOP it decodes to.
			z.Cycles += 6;
			if ((bus & 0xc7) == 0xc7)
			{
				Push16(z.PC);
				z.PC = bus & 0x38;
				z.WZ = z.PC;
			}
			break;

		case 1:
			z.Cycles += 7;
			Push16(z.PC);
			z.PC = 0x0038;
			z.WZ = z.PC;
			break;

		default:
		{
			z.Cycles += 7;
			Push16(z.PC);
			const uint16	vaddr = (uint16) (((uint16) z.I << 8) | bus);
			z.PC = RdMem16(vaddr);
			z.WZ = z.PC;
			break;
		}
	}
}

// ---------------------------------------------------------------------------

void Z80_Reset (void)
{
	if (!TablesBuilt)
		BuildTables();

	memset(&z, 0, sizeof(z));
	z.A = z.F = 0xff;
	z.A2 = z.F2 = 0xff;
	z.B = z.C = z.D = z.E = z.H = z.L = 0xff;
	z.B2 = z.C2 = z.D2 = z.E2 = z.H2 = z.L2 = 0xff;
	z.IX = z.IY = 0xffff;
	z.SP = 0xffff;
	z.PC = 0;
	z.WZ = 0;
	z.I = z.R = 0;
	z.IM = 0;
}

int32 Z80_Execute (int32 cycles)
{
	z.Cycles = 0;

	while (z.Cycles < cycles)
	{
		if (z.NMIPending)
		{
			TakeNMI();
			continue;
		}

		// EI only opens the window once the instruction after it has run.
		const bool8	window = z.EIPending ? FALSE : TRUE;
		z.EIPending = 0;

		if (window && z.IFF1 && z.INTLine)
		{
			TakeINT();
			continue;
		}

		if (z.Halted)
		{
			// Idle at bus speed; an interrupt is what gets us out.
			z.Cycles += 4;
			z.R = (z.R & 0x80) | ((z.R + 1) & 0x7f);
			continue;
		}

		Step();
	}

	z.TotalCycles += (uint32) z.Cycles;
	return (z.Cycles);
}

void Z80_SetINT (bool8 asserted)	{ z.INTLine = asserted ? 1 : 0; }
void Z80_PulseNMI (void)			{ z.NMIPending = 1; }
