#ifndef CDEBUGGER_GB_H
#define CDEBUGGER_GB_H

#include <stdint.h>
#include "CDisasmGb.h"

struct GbStatus
{
	uint16_t af, bc, de, hl;
	uint16_t sp, pc;
	uint8_t  ime, halted, stopped;
	uint8_t  ie, if_;
	uint64_t t_cycles;
};

namespace GbBackend
{
	uint8_t ReadByte(uint16_t addr);
	void    GetStatus(GbStatus *out);
	uint8_t Disassemble(uint16_t pc, DisasmResultGb *out);
}

#endif
