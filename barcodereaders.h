/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _BARCODEREADERS_H_
#define _BARCODEREADERS_H_

// The barcode readers behind the frontends' one Barcodes menu and dialog:
// Epoch's Barcode Battler II on the SNES's port 2 (barcodebattler.h), and on
// player 1's Game Boy link port Namcot's Barcode Boy and Barcode Taisen
// Bardigun's reader (sgb/gb_barcodeboy.h, sgb/gb_bardigun.h). The loaded
// game's system picks the reader.

enum S9xBarcodeReader
{
	S9X_BARCODE_NONE = 0,	// no game loaded
	S9X_BARCODE_BATTLER,
	S9X_BARCODE_BARCODEBOY,
	S9X_BARCODE_BARDIGUN
};

// The reader the loaded game's system takes: the Barcode Battler II for a
// SNES game, Bardigun's reader for that game, the Barcode Boy for any other
// Game Boy game.
S9xBarcodeReader S9xBarcodeReaderFor (bool game_loaded);

// True when the loaded game reads the reader: Scan is offered only then.
bool S9xBarcodeReaderGameReads (S9xBarcodeReader reader);

// Why a Game Boy reader can't go on the link port (the SGB1 has none, or a
// Game Boy Link session holds it), or NULL.
const char *S9xBarcodeReaderPortBusy (S9xBarcodeReader reader);

// Connect, kept across game loads: while it is on, the reader the running
// game's system takes is plugged in and the others are out. Scanning turns
// it on. Once a frontend has used it, S9xMainLoop keeps the readers in step
// each frame (S9xBarcodeReadersFrame), so a new game finds its reader in
// place from its first frame.
void S9xBarcodeReaderConnect (bool connect);
bool S9xBarcodeReaderConnected (void);
void S9xBarcodeReadersFrame (void);

// Hands the reader a code (S9xBarcodeReaderTakes says which lengths);
// returns false, changing nothing, for anything else.
bool S9xBarcodeReaderScan (S9xBarcodeReader reader, const char *code);
const char *S9xBarcodeReaderLastCode (S9xBarcodeReader reader);	// "" before the first scan

// What a frontend shows for a reader.
const char *S9xBarcodeReaderName (S9xBarcodeReader reader);		// "Barcode Battler II", ...
const char *S9xBarcodeReaderPort (S9xBarcodeReader reader);		// "port 2", "the link port"
bool S9xBarcodeReaderTakes (S9xBarcodeReader reader, int digits);	// 8, 12 or 13 digits
// Photos of the known cards: <image><side> in this folder, under the
// frontend's photo locations; the sides list ends in NULL.
const char *S9xBarcodeReaderPhotoFolder (S9xBarcodeReader reader);
const char * const *S9xBarcodeReaderPhotoSides (S9xBarcodeReader reader);

// The reader's known cards, by branch (a Barcode Battler card set or a Game
// Boy game), then by group within it. Consecutive entries sharing a branch
// and a photo stem are one printed card.
struct S9xBarcodeReaderCard
{
	const char	*branch;
	const char	*group;	// NULL when the branch has none
	const char	*name;
	const char	*kana;	// as printed, UTF-8; "" when none
	const char	*code;
	const char	*image;	// photo stem, "" when none
};
const S9xBarcodeReaderCard *S9xBarcodeReaderCards (S9xBarcodeReader reader, int *count);
// The first known card a code (in any form Scan takes) belongs to, or NULL.
const S9xBarcodeReaderCard *S9xBarcodeReaderCardFind (S9xBarcodeReader reader, const char *code);
// The branch to open for the loaded game, or NULL.
const char *S9xBarcodeReaderOpenBranch (S9xBarcodeReader reader);

// The bars of an EAN-13 (a UPC-A reads as one) or EAN-8 code, a module each
// (true = bar): 95 or 67 written, 0 for anything else.
int S9xBarcodeReaderBars (const char *code, bool bars[95]);

#endif
