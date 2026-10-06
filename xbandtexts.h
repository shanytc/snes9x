// Dial-screen texts each server stores in the box's SRAM database (Retrocomputing.Network's from
// roofgarden's send_nuxtpatches). Ours are padded to their sizes so either set fits the same item.

enum { kStringItemType = 49, kWriteableStringType = 70 };	// box DBTypes

static const struct XBandBoxText
{
	uint8_t		type, id;		// kStringItemType items start with an x, y word pair
	uint8_t		pos[4];
	const char	*retro, *local;
} kXBandBoxTexts[] =
{
	{ kStringItemType, 72, { 0x00, 0x00, 0x00, 0x00 }, "Sending connection request\xc9", "Sending connection request\xc9" },
	{ kStringItemType, 73, { 0x00, 0xd4, 0x00, 0xa5 }, "Dialing\xc9", "Dialing\xc9" },
	{ kStringItemType, 74, { 0x00, 0xd4, 0x00, 0xa5 }, "Connected to Retrocomputing.Network!", "Connected to local server!" },
	{ kStringItemType, 80, { 0x00, 0xd4, 0x00, 0xa5 }, "Connection failed. Redialing\xc9", "Local server busy. Redialing\xc9" },
	{ kWriteableStringType, 192, { 0 }, "Welcome to Retrocomputing.Network!", "Welcome to your local server!" },
	{ kWriteableStringType, 193, { 0 }, "Brought to you by Retrocomputing.Network", "Hosted with SuperSnes9x" },
};

// The box's own phone number (gBoxID->boxPhoneNumber, msSetBoxPhoneNumber): phone setup and the
// "You are dialing from ..." register dialog show it.
static const char *const kXBandBoxPhoneRetro = "Retrocomputing.Network";
static const char *const kXBandBoxPhoneLocal = "Local Server";

// The item's data: position (strings only), text, then NULs up to the Retrocomputing size.
static inline size_t XBandBoxTextData (const XBandBoxText &t, bool local, uint8_t *out)
{
	const size_t pos = t.type == kStringItemType ? 4 : 0;
	const size_t size = pos + strlen(t.retro) + 1;
	const char *text = local ? t.local : t.retro;
	memset(out, 0, size);
	memcpy(out, t.pos, pos);
	memcpy(out + pos, text, strlen(text));
	return size;
}
