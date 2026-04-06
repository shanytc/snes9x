/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

/*
 * XBAND modem peripheral emulation.
 *
 * The XBAND was a pass-through cartridge modem by Catapult Entertainment
 * (1994-1997). It contained:
 *   - 1MB firmware ROM (XBAND BIOS)
 *   - 64KB SRAM (profiles, patches, mail, news, icons)
 *   - Rockwell RC2324DP 2400-baud modem (UART-style, 16550-compatible)
 *   - "Fred" chip — up to ~16 patch vectors that intercept game ROM reads
 *     and substitute replacement bytes. This was how XBAND redirected
 *     controller polls to inject network-received inputs.
 *
 * This file emulates the hardware side of XBAND. The original network
 * protocol (a modified early ADSP) is handled by the XBAND firmware
 * itself — we only shuttle raw bytes between the modem's RX/TX FIFOs
 * and a TCP socket connected to a replacement XBAND server (such as
 * xband.retrocomputing.network).
 *
 * References:
 *   - https://github.com/Cinghialotto/xband  (Catapult source dump)
 *   - https://fresh-eggs.github.io/xband_post.html
 *   - https://xbandwiki.retrocomputing.network/
 *   - bsnes-plus xband_support branch (prior art)
 */

// Winsock2 MUST be included before <windows.h> (which snes9x.h pulls in
// transitively), otherwise the old <winsock.h> gets included first and we
// get a cascade of redefinition errors.
#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  typedef int socklen_t;
  typedef SOCKET xband_sock_t;
  #define XBAND_CLOSESOCKET(s)	closesocket((SOCKET)(s))
  #define XBAND_INVALID_SOCKET	((intptr_t)INVALID_SOCKET)
  #define XBAND_SOCKET_ERROR	SOCKET_ERROR
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <errno.h>
  typedef int xband_sock_t;
  #define XBAND_CLOSESOCKET(s)	close((int)(s))
  #define XBAND_INVALID_SOCKET	(-1)
  #define XBAND_SOCKET_ERROR	(-1)
#endif

#include "snes9x.h"
#include "memmap.h"
#include "fscompat.h"
#include "xband.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

// Global instance, referenced by memory dispatch.
struct SXBAND XBand;

#ifdef _WIN32
static bool s_winsock_inited = false;
#endif

// -----------------------------------------------------------------------
// Internal FIFO helpers
// -----------------------------------------------------------------------

static inline uint32 xband_fifo_count (uint32 head, uint32 tail)
{
	return (head - tail) & (XBAND_FIFO_SIZE - 1);
}

static inline bool xband_fifo_empty (uint32 head, uint32 tail)
{
	return head == tail;
}

static inline bool xband_fifo_full (uint32 head, uint32 tail)
{
	return xband_fifo_count(head, tail) == (XBAND_FIFO_SIZE - 1);
}

static void xband_rx_push (uint8 byte)
{
	if (xband_fifo_full(XBand.rx_head, XBand.rx_tail))
		return; // drop — the XBAND server should pace itself
	XBand.rx_fifo[XBand.rx_head] = byte;
	XBand.rx_head = (XBand.rx_head + 1) & (XBAND_FIFO_SIZE - 1);
}

static uint8 xband_rx_pop (void)
{
	if (xband_fifo_empty(XBand.rx_head, XBand.rx_tail))
		return 0;
	uint8 b = XBand.rx_fifo[XBand.rx_tail];
	XBand.rx_tail = (XBand.rx_tail + 1) & (XBAND_FIFO_SIZE - 1);
	return b;
}

static void xband_tx_push (uint8 byte)
{
	if (xband_fifo_full(XBand.tx_head, XBand.tx_tail))
		return; // drop — should flush more aggressively
	XBand.tx_fifo[XBand.tx_head] = byte;
	XBand.tx_head = (XBand.tx_head + 1) & (XBAND_FIFO_SIZE - 1);
}

// -----------------------------------------------------------------------
// Line Status Register maintenance
// -----------------------------------------------------------------------

