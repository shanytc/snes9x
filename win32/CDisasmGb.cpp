#include "CDisasmGb.h"
#include <stdio.h>
#include <string.h>

enum
{
	M_NONE = 0,
	M_D8,
	M_D16,
	M_A16,
	M_R8,
	M_LDH_A8,
	M_SP_R8,
	M_CB,
	M_RST,
	M_BAD
};

struct OpInfo
{
	const char *mnem;
	const char *operand;
	uint8_t     mode;
	uint8_t     length;
};

static const OpInfo kBase[256] =
{
	{"NOP",  "",            M_NONE, 1}, {"LD",   "BC,$%04X",    M_D16,  3}, {"LD",   "(BC),A",      M_NONE, 1}, {"INC",  "BC",          M_NONE, 1},
	{"INC",  "B",           M_NONE, 1}, {"DEC",  "B",           M_NONE, 1}, {"LD",   "B,$%02X",     M_D8,   2}, {"RLCA", "",            M_NONE, 1},
	{"LD",   "($%04X),SP",  M_A16,  3}, {"ADD",  "HL,BC",       M_NONE, 1}, {"LD",   "A,(BC)",      M_NONE, 1}, {"DEC",  "BC",          M_NONE, 1},
	{"INC",  "C",           M_NONE, 1}, {"DEC",  "C",           M_NONE, 1}, {"LD",   "C,$%02X",     M_D8,   2}, {"RRCA", "",            M_NONE, 1},

	{"STOP", "$%02X",       M_D8,   2}, {"LD",   "DE,$%04X",    M_D16,  3}, {"LD",   "(DE),A",      M_NONE, 1}, {"INC",  "DE",          M_NONE, 1},
	{"INC",  "D",           M_NONE, 1}, {"DEC",  "D",           M_NONE, 1}, {"LD",   "D,$%02X",     M_D8,   2}, {"RLA",  "",            M_NONE, 1},
	{"JR",   "$%04X",       M_R8,   2}, {"ADD",  "HL,DE",       M_NONE, 1}, {"LD",   "A,(DE)",      M_NONE, 1}, {"DEC",  "DE",          M_NONE, 1},
	{"INC",  "E",           M_NONE, 1}, {"DEC",  "E",           M_NONE, 1}, {"LD",   "E,$%02X",     M_D8,   2}, {"RRA",  "",            M_NONE, 1},

	{"JR",   "NZ,$%04X",    M_R8,   2}, {"LD",   "HL,$%04X",    M_D16,  3}, {"LD",   "(HL+),A",     M_NONE, 1}, {"INC",  "HL",          M_NONE, 1},
	{"INC",  "H",           M_NONE, 1}, {"DEC",  "H",           M_NONE, 1}, {"LD",   "H,$%02X",     M_D8,   2}, {"DAA",  "",            M_NONE, 1},
	{"JR",   "Z,$%04X",     M_R8,   2}, {"ADD",  "HL,HL",       M_NONE, 1}, {"LD",   "A,(HL+)",     M_NONE, 1}, {"DEC",  "HL",          M_NONE, 1},
	{"INC",  "L",           M_NONE, 1}, {"DEC",  "L",           M_NONE, 1}, {"LD",   "L,$%02X",     M_D8,   2}, {"CPL",  "",            M_NONE, 1},

	{"JR",   "NC,$%04X",    M_R8,   2}, {"LD",   "SP,$%04X",    M_D16,  3}, {"LD",   "(HL-),A",     M_NONE, 1}, {"INC",  "SP",          M_NONE, 1},
	{"INC",  "(HL)",        M_NONE, 1}, {"DEC",  "(HL)",        M_NONE, 1}, {"LD",   "(HL),$%02X",  M_D8,   2}, {"SCF",  "",            M_NONE, 1},
	{"JR",   "C,$%04X",     M_R8,   2}, {"ADD",  "HL,SP",       M_NONE, 1}, {"LD",   "A,(HL-)",     M_NONE, 1}, {"DEC",  "SP",          M_NONE, 1},
	{"INC",  "A",           M_NONE, 1}, {"DEC",  "A",           M_NONE, 1}, {"LD",   "A,$%02X",     M_D8,   2}, {"CCF",  "",            M_NONE, 1},

	{"LD",   "B,B",         M_NONE, 1}, {"LD",   "B,C",         M_NONE, 1}, {"LD",   "B,D",         M_NONE, 1}, {"LD",   "B,E",         M_NONE, 1},
	{"LD",   "B,H",         M_NONE, 1}, {"LD",   "B,L",         M_NONE, 1}, {"LD",   "B,(HL)",      M_NONE, 1}, {"LD",   "B,A",         M_NONE, 1},
	{"LD",   "C,B",         M_NONE, 1}, {"LD",   "C,C",         M_NONE, 1}, {"LD",   "C,D",         M_NONE, 1}, {"LD",   "C,E",         M_NONE, 1},
	{"LD",   "C,H",         M_NONE, 1}, {"LD",   "C,L",         M_NONE, 1}, {"LD",   "C,(HL)",      M_NONE, 1}, {"LD",   "C,A",         M_NONE, 1},

	{"LD",   "D,B",         M_NONE, 1}, {"LD",   "D,C",         M_NONE, 1}, {"LD",   "D,D",         M_NONE, 1}, {"LD",   "D,E",         M_NONE, 1},
	{"LD",   "D,H",         M_NONE, 1}, {"LD",   "D,L",         M_NONE, 1}, {"LD",   "D,(HL)",      M_NONE, 1}, {"LD",   "D,A",         M_NONE, 1},
	{"LD",   "E,B",         M_NONE, 1}, {"LD",   "E,C",         M_NONE, 1}, {"LD",   "E,D",         M_NONE, 1}, {"LD",   "E,E",         M_NONE, 1},
	{"LD",   "E,H",         M_NONE, 1}, {"LD",   "E,L",         M_NONE, 1}, {"LD",   "E,(HL)",      M_NONE, 1}, {"LD",   "E,A",         M_NONE, 1},

	{"LD",   "H,B",         M_NONE, 1}, {"LD",   "H,C",         M_NONE, 1}, {"LD",   "H,D",         M_NONE, 1}, {"LD",   "H,E",         M_NONE, 1},
	{"LD",   "H,H",         M_NONE, 1}, {"LD",   "H,L",         M_NONE, 1}, {"LD",   "H,(HL)",      M_NONE, 1}, {"LD",   "H,A",         M_NONE, 1},
	{"LD",   "L,B",         M_NONE, 1}, {"LD",   "L,C",         M_NONE, 1}, {"LD",   "L,D",         M_NONE, 1}, {"LD",   "L,E",         M_NONE, 1},
	{"LD",   "L,H",         M_NONE, 1}, {"LD",   "L,L",         M_NONE, 1}, {"LD",   "L,(HL)",      M_NONE, 1}, {"LD",   "L,A",         M_NONE, 1},

	{"LD",   "(HL),B",      M_NONE, 1}, {"LD",   "(HL),C",      M_NONE, 1}, {"LD",   "(HL),D",      M_NONE, 1}, {"LD",   "(HL),E",      M_NONE, 1},
	{"LD",   "(HL),H",      M_NONE, 1}, {"LD",   "(HL),L",      M_NONE, 1}, {"HALT", "",            M_NONE, 1}, {"LD",   "(HL),A",      M_NONE, 1},
	{"LD",   "A,B",         M_NONE, 1}, {"LD",   "A,C",         M_NONE, 1}, {"LD",   "A,D",         M_NONE, 1}, {"LD",   "A,E",         M_NONE, 1},
	{"LD",   "A,H",         M_NONE, 1}, {"LD",   "A,L",         M_NONE, 1}, {"LD",   "A,(HL)",      M_NONE, 1}, {"LD",   "A,A",         M_NONE, 1},

	{"ADD",  "A,B",         M_NONE, 1}, {"ADD",  "A,C",         M_NONE, 1}, {"ADD",  "A,D",         M_NONE, 1}, {"ADD",  "A,E",         M_NONE, 1},
	{"ADD",  "A,H",         M_NONE, 1}, {"ADD",  "A,L",         M_NONE, 1}, {"ADD",  "A,(HL)",      M_NONE, 1}, {"ADD",  "A,A",         M_NONE, 1},
	{"ADC",  "A,B",         M_NONE, 1}, {"ADC",  "A,C",         M_NONE, 1}, {"ADC",  "A,D",         M_NONE, 1}, {"ADC",  "A,E",         M_NONE, 1},
	{"ADC",  "A,H",         M_NONE, 1}, {"ADC",  "A,L",         M_NONE, 1}, {"ADC",  "A,(HL)",      M_NONE, 1}, {"ADC",  "A,A",         M_NONE, 1},

	{"SUB",  "B",           M_NONE, 1}, {"SUB",  "C",           M_NONE, 1}, {"SUB",  "D",           M_NONE, 1}, {"SUB",  "E",           M_NONE, 1},
	{"SUB",  "H",           M_NONE, 1}, {"SUB",  "L",           M_NONE, 1}, {"SUB",  "(HL)",        M_NONE, 1}, {"SUB",  "A",           M_NONE, 1},
	{"SBC",  "A,B",         M_NONE, 1}, {"SBC",  "A,C",         M_NONE, 1}, {"SBC",  "A,D",         M_NONE, 1}, {"SBC",  "A,E",         M_NONE, 1},
	{"SBC",  "A,H",         M_NONE, 1}, {"SBC",  "A,L",         M_NONE, 1}, {"SBC",  "A,(HL)",      M_NONE, 1}, {"SBC",  "A,A",         M_NONE, 1},

	{"AND",  "B",           M_NONE, 1}, {"AND",  "C",           M_NONE, 1}, {"AND",  "D",           M_NONE, 1}, {"AND",  "E",           M_NONE, 1},
	{"AND",  "H",           M_NONE, 1}, {"AND",  "L",           M_NONE, 1}, {"AND",  "(HL)",        M_NONE, 1}, {"AND",  "A",           M_NONE, 1},
	{"XOR",  "B",           M_NONE, 1}, {"XOR",  "C",           M_NONE, 1}, {"XOR",  "D",           M_NONE, 1}, {"XOR",  "E",           M_NONE, 1},
	{"XOR",  "H",           M_NONE, 1}, {"XOR",  "L",           M_NONE, 1}, {"XOR",  "(HL)",        M_NONE, 1}, {"XOR",  "A",           M_NONE, 1},

	{"OR",   "B",           M_NONE, 1}, {"OR",   "C",           M_NONE, 1}, {"OR",   "D",           M_NONE, 1}, {"OR",   "E",           M_NONE, 1},
	{"OR",   "H",           M_NONE, 1}, {"OR",   "L",           M_NONE, 1}, {"OR",   "(HL)",        M_NONE, 1}, {"OR",   "A",           M_NONE, 1},
	{"CP",   "B",           M_NONE, 1}, {"CP",   "C",           M_NONE, 1}, {"CP",   "D",           M_NONE, 1}, {"CP",   "E",           M_NONE, 1},
	{"CP",   "H",           M_NONE, 1}, {"CP",   "L",           M_NONE, 1}, {"CP",   "(HL)",        M_NONE, 1}, {"CP",   "A",           M_NONE, 1},

	{"RET",  "NZ",          M_NONE, 1}, {"POP",  "BC",          M_NONE, 1}, {"JP",   "NZ,$%04X",    M_A16,  3}, {"JP",   "$%04X",       M_A16,  3},
	{"CALL", "NZ,$%04X",    M_A16,  3}, {"PUSH", "BC",          M_NONE, 1}, {"ADD",  "A,$%02X",     M_D8,   2}, {"RST",  "$00",         M_NONE, 1},
	{"RET",  "Z",           M_NONE, 1}, {"RET",  "",            M_NONE, 1}, {"JP",   "Z,$%04X",     M_A16,  3}, {"",     "",            M_CB,   2},
	{"CALL", "Z,$%04X",     M_A16,  3}, {"CALL", "$%04X",       M_A16,  3}, {"ADC",  "A,$%02X",     M_D8,   2}, {"RST",  "$08",         M_NONE, 1},

	{"RET",  "NC",          M_NONE, 1}, {"POP",  "DE",          M_NONE, 1}, {"JP",   "NC,$%04X",    M_A16,  3}, {"???",  "",            M_BAD,  1},
	{"CALL", "NC,$%04X",    M_A16,  3}, {"PUSH", "DE",          M_NONE, 1}, {"SUB",  "$%02X",       M_D8,   2}, {"RST",  "$10",         M_NONE, 1},
	{"RET",  "C",           M_NONE, 1}, {"RETI", "",            M_NONE, 1}, {"JP",   "C,$%04X",     M_A16,  3}, {"???",  "",            M_BAD,  1},
	{"CALL", "C,$%04X",     M_A16,  3}, {"???",  "",            M_BAD,  1}, {"SBC",  "A,$%02X",     M_D8,   2}, {"RST",  "$18",         M_NONE, 1},

	{"LDH",  "($FF%02X),A", M_LDH_A8, 2}, {"POP",  "HL",        M_NONE, 1}, {"LD",   "($FF00+C),A", M_NONE, 1}, {"???",  "",            M_BAD,  1},
	{"???",  "",            M_BAD,  1}, {"PUSH", "HL",          M_NONE, 1}, {"AND",  "$%02X",       M_D8,   2}, {"RST",  "$20",         M_NONE, 1},
	{"ADD",  "SP,%d",       M_SP_R8, 2}, {"JP",   "HL",         M_NONE, 1}, {"LD",   "($%04X),A",   M_A16,  3}, {"???",  "",            M_BAD,  1},
	{"???",  "",            M_BAD,  1}, {"???",  "",            M_BAD,  1}, {"XOR",  "$%02X",       M_D8,   2}, {"RST",  "$28",         M_NONE, 1},

	{"LDH",  "A,($FF%02X)", M_LDH_A8, 2}, {"POP",  "AF",        M_NONE, 1}, {"LD",   "A,($FF00+C)", M_NONE, 1}, {"DI",   "",            M_NONE, 1},
	{"???",  "",            M_BAD,  1}, {"PUSH", "AF",          M_NONE, 1}, {"OR",   "$%02X",       M_D8,   2}, {"RST",  "$30",         M_NONE, 1},
	{"LD",   "HL,SP+%d",    M_SP_R8, 2}, {"LD",   "SP,HL",      M_NONE, 1}, {"LD",   "A,($%04X)",   M_A16,  3}, {"EI",   "",            M_NONE, 1},
	{"???",  "",            M_BAD,  1}, {"???",  "",            M_BAD,  1}, {"CP",   "$%02X",       M_D8,   2}, {"RST",  "$38",         M_NONE, 1}
};

