#ifndef CDISASM_65816_H
#define CDISASM_65816_H

#include <stdint.h>

struct DisasmResult65816
{
	uint8_t length;
	uint8_t bytes[4];
	char    mnemonic[8];
	char    operand[40];
};

typedef uint8_t (*Read65816Fn)(uint32_t addr24);

uint8_t Disassemble65816(uint32_t pc24,
                         bool flag_M,
                         bool flag_X,
                         Read65816Fn read,
                         DisasmResult65816 *out);

#endif
