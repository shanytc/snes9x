#ifndef CDISASM_GB_H
#define CDISASM_GB_H

#include <stdint.h>

struct DisasmResultGb
{
	uint8_t  length;
	uint8_t  bytes[3];
	char     mnemonic[8];
	char     operand[40];
	bool     has_effective;
	uint16_t effective_addr;
	uint8_t  effective_value;
	bool     has_branch;
	uint16_t branch_target;
};

struct GbContext
{
	uint16_t af, bc, de, hl, sp;
};

typedef uint8_t (*ReadGbFn)(uint32_t display_addr);

uint8_t DisassembleGb(uint32_t display_pc,
                      const GbContext *ctx,
                      ReadGbFn read,
                      DisasmResultGb *out);

#endif
