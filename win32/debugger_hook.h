#ifndef DEBUGGER_HOOK_H
#define DEBUGGER_HOOK_H

#include <stdint.h>

extern bool g_debugger_attached;
extern bool g_debugger_check_rw;
extern bool g_debugger_heatmap_active;

void S9xDebuggerOnSnesPreInstruction(void);
void S9xDebuggerOnGbPreInstruction(uint16_t pc, uint8_t opcode);

void S9xDebuggerOnSnesMemAccess(uint32_t addr24, uint8_t value, bool is_write);
void S9xDebuggerOnGbMemAccess(uint16_t addr, uint8_t value, bool is_write);

void S9xDebuggerOnEmulatorReset(void);

#endif