static const char *kRegNames8[8] = {"B","C","D","E","H","L","(HL)","A"};

static void FormatCb(uint8_t cb, char *mnem, size_t mnem_cap, char *operand, size_t op_cap)
{
	const uint8_t hi  = (cb >> 6) & 3;
	const uint8_t mid = (cb >> 3) & 7;
	const uint8_t lo  = cb & 7;
	const char *r = kRegNames8[lo];

	if (hi == 0)
	{
		static const char *rot[8] = {"RLC","RRC","RL","RR","SLA","SRA","SWAP","SRL"};
		strncpy(mnem, rot[mid], mnem_cap - 1);
		mnem[mnem_cap - 1] = 0;
		snprintf(operand, op_cap, "%s", r);
	}
	else if (hi == 1)
	{
		strncpy(mnem, "BIT", mnem_cap - 1);
		mnem[mnem_cap - 1] = 0;
		snprintf(operand, op_cap, "%u,%s", mid, r);
	}
	else if (hi == 2)
	{
		strncpy(mnem, "RES", mnem_cap - 1);
		mnem[mnem_cap - 1] = 0;
		snprintf(operand, op_cap, "%u,%s", mid, r);
	}
	else
	{
		strncpy(mnem, "SET", mnem_cap - 1);
		mnem[mnem_cap - 1] = 0;
		snprintf(operand, op_cap, "%u,%s", mid, r);
	}
}