// LSR bits (16550 standard):
//   0  Data Ready (RX FIFO has data)
//   1  Overrun Error
//   2  Parity Error
//   3  Framing Error
//   4  Break Indicator
//   5  THR Empty (TX holding register empty)
//   6  TEMT (TX shift + holding empty)
//   7  FIFO error
#define XBAND_LSR_DR		0x01
#define XBAND_LSR_THRE		0x20
#define XBAND_LSR_TEMT		0x40

static void xband_update_lsr (void)
{
	uint8 lsr = XBand.lsr & ~(XBAND_LSR_DR | XBAND_LSR_THRE | XBAND_LSR_TEMT);

	if (!xband_fifo_empty(XBand.rx_head, XBand.rx_tail))
		lsr |= XBAND_LSR_DR;

	if (xband_fifo_empty(XBand.tx_head, XBand.tx_tail))
		lsr |= (XBAND_LSR_THRE | XBAND_LSR_TEMT);

	XBand.lsr = lsr;
}

// -----------------------------------------------------------------------
// Modem register access (Rockwell RC2324DP, 16550-style UART)
// -----------------------------------------------------------------------
//
// The XBAND maps modem registers at $FB:$C180-$C1BF. Registers are 8-bit
// wide but word-aligned (every other byte), which is why we use offset/2
// to index them. The 16550 UART has 8 logical registers; the RC2324DP
// exposes extensions but the XBAND firmware speaks to them via this
// standard UART interface.
//
//   Reg  DLAB=0         DLAB=1
//   0    RBR (R) / THR (W)    DLL
//   1    IER                  DLM
//   2    IIR (R) / FCR (W)
//   3    LCR (bit 7 = DLAB)
//   4    MCR
//   5    LSR
//   6    MSR
//   7    SCR

static uint8 xband_read_uart (uint8 reg)
{
	bool dlab = (XBand.lcr & 0x80) != 0;

	switch (reg)
	{
		case 0: // RBR (read) or DLL if DLAB
			if (dlab)
				return XBand.dll;
			{
				uint8 b = xband_rx_pop();
				xband_update_lsr();
				return b;
			}

		case 1: // IER or DLM
			return dlab ? XBand.dlm : XBand.ier;

		case 2: // IIR (read)
			return XBand.iir;

		case 3: // LCR
			return XBand.lcr;

		case 4: // MCR
			return XBand.mcr;

		case 5: // LSR
			xband_update_lsr();
			return XBand.lsr;

		case 6: // MSR
			return XBand.msr;

		case 7: // SCR
			return XBand.scr;
	}

	return 0xFF;
}

static void xband_write_uart (uint8 reg, uint8 byte)
{
	bool dlab = (XBand.lcr & 0x80) != 0;

	switch (reg)
	{
		case 0: // THR (write) or DLL if DLAB
			if (dlab)
				XBand.dll = byte;
			else
			{
				xband_tx_push(byte);
				xband_update_lsr();
			}
			return;

		case 1: // IER or DLM
			if (dlab)
				XBand.dlm = byte;
			else
				XBand.ier = byte & 0x0F; // only low 4 bits are defined
			return;

		case 2: // FCR (write)
			XBand.fcr = byte;
			// Bits 1,2: clear RX / TX FIFO
			if (byte & 0x02)
			{
				XBand.rx_head = XBand.rx_tail = 0;
			}
			if (byte & 0x04)
			{
				XBand.tx_head = XBand.tx_tail = 0;
			}
			return;

		case 3: // LCR
			XBand.lcr = byte;
			return;

		case 4: // MCR
			XBand.mcr = byte;
			return;

		case 5: // LSR — writable bits are limited
			XBand.lsr = (XBand.lsr & ~0x1E) | (byte & 0x1E);
			return;

		case 6: // MSR
			XBand.msr = byte;
			return;

		case 7: // SCR
			XBand.scr = byte;
			return;
	}
}

