/*****************************************************************************\
     Shared-memory viewer transport for instanced Game Boy link sessions.

     The master emulates every seat in-process on the split engine (the only
     architecture that keeps link games in sync); spawned windows are thin
     viewers that receive their seat's finished frames through a shared
     mapping and publish their joypad back. No sockets, no timing coupling:
     a viewer can stall, close or lag without the session ever noticing.
\*****************************************************************************/

#ifndef _GB_VIEWER_H_
#define _GB_VIEWER_H_

#include <stdint.h>

// Master (session host) side. Player numbering matches the split engine:
// the master is player 1, remote seats are players 2..4.
bool S9xSGBViewerHostStart(int players);
void S9xSGBViewerHostStop(void);
bool S9xSGBViewerHostActive(void);
// Copies each seat's latest frame into the mapping; call once per host frame.
void S9xSGBViewerHostPush(void);
// BIOS-mode dressing: the master's SNES frame (256x224) doubles as the
// shared SGB border plane; viewers composite their seat into its window.
void S9xSGBViewerHostPushBorder(const uint16_t *snes_frame, uint32_t pitch_pixels);
// The seat's live joypad as published by its viewer (0 if none attached).
uint16_t S9xSGBViewerHostPad(int player);
// All windows share one keyboard, so input follows focus: true when the
// master's own controllers should be honored (always outside viewer mode).
bool S9xSGBViewerHostLocalPads(void);
// The user's background-input preference: when on, every window publishes
// its pad regardless of focus, so unfocused play keeps working.
void S9xSGBViewerSetBackgroundInput(bool on);
// Seats with a viewer window currently attached and heartbeating.
int S9xSGBViewerHostViewerCount(void);

// Viewer (spawned window) side.
bool S9xSGBViewerClientStart(unsigned long master_pid, int seat, int players);
void S9xSGBViewerClientStop(void);
bool S9xSGBViewerClientActive(void);
// False once the master has torn the session down (or died uncleanly).
bool S9xSGBViewerClientAlive(void);
// What the viewer should present: 256x224 once a border plane is live
// (BIOS-mode session), else the bare 160x144 GB frame.
void S9xSGBViewerClientDims(int *w, int *h);
// Latest frame into dest (at the size ClientDims reports); false while
// none arrived.
bool S9xSGBViewerClientBlit(uint16_t *dest, uint32_t pitch_pixels);
void S9xSGBViewerClientSetPad(uint16_t snes_pad_mask);

#endif
