#ifndef __SNES_HPP
#define __SNES_HPP

#include "../../../snes9x.h"
#include "../../resampler.h"
#include "../../../msu1.h"
#include "../../../voicekun.h"
#include "../../../superdisc.h"

#define debugvirtual

namespace SNES
{

struct Processor
{
    unsigned frequency;
    int32 clock;
};

#include "../smp/smp.hpp"
#include "../dsp/sdsp.hpp"

class CPU
{
public:
    uint8 registers[4];
    // A write in the second half of an SPC cycle is seen from the next one (Mesen):
    // held here until smp.clock reaches pending_clock.
    uint8 pending_mask;
    uint8 pending[4];
    int32 pending_clock;

    inline void reset ()
    {
        registers[0] = registers[1] = registers[2] = registers[3] = 0;
        pending_mask = 0;
    }

    inline void apply_pending ()
    {
        for (int p = 0; p < 4; p++)
            if (pending_mask & (1 << p))
                registers[p] = pending[p];
        pending_mask = 0;
    }

    alwaysinline void port_write (uint8 port, uint8 data)
    {
        registers[port & 3] = data;
    }

    alwaysinline uint8 port_read (uint8 port)
    {
        return registers[port & 3];
    }
};

extern CPU cpu;

} // namespace SNES

#endif
