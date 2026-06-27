#include "CDebuggerGb.h"
#include "../sgb/sgb.h"

namespace GbBackend {

uint8_t ReadByte(uint32_t display_addr)
{
	return S9xSGBPeekRAByte(display_addr & 0xFFFFu);
}

void GetStatus(GbStatus *out)
{
	S9xSGBGetCpuRegs(&out->pc, &out->sp, &out->af, &out->bc, &out->de, &out->hl,
	                 &out->ime, &out->halted, &out->stopped,
	                 &out->ie,  &out->if_,
	                 &out->t_cycles);
}

uint8_t Disassemble(uint32_t display_pc, DisasmResultGb *out)
{
	GbStatus s = {};
	GetStatus(&s);
	GbContext ctx{};
	ctx.af = s.af;
	ctx.bc = s.bc;
	ctx.de = s.de;
	ctx.hl = s.hl;
	ctx.sp = s.sp;
	return DisassembleGb(display_pc, &ctx, &ReadByte, out);
}

uint32_t CurrentDisplayPC()
{
	GbStatus s = {};
	GetStatus(&s);
	return (uint32_t)s.pc;
}

uint32_t CartSRAMSize()
{
	return (uint32_t)S9xSGBGetSRAMSize();
}

} // namespace GbBackend
