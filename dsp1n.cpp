/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Native DSP-1/DSP-1B. Each command is a program that resumes where it last stopped:
// at a bus access the clock hasn't reached yet, or at a wait on the CPU. NT_BURN
// owes the instructions the firmware spends between two accesses, so the CPU sees
// every result and status change at the same clock as on the chip.

#include <string.h>
#ifdef _MSC_VER
#include <intrin.h>
#endif
#include "snes9x.h"
#include "dsp.h"
#include "upd7725n.h"

namespace
{

#define NT_IDLE_WAIT	1
#include "upd7725np.h"

uint16	rom[1024];		// the data ROM

// back through $000: wait for the last result to be read, 8-bit mode, DR = $80
#define NT_EPILOGUE(id)		do { NT_BURN(1); NT_XWAIT(id, 4, AtAccess, EpilogueStep); NT_SR((id) + 1, 0x0400); NT_WRITE((id) + 2, 0x0080); } while (0)

// that turn, from its wait: SR, DR, the end, and the command wait's burn, parked at it
bool EpilogueStep (void)
{
	*sr = (uint16) ((*sr & SR_FIXED) | (0x0400 & ~SR_FIXED));
	*dr = 0x0080;
	*sr |= SR_RQM;
	s.running = 0;
	return (LanePark(NT_IDLE_WAIT, 0, AtAccess, NULL));
}

// The DSP-1B's data ROM is the HLE's table; the first DSP-1's has two more words
// at $116, so everything past them sits two later and two table pointers follow.
void BuildDataROM (bool first)
{
	if (!first)
	{
		memcpy(rom, DSP1ROM, sizeof(rom));
		return;
	}
	memcpy(rom, DSP1ROM, 0x116 * 2);
	rom[0x116] = 0x0020;
	rom[0x117] = 0x0040;
	memcpy(rom + 0x118, DSP1ROM + 0x116, (1022 - 0x116) * 2);
	rom[0x21b] += 2;
	rom[0x322] += 2;
}

// the constants past $116, wherever this revision keeps them
inline int16 RomK (int i)
{
	return ((int16) rom[i + (s.variant ? 2 : 0)]);
}

// -x, saturating -(-1) to 1 - 2^-15
inline int16 NegSat (int16 x)
{
	return (x == -32768 ? (int16) 0x7fff : (int16) -x);
}

// shifts until bit 15 and bit 14 differ: the redundant sign bits, 15 at most
inline int NormShift (int16 v)
{
	const uint32	x = (uint32) (uint16) (v ^ (v >> 15)) << 1 | 1;
#ifdef _MSC_VER
	unsigned long	top;
	_BitScanReverse(&top, x);
	return (15 - (int) top);
#else
	return (__builtin_clz(x) - 16);
#endif
}

// The firmware's shared routines: A and B on return, and the instructions from entry through return.
struct SubOut
{
	int16	a, b;
	uint32	cost;
};

// normalize A; B less the shift (0 for A = 0)
SubOut SubNorm (int16 a, int16 b)
{
	if (!a)
		return { 0, 0, 4 };
	const int	n = NormShift(a);
	return { (int16) (a << n), (int16) (b - n), (uint32) (7 + 2 * n) };
}

// the double word A:B as a mantissa and a bit-position exponent
SubOut SubNormD (int16 hi, int16 lo)
{
	if (hi != 0 && hi != -1)
	{
		const SubOut	n = SubNorm(hi, 0x40);
		const int		sh = 0x40 - n.b;
		return { (int16) (n.a + ((lo >> 1 & 0x7fff) * (int16) rom[0x40 - sh] >> 15)), (int16) (31 - sh), 15 + n.cost };
	}
	if ((lo < 0) == (hi < 0))
	{
		const SubOut	n = SubNorm(lo, 15);
		return { n.a, n.b, (uint32) (hi ? 6 : 8) + n.cost };
	}
	return { (int16) ((lo >> 1) ^ 0x8000), 16, (uint32) (hi ? 8 : 10) };
}

// back to fixed point: a table shift right, or saturate
SubOut SubDenorm (int16 a, int16 b)
{
	const SubOut	n = SubNorm(a, b);
	const int16		e = (int16) (n.b - 15);		// 16-bit: a far negative exponent wraps
	int16	v = n.a;
	if (e < 0)
		v = Mul(v, (int16) rom[(0x31 + e) & 0x3ff]);
	else
	if (e > 0)
		v = v < 0 ? (int16) 0x8001 : 0x7fff;
	return { v, 0, 1 + n.cost + 3 + (e ? 5u : 2u) };
}

// to a fixed-point double word, high word in A; a left shift out of range saturates
SubOut SubDenormD (int16 a, int16 b)
{
	const SubOut	n = SubNorm(a, b);
	const int16		e = (int16) (n.b - 15);
	const int16		hi = n.a < 0 ? -1 : 0;
	uint32	cost = 1 + n.cost + 3;
	if (e == 0)
		return { hi, n.a, cost + 2 + (n.a < 0 ? 2u : 1u) };
	if (e < 0)
	{
		const int16	v = Mul(n.a, (int16) rom[(0x31 + e) & 0x3ff]);
		return { (int16) (v < 0 ? -1 : 0), v, cost + 1 + 6 + 1 };
	}
	// multiplies by the table's power of two
	cost += 1 + 4 + (hi ? 1 : 0) + 1 + 3 + 6;
	const int16	l = (int16) rom[(0x41 - e) & 0x3ff];
	const int32	ph = (int32) hi * l;
	const int16	m = (int16) (ph >> 15), nn = (int16) (ph << 1);
	int16	acc = 0;
	bool	ok;
	if (m < 0)
	{
		const int16	t = (int16) ~m;
		cost += 2;
		ok = false;
		if (!t)
		{
			acc = (int16) (t | nn);
			cost += 2;
			ok = acc < 0;
		}
		if (!ok)
			return { (int16) 0x8001, 0, cost + 2 };
	}
	else
	{
		cost += 1;
		ok = false;
		if (!m)
		{
			acc = (int16) (m | nn);
			cost += 2;
			ok = acc >= 0;
		}
		if (!ok)
			return { 0x7fff, (int16) 0xffff, cost + 2 };
	}
	const int16	lo = n.a;
	cost += 2;
	if (lo < 0)
	{
		acc = (int16) (acc + 2 * l);
		cost += 2;
	}
	const int32	pl = (int32) lo * l;
	return { (int16) (acc + (int16) (pl >> 15)), (int16) (pl << 1), cost + 3 };
}

// 1 / (normalized mantissa A, exponent B): a table guess, two Newton steps, exponent 1 - B.
// Exactly 1/2 overflows both steps; -1 loses its sign bit to 1 - 2^-15 first.
SubOut SubInv (int16 a, int16 b)
{
	if (!a)
		return { 0x7fff, 0x2f, 5 };
	const bool	neg = a < 0;
	const int16	c = neg ? (a == -32768 ? 0x7fff : (int16) -a) : a;
	uint32	cost = neg ? (a == -32768 ? 29 : 28) : 24;
	int16	v;
	if (c == 0x4000)
	{
		v = 0x7fff;
		cost += 2;
	}
	else
	{
		int16	i = (int16) rom[((c - 0x4000) >> 7) + 0x0065];
		i = (int16) ((i + (-i * (c * i >> 15) >> 15)) << 1);
		i = (int16) ((i + (-i * (c * i >> 15) >> 15)) << 1);
		v = i;
	}
	return { neg ? (int16) -v : v, (int16) (1 - b), cost };
}

// square root of (mantissa A, exponent B + 1) by the table at $D5; B comes back halved
SubOut SubSqrt (int16 c, int16 e)
{
	const bool	odd = e & 1;
	e >>= 1;
	if (odd)
	{
		c >>= 1;
		e++;
	}
	const int	idx = (0xd6 + (c * 0x40 >> 15)) & 0x3ff;
	const int16	node2 = (int16) rom[idx], node1 = (int16) rom[(idx - 1) & 0x3ff];
	// the first DSP-1 doesn't mask the fraction, so mantissa bit 9 lands in its sign
	const int16	frac = s.variant ? (int16) (c << 6) : (int16) ((c & 0x1ff) << 6);
	const int16	r = (int16) (node1 + ((int16) (node2 - node1) * frac >> 15));
	return { r, e, (s.variant ? 13u : 14u) + (odd ? 2 : 0) };
}

const int16	SinTable[256] =
{
	 0x0000,  0x0324,  0x0647,  0x096a,  0x0c8b,  0x0fab,  0x12c8,  0x15e2,
	 0x18f8,  0x1c0b,  0x1f19,  0x2223,  0x2528,  0x2826,  0x2b1f,  0x2e11,
	 0x30fb,  0x33de,  0x36ba,  0x398c,  0x3c56,  0x3f17,  0x41ce,  0x447a,
	 0x471c,  0x49b4,  0x4c3f,  0x4ebf,  0x5133,  0x539b,  0x55f5,  0x5842,
	 0x5a82,  0x5cb4,  0x5ed7,  0x60ec,  0x62f2,  0x64e8,  0x66cf,  0x68a6,
	 0x6a6d,  0x6c24,  0x6dca,  0x6f5f,  0x70e2,  0x7255,  0x73b5,  0x7504,
	 0x7641,  0x776c,  0x7884,  0x798a,  0x7a7d,  0x7b5d,  0x7c29,  0x7ce3,
	 0x7d8a,  0x7e1d,  0x7e9d,  0x7f09,  0x7f62,  0x7fa7,  0x7fd8,  0x7ff6,
	 0x7fff,  0x7ff6,  0x7fd8,  0x7fa7,  0x7f62,  0x7f09,  0x7e9d,  0x7e1d,
	 0x7d8a,  0x7ce3,  0x7c29,  0x7b5d,  0x7a7d,  0x798a,  0x7884,  0x776c,
	 0x7641,  0x7504,  0x73b5,  0x7255,  0x70e2,  0x6f5f,  0x6dca,  0x6c24,
	 0x6a6d,  0x68a6,  0x66cf,  0x64e8,  0x62f2,  0x60ec,  0x5ed7,  0x5cb4,
	 0x5a82,  0x5842,  0x55f5,  0x539b,  0x5133,  0x4ebf,  0x4c3f,  0x49b4,
	 0x471c,  0x447a,  0x41ce,  0x3f17,  0x3c56,  0x398c,  0x36ba,  0x33de,
	 0x30fb,  0x2e11,  0x2b1f,  0x2826,  0x2528,  0x2223,  0x1f19,  0x1c0b,
	 0x18f8,  0x15e2,  0x12c8,  0x0fab,  0x0c8b,  0x096a,  0x0647,  0x0324,
	-0x0000, -0x0324, -0x0647, -0x096a, -0x0c8b, -0x0fab, -0x12c8, -0x15e2,
	-0x18f8, -0x1c0b, -0x1f19, -0x2223, -0x2528, -0x2826, -0x2b1f, -0x2e11,
	-0x30fb, -0x33de, -0x36ba, -0x398c, -0x3c56, -0x3f17, -0x41ce, -0x447a,
	-0x471c, -0x49b4, -0x4c3f, -0x4ebf, -0x5133, -0x539b, -0x55f5, -0x5842,
	-0x5a82, -0x5cb4, -0x5ed7, -0x60ec, -0x62f2, -0x64e8, -0x66cf, -0x68a6,
	-0x6a6d, -0x6c24, -0x6dca, -0x6f5f, -0x70e2, -0x7255, -0x73b5, -0x7504,
	-0x7641, -0x776c, -0x7884, -0x798a, -0x7a7d, -0x7b5d, -0x7c29, -0x7ce3,
	-0x7d8a, -0x7e1d, -0x7e9d, -0x7f09, -0x7f62, -0x7fa7, -0x7fd8, -0x7ff6,
	-0x7fff, -0x7ff6, -0x7fd8, -0x7fa7, -0x7f62, -0x7f09, -0x7e9d, -0x7e1d,
	-0x7d8a, -0x7ce3, -0x7c29, -0x7b5d, -0x7a7d, -0x798a, -0x7884, -0x776c,
	-0x7641, -0x7504, -0x73b5, -0x7255, -0x70e2, -0x6f5f, -0x6dca, -0x6c24,
	-0x6a6d, -0x68a6, -0x66cf, -0x64e8, -0x62f2, -0x60ec, -0x5ed7, -0x5cb4,
	-0x5a82, -0x5842, -0x55f5, -0x539b, -0x5133, -0x4ebf, -0x4c3f, -0x49b4,
	-0x471c, -0x447a, -0x41ce, -0x3f17, -0x3c56, -0x398c, -0x36ba, -0x33de,
	-0x30fb, -0x2e11, -0x2b1f, -0x2826, -0x2528, -0x2223, -0x1f19, -0x1c0b,
	-0x18f8, -0x15e2, -0x12c8, -0x0fab, -0x0c8b, -0x096a, -0x0647, -0x0324
};

// The interpolation step within a sin table bucket.
const int16	MulTable[256] =
{
	 0x0000,  0x0003,  0x0006,  0x0009,  0x000c,  0x000f,  0x0012,  0x0015,
	 0x0019,  0x001c,  0x001f,  0x0022,  0x0025,  0x0028,  0x002b,  0x002f,
	 0x0032,  0x0035,  0x0038,  0x003b,  0x003e,  0x0041,  0x0045,  0x0048,
	 0x004b,  0x004e,  0x0051,  0x0054,  0x0057,  0x005b,  0x005e,  0x0061,
	 0x0064,  0x0067,  0x006a,  0x006d,  0x0071,  0x0074,  0x0077,  0x007a,
	 0x007d,  0x0080,  0x0083,  0x0087,  0x008a,  0x008d,  0x0090,  0x0093,
	 0x0096,  0x0099,  0x009d,  0x00a0,  0x00a3,  0x00a6,  0x00a9,  0x00ac,
	 0x00af,  0x00b3,  0x00b6,  0x00b9,  0x00bc,  0x00bf,  0x00c2,  0x00c5,
	 0x00c9,  0x00cc,  0x00cf,  0x00d2,  0x00d5,  0x00d8,  0x00db,  0x00df,
	 0x00e2,  0x00e5,  0x00e8,  0x00eb,  0x00ee,  0x00f1,  0x00f5,  0x00f8,
	 0x00fb,  0x00fe,  0x0101,  0x0104,  0x0107,  0x010b,  0x010e,  0x0111,
	 0x0114,  0x0117,  0x011a,  0x011d,  0x0121,  0x0124,  0x0127,  0x012a,
	 0x012d,  0x0130,  0x0133,  0x0137,  0x013a,  0x013d,  0x0140,  0x0143,
	 0x0146,  0x0149,  0x014d,  0x0150,  0x0153,  0x0156,  0x0159,  0x015c,
	 0x015f,  0x0163,  0x0166,  0x0169,  0x016c,  0x016f,  0x0172,  0x0175,
	 0x0178,  0x017c,  0x017f,  0x0182,  0x0185,  0x0188,  0x018b,  0x018e,
	 0x0192,  0x0195,  0x0198,  0x019b,  0x019e,  0x01a1,  0x01a4,  0x01a8,
	 0x01ab,  0x01ae,  0x01b1,  0x01b4,  0x01b7,  0x01ba,  0x01be,  0x01c1,
	 0x01c4,  0x01c7,  0x01ca,  0x01cd,  0x01d0,  0x01d4,  0x01d7,  0x01da,
	 0x01dd,  0x01e0,  0x01e3,  0x01e6,  0x01ea,  0x01ed,  0x01f0,  0x01f3,
	 0x01f6,  0x01f9,  0x01fc,  0x0200,  0x0203,  0x0206,  0x0209,  0x020c,
	 0x020f,  0x0212,  0x0216,  0x0219,  0x021c,  0x021f,  0x0222,  0x0225,
	 0x0228,  0x022c,  0x022f,  0x0232,  0x0235,  0x0238,  0x023b,  0x023e,
	 0x0242,  0x0245,  0x0248,  0x024b,  0x024e,  0x0251,  0x0254,  0x0258,
	 0x025b,  0x025e,  0x0261,  0x0264,  0x0267,  0x026a,  0x026e,  0x0271,
	 0x0274,  0x0277,  0x027a,  0x027d,  0x0280,  0x0284,  0x0287,  0x028a,
	 0x028d,  0x0290,  0x0293,  0x0296,  0x029a,  0x029d,  0x02a0,  0x02a3,
	 0x02a6,  0x02a9,  0x02ac,  0x02b0,  0x02b3,  0x02b6,  0x02b9,  0x02bc,
	 0x02bf,  0x02c2,  0x02c6,  0x02c9,  0x02cc,  0x02cf,  0x02d2,  0x02d5,
	 0x02d8,  0x02db,  0x02df,  0x02e2,  0x02e5,  0x02e8,  0x02eb,  0x02ee,
	 0x02f1,  0x02f5,  0x02f8,  0x02fb,  0x02fe,  0x0301,  0x0304,  0x0307,
	 0x030b,  0x030e,  0x0311,  0x0314,  0x0317,  0x031a,  0x031d,  0x0321
};

int16 Sin (int16 angle)
{
	if (angle < 0)
	{
		if (angle == -32768)
			return (0);
		return ((int16) -Sin((int16) -angle));
	}
	const int32	v = SinTable[angle >> 8] + (MulTable[angle & 0xff] * SinTable[0x40 + (angle >> 8)] >> 15);
	return ((int16) (v > 32767 ? 32767 : v));
}

int16 Cos (int16 angle)
{
	if (angle < 0)
	{
		if (angle == -32768)
			return (-32768);
		angle = (int16) -angle;
	}
	const int32	v = SinTable[0x40 + (angle >> 8)] - (MulTable[angle & 0xff] * SinTable[angle >> 8] >> 15);
	return ((int16) (v < -32768 ? -32767 : v));
}

// Instructions from an angle read to the next wait in the commands that only take sin/cos:
// 4 more for a negative angle, 1 more where the interpolation saturates.
uint32 TriangleCost (int16 angle)
{
	if (angle == -32768)
		return (8);
	const uint16	a = (uint16) (angle < 0 ? -angle : angle);
	const bool		sat = ((a >> 8) == 0x3f && (a & 0xff) >= 0x82) || ((a >> 8) == 0x7f && (a & 0xff) >= 0x8f);
	return ((angle < 0 ? 28 : 24) + (sat ? 1 : 0));
}

// cos in A, sin in B
SubOut SubSinCos (int16 angle)
{
	return { Cos(angle), Sin(angle), TriangleCost(angle) - 2 };
}

// |v| from 2 (x^2 + y^2 + z^2) mod 2^32, and the instructions up to its write
int16 DistanceOf (uint32 sq, uint32 *cost)
{
	const SubOut	n = SubNormD((int16) (sq >> 16), (int16) sq);
	const SubOut	q = SubSqrt(n.a, (int16) (n.b - 1));
	const SubOut	d = SubDenorm(q.a, q.b);
	*cost = 8 + n.cost + q.cost + d.cost;
	return (d.a);
}

// ---- the commands

// Their words in and out as lanes: inputs into s.in[k], outputs from s.out[F + k], each after its gap.
template <int32 N> int32 Words (void)
{
	return (N);
}

void PutIn (int32 k, uint16 v)
{
	s.in[k] = (int16) v;
}

template <int32 F> uint16 GetOut (int32 k)
{
	return ((uint16) s.out[F + k]);
}

#define D1_GAPS(name, ...)	uint32 name##Gap (int32 k) { static const uint8 g[] = { __VA_ARGS__ }; return (g[k]); }
#define D1_IN(name, n, x)	const Lane name = { Words<n>, name##Gap, PutIn, NULL, LaneIn<Words<n>, name##Gap, PutIn, x>, x };
#define D1_OUT(name, f, n, x)	const Lane name = { Words<n>, name##Gap, NULL, GetOut<f>, LaneOut<Words<n>, name##Gap, GetOut<f>, x, 4, EpilogueStep>, x };

D1_GAPS(MultiplyIn, 10, 0)			D1_IN(MultiplyIn, 1, 4)
D1_GAPS(InverseIn, 11, 0)			D1_IN(InverseIn, 1, 4)
D1_GAPS(InverseOut, 0, 1)			D1_OUT(InverseOut, 1, 1, 9)
D1_GAPS(TriangleOut, 1, 1)			D1_OUT(TriangleOut, 1, 1, 9)
D1_GAPS(RadiusIn, 12, 2, 2)			D1_IN(RadiusIn, 2, 6)
D1_GAPS(RadiusOut, 0, 1)			D1_OUT(RadiusOut, 1, 1, 11)
D1_GAPS(RangeIn, 10, 2, 2, 2)		D1_IN(RangeIn, 3, 8)
D1_GAPS(RotateOut, 0, 1)			D1_OUT(RotateOut, 1, 1, 11)
D1_GAPS(PolarOut, 4, 0, 1)			D1_OUT(PolarOut, 1, 2, 19)
D1_GAPS(VectorIn, 11, 2, 1)			D1_IN(VectorIn, 2, 6)		// $0D and $03
D1_GAPS(VectorOut, 3, 3, 1)			D1_OUT(VectorOut, 1, 2, 13)
D1_GAPS(DotIn, 13, 2, 2)			D1_IN(DotIn, 2, 6)			// $28 and $0B
D1_GAPS(GyrateIn, 12, 0, 0, 0, 0)	D1_IN(GyrateIn, 5, 0)
D1_GAPS(GyrateOut, 0, 2, 1)			D1_OUT(GyrateOut, 1, 2, 19)
D1_GAPS(ParameterIn, 9, 0, 0, 1, 1, 2)	D1_IN(ParameterIn, 6, 0)
D1_GAPS(ParameterOut, 0, 8, 3, 2)	D1_OUT(ParameterOut, 1, 3, 25)
D1_GAPS(ProjectIn, 10, 2, 2)		D1_IN(ProjectIn, 2, 6)
D1_GAPS(ProjectOut, 1, 0, 1)		D1_OUT(ProjectOut, 1, 2, 13)
D1_GAPS(TargetIn, 9, 2)				D1_IN(TargetIn, 1, 4)
D1_GAPS(TargetOut, 2, 1)			D1_OUT(TargetOut, 1, 1, 9)

// gaps that take a sine's cost from an angle read two words earlier
uint32 RotateInGap (int32 k)
{
	return (k == 0 ? 10 : k == 1 ? 1 : TriangleCost(s.in[0]) + 3);
}
D1_IN(RotateIn, 2, 6)

uint32 PolarInGap (int32 k)
{
	static const uint8	g[5] = { 11, 1, 0, 0, 0 };
	return (k < 5 ? g[k] : 8 + TriangleCost(s.in[0]) + TriangleCost(s.in[1]));
}
D1_IN(PolarIn, 5, 12)

uint32 AttitudeInGap (int32 k)
{
	static const uint8	g[3] = { 11, 2, 2 };
	return (k < 3 ? g[k] : 4 + TriangleCost(s.in[1]));
}
D1_IN(AttitudeIn, 3, 8)


bool Multiply (void)		// $00: (A * B) >> 15; $20 also sets bit 0 (what's left of the command)
{
	NT_BEGIN
	NT_SR(1, 0x8000);
	s.i = 0; NT_GETS(2, MultiplyIn);
	NT_BURN(MultiplyInGap(1)); NT_WAIT(4); NT_READNF(5, s.in[1]);
	NT_BURN(1); NT_WRITE(6, (int16) (((int32) s.in[0] * s.in[1]) >> 15) | (s.cmd >> 5 & 1));
	NT_EPILOGUE(7);
	NT_END
}

bool Inverse (void)			// $10: 1 / (C * 2^E) as a coefficient and an exponent
{
	NT_BEGIN
	NT_SR(1, 0x8000);
	s.i = 0; NT_GETS(2, InverseIn);
	NT_BURN(InverseInGap(1)); NT_WAIT(4); NT_READNF(5, s.in[1]);
	{
		const SubOut	n = SubNorm(s.in[0], s.in[1]), v = SubInv(n.a, n.b);
		s.out[0] = v.a;
		s.out[1] = v.b;
		s.i = (int32) (3 + n.cost + v.cost);
	}
	NT_BURN(s.i - 1); NT_WRITE(6, s.out[0]);
	s.i = 0; NT_PUTS(7, InverseOut);
	NT_EPILOGUE(9);
	NT_END
}

bool Triangle (void)		// $04: radius * sin, radius * cos
{
	NT_BEGIN
	NT_SR(1, 0x8000);
	NT_BURN(10); NT_WAIT(2); NT_READ(3, s.in[0]);
	NT_BURN(TriangleCost(s.in[0])); NT_WAIT(4); NT_READNF(5, s.in[1]);
	s.out[0] = Mul(Sin(s.in[0]), s.in[1]);
	s.out[1] = Mul(Cos(s.in[0]), s.in[1]);
	NT_BURN(1); NT_WRITE(6, s.out[0]);
	s.i = 0; NT_PUTS(7, TriangleOut);
	NT_EPILOGUE(9);
	NT_END
}

bool Radius (void)			// $08: 2 * (X^2 + Y^2 + Z^2) as two words
{
	NT_BEGIN
	NT_SR(1, 0x8000);
	s.i = 0; NT_GETS(2, RadiusIn);
	NT_BURN(RadiusInGap(2)); NT_WAIT(6); NT_READNF(7, s.in[2]);
	{
		const uint32	size = 2u * ((uint32) (s.in[0] * s.in[0]) + (uint32) (s.in[1] * s.in[1]) + (uint32) (s.in[2] * s.in[2]));
		s.out[0] = (int16) size;
		s.out[1] = (int16) (size >> 16);
	}
	NT_BURN(3); NT_WRITE(8, s.out[0]);
	s.i = 0; NT_PUTS(9, RadiusOut);
	NT_EPILOGUE(11);
	NT_END
}

bool Range (void)			// $18: (X^2 + Y^2 + Z^2 - R^2) >> 15; $38 one more
{
	NT_BEGIN
	NT_SR(1, 0x8000);
	s.i = 0; NT_GETS(2, RangeIn);
	NT_BURN(RangeInGap(3)); NT_WAIT(8); NT_READNF(9, s.in[3]);
	{
		const uint32	sq = (uint32) (s.in[0] * s.in[0]) + (uint32) (s.in[1] * s.in[1]) + (uint32) (s.in[2] * s.in[2]) - (uint32) (s.in[3] * s.in[3]);
		s.out[0] = (int16) ((int32) sq >> 15);
	}
	if (s.cmd & 0x20)
		s.out[0]++;
	NT_BURN(3); NT_WRITE(10, s.out[0]);
	NT_EPILOGUE(11);
	NT_END
}

bool Rotate (void)			// $0C: (X, Y) turned by A
{
	NT_BEGIN
	NT_SR(1, 0x8000);
	s.i = 0; NT_GETS(2, RotateIn);
	NT_BURN(RotateInGap(2)); NT_WAIT(6); NT_READNF(7, s.in[2]);
	s.out[0] = (int16) (Mul(s.in[2], Sin(s.in[0])) + Mul(s.in[1], Cos(s.in[0])));
	s.out[1] = (int16) (Mul(s.in[2], Cos(s.in[0])) - Mul(s.in[1], Sin(s.in[0])));
	ram[0x40] = (uint16) s.in[1];
	NT_BURN(1); NT_WRITE(8, s.out[0]);
	s.i = 0; NT_PUTS(9, RotateOut);
	NT_EPILOGUE(11);
	NT_END
}

bool Polar (void)			// $1C: (X, Y, Z) turned about Z, then Y, then X
{
	NT_BEGIN
	NT_SR(1, 0x8000);
	s.i = 0; NT_GETS(2, PolarIn);						// about Z, Y and X, then X and Y
	NT_BURN(PolarInGap(5)); NT_WAIT(12); NT_READNF(13, s.in[5]);	// Z
	{
		const int16	x1 = (int16) (Mul(s.in[4], Sin(s.in[0])) + Mul(s.in[3], Cos(s.in[0])));
		const int16	y1 = (int16) (Mul(s.in[4], Cos(s.in[0])) - Mul(s.in[3], Sin(s.in[0])));
		const int16	z2 = (int16) (Mul(x1, Sin(s.in[1])) + Mul(s.in[5], Cos(s.in[1])));
		const int16	x2 = (int16) (Mul(x1, Cos(s.in[1])) - Mul(s.in[5], Sin(s.in[1])));
		s.out[0] = x2;
		s.out[1] = (int16) (Mul(z2, Sin(s.in[2])) + Mul(y1, Cos(s.in[2])));
		s.out[2] = (int16) (Mul(z2, Cos(s.in[2])) - Mul(y1, Sin(s.in[2])));
		ram[0x40] = (uint16) x2; ram[0x41] = (uint16) y1;
		ram[0x42] = (uint16) z2; ram[0x43] = (uint16) s.in[2];
	}
	NT_BURN(6 + TriangleCost(s.in[2])); NT_WRITE(14, s.out[0]);
	s.i = 0; NT_PUTS(15, PolarOut);
	NT_EPILOGUE(19);
	NT_END
}

bool Distance (void)		// $28: |(X, Y, Z)|
{
	NT_BEGIN
	NT_SR(1, 0x8000);
	s.i = 0; NT_GETS(2, DotIn);
	NT_BURN(DotInGap(2)); NT_WAIT(6); NT_READNF(7, s.in[2]);
	{
		const uint32	sq = 2u * ((uint32) (s.in[0] * s.in[0]) + (uint32) (s.in[1] * s.in[1]) + (uint32) (s.in[2] * s.in[2]));
		uint32			cost;
		s.out[0] = DistanceOf(sq, &cost);
		s.i = (int32) cost;
	}
	NT_BURN(s.i - 1); NT_WRITE(8, s.out[0]);
	NT_EPILOGUE(9);
	NT_END
}

// The matrices, picked by command bits 4-5. Row r is at the base's high nibble ^ r, column c
// at its low nibble - c (wrapping in the nibble). Index 3's base lands in scratch RAM, where the
// multiplier's RAM port (address | $40) reads other scratch words.
const uint8	MatrixBase[4] = { 0xff, 0xfc, 0xf9, 0x01 };
const uint8	ScalarBase[4] = { 0xff, 0xfc, 0xf9, 0xff };

inline uint8 Elem (uint8 base, int row, int col)
{
	return ((uint8) (((base ^ (row << 4)) & 0xf0) | ((base - col) & 0x0f)));
}

// through the RAM port, or read plain
inline int16 MatK (uint8 base, int row, int col)
{
	return ((int16) ram[Elem(base, row, col) | 0x40]);
}

inline int16 MatP (uint8 base, int row, int col)
{
	return ((int16) ram[Elem(base, row, col)]);
}

bool Attitude (void)		// $01/$11/$21: rotation about Z, then Y, then X, scaled by m/2
{
	NT_BEGIN
	NT_SR(1, 0x8000);
	s.i = 0; NT_GETS(2, AttitudeIn);					// m, about Z, about Y
	NT_BURN(AttitudeInGap(3)); NT_WAIT(8); NT_READNF(9, s.in[3]);	// about X
	{
		const uint8	b = MatrixBase[s.cmd >> 4 & 3];
		const int16	m = s.in[0] >> 1;
		const int16	sz = Sin(s.in[1]), cz = Cos(s.in[1]), sy = Sin(s.in[2]), cy = Cos(s.in[2]);
		const int16	sx = Sin(s.in[3]), cx = Cos(s.in[3]);
		// m cos Z and m sin Z pass through the matrix's own slots and back through the RAM
		// port, where index 3 finds the scratch word at $41 (and $4F) instead
		const bool	scratch = b == 0x01;
		const int16	r41 = (int16) ram[0x41];
		const int16	mcz = scratch ? Mul(r41, cz) : Mul(m, cz);
		const int16	msz = scratch ? Mul(r41, sz) : Mul(m, sz);
		const int16	mczx = scratch ? Mul(r41, sx) : Mul(mcz, sx);
		int16	v[3][3];
		v[0][0] = scratch ? Mul(r41, cy) : Mul(mcz, cy);
		v[0][1] = (int16) -Mul(msz, cy);
		v[0][2] = scratch ? Mul((int16) ram[0x4f], sy) : Mul(m, sy);
		v[1][0] = (int16) (Mul(msz, cx) + Mul(mczx, sy));
		v[1][1] = (int16) (Mul(mcz, cx) - Mul(Mul(msz, sx), sy));
		v[1][2] = (int16) -Mul(Mul(m, sx), cy);
		v[2][0] = (int16) (Mul(msz, sx) - Mul(Mul(mcz, cx), sy));
		v[2][1] = (int16) (mczx + Mul(Mul(msz, cx), sy));
		v[2][2] = Mul(Mul(m, cx), cy);
		for (int r = 0; r < 3; r++)
			for (int c = 0; c < 3; c++)
				ram[Elem(b, r, c)] = (uint16) v[r][c];
		ram[(b & 0xf0) ^ 0xf0] = b;		// where it keeps the base across its sin/cos calls
	}
	NT_BURN(40 + TriangleCost(s.in[3]) + TriangleCost(s.in[2]) - 1); NT_SR(10, 0x0400);
	NT_WRITE(11, 0x0080);
	NT_END
}

bool Objective (void)		// $0D/$1D/$2D: (F, L, U) = matrix * (X, Y, Z)
{
	NT_BEGIN
	NT_SR(1, 0x8000);
	s.i = 0; NT_GETS(2, VectorIn);
	NT_BURN(VectorInGap(2)); NT_WAIT(6); NT_READNF(7, s.in[2]);
	{
		const uint8	b = MatrixBase[s.cmd >> 4 & 3];
		const int16	x = s.in[0], y = s.in[1], z = s.in[2];
		s.out[0] = (int16) (Mul(x, MatK(b, 0, 0)) + Mul(y, MatK(b, 0, 1)) + Mul(z, MatK(b, 0, 2)));
		s.out[1] = (int16) (Mul(x, MatK(b, 1, 0)) + Mul(y, MatK(b, 1, 1)) + Mul(z, MatP(b, 1, 2)));
		s.out[2] = (int16) (Mul(x, MatP(b, 2, 0)) + Mul(y, MatK(b, 2, 1)) + Mul(z, MatK(b, 2, 2)));
	}
	NT_BURN(2); NT_WRITE(8, s.out[0]);
	s.i = 0; NT_PUTS(9, VectorOut);
	NT_EPILOGUE(13);
	NT_END
}

bool Subjective (void)		// $03/$13/$23: (X, Y, Z) = transposed matrix * (F, L, U)
{
	NT_BEGIN
	NT_SR(1, 0x8000);
	s.i = 0; NT_GETS(2, VectorIn);
	NT_BURN(VectorInGap(2)); NT_WAIT(6); NT_READNF(7, s.in[2]);
	{
		const uint8	b = MatrixBase[s.cmd >> 4 & 3];
		const int16	f = s.in[0], l = s.in[1], u = s.in[2];
		s.out[0] = (int16) (Mul(f, MatK(b, 0, 0)) + Mul(l, MatK(b, 1, 0)) + Mul(u, MatK(b, 2, 0)));
		s.out[1] = (int16) (Mul(f, MatK(b, 0, 1)) + Mul(l, MatK(b, 1, 1)) + Mul(u, MatP(b, 2, 1)));
		s.out[2] = (int16) (Mul(f, MatP(b, 0, 2)) + Mul(l, MatK(b, 1, 2)) + Mul(u, MatK(b, 2, 2)));
	}
	NT_BURN(2); NT_WRITE(8, s.out[0]);
	s.i = 0; NT_PUTS(9, VectorOut);
	NT_EPILOGUE(13);
	NT_END
}

bool Scalar (void)			// $0B/$1B/$2B: the matrix's first row . (X, Y, Z), rounded once
{
	NT_BEGIN
	NT_SR(1, 0x8000);
	s.i = 0; NT_GETS(2, DotIn);
	NT_BURN(DotInGap(2)); NT_WAIT(6); NT_READNF(7, s.in[2]);
	{
		const uint8	b = ScalarBase[s.cmd >> 4 & 3];
		// the products' double words add up in 32 bits; the result is the high word
		uint32	acc = 0;
		for (int c = 0; c < 3; c++)
			acc += (uint32) ((int32) s.in[c] * MatK(b, 0, c)) << 1;
		s.out[0] = (int16) (acc >> 16);
	}
	NT_BURN(2); NT_WRITE(8, s.out[0]);
	NT_EPILOGUE(9);
	NT_END
}

bool Gyrate (void)			// $14: (Zr, Xr, Yr) after turning by (U, F, L) in the body's own frame
{
	NT_BEGIN
	NT_SR(1, 0x8000);
	s.i = 0; NT_GETS(2, GyrateIn);						// Zr, Xr, Yr, U, F
	{
		const int16	zr = s.in[0], xr = s.in[1], yr = s.in[2], u = s.in[3], f = s.in[4];
		// $34 takes Xr's sin and cos with bit 0 set (what's left of the command)
		const SubOut	scx = SubSinCos((int16) (xr | (s.cmd >> 5 & 1))), scy = SubSinCos(yr);
		// 1 / cos Xr and tan Xr as mantissas with exponents
		const SubOut	nc = SubNorm(scx.a, 0x10), sec = SubInv(nc.a, nc.b);
		const SubOut	ns = SubNorm(scx.b, sec.b);
		const int16		tan = Mul(sec.a, ns.a);
		// U cos Y -/+ F sin Y as double words
		const int32		pu = (int32) u * scy.a, pf = (int32) f * scy.b;
		const uint32	d1 = (uint32) (pu - pf) << 1, d2 = (uint32) (pu + pf) << 1;
		const SubOut	n1 = SubNormD((int16) (d1 >> 16), (int16) d1), z = SubDenorm(Mul(sec.a, n1.a), (int16) (n1.b + sec.b));
		const SubOut	n2 = SubNormD((int16) (d2 >> 16), (int16) d2), y = SubDenorm((int16) -Mul(tan, n2.a), (int16) (n2.b + ns.b));
		s.out[0] = (int16) (zr + z.a);
		s.out[1] = (int16) (xr + Mul(u, scy.b) + Mul(f, scy.a));
		s.out[2] = (int16) (yr + y.a);		// + L, once read
		s.i = (int32) (42 + scx.cost + nc.cost + sec.cost + ns.cost + scy.cost + n1.cost + z.cost + n2.cost + y.cost);
		// the angles, U and F, and its working values
		ram[0x41] = (uint16) u; ram[0x42] = (uint16) f;
		ram[0x00] = (uint16) sec.b; ram[0x40] = (uint16) sec.a; ram[0x10] = (uint16) ns.b; ram[0x50] = (uint16) tan;
		ram[0x02] = (uint16) ((uint32) pu << 1); ram[0x01] = (uint16) d2;
		ram[0xf0] = (uint16) s.out[0]; ram[0xe0] = (uint16) s.out[1];
	}
	NT_BURN(s.i); NT_WAIT(12); NT_READNF(13, s.in[5]);	// L
	s.out[2] = (int16) (s.out[2] + s.in[5]);
	ram[0xd0] = (uint16) s.out[2];
	NT_WRITE(14, s.out[0]);
	s.i = 0; NT_PUTS(15, GyrateOut);
	NT_EPILOGUE(19);
	NT_END
}

// $02 from its first read of Azs: the centre of projection, the angles' sin and cos, the steepest zenith
// angle the view plane allows, and the instructions to the next read of Azs
struct ParamGeo
{
	int16	cx0, cy0, cz0, sa, ca, mx;
	int32	cost;
};

ParamGeo ParamFrame (bool store)
{
	const int16	fx = s.in[0], fy = s.in[1], fz = s.in[2], lfe = s.in[3], les = s.in[4], azs = s.in[6];
	const SubOut	sca = SubSinCos(s.in[5]), scz = SubSinCos(azs);
	const int16	ca = sca.a, sa = sca.b, nsa = NegSat(sa), cz = scz.a, sz = scz.b, nsz = NegSat(sz);
	// the screen's normal, horizontal and vertical vectors
	const int16	nx = Mul(sz, nsa), ny = Mul(sz, ca), nz = Mul(cz, 0x7fff);
	// the centre of projection, and the eye G = centre - Les * normal, with its sign words
	const int16	cx0 = (int16) (fx + Mul(nx, lfe)), cy0 = (int16) (fy + Mul(ny, lfe)), cz0 = (int16) (fz + Mul(nz, lfe));
	// the view plane, and the steepest zenith angle it allows
	const SubOut	vp = SubNorm(cz0, 15);
	const SubOut	mn = SubNorm((int16) (0x1200 + vp.b), -11), mi = SubInv(mn.a, mn.b), mx = SubDenorm(mi.a, mi.b);
	if (store)
	{
		ram[0x7a] = (uint16) nx; ram[0x6a] = (uint16) ny; ram[0x5a] = (uint16) nz;
		ram[0x7c] = (uint16) Mul(0x7fff, ca); ram[0x6c] = (uint16) Mul(0x7fff, sa); ram[0x5c] = 0;
		ram[0x7b] = (uint16) Mul(cz, nsa); ram[0x6b] = (uint16) Mul(cz, ca); ram[0x5b] = (uint16) Mul(nsz, 0x7fff);
		ram[0x6e] = (uint16) ca; ram[0x6f] = (uint16) sa; ram[0x7e] = (uint16) nsa; ram[0x7f] = (uint16) ca;
		ram[0x49] = (uint16) sz;
		// what's left of the command lands here; $06 adds $4E above Les
		ram[0x4e] = ram[0x4f] = (uint16) (s.cmd >> 4 & 3);
		ram[0x5f] = 0;
		const int16	gx = (int16) (cx0 - Mul(nx, les)), gy = (int16) (cy0 - Mul(ny, les)), gz = (int16) (cz0 - Mul(nz, les));
		ram[0xbb] = (uint16) gx; ram[0xab] = (uint16) gy; ram[0x9b] = (uint16) gz;
		ram[0x3b] = (uint16) Mul(gx, RomK(0x329)); ram[0x2b] = (uint16) Mul(gy, RomK(0x329)); ram[0x1b] = (uint16) Mul(gz, RomK(0x329));
		ram[0xce] = (uint16) les;
		ram[0x4c] = (uint16) vp.a; ram[0xcc] = (uint16) vp.b;
	}
	const ParamGeo	g = { cx0, cy0, cz0, sa, ca, mx.a, (int32) (134 + scz.cost + (sz == -32768 ? 1 : 0) + 1 + vp.cost + mn.cost + mi.cost + mx.cost) };
	return (g);
}

// |Azs| and the clip go through the saturation value: 1 - 2^-15 for a negative Azs, else -1
inline int16 SatSign (int16 azs)
{
	return (azs < 0 ? (int16) 0x7fff : (int16) -32768);
}

// how far Azs is past the steepest angle: clipped when not negative
inline int16 ParamClip (int16 mx, int16 azs)
{
	return ((int16) ((int16) -Mul(azs, SatSign(azs)) - mx));
}

bool Parameter (void)		// $02: the projection: eye, view plane and angles; Vof, Vva, Cx, Cy
{
	NT_BEGIN
	NT_SR(1, 0x8000);
	s.i = 0; NT_GETS(2, ParameterIn);					// Fx, Fy, Fz, Lfe, Les, Aas
	{
		const SubOut	sca = SubSinCos(s.in[5]);
		s.i = (int32) (18 + sca.cost + (sca.b == -32768 ? 1 : 0));
	}
	NT_BURN(s.i); NT_WAIT(14); NT_READNF(15, s.in[6]);	// Azs
	s.i = ParamFrame(true).cost;
	// Azs is read twice more, straight from DR: a CPU byte in between lands in the later reads
	NT_BURN(s.i - 1); NT_READNF(16, s.in[7]);			// Azs again, for the clip
	s.out[4] = ParamClip(ParamFrame(false).mx, s.in[7]) >= 0;
	if (!s.out[4])
	{
		NT_BURN(5); NT_READNF(17, s.in[7]);				// and once more, unclipped
	}
	{
		const ParamGeo	f = ParamFrame(false);
		const int16	les = s.in[4], cx0 = f.cx0, cy0 = f.cy0, cz0 = f.cz0, sa = f.sa, ca = f.ca;
		const bool	clip = s.out[4] != 0;
		// clipped, the angle comes from the clip's read of Azs
		const int16	sg = SatSign(s.in[7]), d = ParamClip(f.mx, s.in[7]);
		const int16	az = clip ? (int16) -Mul(f.mx, sg) : s.in[7];
		const int16	aux = clip ? (int16) ((Mul(d, sg) << 2) | 3) : 0;
		ram[0x40] = (uint16) aux;
		const SubOut	sc = SubSinCos(az);
		const SubOut	n1 = SubNorm(sc.a, 0), c1 = SubInv(n1.a, n1.b);
		ram[0x4a] = (uint16) c1.a; ram[0xca] = (uint16) c1.b;
		// past the clip, polynomials in aux correct Vof and cos
		const int16	vof = (int16) -Mul(les, Mul(aux, (int16) (RomK(0x327) + Mul(aux, Mul(aux, RomK(0x328))))));
		const int16	a2 = Mul(aux, aux);
		const int16	cc = (int16) (sc.a + Mul(sc.a, Mul(a2, (int16) (RomK(0x325) + Mul(a2, RomK(0x324))))));
		const int16	voff = Mul(les, cc);
		ram[0xc8] = (uint16) voff;
		const SubOut	n2 = SubNorm(cc, 0), c2 = SubInv(n2.a, n2.b);
		ram[0x4b] = (uint16) c2.a; ram[0xcb] = (uint16) c2.b;
		const SubOut	n3 = SubNorm(sc.b, -15), cs = SubInv(n3.a, n3.b);
		const SubOut	n4 = SubNorm(voff, cs.b), vva = SubDenorm((int16) -Mul(cs.a, n4.a), n4.b);
		const SubOut	vz = SubNorm(cz0, 15), dc = SubDenorm(Mul(c1.a, vz.a), (int16) (vz.b + c1.b));
		const int16	t = Mul(sc.b, dc.a);
		s.out[0] = vof;
		s.out[1] = vva.a;
		s.out[2] = (int16) (Mul(sa, t) + cx0);
		s.out[3] = (int16) (cy0 - Mul(ca, t));
		ram[0xb9] = (uint16) s.out[2]; ram[0xa9] = (uint16) s.out[3];
		s.out[5] = (int16) ((clip ? 70 : 64) + sc.cost + n1.cost + c1.cost + n2.cost + c2.cost + n3.cost + cs.cost + n4.cost + vva.cost + 1 + vz.cost + dc.cost);
	}
	NT_BURN(s.out[5] - 1); NT_WRITE(18, s.out[0]);
	s.i = 0; NT_PUTS(19, ParameterOut);
	NT_BURN(ParameterOutGap(3)); NT_XWAIT(25, 4, AtAccess, EpilogueStep); NT_SR(26, 0x0400);
	NT_WRITE(27, 0x0080);
	NT_END
}

bool Project (void)			// $06: (X, Y, Z) onto the screen $02 set up: H, V and the scale M
{
	NT_BEGIN
	NT_SR(1, 0x8000);
	s.i = 0; NT_GETS(2, ProjectIn);
	NT_BURN(ProjectInGap(2)); NT_WAIT(6); NT_READNF(7, s.in[2]);
	{
		static const uint8	g[3] = { 0xbb, 0xab, 0x9b }, gs[3] = { 0x3b, 0x2b, 0x1b };
		uint32	cost = 104;
		// P = (X, Y, Z) - G in 32 bits, each normalized and halved
		int16	pm[3], pe[3];
		for (int i = 0; i < 3; i++)
		{
			const int16		v = s.in[i];
			// X's sign word picks up what's left of the command
			const uint16	sw = (uint16) (Mul(v, 1) | (i ? 0 : s.cmd >> 4 & 3));
			const uint32	d = ((uint32) sw << 16 | (uint16) v) - ((uint32) ram[gs[i]] << 16 | ram[g[i]]);
			const SubOut	n = SubNormD((int16) (d >> 16), (int16) d);
			pm[i] = n.a >> 1;
			pe[i] = (int16) (n.b + 1);
			cost += n.cost;
		}
		// all to the largest exponent
		int16	em;
		if (pe[0] >= pe[1])
		{
			em = pe[0] >= pe[2] ? pe[0] : pe[2];
			cost += 3 + (pe[0] >= pe[2] ? 2 : 3);
		}
		else
		{
			em = (int16) (pe[1] - pe[2]) >= 0 ? pe[1] : pe[2];
			cost += 5;
		}
		int16	q[3];
		for (int i = 0; i < 3; i++)
			q[i] = Mul(pm[i], (int16) rom[(0x31 + em - pe[i]) & 0x3ff]);
		// dotted with the screen's horizontal, vertical and normal vectors
		const int16	ch = (int16) (Mul((int16) ram[0x7c], q[0]) + Mul((int16) ram[0x6c], q[1]) + Mul((int16) ram[0x5c], q[2]));
		const int16	cv = (int16) (Mul((int16) ram[0x7b], q[0]) + Mul((int16) ram[0x6b], q[1]) + Mul((int16) ram[0x5b], q[2]));
		const int16	cn = (int16) -(Mul((int16) ram[0x7a], q[0]) + Mul((int16) ram[0x6a], q[1]) + Mul((int16) ram[0x5a], q[2]));
		// Les plus the depth in 32 bits; a high word that overflows saturates
		const SubOut	dd = SubDenormD(cn, em);
		const uint32	lo = (uint32) (uint16) dd.b + ram[0xce];
		const uint16	dh = (uint16) dd.a, pw = ram[0x4e], hv = (uint16) (dh + pw + (lo >> 16));
		const bool		ovf = ((dh ^ hv) & (pw ^ hv)) >> 15;
		const int16		hz = ovf ? ((int16) hv < 0 ? (int16) 0x7fff : (int16) -32768) : (int16) hv;
		cost += ovf;
		// its inverse, times Les: the scale
		const SubOut	nz = SubNormD(hz, (int16) lo), iv = SubInv(nz.a, nz.b), nl = SubNorm((int16) ram[0xce], 15);
		const int16	scale = Mul(iv.a, nl.a), es = (int16) (nl.b + iv.b);
		const SubOut	m = SubDenorm(scale, (int16) (es + 8)), v = SubDenorm(Mul(cv, scale), (int16) (es + em)), h = SubDenorm(Mul(ch, scale), (int16) (es + em));
		s.out[0] = h.a;
		s.out[1] = v.a;
		s.out[2] = m.a;
		s.i = (int32) (cost + 18 + dd.cost + nz.cost + iv.cost + 1 + nl.cost + m.cost + v.cost + h.cost);
	}
	NT_BURN(s.i - 1); NT_WRITE(8, s.out[0]);
	s.i = 0; NT_PUTS(9, ProjectOut);
	NT_EPILOGUE(13);
	NT_END
}

bool Target (void)			// $0E: the ground point (X, Y) under screen position (H, V)
{
	NT_BEGIN
	NT_SR(1, 0x8000);
	s.i = 0; NT_GETS(2, TargetIn);
	NT_BURN(TargetInGap(1)); NT_WAIT(4); NT_READNF(5, s.in[1]);
	{
		// H and V in 8.8; what's left of the command adds into H
		const int16	h = (int16) ((s.cmd >> 4 & 3) + (int16) (s.in[0] << 8)), v = (int16) (s.in[1] << 8);
		const SubOut	n = SubNorm((int16) (Mul((int16) ram[0x49], s.in[1]) + (int16) ram[0xc8]), 8), iv = SubInv(n.a, n.b);
		const int16	c1 = Mul((int16) ram[0x4c], iv.a), e1 = (int16) (iv.b + (int16) ram[0xcc]);
		const SubOut	dh = SubDenorm(c1, e1), dv = SubDenorm(Mul((int16) ram[0x4a], c1), (int16) (e1 + (int16) ram[0xca]));
		const int16	th = Mul(h, dh.a), tv = Mul(v, dv.a);
		s.out[0] = (int16) ((int16) ram[0xb9] + Mul((int16) ram[0x7f], th) + Mul((int16) ram[0x7e], tv));
		s.out[1] = (int16) ((int16) ram[0xa9] - Mul((int16) ram[0x6f], th) + Mul((int16) ram[0x6e], tv));
		s.i = (int32) (35 + n.cost + iv.cost + dh.cost + dv.cost);
	}
	NT_BURN(s.i - 1); NT_WRITE(6, s.out[0]);
	s.i = 0; NT_PUTS(7, TargetOut);
	NT_EPILOGUE(9);
	NT_END
}

// One raster line from Vs: A, B, C, D in out[0-3], the instructions to A's write in out[4].
struct RasterWords
{
	int16	a, b, c, d;
	int32	cost;
};

RasterWords RasterCalc (int16 vs)
{
	const SubOut	n = SubNorm((int16) (Mul((int16) ram[0x49], vs) + (int16) ram[0xc8]), 7), iv = SubInv(n.a, n.b);
	const int16	c = Mul((int16) ram[0x4c], iv.a), e = (int16) (iv.b + (int16) ram[0xcc]);
	const SubOut	d1 = SubDenorm(c, e), d2 = SubDenorm(Mul((int16) ram[0x4b], c), (int16) (e + (int16) ram[0xcb]));
	const RasterWords	w = { Mul((int16) ram[0x7f], d1.a), Mul((int16) ram[0x7e], d2.a), Mul((int16) ram[0x6f], d1.a), Mul((int16) ram[0x6e], d2.a),
		(int32) (n.cost + iv.cost + d1.cost + d2.cost) };
	return (w);
}

void RasterOut (const RasterWords &w)
{
	s.out[0] = w.a;
	s.out[1] = w.b;
	s.out[2] = w.c;
	s.out[3] = w.d;
	s.out[4] = (int16) w.cost;
}

void RasterLine (int16 vs)
{
	RasterOut(RasterCalc(vs));
}

// $0A's lanes, wait to wait: B, C and D, each once the CPU took the word before; after D, the check that
// the CPU didn't write over it, then the next line's A
bool RasterStepB (void);
bool RasterStepC (void);
bool RasterStepD (void);
bool RasterStepA (void);

// from D's wait: the check, the next line, its A and the wait for B; that line kept for its step
RasterWords	raster_next;

inline uint32 RasterSpanA (void)
{
	raster_next = RasterCalc((int16) (s.in[0] + 1));
	return ((uint32) (37 + raster_next.cost));
}

// what the CPU first sees from there: A, a step short of the wait, or with D written over, the end's SR
uint32 RasterEffectA (void)
{
	return (*dr == (uint16) s.out[3] ? lanes->span - 1 : 5);
}

bool RasterStepB (void)
{
	*dr = (uint16) s.out[1];
	*sr |= SR_RQM;
	return (LanePark(7, 3, AtAccess, RasterStepC));
}

bool RasterStepC (void)
{
	*dr = (uint16) s.out[2];
	*sr |= SR_RQM;
	return (LanePark(9, 2, AtAccess, RasterStepD));
}

bool RasterStepD (void)
{
	*dr = (uint16) s.out[3];
	*sr |= SR_RQM;
	return (LanePark(11, RasterSpanA(), RasterEffectA, RasterStepA));
}

bool RasterStepA (void)
{
	// written over: the raster ends, the program's way
	if (*dr != (uint16) s.out[3])
		return (false);
	s.in[1] = (int16) *dr;
	s.in[0]++;
	s.i = 34;
	RasterOut(raster_next);
	*dr = (uint16) s.out[0];
	*sr |= SR_RQM;
	return (LanePark(5, 3, AtAccess, RasterStepB));
}

bool Raster (void)			// $0A: A, B, C, D per scanline from Vs up, until the CPU writes over D
{
	NT_BEGIN
	NT_SR(1, 0x8000);
	NT_BURN(10); NT_WAIT(2); NT_READNF(3, s.in[0]);	// Vs
	s.i = 35;
	for (;;)
	{
		RasterLine(s.in[0]);
		NT_BURN(s.i + s.out[4] - 1); NT_WRITE(4, s.out[0]);
		if (!(s.cmd >> 4 & 3))
		{
			NT_BURN(1); NT_XWAIT(5, 3, AtAccess, RasterStepB); NT_WRITE(6, s.out[1]);
			NT_BURN(1); NT_XWAIT(7, 3, AtAccess, RasterStepC); NT_WRITE(8, s.out[2]);
			NT_BURN(1); NT_XWAIT(9, 2, AtAccess, RasterStepD); NT_WRITE(10, s.out[3]);
			NT_XWAIT(11, RasterSpanA(), RasterEffectA, RasterStepA); NT_READNF(12, s.in[1]);
		}
		else
		{
			// $1A-$3A: B, C and D back to back, SR bit 0 up from after A until after D
			NT_BURN(1); NT_SR(13, 0x0001);
			NT_WAIT(14); NT_WRITE(15, s.out[1]);
			NT_BURN(1); NT_WRITE(16, s.out[2]);
			NT_BURN(1); NT_WRITE(17, s.out[3]);
			NT_SR(18, 0x0000);
			NT_READNF(19, s.in[1]);
		}
		if ((uint16) s.in[1] != (uint16) s.out[3])
			break;
		s.in[0]++;
		s.i = 34;
	}
	NT_BURN(2); NT_SR(20, 0x0400);
	NT_WRITE(21, 0x0080);
	NT_END
}

bool MemTest (void)			// $07/$0F: fills RAM, waits on RQM, checks twice; working RAM reports 0
{
	NT_BEGIN
	NT_SR(1, 0x8000);
	NT_BURN(782); NT_WAIT(2);
	for (int i = 0; i < 256; i++)
		ram[i] = (i & 1) ? 0x5555 : 0xaaaa;		// what the second pass leaves
	NT_BURN(3335); NT_WRITE(3, 0);
	NT_EPILOGUE(4);
	NT_END
}

bool RomDump (void)			// $17/$1F: the data ROM's 1024 words ($37/$3F: from the second)
{
	NT_BEGIN
	NT_SR(1, 0x8000);
	NT_BURN(11);
	for (s.i = 0; ; )
	{
		NT_WAIT(2); NT_WRITE(3, rom[((s.cmd >> 5 & 1) + s.i) & 0x3ff]);
		if (++s.i == 1024)
			break;
		NT_BURN(2);
	}
	NT_BURN(2); NT_XWAIT(4, 4, AtAccess, EpilogueStep); NT_SR(5, 0x0400);
	NT_WRITE(6, 0x0080);
	NT_END
}

bool Version (void)			// $27/$2F: once the CPU touches DR, the firmware's version
{
	NT_BEGIN
	NT_SR(1, 0x8000);
	NT_BURN(12); NT_WAIT(2); NT_WRITE(3, s.variant ? 0x0100 : 0x0101);
	NT_EPILOGUE(4);
	NT_END
}

// ---- power-on and the command wait

bool Boot (void)			// $000-$002: 8-bit mode, DR = $80
{
	NT_BEGIN
	NT_BURN(1);
	NT_SR(1, 0x0400);
	NT_WRITE(2, 0x0080);
	NT_END
}

bool Idle (void)			// $003-$006: wait for a command; bits 6-7 send it back to the wait
{
	NT_BEGIN
	for (;;)
	{
		NT_BURN(1); NT_WAIT(1); NT_READ(2, s.in[0]);
		s.cmd = (uint8) s.in[0];
		NT_BURN(1);
		if (!(s.cmd & 0xc0))
			break;
	}
	NT_END
}

bool (*Program (uint8 cmd)) (void)
{
	switch (cmd & 0x3f)
	{
		case 0x00: case 0x20:	return (Multiply);
		case 0x10: case 0x30:	return (Inverse);
		case 0x04: case 0x24:	return (Triangle);
		case 0x08:				return (Radius);
		case 0x18: case 0x38:	return (Range);
		case 0x0c: case 0x2c:	return (Rotate);
		case 0x1c: case 0x3c:	return (Polar);
		case 0x28:				return (Distance);
		case 0x14: case 0x34:	return (Gyrate);
		case 0x02: case 0x12: case 0x22: case 0x32:	return (Parameter);
		case 0x06: case 0x16: case 0x26: case 0x36:	return (Project);
		case 0x0e: case 0x1e: case 0x2e: case 0x3e:	return (Target);
		case 0x0a: case 0x1a: case 0x2a: case 0x3a:	return (Raster);
		case 0x03: case 0x13: case 0x23: case 0x33:	return (Subjective);
		case 0x0b: case 0x1b: case 0x2b: case 0x3b:	return (Scalar);
		case 0x07: case 0x0f:	return (MemTest);
		case 0x17: case 0x1f: case 0x37: case 0x3f:	return (RomDump);
		case 0x27: case 0x2f:	return (Version);
		case 0x09: case 0x0d: case 0x19: case 0x1d: case 0x29: case 0x2d: case 0x39: case 0x3d:	return (Objective);
		default:				return (Attitude);		// $01/$05, $11/$15, $21/$25, $31/$35
	}
}

void ChipReset (bool variant)
{
	BuildDataROM(variant);
}

void ChipLoaded (void)
{
	BuildDataROM(s.variant != 0);
}

}	// namespace

const S9xUPD7725Native	S9xDSP1Native = NT_CHIP;

#ifdef UPD7725N_LAB
// The lab's direct checks of the shared routines.
bool S9xDSP1NLabSub (const char *name, int16 a, int16 b, int16 *ao, int16 *bo, uint32 *cost)
{
	SubOut	o;
	if (!strcmp(name, "norm"))			o = SubNorm(a, b);
	else if (!strcmp(name, "normd"))	o = SubNormD(a, b);
	else if (!strcmp(name, "denorm"))	o = SubDenorm(a, b);
	else if (!strcmp(name, "denormd"))	o = SubDenormD(a, b);
	else if (!strcmp(name, "inv"))		o = SubInv(a, b);
	else if (!strcmp(name, "sqrt"))		o = SubSqrt(a, b);
	else if (!strcmp(name, "sincos"))	o = SubSinCos(a);
	else return (false);
	*ao = o.a; *bo = o.b; *cost = o.cost;
	return (true);
}

int16 S9xDSP1NLabDistance (uint32 sq, uint32 *cost)
{
	return (DistanceOf(sq, cost));
}
#endif