// -----------------------------------------------------------------------
// Fred chip register access
// -----------------------------------------------------------------------
//
// The Fred chip sits at the high end of the MMIO region, past the UART.
// Register layout (relative to $C180):
//   0x20       Fred control
//   0x21       Fred status
//   0x22-0x3F  Patch vector programming window (index + data)
//
// The precise layout of the Fred chip is still partially reverse-engineered.
// We expose a simple programming interface that's enough to carry the
// XBAND firmware's patch-loading routines. A write to 0x22 selects a
// patch slot; subsequent writes to 0x23-0x29 program the address/length/
// bytes/enable fields.

static uint8 xband_fred_selected_slot = 0;
static uint8 xband_fred_field_index   = 0;

static uint8 xband_read_fred (uint8 reg)
{
	switch (reg)
	{
		case 0x00: return XBand.fred_control;
		case 0x01: return XBand.fred_status;

		case 0x02: return xband_fred_selected_slot;

		case 0x03:
		{
			// Read back the currently-indexed field of the selected slot.
			SXBandPatch &p = XBand.patches[xband_fred_selected_slot & (XBAND_NUM_PATCHES - 1)];
			switch (xband_fred_field_index)
			{
				case 0: return p.enabled ? 1 : 0;
				case 1: return (p.address >> 16) & 0xFF;
				case 2: return (p.address >>  8) & 0xFF;
				case 3: return  p.address        & 0xFF;
				case 4: return p.length;
				case 5: return p.data[0];
				case 6: return p.data[1];
				case 7: return p.data[2];
				case 8: return p.data[3];
			}
			return 0;
		}
	}

	return 0;
}

static void xband_write_fred (uint8 reg, uint8 byte)
{
	switch (reg)
	{
		case 0x00:
			XBand.fred_control = byte;
			// Bit 7 is a master enable for patches
			return;

		case 0x01:
			XBand.fred_status = byte;
			return;

		case 0x02:
			xband_fred_selected_slot = byte & (XBAND_NUM_PATCHES - 1);
			xband_fred_field_index   = 0;
			return;

		case 0x03:
		{
			// Program the currently-indexed field of the selected slot,
			// then advance the field index. A small, deterministic
			// protocol that mirrors how Fred-style chips are typically
			// accessed via a single data port.
			SXBandPatch &p = XBand.patches[xband_fred_selected_slot & (XBAND_NUM_PATCHES - 1)];
			switch (xband_fred_field_index)
			{
				case 0: p.enabled  = (byte & 1) ? TRUE : FALSE;             break;
				case 1: p.address  = (p.address & 0x00FFFF) | (byte << 16); break;
				case 2: p.address  = (p.address & 0xFF00FF) | (byte <<  8); break;
				case 3: p.address  = (p.address & 0xFFFF00) |  byte;        break;
				case 4: p.length   = byte > 4 ? 4 : byte;                   break;
				case 5: p.data[0]  = byte;                                  break;
				case 6: p.data[1]  = byte;                                  break;
				case 7: p.data[2]  = byte;                                  break;
				case 8: p.data[3]  = byte;                                  break;
			}
			if (xband_fred_field_index < 8)
				xband_fred_field_index++;
			return;
		}
	}
}

// -----------------------------------------------------------------------
// Public memory dispatch — called from getset.h via MAP_XBAND
// -----------------------------------------------------------------------

uint8 S9xGetXBand (uint32 address)
{
	uint8  bank   = (address >> 16) & 0xFF;
	uint16 offset =  address        & 0xFFFF;

	// XBAND SRAM at $E0:$0000-$FFFF
	if (bank == 0xE0)
		return XBand.sram[offset];

	// XBAND firmware ROM at $D0-$DF is mapped as direct pointer, so we
	// shouldn't normally land here for that range. Handle it defensively.

	// Modem + Fred MMIO at $FB:$C180-$C1BF
	if (bank == XBAND_MMIO_BANK && offset >= XBAND_MMIO_BASE && offset <= XBAND_MMIO_END)
	{
		uint8 local = (uint8)(offset - XBAND_MMIO_BASE);
		// Word-aligned registers: the UART occupies the low 16 bytes,
		// Fred occupies the next 16. Each logical register is 2 bytes wide.
		uint8 reg = local >> 1;
		if (local < 0x10)
			return xband_read_uart(reg & 0x07);
		else
			return xband_read_fred(reg - 0x08);
	}

	return OpenBus;
}

