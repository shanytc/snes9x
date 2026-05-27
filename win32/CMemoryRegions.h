#ifndef CMEMORY_REGIONS_H
#define CMEMORY_REGIONS_H

#include <stdint.h>
#include "CDebugger.h"

struct MemRegion
{
	const char *name;
	uint32_t (*get_size)();
	uint8_t  (*read_byte)(uint32_t offset);
	uint32_t addr_format_width;
	uint32_t (*to_cpu_addr)(uint32_t offset);
};

static constexpr uint32_t kNoCpuAddr = 0xFFFFFFFFu;

int               MemRegionCount(DbgSystem sys);
const MemRegion  *MemRegionAt(DbgSystem sys, int index);

#endif
