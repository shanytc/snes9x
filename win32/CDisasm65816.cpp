#include "CDisasm65816.h"
#include <stdio.h>
#include <string.h>

static const char *kMnemonics[256] =
{
	"BRK", "ORA", "COP", "ORA", "TSB", "ORA", "ASL", "ORA",
	"PHP", "ORA", "ASL", "PHD", "TSB", "ORA", "ASL", "ORA",
	"BPL", "ORA", "ORA", "ORA", "TRB", "ORA", "ASL", "ORA",
	"CLC", "ORA", "INC", "TCS", "TRB", "ORA", "ASL", "ORA",
	"JSR", "AND", "JSL", "AND", "BIT", "AND", "ROL", "AND",
	"PLP", "AND", "ROL", "PLD", "BIT", "AND", "ROL", "AND",
	"BMI", "AND", "AND", "AND", "BIT", "AND", "ROL", "AND",
	"SEC", "AND", "DEC", "TSC", "BIT", "AND", "ROL", "AND",
	"RTI", "EOR", "WDM", "EOR", "MVP", "EOR", "LSR", "EOR",
	"PHA", "EOR", "LSR", "PHK", "JMP", "EOR", "LSR", "EOR",
	"BVC", "EOR", "EOR", "EOR", "MVN", "EOR", "LSR", "EOR",
	"CLI", "EOR", "PHY", "TCD", "JMP", "EOR", "LSR", "EOR",
	"RTS", "ADC", "PER", "ADC", "STZ", "ADC", "ROR", "ADC",
	"PLA", "ADC", "ROR", "RTL", "JMP", "ADC", "ROR", "ADC",
	"BVS", "ADC", "ADC", "ADC", "STZ", "ADC", "ROR", "ADC",
	"SEI", "ADC", "PLY", "TDC", "JMP", "ADC", "ROR", "ADC",
	"BRA", "STA", "BRL", "STA", "STY", "STA", "STX", "STA",
	"DEY", "BIT", "TXA", "PHB", "STY", "STA", "STX", "STA",
	"BCC", "STA", "STA", "STA", "STY", "STA", "STX", "STA",
	"TYA", "STA", "TXS", "TXY", "STZ", "STA", "STZ", "STA",
	"LDY", "LDA", "LDX", "LDA", "LDY", "LDA", "LDX", "LDA",
	"TAY", "LDA", "TAX", "PLB", "LDY", "LDA", "LDX", "LDA",
	"BCS", "LDA", "LDA", "LDA", "LDY", "LDA", "LDX", "LDA",
	"CLV", "LDA", "TSX", "TYX", "LDY", "LDA", "LDX", "LDA",
	"CPY", "CMP", "REP", "CMP", "CPY", "CMP", "DEC", "CMP",
	"INY", "CMP", "DEX", "WAI", "CPY", "CMP", "DEC", "CMP",
	"BNE", "CMP", "CMP", "CMP", "PEI", "CMP", "DEC", "CMP",
	"CLD", "CMP", "PHX", "STP", "JML", "CMP", "DEC", "CMP",
	"CPX", "SBC", "SEP", "SBC", "CPX", "SBC", "INC", "SBC",
	"INX", "SBC", "NOP", "XBA", "CPX", "SBC", "INC", "SBC",
	"BEQ", "SBC", "SBC", "SBC", "PEA", "SBC", "INC", "SBC",
	"SED", "SBC", "PLX", "XCE", "JSR", "SBC", "INC", "SBC"
};

