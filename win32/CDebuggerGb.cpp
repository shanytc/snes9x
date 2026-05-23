#include "CDebuggerGb.h"
#include "../sgb/sgb.h"

namespace GbBackend {

uint8_t ReadByte(uint16_t addr)
{
	return S9xSGBPeekRAByte((uint32_t)addr);
}

void GetStatus(GbStatus *out)
{
	S9xSGBGetCpuRegs(&out->pc, &out->sp, &out->af, &out->bc, &out->de, &out->hl,
	                 &out->ime, &out->halted, &out->stopped,
	                 &out->ie,  &out->if_,
	                 &out->t_cycles);
}

uint8_t Disassemble(uint16_t pc, DisasmResultGb *out)
{
	return DisassembleGb(pc, &ReadByte, out);
}

} // namespace GbBackend
