#ifndef CDISASM_GB_H
#define CDISASM_GB_H

#include <stdint.h>

struct DisasmResultGb
{
	uint8_t length;
	uint8_t bytes[3];
	char    mnemonic[8];
	char    operand[40];
};

typedef uint8_t (*ReadGbFn)(uint16_t addr);

uint8_t DisassembleGb(uint16_t pc,
                      ReadGbFn read,
                      DisasmResultGb *out);

#endif
