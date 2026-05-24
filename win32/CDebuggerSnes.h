#ifndef CDEBUGGER_SNES_H
#define CDEBUGGER_SNES_H

#include <stdint.h>
#include "CDisasm65816.h"

struct SnesStatus
{
	uint16_t A, X, Y, S, D;
	uint8_t  data_bank;
	uint8_t  prog_bank;
	uint16_t PC;
	uint8_t  P;
	bool     emulation;
	uint32_t cycles;
	uint16_t v_counter;
	uint16_t h_counter;
	uint32_t frame;
	uint16_t vram_addr;
	uint16_t oam_addr;
	uint16_t cgram_addr;
	bool     irq_nmi_enabled;
	bool     irq_h_enabled;
	bool     irq_v_enabled;
	bool     coprocessor_irq;
};

namespace SnesBackend
{
	uint8_t ReadByte(uint32_t addr24);
	void    GetStatus(SnesStatus *out);
	uint8_t Disassemble(uint32_t pc24, DisasmResult65816 *out);
	uint8_t DisassembleAt(uint32_t pc24, const Snes65816Context *ctx, DisasmResult65816 *out);
}

#endif