static const int kAddrModes[256] =
{
	 3, 10,  3, 19,  6,  6,  6, 12,  0,  1, 24,  0, 14, 14, 14, 17,
	 4, 11,  9, 20,  6,  7,  7, 13,  0, 16, 24,  0, 14, 15, 15, 18,
	14, 10, 17, 19,  6,  6,  6, 12,  0,  1, 24,  0, 14, 14, 14, 17,
	 4, 11,  9, 20,  7,  7,  7, 13,  0, 16, 24,  0, 15, 15, 15, 18,
	 0, 10,  3, 19, 25,  6,  6, 12,  0,  1, 24,  0, 14, 14, 14, 17,
	 4, 11,  9, 20, 25,  7,  7, 13,  0, 16,  0,  0, 17, 15, 15, 18,
	 0, 10,  5, 19,  6,  6,  6, 12,  0,  1, 24,  0, 21, 14, 14, 17,
	 4, 11,  9, 20,  7,  7,  7, 13,  0, 16,  0,  0, 23, 15, 15, 18,
	 4, 10,  5, 19,  6,  6,  6, 12,  0,  1,  0,  0, 14, 14, 14, 17,
	 4, 11,  9, 20,  7,  7,  8, 13,  0, 16,  0,  0, 14, 15, 15, 18,
	 2, 10,  2, 19,  6,  6,  6, 12,  0,  1,  0,  0, 14, 14, 14, 17,
	 4, 11,  9, 20,  7,  7,  8, 13,  0, 16,  0,  0, 15, 15, 16, 18,
	 2, 10,  3, 19,  6,  6,  6, 12,  0,  1,  0,  0, 14, 14, 14, 17,
	 4, 11,  9,  9, 27,  7,  7, 13,  0, 16,  0,  0, 22, 15, 15, 18,
	 2, 10,  3, 19,  6,  6,  6, 12,  0,  1,  0,  0, 14, 14, 14, 17,
	 4, 11,  9, 20, 26,  7,  7, 13,  0, 16,  0,  0, 23, 15, 15, 18
};

static void SetEff(DisasmResult65816 *out, uint32_t addr24, Read65816Fn read)
{
	out->has_effective   = true;
	out->effective_addr  = addr24 & 0xFFFFFF;
	out->effective_value = read(out->effective_addr);
}

