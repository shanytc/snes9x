#include "CDebuggerGb.h"
#include "../sgb/sgb.h"

namespace GbBackend {

uint8_t ReadByte(uint32_t display_addr)
{
	const uint8_t  bank = (uint8_t)((display_addr >> 16) & 0xFF);
	const uint16_t cpu  = (uint16_t)(display_addr & 0xFFFF);

	if (bank == 0 && cpu < 0x4000)
		return S9xSGBPeekROMByte((uint32_t)cpu);
	if (bank > 0 && cpu >= 0x4000 && cpu < 0x8000)
		return S9xSGBPeekROMByte((uint32_t)bank * 0x4000u + (cpu - 0x4000u));

	return S9xSGBPeekRAByte((uint32_t)cpu);
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
	const uint16_t pc = s.pc;
	uint8_t bank = 0;
	if (pc >= 0x4000 && pc < 0x8000)
		bank = (uint8_t)(S9xSGBGetCurrentRomBank() & 0xFF);
	return ((uint32_t)bank << 16) | pc;
}

uint32_t TotalROMBanks()
{
	const uint32_t sz = S9xSGBGetROMSize();
	return sz ? (sz + 0x3FFFu) / 0x4000u : 0u;
}

} // namespace GbBackend
