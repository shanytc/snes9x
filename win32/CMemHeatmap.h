#ifndef CMEM_HEATMAP_H
#define CMEM_HEATMAP_H

#include "CDebugger.h"
#include <stdint.h>

enum class MemZone { None, Reads, Writes };

namespace MemHeatmap
{
	void Reset();

	void Acquire();
	void Release();

	void TrackSnes(uint32_t addr24, bool is_write);
	void TrackGb(uint16_t addr16, bool is_write);

	uint16_t GetSnes(uint32_t addr24, bool reads);
	uint16_t GetGb(uint16_t addr16, bool reads);

	uint16_t Max(DbgSystem sys, bool reads);

	// Apply one decay step. All counters >>= 1, so idle bytes fade and only
	// recently-active bytes stay bright. Call periodically (e.g., once per
	// second of wall time) while the heatmap is acquired.
	void Decay();
}

extern bool g_debugger_heatmap_active;

#endif