uint8_t Disassemble65816(uint32_t pc24, const Snes65816Context *ctx, Read65816Fn read, DisasmResult65816 *out)
{
	const uint8_t opcode = read(pc24);
	const uint8_t op1    = read((pc24 + 1) & 0xFFFFFF);
	const uint8_t op2    = read((pc24 + 2) & 0xFFFFFF);
	const uint8_t op3    = read((pc24 + 3) & 0xFFFFFF);

	const uint16_t pc16 = (uint16_t)(pc24 & 0xFFFF);

	const bool flag_M = ctx ? ctx->flag_M : true;
	const bool flag_X = ctx ? ctx->flag_X : true;

	memset(out, 0, sizeof(*out));
	out->bytes[0] = opcode;
	out->bytes[1] = op1;
	out->bytes[2] = op2;
	out->bytes[3] = op3;
	strncpy(out->mnemonic, kMnemonics[opcode], sizeof(out->mnemonic) - 1);

	const int mode = kAddrModes[opcode];
	uint8_t size = 1;
	char *o = out->operand;
	const size_t cap = sizeof(out->operand);

	switch (mode)
	{
		case 0:
			o[0] = 0;
			size = 1;
			break;

		case 1:
			if (!flag_M) { snprintf(o, cap, "#$%02X%02X", op2, op1); size = 3; }
			else         { snprintf(o, cap, "#$%02X", op1);          size = 2; }
			break;

		case 2:
			if (!flag_X) { snprintf(o, cap, "#$%02X%02X", op2, op1); size = 3; }
			else         { snprintf(o, cap, "#$%02X", op1);          size = 2; }
			break;

		case 3:
			snprintf(o, cap, "#$%02X", op1);
			size = 2;
			break;

		case 4:
		{
			int8_t  sb   = (int8_t)op1;
			uint16_t tgt = (uint16_t)(pc16 + 2 + sb);
			snprintf(o, cap, "$%04X", tgt);
			out->has_branch    = true;
			out->branch_target = (pc24 & 0xFF0000) | tgt;
			size = 2;
			break;
		}

		case 5:
		{
			int16_t sw  = (int16_t)(((uint16_t)op2 << 8) | op1);
			uint16_t tgt = (uint16_t)(pc16 + 3 + sw);
			snprintf(o, cap, "$%04X", tgt);
			out->has_branch    = true;
			out->branch_target = (pc24 & 0xFF0000) | tgt;
			size = 3;
			break;
		}

		case 6:
			snprintf(o, cap, "$%02X", op1);
			if (ctx) SetEff(out, (uint16_t)(ctx->D + op1), read);
			size = 2;
			break;

		case 7:
			snprintf(o, cap, "$%02X,x", op1);
			if (ctx) SetEff(out, (uint16_t)(ctx->D + op1 + ctx->X), read);
			size = 2;
			break;

		case 8:
			snprintf(o, cap, "$%02X,y", op1);
			if (ctx) SetEff(out, (uint16_t)(ctx->D + op1 + ctx->Y), read);
			size = 2;
			break;

		case 9:
			snprintf(o, cap, "($%02X)", op1);
			size = 2;
			break;

		case 10:
			snprintf(o, cap, "($%02X,x)", op1);
			size = 2;
			break;

		case 11:
			snprintf(o, cap, "($%02X),y", op1);
			size = 2;
			break;

		case 12:
			snprintf(o, cap, "[$%02X]", op1);
			size = 2;
			break;

		case 13:
			snprintf(o, cap, "[$%02X],y", op1);
			size = 2;
			break;

		case 14:
		{
			const uint16_t a16 = (uint16_t)(((uint16_t)op2 << 8) | op1);
			snprintf(o, cap, "$%02X%02X", op2, op1);
			if (opcode == 0x4C || opcode == 0x20)
			{
				out->has_branch    = true;
				out->branch_target = (pc24 & 0xFF0000) | a16;
			}
			else if (ctx)
			{
				SetEff(out, ((uint32_t)ctx->DB << 16) | a16, read);
			}
			size = 3;
			break;
		}

		case 15:
		{
			const uint16_t a16 = (uint16_t)(((uint16_t)op2 << 8) | op1);
			snprintf(o, cap, "$%02X%02X,x", op2, op1);
			if (ctx) SetEff(out, ((uint32_t)ctx->DB << 16) | (uint16_t)(a16 + ctx->X), read);
			size = 3;
			break;
		}

		case 16:
		{
			const uint16_t a16 = (uint16_t)(((uint16_t)op2 << 8) | op1);
			snprintf(o, cap, "$%02X%02X,y", op2, op1);
			if (ctx) SetEff(out, ((uint32_t)ctx->DB << 16) | (uint16_t)(a16 + ctx->Y), read);
			size = 3;
			break;
		}

		case 17:
		{
			const uint32_t a24 = ((uint32_t)op3 << 16) | ((uint32_t)op2 << 8) | op1;
			snprintf(o, cap, "$%02X%02X%02X", op3, op2, op1);
			if (opcode == 0x5C || opcode == 0x22)
			{
				out->has_branch    = true;
				out->branch_target = a24;
			}
			else if (ctx)
			{
				SetEff(out, a24, read);
			}
			size = 4;
			break;
		}

		case 18:
		{
			const uint32_t a24 = ((uint32_t)op3 << 16) | ((uint32_t)op2 << 8) | op1;
			snprintf(o, cap, "$%02X%02X%02X,x", op3, op2, op1);
			if (ctx) SetEff(out, (a24 + ctx->X) & 0xFFFFFF, read);
			size = 4;
			break;
		}

		case 19:
			snprintf(o, cap, "$%02X,s", op1);
			if (ctx) SetEff(out, (uint16_t)(ctx->S + op1), read);
			size = 2;
			break;

		case 20:
			snprintf(o, cap, "($%02X,s),y", op1);
			size = 2;
			break;

		case 21:
			snprintf(o, cap, "($%02X%02X)", op2, op1);
			size = 3;
			break;

		case 22:
			snprintf(o, cap, "[$%02X%02X]", op2, op1);
			size = 3;
			break;

		case 23:
			snprintf(o, cap, "($%02X%02X,x)", op2, op1);
			size = 3;
			break;

		case 24:
			snprintf(o, cap, "A");
			size = 1;
			break;

		case 25:
			snprintf(o, cap, "%02X %02X", op2, op1);
			size = 3;
			break;

		case 26:
			snprintf(o, cap, "$%02X%02X", op2, op1);
			size = 3;
			break;

		case 27:
			snprintf(o, cap, "($%02X)", op1);
			size = 2;
			break;

		default:
			o[0] = 0;
			size = 1;
			break;
	}

	out->length = size;
	return size;
}