uint8_t DisassembleGb(uint16_t pc, ReadGbFn read, DisasmResultGb *out)
{
	const uint8_t opcode = read(pc);
	const uint8_t op1    = read((uint16_t)(pc + 1));
	const uint8_t op2    = read((uint16_t)(pc + 2));

	memset(out, 0, sizeof(*out));
	out->bytes[0] = opcode;
	out->bytes[1] = op1;
	out->bytes[2] = op2;

	const OpInfo &info = kBase[opcode];
	uint8_t length = info.length;

	if (info.mode == M_CB)
	{
		FormatCb(op1, out->mnemonic, sizeof(out->mnemonic),
		         out->operand,  sizeof(out->operand));
		out->length = length;
		return length;
	}

	strncpy(out->mnemonic, info.mnem, sizeof(out->mnemonic) - 1);

	switch (info.mode)
	{
		case M_NONE:
			snprintf(out->operand, sizeof(out->operand), "%s", info.operand);
			break;
		case M_D8:
			snprintf(out->operand, sizeof(out->operand), info.operand, op1);
			break;
		case M_D16:
		case M_A16:
		{
			const uint16_t w = (uint16_t)(((uint16_t)op2 << 8) | op1);
			snprintf(out->operand, sizeof(out->operand), info.operand, w);
			break;
		}
		case M_R8:
		{
			const int8_t  sb  = (int8_t)op1;
			const uint16_t tgt = (uint16_t)(pc + 2 + sb);
			snprintf(out->operand, sizeof(out->operand), info.operand, tgt);
			break;
		}
		case M_LDH_A8:
			snprintf(out->operand, sizeof(out->operand), info.operand, op1);
			break;
		case M_SP_R8:
		{
			const int8_t sb = (int8_t)op1;
			snprintf(out->operand, sizeof(out->operand), info.operand, (int)sb);
			break;
		}
		default:
			out->operand[0] = 0;
			break;
	}

	out->length = length;
	return length;
}
