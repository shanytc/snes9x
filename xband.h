/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

/*
 * XBAND modem peripheral emulation.
 *
 * XBAND (Catapult Entertainment, 1994-1997) was a pass-through cartridge
 * that contained its own 1MB firmware ROM, 64KB SRAM, a Rockwell RC2324DP
 * 2400-baud modem, and a custom "Fred" chip that applied per-game binary
 * patches at runtime. It enabled online multiplayer for roughly a dozen
 * SNES games over dial-up.
 *
 * Memory map (SNES address space, standalone XBAND BIOS mode):
 *   $00-$3F:$8000-$FFFF  XBAND firmware ROM (LoROM-style)
 *   $80-$BF:$8000-$FFFF  XBAND firmware ROM (mirror)
 *   $E0:$0000-$FFFF      XBAND SRAM (64KB), via MAP_XBAND
 *   $FB:$C000-$CFFF      Modem + Fred MMIO, via MAP_XBAND
 *                        (register sub-range decoded as $FB:$C180-$C1BF)
 *
 * When a game cartridge is plugged in on top, the XBAND firmware swaps
 * the game ROM into the cartridge space and keeps its SRAM/MMIO regions.
 * That pass-through behavior is not yet implemented here.
 *
 * The modem data path is bridged to a TCP socket so the emulator can
 * connect to the replacement XBAND server at xband.retrocomputing.network.
 * We don't implement the ADSP protocol itself — the firmware handles that.
 * We just shuttle bytes between the UART registers and the TCP socket.
 */

#ifndef _XBAND_H_
#define _XBAND_H_

#include <cstdint>

#define XBAND_ROM_SIZE		0x100000	// 1MB firmware ROM
#define XBAND_SRAM_SIZE		0x010000	// 64KB SRAM
#define XBAND_FIFO_SIZE		0x1000		// 4KB rx/tx FIFO each
#define XBAND_NUM_PATCHES	16			// Fred chip patch vectors

// Modem MMIO base in SNES address space (bank:offset = $FB:$C180)
#define XBAND_MMIO_BANK		0xFB
#define XBAND_MMIO_BASE		0xC180
#define XBAND_MMIO_END		0xC1BF

struct SXBandPatch
{
	bool8	enabled;
	uint32	address;	// 24-bit SNES address to intercept
	uint8	length;		// Number of bytes to override (1-4)
	uint8	data[4];	// Replacement bytes
};

struct SXBAND
{
	// Enable / state flags
	bool8	enabled;
	bool8	bios_loaded;
	bool8	connected;
	bool8	sram_dirty;

	// Rockwell RC2324DP modem registers (16550-compatible UART core).
	// DLAB=0: RBR (R)/THR (W) at 0x00, IER at 0x02
	// DLAB=1: DLL at 0x00, DLM at 0x02
	// Always:  IIR (R)/FCR (W) at 0x04, LCR at 0x06,
	//          MCR at 0x08, LSR at 0x0A, MSR at 0x0C, SCR at 0x0E
	uint8	ier;			// Interrupt Enable Register
	uint8	iir;			// Interrupt Identification Register (read)
	uint8	fcr;			// FIFO Control Register (write)
	uint8	lcr;			// Line Control Register (DLAB in bit 7)
	uint8	mcr;			// Modem Control Register
	uint8	lsr;			// Line Status Register
	uint8	msr;			// Modem Status Register
	uint8	scr;			// Scratch register
	uint8	dll;			// Divisor latch LSB
	uint8	dlm;			// Divisor latch MSB

	// RX FIFO: bytes received from the network, waiting for the SNES to read
	uint8	rx_fifo[XBAND_FIFO_SIZE];
	uint32	rx_head;
	uint32	rx_tail;

	// TX FIFO: bytes written by the SNES, waiting to flush to the network
	uint8	tx_fifo[XBAND_FIFO_SIZE];
	uint32	tx_head;
	uint32	tx_tail;

	// Fred chip — runtime game ROM patch vectors
	uint8			fred_control;
	uint8			fred_status;
	SXBandPatch		patches[XBAND_NUM_PATCHES];

	// SRAM (player profiles, patches, mail, news, icons)
	uint8	sram[XBAND_SRAM_SIZE];

	// Network socket (platform-agnostic; -1 when disconnected).
	// Stored as intptr_t so it holds a Windows SOCKET without truncation
	// on 64-bit builds. Not serialized in save states.
	intptr_t	socket_fd;
};

extern struct SXBAND XBand;

// Core hardware interface (called from memory dispatch).
uint8	S9xGetXBand (uint32 address);
void	S9xSetXBand (uint8 byte, uint32 address);
uint8  *S9xGetBasePointerXBand (uint32 address);

// Lifecycle.
void	S9xInitXBand (void);
void	S9xResetXBand (void);
void	S9xXBandPostLoadState (void);
bool8	S9xLoadXBandBIOS (void);

// Network bridging.
bool8	S9xXBandConnect (const char *host, int port);
void	S9xXBandDisconnect (void);
void	S9xXBandPoll (void);

// Fred chip patch vector application.
// Returns TRUE and writes the patched byte to *out_byte if address is patched.
bool8	S9xXBandTryPatch (uint32 address, uint8 *out_byte);

#endif
