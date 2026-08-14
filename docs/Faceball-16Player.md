# Faceball 2000: the dormant 16-player mode — investigation notes

Collected 2026-08-14 while finishing the DMG-07 4-player link work (branch
`link_cable_support`, PR #232). Parked here for a possible future project.

## TL;DR

- The famous "Faceball 2000 supports 16 players" claim is **not** a DMG-07
  feature. Four players is the DMG-07's hardware maximum; chaining adapters
  does not work.
- The 16-player mode used a **proprietary cable built solely for Faceball**
  by its developer, Robert Champagne. It "works differently from both the
  DMG-07 and the regular Link Cables" (shonumi). The cable essentially
  never reached the public.
- **The 16-player code is still inside the shipped ROM.** Nobody has run it
  — not on hardware (the cable barely existed) and not in emulation (the
  protocol has never been reverse-engineered). Emulating it would be a
  genuine "first time anywhere".

## Background: why the ring theory is plausible

Faceball 2000 is the Game Boy port of **MIDI Maze** (Xanth Software F/X,
Atari ST). MIDI Maze's 16-player mode ran over a **MIDI ring network**:
each machine's MIDI OUT feeds the next machine's MIDI IN, packets are
store-and-forwarded around the loop, one machine acts as the ring master.
The custom Game Boy cable almost certainly recreated something like that
daisy-chain/ring rather than a broadcast hub — which is also why the
DMG-07 (a hub with its own clock and a 4-seat status protocol) cannot
imitate it.

## What is known vs unknown

Known:
- 16-player menu options exist in the published game.
- The custom cable is distinct from both the standard link cable and the
  DMG-07 (per shonumi, who corresponded on the topic).
- Faceball's DMG-07 mode (up to 4 players) is separate and works — it is
  a compatible title for our hub emulation.

Unknown (the actual investigation):
- The wire topology of the custom cable (ring? bus? who clocks whom?).
- The handshake: how the game distinguishes the custom cable from a
  DMG-07 / direct cable at the serial level.
- Player ID assignment, packet format, timing, and how data propagates
  across 16 units.
- Whether the mode is reachable from the shipped menus alone or gated on
  a detection that must succeed first.

## Investigation plan

1. **The ROM is the spec.** Disassemble Faceball 2000 (World) serial code:
   - Find the SIO interrupt handler(s) and every write of $80/$81 to SC.
   - Find the multiplayer lobby code: where the player-count (up to 16) is
     selected and which code path it gates.
   - Diff the DMG-07 path (it must contain the $FE ping / $88 ack / $AA
     switch handling) against the other link path(s); whatever remains is
     the custom-cable protocol.
   - Our own debugger branch (watchpoints on SB/SC, callstack) is the
     right tool; SameBoy can serve as a cross-check core.
2. **Study MIDI Maze's protocol** (Atari ST community documentation) as a
   likely template: ring master election, store-and-forward, packet
   framing. Expect the GB version to be a serial-port translation of it.
3. **Possibly contact Robert Champagne** — shonumi's own suggestion; he
   built the cable and wrote the code.

## How it would fit our emulator

- The transport (`sgb/gb_link.cpp`) already does multi-seat sessions; a
  ring is a different session shape, not a bigger hub: N point-to-point
  wires, each instance master toward its downstream neighbour. Our BGB
  wire already does per-link master/slave, and the DMG-07 work added
  sequence-tagged async replies + paced delivery — both reusable.
- Win32 session machinery (sequential spawn, pause/savestate fan-out,
  per-player .savN / _pN files) generalizes mechanically.
- Practical ceiling: only 8 joypad binding sets exist; 16 seats would
  need gamepads for most players. Sequential bring-up of 15 instances
  would take a while.

## DMG-07 reference (for context, all implemented on the branch)

- Protocol: Pan Docs "4-Player Adapter" page is the full wire spec
  (ping FE/STAT×3 with $88 acks, RATE/SIZE from player 1, $AA→$CC entry
  into transmission, SIZE bytes per player with one-packet delay,
  $FF×3+ restart). GBE+ `src/dmg/dmg07.cpp` is the field-tested
  implementation reference.
- Verified-compatible titles (test candidates beyond F-1 Race):
  Faceball 2000 (4p mode), Wave Race (SIZE=1 packets — exercises the
  negotiation differently), Yoshi's Cookie, Super R.C. Pro-Am,
  Top Rank Tennis, Trax.
- Reported but less well sourced: Kunio-kun no Nekketsu Daiundoukai,
  Super Momotaro Dentetsu, Chachamaru Panic, Uno: Small World 2,
  Monopoly, Penguin Wars ("up to 10"), Gauntlet II.
- Pokemon (all generations on GB/GBC) is strictly 2-player; different
  cartridges of a generation inter-link, but only pairwise — that is our
  "Link Other Game" mode, never a hub scenario.

## Sources

- shonumi, "Edge of Emulation: Game Boy 4-Player Adapter" —
  https://shonumi.github.io/articles/art9.html
  (DMG-07 reverse engineering; the Faceball custom-cable statements)
- Pan Docs, "4-Player Adapter" —
  https://gbdev.io/pandocs/Four_Player_Adapter.html
- GBE+ DMG-07 implementation —
  https://github.com/shonumi/gbe-plus/blob/master/src/dmg/dmg07.cpp
- NintendoWiki, "Four Player Adapter" —
  https://niwanetwork.org/wiki/Four_Player_Adapter
- MiSTer forum thread with 3/4-player game list —
  https://misterfpga.org/viewtopic.php?t=2673
- Our implementation: branch `link_cable_support`, PR
  https://github.com/shanytc/snes9x/pull/232 (`sgb/gb_serial.cpp` hub
  section, `sgb/gb_link.cpp` transport seats)
