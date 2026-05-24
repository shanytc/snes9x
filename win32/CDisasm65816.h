#ifndef CDISASM_65816_H
#define CDISASM_65816_H

#include <stdint.h>

struct DisasmResult65816
{
	uint8_t  length;
	uint8_t  bytes[4];
	char     mnemonic[8];
	char     operand[40];
	bool     has_effective;
	uint32_t effective_addr;
	uint8_t  effective_value;
	bool     has_branch;
	uint32_t branch_target;
};

struct Snes65816Context
{
	bool     flag_M;
	bool     flag_X;
	uint16_t D;
	uint16_t X;
	uint16_t Y;
	uint16_t S;
	uint8_t  DB;
};

typedef uint8_t (*Read65816Fn)(uint32_t addr24);

uint8_t Disassemble65816(uint32_t pc24,
                         const Snes65816Context *ctx,
                         Read65816Fn read,
                         DisasmResult65816 *out);

#endif
