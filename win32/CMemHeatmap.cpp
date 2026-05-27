#include "CMemHeatmap.h"
#include <algorithm>
#include <vector>

bool g_debugger_heatmap_active = false;

namespace
{
	constexpr uint32_t kSnesSpan = 0x1000000u;
	constexpr uint32_t kGbSpan   = 0x10000u;

	std::vector<uint16_t> g_snes_reads;
	std::vector<uint16_t> g_snes_writes;
	std::vector<uint16_t> g_gb_reads;
	std::vector<uint16_t> g_gb_writes;

	uint16_t g_snes_max_reads  = 0;
	uint16_t g_snes_max_writes = 0;
	uint16_t g_gb_max_reads    = 0;
	uint16_t g_gb_max_writes   = 0;

	int g_acquire_count = 0;

	void EnsureCapacity()
	{
		if (g_snes_reads.empty())  g_snes_reads.assign(kSnesSpan, 0);
		if (g_snes_writes.empty()) g_snes_writes.assign(kSnesSpan, 0);
		if (g_gb_reads.empty())    g_gb_reads.assign(kGbSpan, 0);
		if (g_gb_writes.empty())   g_gb_writes.assign(kGbSpan, 0);
	}

	void ClearCounters_()
	{
		if (!g_snes_reads.empty())  std::fill(g_snes_reads.begin(),  g_snes_reads.end(),  (uint16_t)0);
		if (!g_snes_writes.empty()) std::fill(g_snes_writes.begin(), g_snes_writes.end(), (uint16_t)0);
		if (!g_gb_reads.empty())    std::fill(g_gb_reads.begin(),    g_gb_reads.end(),    (uint16_t)0);
		if (!g_gb_writes.empty())   std::fill(g_gb_writes.begin(),   g_gb_writes.end(),   (uint16_t)0);
		g_snes_max_reads  = 0;
		g_snes_max_writes = 0;
		g_gb_max_reads    = 0;
		g_gb_max_writes   = 0;
	}
}

namespace MemHeatmap
{

void Reset()
{
	if (!g_snes_reads.empty())  std::fill(g_snes_reads.begin(),  g_snes_reads.end(),  (uint16_t)0);
	if (!g_snes_writes.empty()) std::fill(g_snes_writes.begin(), g_snes_writes.end(), (uint16_t)0);
	if (!g_gb_reads.empty())    std::fill(g_gb_reads.begin(),    g_gb_reads.end(),    (uint16_t)0);
	if (!g_gb_writes.empty())   std::fill(g_gb_writes.begin(),   g_gb_writes.end(),   (uint16_t)0);
	g_snes_max_reads  = 0;
	g_snes_max_writes = 0;
	g_gb_max_reads    = 0;
	g_gb_max_writes   = 0;
}

void Acquire()
{
	if (g_acquire_count == 0)
	{
		EnsureCapacity();
		g_debugger_heatmap_active = true;
	}
	g_acquire_count++;
}

void Release()
{
	if (g_acquire_count == 0) return;
	g_acquire_count--;
	if (g_acquire_count == 0)
	{
		g_debugger_heatmap_active = false;
		ClearCounters_();
	}
}

void TrackSnes(uint32_t addr24, bool is_write)
{
	if (!g_debugger_heatmap_active) return;
	addr24 &= 0xFFFFFFu;
	if (is_write)
	{
		if (g_snes_writes.empty()) return;
		uint16_t &c = g_snes_writes[addr24];
		if (c < 0xFFFFu) c++;
		if (c > g_snes_max_writes) g_snes_max_writes = c;
	}
	else
	{
		if (g_snes_reads.empty()) return;
		uint16_t &c = g_snes_reads[addr24];
		if (c < 0xFFFFu) c++;
		if (c > g_snes_max_reads) g_snes_max_reads = c;
	}
}

void TrackGb(uint16_t addr16, bool is_write)
{
	if (!g_debugger_heatmap_active) return;
	if (is_write)
	{
		if (g_gb_writes.empty()) return;
		uint16_t &c = g_gb_writes[addr16];
		if (c < 0xFFFFu) c++;
		if (c > g_gb_max_writes) g_gb_max_writes = c;
	}
	else
	{
		if (g_gb_reads.empty()) return;
		uint16_t &c = g_gb_reads[addr16];
		if (c < 0xFFFFu) c++;
		if (c > g_gb_max_reads) g_gb_max_reads = c;
	}
}

uint16_t GetSnes(uint32_t addr24, bool reads)
{
	addr24 &= 0xFFFFFFu;
	const auto &v = reads ? g_snes_reads : g_snes_writes;
	if (v.empty()) return 0;
	return v[addr24];
}

uint16_t GetGb(uint16_t addr16, bool reads)
{
	const auto &v = reads ? g_gb_reads : g_gb_writes;
	if (v.empty()) return 0;
	return v[addr16];
}

uint16_t Max(DbgSystem sys, bool reads)
{
	if (sys == DbgSystem::Snes) return reads ? g_snes_max_reads : g_snes_max_writes;
	if (sys == DbgSystem::Gb)   return reads ? g_gb_max_reads   : g_gb_max_writes;
	return 0;
}

static void DecayVec(std::vector<uint16_t> &v, uint16_t &running_max)
{
	if (v.empty()) return;
	uint16_t *p = v.data();
	const size_t n = v.size();
	uint16_t new_max = 0;
	for (size_t i = 0; i < n; i++)
	{
		uint16_t c = (uint16_t)(p[i] >> 1);
		p[i] = c;
		if (c > new_max) new_max = c;
	}
	running_max = new_max;
}

void Decay()
{
	if (!g_debugger_heatmap_active) return;
	DecayVec(g_snes_reads,  g_snes_max_reads);
	DecayVec(g_snes_writes, g_snes_max_writes);
	DecayVec(g_gb_reads,    g_gb_max_reads);
	DecayVec(g_gb_writes,   g_gb_max_writes);
}

}
