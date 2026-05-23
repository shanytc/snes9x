#include "CDebuggerSnes.h"
#include "../snes9x.h"
#include "../memmap.h"
#include "../ppu.h"
#include "../65c816.h"
#include "../cpuexec.h"

namespace SnesBackend {

uint8_t ReadByte(uint32_t addr24)
{
	const uint32_t a    = addr24 & 0xFFFFFF;
	const int block     = a >> MEMMAP_SHIFT;
	uint8_t *getAddress = Memory.Map[block];

	if (getAddress >= (uint8_t *) CMemory::MAP_LAST)
		return *(getAddress + (a & 0xFFFF));

	switch ((pint)getAddress)
	{
		case CMemory::MAP_LOROM_SRAM:
		case CMemory::MAP_SA1RAM:
			return *(Memory.SRAM + ((((a & 0xff0000) >> 1) | (a & 0x7fff)) & Memory.SRAMMask));
		case CMemory::MAP_LOROM_SRAM_B:
			return *(Multi.sramB + ((((a & 0xff0000) >> 1) | (a & 0x7fff)) & Multi.sramMaskB));
		case CMemory::MAP_HIROM_SRAM:
		case CMemory::MAP_RONLY_SRAM:
			return *(Memory.SRAM + (((a & 0x7fff) - 0x6000 + ((a & 0xf0000) >> 3)) & Memory.SRAMMask));
		case CMemory::MAP_BWRAM:
			return *(Memory.BWRAM + ((a & 0x7fff) - 0x6000));
		default:
			return 0;
	}
}

void GetStatus(SnesStatus *out)
{
	out->A  = Registers.A.W;
	out->X  = Registers.X.W;
	out->Y  = Registers.Y.W;
	out->S  = Registers.S.W;
	out->D  = Registers.D.W;
	out->data_bank = Registers.DB;
	out->prog_bank = Registers.PB;
	out->PC = Registers.PCw;
	out->P  = Registers.PL;
	out->emulation = (Registers.P.W & Emulation) != 0;
	out->cycles    = (uint32_t)CPU.Cycles;
	out->v_counter = (uint16_t)CPU.V_Counter;
	out->h_counter = (uint16_t)CPU.Cycles;
	out->frame     = IPPU.TotalEmulatedFrames;
	out->vram_addr = PPU.VMA.Address;
	out->oam_addr  = PPU.OAMAddr;
	out->cgram_addr= PPU.CGADD;
	out->irq_nmi_enabled = (Memory.FillRAM[0x4200] & 0x80) != 0;
	out->irq_h_enabled   = PPU.HTimerEnabled;
	out->irq_v_enabled   = PPU.VTimerEnabled;
	out->coprocessor_irq = CPU.IRQExternal;
}

uint8_t Disassemble(uint32_t pc24, DisasmResult65816 *out)
{
	const bool m = CheckMemory() != 0;
	const bool x = CheckIndex()  != 0;
	return DisassembleAt(pc24, m, x, out);
}

uint8_t DisassembleAt(uint32_t pc24, bool flag_M, bool flag_X, DisasmResult65816 *out)
{
	return Disassemble65816(pc24, flag_M, flag_X, &ReadByte, out);
}

} // namespace SnesBackend
