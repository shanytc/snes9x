#ifndef DEBUGGER_HOOK_H
#define DEBUGGER_HOOK_H

#include <stdint.h>

extern bool g_debugger_attached;

void S9xDebuggerOnSnesPreInstruction(void);
void S9xDebuggerOnGbPreInstruction(uint16_t pc, uint8_t opcode);

#endif
