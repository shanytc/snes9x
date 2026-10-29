/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include <atomic>
#include <string.h>
#include <vector>

#include "snes9x.h"
#include "display.h"
#include "barcodebattler.h"
#include "barcodereaders.h"
#include "sgb/sgb.h"
#include "sgb/gb_barcodeboy.h"
#include "sgb/gb_bardigun.h"

// Connect is written by the frontend's thread and read by the frame hook on
// the emulation thread. Until a frontend uses it, the readers are left to
// their own connect calls (libretro's port 2 device, say).
static std::atomic<bool>	br_connect{false};
static std::atomic<bool>	br_managed{false};

S9xBarcodeReader S9xBarcodeReaderFor (bool game_loaded)
{
	if (!game_loaded)
		return (S9X_BARCODE_NONE);
	if (Settings.SuperGameBoy || Settings.SGB_BIOSModeActive)
		return (S9xSGBBardigunGame() ? S9X_BARCODE_BARDIGUN : S9X_BARCODE_BARCODEBOY);
	return (S9X_BARCODE_BATTLER);
}

bool S9xBarcodeReaderGameReads (S9xBarcodeReader reader)
{
	switch (reader)
	{
		case S9X_BARCODE_BATTLER:		return (S9xBarcodeBattlerGameSupported());
		case S9X_BARCODE_BARCODEBOY:	return (S9xSGBBarcodeBoyGame() != NULL);
		case S9X_BARCODE_BARDIGUN:		return (S9xSGBBardigunGame() != NULL);
		default:						return (false);
	}
}

const char *S9xBarcodeReaderPortBusy (S9xBarcodeReader reader)
{
	if (reader != S9X_BARCODE_BARCODEBOY && reader != S9X_BARCODE_BARDIGUN)
		return (NULL);
	if (!S9xSGBBarcodeBoyHasPort())
		return ("the Super Game Boy (SGB1) has no link port");
	if (S9xSGBLinkIsEnabled() || S9xSGBSplitActive())
		return ("a Game Boy Link session holds the link port");
	return (NULL);
}

void S9xBarcodeReaderConnect (bool connect)
{
	br_connect.store(connect);
	br_managed.store(true);
}

bool S9xBarcodeReaderConnected (void)
{
	return (br_connect.load());
}

void S9xBarcodeReadersFrame (void)
{
	if (!br_managed.load(std::memory_order_relaxed))
		return;

	// S9xMainLoop only runs with a game loaded. A Game Boy reader needs the
	// link port, which the SGB1 doesn't have.
	const S9xBarcodeReader	reader = S9xBarcodeReaderFor(true);
	const bool				on = br_connect.load();
	const bool				port = S9xSGBBarcodeBoyHasPort();
	const bool				battler = on && reader == S9X_BARCODE_BATTLER;
	const bool				boy = on && port && reader == S9X_BARCODE_BARCODEBOY;
	const bool				bardigun = on && port && reader == S9X_BARCODE_BARDIGUN;

	if (S9xBarcodeBattlerConnected() != battler)
		S9xBarcodeBattlerConnect(battler);
	if (S9xSGBBarcodeBoyConnected() != boy)
		S9xSGBBarcodeBoyConnect(boy);
	if (S9xSGBBardigunConnected() != bardigun)
		S9xSGBBardigunConnect(bardigun);

	// The Game Boy readers' line for a card a game took whole; the Barcode
	// Battler has its own. Run-ahead rolls the counts back and forth, so
	// only a new highest one counts.
	static unsigned	boy_seen = 0, bardigun_seen = 0;
	const unsigned	boy_reads = S9xSGBBarcodeBoyReadCount();
	const unsigned	bardigun_reads = S9xSGBBardigunReadCount();
	if (boy && boy_reads > boy_seen)
		S9xSetInfoString("Barcode Boy: card read");
	if (bardigun && bardigun_reads > bardigun_seen)
		S9xSetInfoString("Barcode Taisen Bardigun: card read");
	if (boy_reads > boy_seen)
		boy_seen = boy_reads;
	if (bardigun_reads > bardigun_seen)
		bardigun_seen = bardigun_reads;
}

bool S9xBarcodeReaderScan (S9xBarcodeReader reader, const char *code)
{
	bool	ok;

	switch (reader)
	{
		case S9X_BARCODE_BATTLER:		ok = S9xBarcodeBattlerScan(code); break;
		case S9X_BARCODE_BARCODEBOY:	ok = S9xSGBBarcodeBoyScan(code); break;
		case S9X_BARCODE_BARDIGUN:		ok = S9xSGBBardigunScan(code); break;
		default:						ok = false; break;
	}

	if (ok)
		S9xBarcodeReaderConnect(true);
	return (ok);
}

const char *S9xBarcodeReaderLastCode (S9xBarcodeReader reader)
{
	switch (reader)
	{
		case S9X_BARCODE_BATTLER:		return (S9xBarcodeBattlerLastCode());
		case S9X_BARCODE_BARCODEBOY:	return (S9xSGBBarcodeBoyLastCode());
		case S9X_BARCODE_BARDIGUN:		return (S9xSGBBardigunLastCode());
		default:						return ("");
	}
}

