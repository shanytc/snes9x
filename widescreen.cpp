/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "snes9x.h"
#include "widescreen.h"
#include "widescreen_patches.h"

#include <cstring>

// Its SA-1 RAM clear runs faster here than on hardware, so its sample-upload
// request beat the SPC driver's look for it: lengthen the delay it already has.
static const struct SWidescreenEdit	smw_widescreen_edits[] =
{
	{ 0x0A44A7, 2, { 0x80, 0x11 }, { 0x00, 0x50 } },	// 14:C4A7 LDX #$1180 -> #$5000
};

// Widescreen ROM hacks, keyed on the image: every release of Super Mario World
// Widescreen keeps the retail title, as do the other SA-1 hacks of the game.
static const struct SWidescreenGame	widescreen_games[] =
{
	// Super Mario World Widescreen v1.11 (VitorVilela7/wide-snes), its two
	// released widths: objects wherever the game put them, windows in its own
	// coordinates. The 448 and 480 widths are unreleased and unfinished.
	{ 0xB19ED489, 0x24389EDC, "SUPER MARIOWORLD", "Normal (16:9, 16:10)",
	  kSmwWidescreenBps, sizeof(kSmwWidescreenBps), smw_widescreen_edits, 1,
	  WS_MODE_ON, 48, WS_OBJ_UNSAFE, { WS_BG_ON, WS_BG_ON, WS_BG_ON, WS_BG_AUTO_HV },
	  TRUE, WS_WINDOW_NORMAL, 128 },
	{ 0xB19ED489, 0xE7207F98, "SUPER MARIOWORLD", "Extra (16:9, 2:1)",
	  kSmwExtrawideBps, sizeof(kSmwExtrawideBps), smw_widescreen_edits, 1,
	  WS_MODE_ON, 64, WS_OBJ_UNSAFE, { WS_BG_ON, WS_BG_ON, WS_BG_ON, WS_BG_AUTO_HV },
	  TRUE, WS_WINDOW_NORMAL, 128 },
	// The two widths the source offers but no release ships: our own builds
	// of the v1.11 source on its 384 base, which its author calls unfinished.
	{ 0xB19ED489, 0x3DDDEE65, "SUPER MARIOWORLD", "Ultra (2:1, 20.5:9, 21:9, 64:27) experimental",
	  kSmwUltrawideBps, sizeof(kSmwUltrawideBps), smw_widescreen_edits, 1,
	  WS_MODE_ON, 96, WS_OBJ_UNSAFE, { WS_BG_ON, WS_BG_ON, WS_BG_ON, WS_BG_AUTO_HV },
	  TRUE, WS_WINDOW_NORMAL, 128 },
	{ 0xB19ED489, 0x58EA7EE9, "SUPER MARIOWORLD", "Hyper (21:9, 64:27) experimental",
	  kSmwHyperwideBps, sizeof(kSmwHyperwideBps), smw_widescreen_edits, 1,
	  WS_MODE_ON, 112, WS_OBJ_UNSAFE, { WS_BG_ON, WS_BG_ON, WS_BG_ON, WS_BG_AUTO_HV },
	  TRUE, WS_WINDOW_NORMAL, 128 },
};

#define WIDESCREEN_GAMES	(sizeof(widescreen_games) / sizeof(widescreen_games[0]))

static const struct SWidescreenGame	*widescreen_game = NULL;
static bool8	widescreen_patched = FALSE;			// the image in memory is a hack
static bool8	widescreen_patched_here = FALSE;	// and the load path made it so

const struct SWidescreenGame *S9xFindWidescreenGame (uint32 crc32, const char *title, uint16 columns)
{
	for (size_t i = 0; i < WIDESCREEN_GAMES; i++)
	{
		const struct SWidescreenGame	*game = &widescreen_games[i];

		if (title && strcmp(game->Title, title) != 0)
			continue;
		if (crc32 == game->TargetCRC32)
			return (game);
		if (crc32 == game->SourceCRC32 && (columns == 0 || columns == game->Aspect))
			return (game);
	}

	return (NULL);
}

void S9xSetWidescreenGame (uint32 crc32, const char *title, bool8 patched_here)
{
	widescreen_game         = S9xFindWidescreenGame(crc32, title, 0);
	widescreen_patched      = widescreen_game && crc32 == widescreen_game->TargetCRC32;
	widescreen_patched_here = widescreen_patched && patched_here;
	S9xUpdateWidescreen();
}

const struct SWidescreenGame *S9xWidescreenGame (void)
{
	return (widescreen_game);
}

bool S9xWidescreenPatched (void)
{
	return (widescreen_patched);
}

int S9xWidescreenVariants (const struct SWidescreenGame **rows, int max)
{
	int	n = 0;

	if (!widescreen_game)
		return (0);

	for (size_t i = 0; i < WIDESCREEN_GAMES && n < max; i++)
		if (widescreen_games[i].SourceCRC32 == widescreen_game->SourceCRC32)
			rows[n++] = &widescreen_games[i];

	return (n);
}

bool S9xApplyWidescreenGameEdits (uint8 *rom, uint32 size)
{
	if (!widescreen_game || !widescreen_patched)
		return (false);

	const struct SWidescreenGame	*game = widescreen_game;

	for (int i = 0; i < game->EditCount; i++)
	{
		const struct SWidescreenEdit	&e = game->Edits[i];

		if (e.Len > 4 || e.Offset + e.Len > size || memcmp(rom + e.Offset, e.Old, e.Len) != 0)
			return (false);
	}

	for (int i = 0; i < game->EditCount; i++)
		memcpy(rom + game->Edits[i].Offset, game->Edits[i].New, game->Edits[i].Len);

	return (true);
}

bool S9xWidescreenReloadNeeded (void)
{
	if (!widescreen_game)
		return (false);

	// Off with our patch in: take it out.
	if (Settings.Widescreen.Mode == WS_MODE_OFF)
		return (widescreen_patched_here);

	// On with the retail cart in: patch it. On with our patch in but another
	// width picked: swap it. Either way only for a width the table has.
	if (!S9xFindWidescreenGame(widescreen_game->SourceCRC32, NULL, Settings.Widescreen.Aspect))
		return (false);

	if (!widescreen_patched)
		return (true);

	return (widescreen_patched_here && widescreen_game->Aspect != Settings.Widescreen.Aspect);
}