void S9xSetXBand (uint8 byte, uint32 address)
{
	uint8  bank   = (address >> 16) & 0xFF;
	uint16 offset =  address        & 0xFFFF;

	if (bank == 0xE0)
	{
		if (XBand.sram[offset] != byte)
		{
			XBand.sram[offset] = byte;
			XBand.sram_dirty   = TRUE;
			CPU.SRAMModified   = TRUE;
		}
		return;
	}

	if (bank == XBAND_MMIO_BANK && offset >= XBAND_MMIO_BASE && offset <= XBAND_MMIO_END)
	{
		uint8 local = (uint8)(offset - XBAND_MMIO_BASE);
		uint8 reg = local >> 1;
		if (local < 0x10)
			xband_write_uart(reg & 0x07, byte);
		else
			xband_write_fred(reg - 0x08, byte);
		return;
	}
}

uint8 *S9xGetBasePointerXBand (uint32 address)
{
	uint8 bank = (address >> 16) & 0xFF;
	if (bank == 0xE0)
	{
		// Convention: callers compute byte = base[Address & 0xFFFF],
		// so the base pointer must equal the start of the bank's data.
		return XBand.sram;
	}
	// MMIO is I/O with no linear base pointer
	return NULL;
}

// -----------------------------------------------------------------------
// Fred chip patch application
// -----------------------------------------------------------------------

bool8 S9xXBandTryPatch (uint32 address, uint8 *out_byte)
{
	if (!XBand.enabled)
		return FALSE;
	if (!(XBand.fred_control & 0x80))
		return FALSE;

	uint32 a = address & 0xFFFFFF;
	for (int i = 0; i < XBAND_NUM_PATCHES; i++)
	{
		SXBandPatch &p = XBand.patches[i];
		if (!p.enabled || p.length == 0)
			continue;
		if (a >= p.address && a < p.address + p.length)
		{
			*out_byte = p.data[a - p.address];
			return TRUE;
		}
	}
	return FALSE;
}

// -----------------------------------------------------------------------
// BIOS loading
// -----------------------------------------------------------------------

bool8 S9xLoadXBandBIOS (void)
{
	const char *candidates[] = {
		"XBAND.bios",
		"XBAND.bin",
		"xband.bios",
		"xband.bin",
		NULL
	};

	for (int i = 0; candidates[i] != NULL; i++)
	{
		std::string path = S9xGetDirectory(BIOS_DIR);
		path += SLASH_STR;
		path += candidates[i];

		FILE *f = fopen(path.c_str(), "rb");
		if (!f)
			continue;

		fseek(f, 0, SEEK_END);
		long size = ftell(f);
		fseek(f, 0, SEEK_SET);

		if (size == XBAND_ROM_SIZE)
		{
			// Load into the BIOSROM buffer maintained by the core.
			size_t r = fread(Memory.BIOSROM, 1, XBAND_ROM_SIZE, f);
			fclose(f);
			if (r == XBAND_ROM_SIZE)
			{
				XBand.bios_loaded = TRUE;
				return TRUE;
			}
			return FALSE;
		}

		fclose(f);
	}

	XBand.bios_loaded = FALSE;
	return FALSE;
}

// -----------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------

void S9xInitXBand (void)
{
	// This is called from InitROM before header parsing, so don't assume
	// Settings.XBAND is set yet. Just clear state and leave detection
	// to the ROM loader.
	memset(&XBand, 0, sizeof(XBand));
	XBand.socket_fd = XBAND_INVALID_SOCKET;
}