const char *S9xBarcodeReaderName (S9xBarcodeReader reader)
{
	switch (reader)
	{
		case S9X_BARCODE_BATTLER:		return ("Barcode Battler II");
		case S9X_BARCODE_BARCODEBOY:	return ("Barcode Boy");
		case S9X_BARCODE_BARDIGUN:		return ("Barcode Taisen Bardigun");
		default:						return ("Barcodes");
	}
}

const char *S9xBarcodeReaderPort (S9xBarcodeReader reader)
{
	return (reader == S9X_BARCODE_BATTLER ? "port 2" : "the link port");
}

bool S9xBarcodeReaderTakes (S9xBarcodeReader reader, int digits)
{
	switch (reader)
	{
		case S9X_BARCODE_BATTLER:		return (digits == 13 || digits == 12 || digits == 8);
		case S9X_BARCODE_BARCODEBOY:	return (digits == 13);
		case S9X_BARCODE_BARDIGUN:		return (digits == 13 || digits == 8);
		default:						return (false);
	}
}

const char *S9xBarcodeReaderPhotoFolder (S9xBarcodeReader reader)
{
	return (reader == S9X_BARCODE_BATTLER ? "battler" : "barcodeboy");
}

const char * const *S9xBarcodeReaderPhotoSides (S9xBarcodeReader reader)
{
	static const char * const	battler[] = { "-front.jxl", "-back.jxl", "-fluorescent.jxl", NULL };
	static const char * const	gb[] = { "-front.jxl", "-back.jxl", NULL };

	return (reader == S9X_BARCODE_BATTLER ? battler : gb);
}

// The cards in one layout for every reader, built on first use.
static const std::vector<S9xBarcodeReaderCard> &CardList (S9xBarcodeReader reader)
{
	static std::vector<S9xBarcodeReaderCard>	lists[4];
	std::vector<S9xBarcodeReaderCard>			&list = lists[reader];

	if (!list.empty())
		return (list);

	int	count = 0;
	if (reader == S9X_BARCODE_BATTLER)
	{
		const S9xBarcodeCard	*cards = S9xBarcodeCards(&count);
		for (int i = 0; i < count; i++)
			list.push_back({ cards[i].set, NULL, cards[i].name, "", cards[i].code, cards[i].image });
	}
	else if (reader == S9X_BARCODE_BARCODEBOY || reader == S9X_BARCODE_BARDIGUN)
	{
		const SGB::BarcodeBoyCard	*cards = reader == S9X_BARCODE_BARCODEBOY ? SGB::BarcodeBoyCards(&count)
		                                                                      : SGB::BardigunCards(&count);
		for (int i = 0; i < count; i++)
			list.push_back({ cards[i].game, cards[i].group, cards[i].name, cards[i].kana, cards[i].code, cards[i].image });
	}

	return (list);
}

const S9xBarcodeReaderCard *S9xBarcodeReaderCards (S9xBarcodeReader reader, int *count)
{
	const std::vector<S9xBarcodeReaderCard>	&list = CardList(reader);

	*count = (int) list.size();
	return (list.empty() ? NULL : list.data());
}

const S9xBarcodeReaderCard *S9xBarcodeReaderCardFind (S9xBarcodeReader reader, const char *code)
{
	const std::vector<S9xBarcodeReaderCard>	&list = CardList(reader);

	if (reader == S9X_BARCODE_BATTLER)
	{
		// Its table, in the same order, knows the shorter forms of a code.
		int						count = 0;
		const S9xBarcodeCard	*cards = S9xBarcodeCards(&count);
		const S9xBarcodeCard	*card = S9xBarcodeCardFind(code);
		return (card && card - cards < (int) list.size() ? &list[card - cards] : NULL);
	}

	char	digits[14];
	int		n = 0;
	for (const char *c = code; c && *c; c++)
	{
		if (*c == ' ' || *c == '-')
			continue;
		if (n == 13)
			return (NULL);
		digits[n++] = *c;
	}
	digits[n] = 0;

	for (const S9xBarcodeReaderCard &card : list)
		if (!strcmp(card.code, digits))
			return (&card);

	return (NULL);
}

const char *S9xBarcodeReaderOpenBranch (S9xBarcodeReader reader)
{
	switch (reader)
	{
		case S9X_BARCODE_BARCODEBOY:	return (S9xSGBBarcodeBoyGame());
		case S9X_BARCODE_BARDIGUN:		return (S9xSGBBardigunGame());
		default:						return (NULL);
	}
}

int S9xBarcodeReaderBars (const char *code, bool bars[95])
{
	char	digits[14];
	int		n = 0;

	for (const char *c = code; c && *c; c++)
	{
		if (*c == ' ' || *c == '-')
			continue;
		if (*c < '0' || *c > '9' || n == 13)
			return (0);
		digits[n++] = *c;
	}

	// A UPC-A is an EAN-13 with a leading zero.
	if (n == 12)
	{
		memmove(digits + 1, digits, 12);
		digits[0] = '0';
		n = 13;
	}
	digits[n] = 0;

	return (SGB::EanBars(digits, bars));
}
