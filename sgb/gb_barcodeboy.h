/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _SGB_GB_BARCODEBOY_H_
#define _SGB_GB_BARCODEBOY_H_

#include <cstddef>
#include <cstdint>

namespace SGB {

// Namcot's Barcode Boy (1992), a card scanner on player 1's link port (Dan
// Docs, Barcode Boy). The game knocks with 10 07 10 07 on its own clock and
// the scanner answers FF FF 10 07. The game then listens on the external
// clock, and the scanner clocks a scanned card in: STX, 13 ASCII digits, ETX,
// twice. It wants the knock again before the next card.

void BarcodeBoyConnect(bool on);
bool BarcodeBoyConnected();

// Scans a card: 13 digits (EAN-13/JAN; spaces and dashes are ignored). It
// waits in the scanner until the game has knocked and listens. Returns false,
// changing nothing, for anything else.
bool BarcodeBoyScan(const char *code);
bool BarcodeBoyPending();
const char *BarcodeBoyLastCode();   // "" before the first scan
uint32_t BarcodeBoyReadCount();     // cards clocked all the way into a game

// gb_serial, while the scanner holds the port. Exchange: the byte it shifts
// back for one the game clocks out. Clock: its own clock, `cycles` real
// T-cycles at a time; the next byte of a card when one lands, else -1.
// `listening` is the game armed on the external clock.
uint8_t BarcodeBoyExchange(uint8_t out);
uint8_t BarcodeBoyReply();          // what the last exchange shifts back
int     BarcodeBoyClock(int32_t cycles, bool listening);

// Savestates carry the scanner's side of the wire and a card waiting in it.
size_t BarcodeBoyStateBytes();
void   BarcodeBoyStateSave(uint8_t *out);
void   BarcodeBoyStateLoad(const uint8_t *in);

// The known cards, grouped per game and per printed card (top barcode first).
// A card's entries share its photos, <image>-front.jxl and -back.jxl in the
// frontend's barcodeboy folder.
struct BarcodeBoyCard
{
	const char *game;
	const char *name;   // English
	const char *kana;   // as printed on the card, UTF-8
	const char *code;   // 13 digits
	const char *image;  // "<game folder>/<the card's first code>"
};
const BarcodeBoyCard *BarcodeBoyCards(int *count);

// The game a cart header title belongs to ("Battle Space", ...), or nullptr.
const char *BarcodeBoyGame(const char *title);

// Looks for an EAN-13 barcode (a UPC-A reads as one with a leading zero) in an
// 8-bit greyscale picture, across rows and columns and both ways up. On success
// writes the 13 digits, NUL-terminated, into out[14].
bool BarcodeBoyDecode(const uint8_t *luma, int width, int height, int stride, char out[14]);

} // namespace SGB

#endif