void S9xResetXBand (void)
{
	// Reset modem state but preserve SRAM (like a real power cycle
	// with the battery still connected).
	XBand.ier = 0;
	XBand.iir = 0x01; // no interrupt pending
	XBand.fcr = 0;
	XBand.lcr = 0;
	XBand.mcr = 0;
	XBand.lsr = XBAND_LSR_THRE | XBAND_LSR_TEMT;
	XBand.msr = 0;
	XBand.scr = 0;
	XBand.dll = 0;
	XBand.dlm = 0;

	XBand.rx_head = XBand.rx_tail = 0;
	XBand.tx_head = XBand.tx_tail = 0;

	XBand.fred_control = 0;
	XBand.fred_status  = 0;
	xband_fred_selected_slot = 0;
	xband_fred_field_index   = 0;

	for (int i = 0; i < XBAND_NUM_PATCHES; i++)
		memset(&XBand.patches[i], 0, sizeof(SXBandPatch));
}

void S9xXBandPostLoadState (void)
{
	// Re-derive LSR from FIFO state
	xband_update_lsr();
	// Don't touch the socket — the user must reconnect after loading
	// a save state, just like a real modem call being hung up.
}

// -----------------------------------------------------------------------
// Network bridging — TCP socket shuttle
// -----------------------------------------------------------------------

static bool xband_set_nonblocking (xband_sock_t fd)
{
#ifdef _WIN32
	u_long mode = 1;
	return ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags == -1)
		return false;
	return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

bool8 S9xXBandConnect (const char *host, int port)
{
	if (XBand.socket_fd != XBAND_INVALID_SOCKET)
		S9xXBandDisconnect();

#ifdef _WIN32
	if (!s_winsock_inited)
	{
		WSADATA wsa;
		if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
			return FALSE;
		s_winsock_inited = true;
	}
#endif

	struct addrinfo hints;
	struct addrinfo *result = NULL;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family   = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	char port_str[16];
	snprintf(port_str, sizeof(port_str), "%d", port);

	if (getaddrinfo(host, port_str, &hints, &result) != 0 || !result)
		return FALSE;

	xband_sock_t fd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
	if ((intptr_t)fd == XBAND_INVALID_SOCKET)
	{
		freeaddrinfo(result);
		return FALSE;
	}

	if (connect(fd, result->ai_addr, (socklen_t)result->ai_addrlen) == XBAND_SOCKET_ERROR)
	{
		XBAND_CLOSESOCKET(fd);
		freeaddrinfo(result);
		return FALSE;
	}

	freeaddrinfo(result);

	// Disable Nagle — low-latency input exchange matters
	int flag = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&flag, sizeof(flag));

	xband_set_nonblocking(fd);

	XBand.socket_fd = (intptr_t)fd;
	XBand.connected = TRUE;

	// Raise CD / DSR in MSR so the firmware sees the modem as connected.
	XBand.msr = 0x90; // bit 7 = DCD, bit 4 = CTS
	return TRUE;
}

void S9xXBandDisconnect (void)
{
	if (XBand.socket_fd != XBAND_INVALID_SOCKET)
	{
		XBAND_CLOSESOCKET(XBand.socket_fd);
		XBand.socket_fd = XBAND_INVALID_SOCKET;
	}
	XBand.connected = FALSE;
	XBand.msr = 0;
}

void S9xXBandPoll (void)
{
	if (XBand.socket_fd == XBAND_INVALID_SOCKET)
		return;

	xband_sock_t fd = (xband_sock_t)XBand.socket_fd;

	// Flush TX FIFO to the socket
	while (!xband_fifo_empty(XBand.tx_head, XBand.tx_tail))
	{
		uint8 b = XBand.tx_fifo[XBand.tx_tail];
		int sent = (int)send(fd, (const char *)&b, 1, 0);
		if (sent != 1)
			break; // socket would block or is broken
		XBand.tx_tail = (XBand.tx_tail + 1) & (XBAND_FIFO_SIZE - 1);
	}

	// Drain any bytes waiting on the socket into the RX FIFO
	for (;;)
	{
		if (xband_fifo_full(XBand.rx_head, XBand.rx_tail))
			break;

		uint8 b;
		int got = (int)recv(fd, (char *)&b, 1, 0);
		if (got == 1)
			xband_rx_push(b);
		else
			break;
	}

	xband_update_lsr();
}
