#ifndef CDEBUGGER_LABELS_H
#define CDEBUGGER_LABELS_H

#include <stdint.h>
#include "CDebugger.h"

const char *LookupSnesLabel(uint32_t addr24);
const char *LookupGbLabel(uint16_t addr);

const char *LookupLabel(DbgSystem sys, uint32_t addr);

#endif
