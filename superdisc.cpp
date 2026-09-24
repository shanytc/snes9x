/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Super Disc (issue #286), after nocash's fullsnes notes and the BIOS's own
// code. The mechacon is modelled at the level the BIOS talks to it: every
// nibble written to $21E1 is answered by one nibble and an IRQ, and a
// command's last answer is the drive state. The CXD1800 decoder stores each
// sector the drive plays into its 32K buffer and raises DECINT; the BIOS
// then points the decoder's DMA at the sector and pulls it through $21E3.

#include "snes9x.h"
#include "memmap.h"
#include "superdisc.h"
#include "apu/resampler.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <string>
#include <vector>

#define SECTOR_RAW			2352
#define SECTOR_STORED		0x924		// raw sector minus its 12 sync bytes
#define PREGAP				150			// LBA 0 is MSF 00:02:00

#define MECH_REPLY_LINES	8			// nibble turnaround, ~0.5ms
#define TOC_TICKS			60			// sectors spent reading the TOC
#define CDDA_FIFO_FRAMES	(588 * 8)
#define XA_FIFO_FRAMES		(4032 * 4)

enum { TRACK_AUDIO = 0, TRACK_MODE1, TRACK_MODE2 };

struct SDiscTrack
{
	int		number;
	int		type;
	int		sector_size;	// bytes per sector in the image file
	int		file;			// index into SDiscImage::files
	int32	file_lba;		// LBA of the file's first sector
	int32	start;			// LBA of index 01
	int32	pregap;			// LBA of index 00 (== start when there is none)
	int32	gap;			// sectors of a cue PREGAP, which the image leaves out
	int32	end;			// first LBA past the track
};

struct SDiscImage
{
	std::string					path;
	std::string					name;		// ISO volume identifier, else the file name
	std::vector<FILE *>			files;
	std::vector<SDiscTrack>		tracks;
	int32						leadout;
};

// Everything a savestate carries. Plain data only.
struct SSuperDisc
{
	// board
	uint8	Control;			// $21E4: bit3 mechacon IRQ, bit2 decoder IRQ
	uint8	SRAMUnlockNext;		// next $21E0 value of the unlock ramp, 0 = not armed
	uint8	SRAMUnlocked;

	// mechacon link
	uint8	MechReady;			// $21E1.7
	uint8	MechNibble;			// $21E1.3-0
	uint8	MechPending;		// answer waiting out MECH_REPLY_LINES
	uint8	MechPendingValid;
	int32	MechDelay;
	uint8	MechPhase;			// MECH_*
	uint8	MechCmd[8];
	uint8	MechCmdLen;
	uint8	MechCmdWant;		// nibbles the command takes before its F
	uint8	MechData[16];
	uint8	MechDataLen;
	uint8	MechDataPos;

	// drive
	uint8	DriveState;
	uint8	DiscPresent;
	uint8	DoubleSpeed;
	uint8	PlayAfterSeek;
	uint8	AutoPause;			// 0 continuous, 1 track, 2 index
	int32	HeadLBA;
	int32	SeekTarget;
	int32	BusyTicks;
	uint32	SectorPhase;

	// CXD1800
	uint8	RegAdr;
	uint8	DrvIf, ChipCtl, DecCtl, IntMask, IntSts, CI, PLBA;
	uint8	DmaEn;
	uint16	DmaAdr, DmaXfr, DrvAdr, CMAdr;
	uint8	Sts, HdrFlg, Mdfm, AdpCI;
	uint8	Hdr[4], SubHdr[4];
	uint8	SectorIsXA;
	int32	XAOld[2], XAOlder[2];

	uint8	Buffer[SDISC_BUFFER_SIZE];
	uint8	DRAM[SDISC_DRAM_SIZE];
};

enum { MECH_IDLE = 0, MECH_PARAMS, MECH_DATA };

static SSuperDisc	SD;
static SDiscImage	Disc;
static bool8		Active = FALSE;
static char			BIOSVersion[16];	// "0.95", from the boot ROM's banner

static Resampler	*AudioOut = NULL;
static int16		CddaFifo[CDDA_FIFO_FRAMES * 2];
static int			CddaHead, CddaCount;
static bool			CddaPrimed;
static int16		XAFifo[XA_FIFO_FRAMES * 2];
static int			XAHead, XACount;
static int			XARate = 37800;
static uint32		XAPhase;
static int16		XACur[2], XAPrev[2];
static uint32		OutPhase;

static const uint8	SyncPattern[12] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };

static inline uint8 ToBCD (int v)	{ return (uint8) (((v / 10) << 4) | (v % 10)); }
static inline int FromBCD (uint8 v)	{ return (v >> 4) * 10 + (v & 15); }

// ---------------------------------------------------------------------------
// Disc images: .cue (multi-track, one or more BINs), .iso and bare .bin

static std::string DirOf (const std::string &path)
{
	size_t	slash = path.find_last_of("/\\");
	return (slash == std::string::npos) ? std::string() : path.substr(0, slash + 1);
}

static bool HasExt (const char *path, const char *ext)
{
	const char	*dot = strrchr(path, '.');
	if (!dot)
		return (false);
	for (; *dot && *ext; dot++, ext++)
		if (tolower((unsigned char) *dot) != *ext)
			return (false);
	return (*dot == 0 && *ext == 0);
}

static long FileLength (FILE *f)
{
	long	pos = ftell(f), len;
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, pos, SEEK_SET);
	return (len);
}

static void CloseDisc (void)
{
	for (size_t i = 0; i < Disc.files.size(); i++)
		if (Disc.files[i])
			fclose(Disc.files[i]);
	Disc.files.clear();
	Disc.tracks.clear();
	Disc.path.clear();
	Disc.name.clear();
	Disc.leadout = 0;
}

static int32 ParseMSF (const char *s)
{
	int	m = 0, sec = 0, f = 0;
	if (sscanf(s, "%d:%d:%d", &m, &sec, &f) != 3)
		return (-1);
	return ((m * 60 + sec) * 75 + f);
}

static bool OpenCue (const char *path)
{
	FILE	*cue = fopen(path, "rb");
	if (!cue)
		return (false);

	const std::string	dir = DirOf(path);
	char				line[1024];
	int32				file_lba = 0, pregap_total = 0;
	long				file_bytes = 0;
	int					sector_size = SECTOR_RAW;
	bool				ok = true;

	while (ok && fgets(line, sizeof line, cue))
	{
		char	*p = line;
		while (*p == ' ' || *p == '\t')
			p++;

		if (!strncmp(p, "FILE", 4))
		{
			// The previous file's sectors end where this one's begin.
			if (!Disc.files.empty())
				file_lba += (int32) (file_bytes / sector_size);

			char	*q1 = strchr(p, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
			std::string	name;
			if (q1 && q2)
				name.assign(q1 + 1, q2 - q1 - 1);
			else
			{
				char	buf[512] = { 0 };
				sscanf(p + 4, "%511s", buf);
				name = buf;
			}

			FILE	*f = fopen((dir + name).c_str(), "rb");
			if (!f)
				f = fopen(name.c_str(), "rb");
			if (!f)
			{
				ok = false;
				break;
			}
			Disc.files.push_back(f);
			file_bytes = FileLength(f);
		}
		else
		if (!strncmp(p, "TRACK", 5))
		{
			SDiscTrack	t;
			char		mode[32] = { 0 };
			memset(&t, 0, sizeof t);
			if (Disc.files.empty() || sscanf(p + 5, "%d %31s", &t.number, mode) != 2)
			{
				ok = false;
				break;
			}

			if (!strcmp(mode, "AUDIO"))				{ t.type = TRACK_AUDIO; t.sector_size = SECTOR_RAW; }
			else if (!strcmp(mode, "MODE1/2352"))	{ t.type = TRACK_MODE1; t.sector_size = SECTOR_RAW; }
			else if (!strcmp(mode, "MODE1/2048"))	{ t.type = TRACK_MODE1; t.sector_size = 2048; }
			else if (!strcmp(mode, "MODE2/2352"))	{ t.type = TRACK_MODE2; t.sector_size = SECTOR_RAW; }
			else if (!strcmp(mode, "MODE2/2336"))	{ t.type = TRACK_MODE2; t.sector_size = 2336; }
			else
			{
				ok = false;
				break;
			}

			// Mixed sector sizes inside one BIN are not a thing; the first
			// track of a file sets how its length divides.
			if (Disc.tracks.empty() || Disc.tracks.back().file != (int) Disc.files.size() - 1)
				sector_size = t.sector_size;

			t.file = (int) Disc.files.size() - 1;
			t.file_lba = file_lba + pregap_total;
			t.start = t.pregap = -1;
			Disc.tracks.push_back(t);
		}
		else
		if (!strncmp(p, "PREGAP", 6) && !Disc.tracks.empty())
		{
			// A gap the image leaves out: it shifts everything after it.
			int32	len = ParseMSF(p + 6);
			if (len > 0)
			{
				SDiscTrack	&t = Disc.tracks.back();
				pregap_total += len;
				t.file_lba += len;
				t.gap = len;
			}
		}
		else
		if (!strncmp(p, "INDEX", 5) && !Disc.tracks.empty())
		{
			int		idx = -1;
			char	msf[32] = { 0 };
			if (sscanf(p + 5, "%d %31s", &idx, msf) != 2)
				continue;
			int32	at = ParseMSF(msf);
			if (at < 0)
				continue;

			SDiscTrack	&t = Disc.tracks.back();
			if (idx == 0 && t.pregap < 0)
				t.pregap = file_lba + pregap_total + at;
			else
			if (idx == 1)
				t.start = file_lba + pregap_total + at;
		}
	}
	fclose(cue);

	if (ok && !Disc.files.empty())
		file_lba += (int32) (file_bytes / sector_size);

	if (!ok || Disc.tracks.empty())
		return (false);

	for (size_t i = 0; i < Disc.tracks.size(); i++)
	{
		SDiscTrack	&t = Disc.tracks[i];
		if (t.start < 0)
			return (false);
		if (t.gap)
			t.pregap = t.start - t.gap;
		else
		if (t.pregap < 0 || t.pregap > t.start)
			t.pregap = t.start;
	}
	for (size_t i = 0; i < Disc.tracks.size(); i++)
		Disc.tracks[i].end = (i + 1 < Disc.tracks.size()) ? Disc.tracks[i + 1].pregap
														   : file_lba + pregap_total;
	Disc.leadout = Disc.tracks.back().end;
	return (true);
}

// A single-track image: .iso is cooked 2048-byte sectors, a .bin that opens
// with a sync mark is raw 2352-byte ones.
static bool OpenFlat (const char *path)
{
	FILE	*f = fopen(path, "rb");
	if (!f)
		return (false);

	uint8	head[16] = { 0 };
	size_t	got = fread(head, 1, sizeof head, f);
	long	len = FileLength(f);

	SDiscTrack	t;
	memset(&t, 0, sizeof t);
	t.number = 1;
	if (got == sizeof head && !memcmp(head, SyncPattern, 12) && len % SECTOR_RAW == 0)
	{
		t.sector_size = SECTOR_RAW;
		t.type = (head[15] == 2) ? TRACK_MODE2 : TRACK_MODE1;
	}
	else
	if (len % 2048 == 0)
	{
		t.sector_size = 2048;
		t.type = TRACK_MODE1;
	}
	else
	{
		fclose(f);
		return (false);
	}

	Disc.files.push_back(f);
	t.end = (int32) (len / t.sector_size);
	Disc.tracks.push_back(t);
	Disc.leadout = t.end;
	return (true);
}

static bool OpenDisc (const char *path)
{
	CloseDisc();
	bool	ok = HasExt(path, ".cue") ? OpenCue(path) : OpenFlat(path);
	if (!ok || Disc.leadout <= 0)
	{
		CloseDisc();
		return (false);
	}
	Disc.path = path;
	return (true);
}

static const SDiscTrack *TrackAt (int32 lba)
{
	for (size_t i = 0; i < Disc.tracks.size(); i++)
		if (lba >= Disc.tracks[i].pregap && lba < Disc.tracks[i].end)
			return (&Disc.tracks[i]);
	// The lead-in before track 1 belongs to it.
	if (!Disc.tracks.empty() && lba < Disc.tracks[0].pregap)
		return (&Disc.tracks[0]);
	return (NULL);
}

// Fills a full 2352-byte sector. Sectors the image does not hold (the
// lead-in, a PREGAP) come back as empty sectors of the track's mode.
static void ReadRawSector (int32 lba, uint8 *raw, int *type)
{
	const SDiscTrack	*t = TrackAt(lba);
	*type = t ? t->type : TRACK_MODE1;
	memset(raw, 0, SECTOR_RAW);

	bool	have = false;
	if (t && lba >= t->file_lba && !(t->gap && lba < t->start))
	{
		FILE	*f = Disc.files[t->file];
		long	off = (long) (lba - t->file_lba) * t->sector_size;
		if (fseek(f, off, SEEK_SET) == 0)
		{
			if (t->sector_size == SECTOR_RAW)
				have = fread(raw, 1, SECTOR_RAW, f) == SECTOR_RAW;
			else
			if (t->sector_size == 2336)
				have = fread(raw + 16, 1, 2336, f) == 2336;
			else
				have = fread(raw + 16, 1, 2048, f) == 2048;
		}
	}

	if (*type == TRACK_AUDIO)
		return;

	// Cooked sectors and gaps get the sync and header the disc would carry.
	if (!have || !t || t->sector_size != SECTOR_RAW)
	{
		const int32	a = lba + PREGAP;
		memcpy(raw, SyncPattern, 12);
		raw[12] = ToBCD(a / (60 * 75));
		raw[13] = ToBCD((a / 75) % 60);
		raw[14] = ToBCD(a % 75);
		raw[15] = (*type == TRACK_MODE2) ? 2 : 1;
	}
}

// The volume identifier from the ISO descriptor at sector 16 (bytes 28h-47h),
// falling back to the image's file name when the disc leaves it blank.
static void ReadDiscName (void)
{
	uint8	raw[SECTOR_RAW];
	int		type;
	ReadRawSector(16, raw, &type);

	const uint8	*vd = raw + ((raw[15] == 2) ? 24 : 16);
	std::string	name;
	if (type != TRACK_AUDIO && vd[0] == 0x01 && !memcmp(vd + 1, "CD001", 5))
		for (int i = 0x28; i < 0x48; i++)
			name += (vd[i] > 0x20 && vd[i] < 0x7F && vd[i] != '_') ? (char) vd[i] : ' ';	// ISO names spell spaces as '_'

	size_t	end = name.find_last_not_of(" _");
	name = (end == std::string::npos) ? std::string() : name.substr(0, end + 1);
	if (name.empty())
	{
		size_t	slash = Disc.path.find_last_of("/\\");
		name = Disc.path.substr(slash == std::string::npos ? 0 : slash + 1);
		size_t	dot = name.rfind('.');
		if (dot != std::string::npos)
			name.erase(dot);
	}
	Disc.name = name;
}

// ---------------------------------------------------------------------------
// Audio: CD-DA straight off the disc, XA-ADPCM from the decoder

static void CddaPush (const uint8 *raw)
{
	for (int i = 0; i < 588; i++)
	{
		if (CddaCount >= CDDA_FIFO_FRAMES)
			return;
		int	pos = (CddaHead + CddaCount) % CDDA_FIFO_FRAMES;
		CddaFifo[pos * 2 + 0] = (int16) (raw[i * 4 + 0] | (raw[i * 4 + 1] << 8));
		CddaFifo[pos * 2 + 1] = (int16) (raw[i * 4 + 2] | (raw[i * 4 + 3] << 8));
		CddaCount++;
	}
}

static void XAPush (int16 l, int16 r)
{
	if (XACount >= XA_FIFO_FRAMES)
		return;
	int	pos = (XAHead + XACount) % XA_FIFO_FRAMES;
	XAFifo[pos * 2 + 0] = l;
	XAFifo[pos * 2 + 1] = r;
	XACount++;
}

static void ClearAudio (void)
{
	CddaHead = CddaCount = 0;
	CddaPrimed = false;
	XAHead = XACount = 0;
	XAPhase = OutPhase = 0;
	XACur[0] = XACur[1] = XAPrev[0] = XAPrev[1] = 0;
}

// psx-spx "XA-ADPCM": 18 portions of 128 bytes, each 16 header bytes and
// 28 words carrying eight 4-bit (or four 8-bit) sound units.
static void DecodeXASector (const uint8 *stored, uint8 ci)
{
	static const int	pos[4] = { 0, 60, 115, 98 };
	static const int	neg[4] = { 0, 0, -52, -55 };

	const bool	stereo = (ci & 0x01) != 0;
	const bool	eight = (ci & 0x10) != 0;
	const int	units = eight ? 4 : 8;
	const uint8	*data = stored + 12;
	int16		out[2][28 * 8];
	int			count[2];

	XARate = (ci & 0x04) ? 18900 : 37800;

	for (int g = 0; g < 18; g++)
	{
		const uint8	*grp = data + g * 128;
		count[0] = count[1] = 0;

		for (int u = 0; u < units; u++)
		{
			const int	ch = stereo ? (u & 1) : 0;
			const uint8	param = grp[4 + u];
			int			shift = (eight ? 8 : 12) - (param & 0x0F);
			const int	filter = (param >> 4) & 3;
			if (shift < 0)
				shift = 0;

			for (int j = 0; j < 28; j++)
			{
				int		t;
				if (eight)
					t = (int8) grp[16 + u + j * 4];
				else
				{
					int	nib = (grp[16 + (u >> 1) + j * 4] >> ((u & 1) * 4)) & 0x0F;
					t = (nib & 8) ? nib - 16 : nib;
				}

				int	s = (t << shift) + ((SD.XAOld[ch] * pos[filter] + SD.XAOlder[ch] * neg[filter] + 32) >> 6);
				if (s > 32767)	s = 32767;
				if (s < -32768)	s = -32768;
				SD.XAOlder[ch] = SD.XAOld[ch];
				SD.XAOld[ch] = s;
				out[ch][count[ch]++] = (int16) s;
			}
		}

		if (stereo)
			for (int i = 0; i < count[0] && i < count[1]; i++)
				XAPush(out[0][i], out[1][i]);
		else
			for (int i = 0; i < count[0]; i++)
				XAPush(out[0][i], out[0][i]);
	}
}

void S9xSuperDiscSetOutput (Resampler *resampler)
{
	AudioOut = resampler;
}

void S9xSuperDiscGenerate (size_t sample_count)
{
	if (!AudioOut)
		return;

	OutPhase += 4410 * (uint32) (sample_count / 2);
	while (OutPhase >= 3204)
	{
		OutPhase -= 3204;
		int32	l = 0, r = 0;

		// Hold off after an underrun until a sector's worth is queued, so a
		// late sector reads as one gap rather than a stream of clicks.
		if (!CddaPrimed && CddaCount >= 588 * 2)
			CddaPrimed = true;
		if (CddaPrimed)
		{
			if (CddaCount)
			{
				l = CddaFifo[CddaHead * 2 + 0];
				r = CddaFifo[CddaHead * 2 + 1];
				CddaHead = (CddaHead + 1) % CDDA_FIFO_FRAMES;
				CddaCount--;
			}
			else
				CddaPrimed = false;
		}

		XAPhase += XARate;
		while (XAPhase >= 44100)
		{
			XAPhase -= 44100;
			XAPrev[0] = XACur[0];
			XAPrev[1] = XACur[1];
			if (XACount)
			{
				XACur[0] = XAFifo[XAHead * 2 + 0];
				XACur[1] = XAFifo[XAHead * 2 + 1];
				XAHead = (XAHead + 1) % XA_FIFO_FRAMES;
				XACount--;
			}
			else
				XACur[0] = XACur[1] = 0;
		}
		l += XAPrev[0] + (int32) (XACur[0] - XAPrev[0]) * (int32) XAPhase / 44100;
		r += XAPrev[1] + (int32) (XACur[1] - XAPrev[1]) * (int32) XAPhase / 44100;

		if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
		if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
		AudioOut->push_sample((int16) l, (int16) r);
	}
}

// ---------------------------------------------------------------------------
// IRQ line and the battery RAM lock

static void UpdateIRQ (void)
{
	bool	mech = (SD.Control & 0x08) && SD.MechReady;
	bool	dec = (SD.Control & 0x04) && (SD.IntSts & SD.IntMask);
	CPU.IRQExternal = (mech || dec) ? TRUE : FALSE;
}

void S9xSuperDiscRemapSRAM (void)
{
	if (!Active)
		return;
	for (int blk = 0x8; blk <= 0x9; blk++)
	{
		const int	p = (0x90 << 4) | blk;
		Memory.WriteMap[p] = SD.SRAMUnlocked ? Memory.Map[p] : (uint8 *) CMemory::MAP_NONE;
	}
}

static void LockSRAM (bool8 locked)
{
	SD.SRAMUnlocked = locked ? 0 : 1;
	S9xSuperDiscRemapSRAM();
}

// ---------------------------------------------------------------------------
// Drive

static void EnterIdleState (void)
{
	SD.DriveState = SD.DiscPresent ? SDISC_STOP : SDISC_NO_DISC;
	SD.BusyTicks = 0;
	SD.PlayAfterSeek = 0;
}

static void CloseTray (void)
{
	if (SD.DiscPresent)
	{
		SD.DriveState = SDISC_READ_TOC;
		SD.BusyTicks = TOC_TICKS;
		SD.HeadLBA = 0;
	}
	else
		EnterIdleState();
}

static void StartSeek (int32 lba)
{
	if (!SD.DiscPresent)
		return;
	if (lba < -PREGAP)
		lba = -PREGAP;
	if (lba > Disc.leadout)
		lba = Disc.leadout;

	int32	dist = lba - SD.HeadLBA;
	if (dist < 0)
		dist = -dist;
	int32	ticks = 8 + dist / 4000;
	if (ticks > 30)
		ticks = 30;
	if (SD.DriveState == SDISC_STOP)
		ticks += 20;	// spin-up

	SD.SeekTarget = lba;
	SD.BusyTicks = ticks;
	SD.DriveState = SDISC_SEEK;
	SD.PlayAfterSeek = 0;
}

static bool TrackStart (int track, int index, int32 *lba)
{
	for (size_t i = 0; i < Disc.tracks.size(); i++)
		if (Disc.tracks[i].number == track)
		{
			*lba = (index == 0) ? Disc.tracks[i].pregap : Disc.tracks[i].start;
			return (true);
		}
	return (false);
}

// `raw` is null when there is nothing to sync to; the chip still raises
// DECINT with NOSYNC, which the BIOS's sectors-per-second self-test counts.
static void DecoderSector (const uint8 *raw)
{
	const uint8	mode = SD.DecCtl & 7;
	if (mode < 2 || mode > 6 || (SD.ChipCtl & 0x08))
		return;

	if (!raw)
	{
		SD.Sts = 0x01;			// NOSYNC
		SD.HdrFlg = 0xFF;
		SD.SectorIsXA = 0;
		SD.IntSts |= 0x10;
		UpdateIRQ();
		return;
	}

	memcpy(SD.Hdr, raw + 12, 4);
	memcpy(SD.SubHdr, raw + 16, 4);
	SD.Sts = 0x0C;				// EDC and ECC fine
	SD.HdrFlg = 0x00;
	SD.CMAdr = SD.DrvAdr;

	const uint8	rawmode = raw[15];
	const bool	mode2 = rawmode == 2;
	const bool	form2 = mode2 && (raw[18] & 0x20);
	SD.Mdfm = (uint8) (((rawmode & 0xFC) ? 0x10 : 0) | ((rawmode & 3) << 2) |
					   (mode2 ? 0x02 : 0) | (form2 ? 0x01 : 0));

	const uint8	ci = (SD.DecCtl & 0x80) ? raw[19] : SD.CI;
	SD.AdpCI = (uint8) ((ci & 0x55) | ((mode2 && (raw[18] & 0x01)) ? 0x20 : 0));
	SD.SectorIsXA = mode2 && form2 && (raw[18] & 0x04);

	// Write modes store the sector; monitor-only just shows the header.
	if (mode >= 4)
	{
		for (int i = 0; i < SECTOR_STORED; i++)
			SD.Buffer[(SD.DrvAdr + i) & (SDISC_BUFFER_SIZE - 1)] = raw[12 + i];
		SD.DrvAdr = (uint16) ((SD.DrvAdr + SECTOR_STORED) & (SDISC_BUFFER_SIZE - 1));
	}

	SD.IntSts |= 0x10;			// DECINT
	UpdateIRQ();
}

static void DriveTick (void)
{
	if (SD.BusyTicks > 0 && --SD.BusyTicks == 0)
	{
		if (SD.DriveState == SDISC_READ_TOC)
			SD.DriveState = SDISC_STOP;
		else
		if (SD.DriveState == SDISC_SEEK)
		{
			// Settle a couple of sectors early, as the BIOS expects.
			SD.HeadLBA = SD.SeekTarget - 2;
			if (SD.HeadLBA < -PREGAP)
				SD.HeadLBA = -PREGAP;
			SD.DriveState = SD.PlayAfterSeek ? SDISC_PLAY : SDISC_PAUSE;
			SD.PlayAfterSeek = 0;
		}
	}

	switch (SD.DriveState)
	{
		case SDISC_PLAY:
		{
			if (SD.HeadLBA >= Disc.leadout)
			{
				SD.DriveState = SDISC_PAUSE;
				DecoderSector(NULL);
				break;
			}

			uint8	raw[SECTOR_RAW];
			int		type;
			ReadRawSector(SD.HeadLBA, raw, &type);

			if (type == TRACK_AUDIO)
			{
				CddaPush(raw);
				DecoderSector(NULL);
			}
			else
				DecoderSector(raw);

			const SDiscTrack	*before = TrackAt(SD.HeadLBA);
			SD.HeadLBA++;
			const SDiscTrack	*after = TrackAt(SD.HeadLBA);
			if (SD.AutoPause && before && after != before)
				SD.DriveState = SDISC_PAUSE;
			break;
		}

		case SDISC_FAST_FWD:
		case SDISC_SLOW_FWD:
			SD.HeadLBA += (SD.DriveState == SDISC_FAST_FWD) ? 10 : 1;
			if (SD.HeadLBA >= Disc.leadout)
			{
				SD.HeadLBA = Disc.leadout;
				SD.DriveState = SDISC_PAUSE;
			}
			DecoderSector(NULL);
			break;

		case SDISC_FAST_REV:
		case SDISC_SLOW_REV:
			SD.HeadLBA -= (SD.DriveState == SDISC_FAST_REV) ? 10 : 1;
			if (SD.HeadLBA < 0)
			{
				SD.HeadLBA = 0;
				SD.DriveState = SDISC_PAUSE;
			}
			DecoderSector(NULL);
			break;

		default:
			DecoderSector(NULL);
			break;
	}
}

// ---------------------------------------------------------------------------
// Mechacon

static void MechAnswer (uint8 nibble)
{
	SD.MechPending = nibble & 0x0F;
	SD.MechPendingValid = 1;
	SD.MechDelay = MECH_REPLY_LINES;
}

// Sub-Q as eight BCD bytes: track, index, relative MSF, absolute MSF.
static void BuildSubQ (void)
{
	const SDiscTrack	*t = SD.DiscPresent ? TrackAt(SD.HeadLBA) : NULL;
	int32				apos = SD.HeadLBA + PREGAP, rel = 0;
	uint8				q[8];

	memset(q, 0, sizeof q);
	if (t)
	{
		rel = SD.HeadLBA - t->start;
		if (rel < 0)
			rel = -rel;
		q[0] = ToBCD(t->number);
		q[1] = (SD.HeadLBA < t->start) ? 0 : 1;
	}
	if (apos < 0)
		apos = 0;
	q[2] = ToBCD(rel / (60 * 75));
	q[3] = ToBCD((rel / 75) % 60);
	q[4] = ToBCD(rel % 75);
	q[5] = ToBCD(apos / (60 * 75));
	q[6] = ToBCD((apos / 75) % 60);
	q[7] = ToBCD(apos % 75);

	for (int i = 0; i < 8; i++)
	{
		SD.MechData[i * 2 + 0] = q[i] >> 4;
		SD.MechData[i * 2 + 1] = q[i] & 15;
	}
	SD.MechDataLen = 16;
}

// Digit 0 is the disc type, not the track under the head: the BIOS checks
// it mid-seek and on every poll of a load, wherever the head happens to be.
static void BuildStatus (void)
{
	const bool	data = SD.DiscPresent && !Disc.tracks.empty() && Disc.tracks[0].type != TRACK_AUDIO;
	memset(SD.MechData, 0, 5);
	SD.MechData[0] = data ? 1 : 0;
	SD.MechData[2] = SD.DriveState;
	SD.MechDataLen = 5;
}

// Runs a command once its terminating F has arrived. Returns true when a
// data phase (Q-data, status) follows.
static bool MechExecute (void)
{
	const uint8	*c = SD.MechCmd;

	switch (c[0])
	{
		case 0xB:		// access MM:SS:FF
		{
			int32	msf = (FromBCD((uint8) (c[1] << 4 | c[2])) * 60 +
						   FromBCD((uint8) (c[3] << 4 | c[4]))) * 75 +
						   FromBCD((uint8) (c[5] << 4 | c[6]));
			StartSeek(msf - PREGAP);
			return (false);
		}

		case 0xC:		// access track/index
		{
			int32	lba;
			if (TrackStart(FromBCD((uint8) (c[1] << 4 | c[2])), FromBCD((uint8) (c[3] << 4 | c[4])), &lba))
				StartSeek(lba);
			return (false);
		}

		case 0xD:
			break;

		default:
			return (false);
	}

	const uint8	op = (uint8) (c[1] << 4 | c[2]);
	const bool	spinning = SD.DiscPresent && SD.DriveState != SDISC_TRAY_OPEN &&
						   SD.DriveState != SDISC_READ_TOC;

	switch (op)
	{
		case 0x01:		// stop
			if (SD.DriveState != SDISC_TRAY_OPEN)
				EnterIdleState();
			break;

		case 0x02:		// play
			if (SD.DriveState == SDISC_SEEK)
				SD.PlayAfterSeek = 1;
			else
			if (spinning)
				SD.DriveState = SDISC_PLAY;
			break;

		case 0x03:		// pause
			if (SD.DriveState == SDISC_SEEK)
				SD.PlayAfterSeek = 0;
			else
			if (spinning)
				SD.DriveState = SDISC_PAUSE;
			break;

		case 0x04:		// open/close
			if (SD.DriveState == SDISC_TRAY_OPEN)
				CloseTray();
			else
			{
				SD.DriveState = SDISC_TRAY_OPEN;
				SD.BusyTicks = 0;
			}
			break;

		case 0x10: if (spinning) SD.DriveState = SDISC_FAST_FWD; break;
		case 0x11: if (spinning) SD.DriveState = SDISC_FAST_REV; break;
		case 0x12: if (spinning) SD.DriveState = SDISC_SLOW_FWD; break;
		case 0x13: if (spinning) SD.DriveState = SDISC_SLOW_REV; break;

		case 0x42: SD.AutoPause = 0; break;
		case 0x43: SD.AutoPause = 1; break;
		case 0x44: SD.AutoPause = 2; break;
		case 0x45: SD.DoubleSpeed = 0; break;
		case 0x46: SD.DoubleSpeed = 1; break;

		case 0x50:
			BuildSubQ();
			return (true);

		case 0x51:
			BuildStatus();
			return (true);

		default:		// key direct/ignore and the unknown D14/D15
			break;
	}
	return (false);
}

static void MechWrite (uint8 nibble)
{
	nibble &= 0x0F;

	switch (SD.MechPhase)
	{
		case MECH_IDLE:
			SD.MechCmd[0] = nibble;
			SD.MechCmdLen = 1;
			switch (nibble)
			{
				case 0xB: SD.MechCmdWant = 6; break;
				case 0xC: SD.MechCmdWant = 4; break;
				case 0xD: SD.MechCmdWant = 2; break;
				case 0xF:		// flush / no-op
					MechAnswer(0xA);
					return;
				default:  SD.MechCmdWant = 0; break;
			}
			SD.MechPhase = MECH_PARAMS;
			MechAnswer(0xF);
			return;

		case MECH_PARAMS:
			if (SD.MechCmdLen <= SD.MechCmdWant && nibble != 0xF)
			{
				SD.MechCmd[SD.MechCmdLen++] = nibble;
				MechAnswer(0xF);
				return;
			}
			if (SD.MechCmdLen <= SD.MechCmdWant)
			{
				// Terminated early: drop it.
				SD.MechPhase = MECH_IDLE;
				MechAnswer(SD.DriveState);
				return;
			}
			if (MechExecute())
			{
				SD.MechPhase = MECH_DATA;
				SD.MechDataPos = 0;
				MechAnswer(0xF);
			}
			else
			{
				SD.MechPhase = MECH_IDLE;
				MechAnswer(SD.DriveState);
			}
			return;

		case MECH_DATA:
			if (SD.MechDataPos < SD.MechDataLen)
			{
				MechAnswer(SD.MechData[SD.MechDataPos++]);
				return;
			}
			SD.MechPhase = MECH_IDLE;
			MechAnswer(SD.DriveState);
			return;
	}
}

// ---------------------------------------------------------------------------
// CXD1800 registers

static void DecoderReset (void)
{
	SD.RegAdr = 0;
	SD.ChipCtl = SD.DecCtl = SD.IntMask = SD.IntSts = SD.CI = SD.PLBA = 0;
	SD.DmaEn = 0;
	SD.DmaAdr = SD.DmaXfr = SD.DrvAdr = SD.CMAdr = 0;
	SD.Sts = SD.HdrFlg = SD.Mdfm = SD.AdpCI = 0;
	memset(SD.Hdr, 0, 4);
	memset(SD.SubHdr, 0, 4);
	SD.SectorIsXA = 0;
}

static inline void StepRegAdr (void)
{
	if (SD.RegAdr & 0x0F)
		SD.RegAdr = (uint8) ((SD.RegAdr & 0x10) | ((SD.RegAdr + 1) & 0x0F));
}

static uint8 DecoderRead (void)
{
	const uint8	r = SD.RegAdr & 0x1F;
	uint8		v = 0;

	switch (r)
	{
		case 0x00:
			v = SD.Buffer[SD.DmaAdr & (SDISC_BUFFER_SIZE - 1)];
			SD.DmaAdr = (uint16) ((SD.DmaAdr + 1) & 0x7FFF);
			if (SD.DmaEn && SD.DmaXfr)
			{
				SD.DmaXfr--;
				if (!SD.DmaXfr)
				{
					SD.IntSts |= 0x20;	// DMACMP
					UpdateIRQ();
				}
			}
			return (v);		// DMADATA never steps REGADR

		case 0x01: v = SD.IntSts; break;
		case 0x02: v = SD.Sts; break;
		case 0x03: v = SD.HdrFlg; break;
		case 0x04: case 0x14: v = SD.Hdr[0]; break;
		case 0x05: case 0x15: v = SD.Hdr[1]; break;
		case 0x06: case 0x16: v = SD.Hdr[2]; break;
		case 0x07: case 0x17: v = SD.Hdr[3]; break;
		case 0x08: v = SD.SubHdr[0]; break;
		case 0x09: v = SD.SubHdr[1]; break;
		case 0x0A: v = SD.SubHdr[2]; break;
		case 0x0B: v = SD.SubHdr[3]; break;
		case 0x0C: v = (uint8) SD.CMAdr; break;
		case 0x0D: v = (uint8) (SD.CMAdr >> 8); break;
		case 0x0E: case 0x1E: v = SD.Mdfm; break;
		case 0x0F: case 0x1F: v = SD.AdpCI; break;
		case 0x18: v = (uint8) SD.DmaXfr; break;
		case 0x19: v = (uint8) (SD.DmaXfr >> 8); break;
		case 0x1A: v = (uint8) SD.DmaAdr; break;
		case 0x1B: v = (uint8) (SD.DmaAdr >> 8); break;
		case 0x1C: v = (uint8) SD.DrvAdr; break;
		case 0x1D: v = (uint8) (SD.DrvAdr >> 8); break;
		default:   v = 0; break;
	}

	StepRegAdr();
	return (v);
}

static void DecoderWrite (uint8 byte)
{
	const uint8	r = SD.RegAdr & 0x1F;
	const uint8	w = (r >= 0x10 && r <= 0x1C) ? r - 0x10 : r;

	switch (w)
	{
		case 0x01: SD.DrvIf = byte; break;

		case 0x02:
			if (byte & 0x10)
			{
				DecoderReset();
				UpdateIRQ();
				return;
			}
			// ADPEN plays the sector the last DECINT delivered.
			SD.ChipCtl = byte & 0x0F;
			if ((byte & 0x01) && SD.SectorIsXA)
			{
				uint8	stored[SECTOR_STORED];
				for (int i = 0; i < SECTOR_STORED; i++)
					stored[i] = SD.Buffer[(SD.CMAdr + i) & (SDISC_BUFFER_SIZE - 1)];
				DecodeXASector(stored, SD.AdpCI);
				SD.SectorIsXA = 0;
			}
			break;

		case 0x03: SD.DecCtl = byte; break;
		case 0x04: SD.IntMask = byte; UpdateIRQ(); break;
		case 0x05: SD.IntSts &= ~byte; UpdateIRQ(); break;
		case 0x06: SD.CI = byte; break;
		case 0x07: SD.DmaAdr = (uint16) ((SD.DmaAdr & 0x7F00) | byte); break;
		case 0x08: SD.DmaAdr = (uint16) ((SD.DmaAdr & 0x00FF) | ((byte & 0x7F) << 8)); break;
		case 0x09: SD.DmaXfr = (uint16) ((SD.DmaXfr & 0x0F00) | byte); break;

		case 0x0A:
		{
			SD.DmaXfr = (uint16) ((SD.DmaXfr & 0x00FF) | ((byte & 0xF0) << 4));
			SD.DmaEn = (byte >> 3) & 1;
			// The BIOS re-arms every sector without dropping DMAEN in
			// between, so any write that sets it selects DMADATA.
			if (SD.DmaEn)
			{
				SD.RegAdr = 0;
				return;
			}
			break;
		}

		case 0x0B: SD.DrvAdr = (uint16) ((SD.DrvAdr & 0x7F00) | byte); break;
		case 0x0C: SD.DrvAdr = (uint16) ((SD.DrvAdr & 0x00FF) | ((byte & 0x7F) << 8)); break;
		case 0x0D: SD.PLBA = byte; break;
		default:   break;
	}

	StepRegAdr();
}

// ---------------------------------------------------------------------------
// Bus

uint8 S9xGetSuperDisc (uint16 address)
{
	switch (address)
	{
		case 0x21E1:
		{
			uint8	v = (uint8) ((SD.MechReady ? 0x80 : 0) | SD.MechNibble);
			if (SD.MechReady)
			{
				SD.MechReady = 0;
				UpdateIRQ();
			}
			return (v);
		}

		case 0x21E2:
			return (SD.RegAdr);

		case 0x21E3:
			return (DecoderRead());

		default:
			return (OpenBus);
	}
}

void S9xSetSuperDisc (uint8 byte, uint16 address)
{
	switch (address)
	{
		case 0x21D0:		// lock
			SD.SRAMUnlockNext = 0;
			LockSRAM(TRUE);
			break;

		case 0x21E5:		// unlock step 1, and a lock of its own
			LockSRAM(TRUE);
			SD.SRAMUnlockNext = (byte == 0xFF) ? 0x0F : 0;
			break;

		case 0x21E0:		// unlock step 2: 0Fh down to 01h
			if (SD.SRAMUnlockNext && byte == SD.SRAMUnlockNext)
			{
				if (--SD.SRAMUnlockNext == 0)
					LockSRAM(FALSE);
			}
			else
				SD.SRAMUnlockNext = 0;
			break;

		case 0x21E1:
			MechWrite(byte);
			break;

		case 0x21E2:
			SD.RegAdr = byte & 0x1F;
			break;

		case 0x21E3:
			DecoderWrite(byte);
			break;

		case 0x21E4:
			SD.Control = byte;
			UpdateIRQ();
			break;

		default:
			break;
	}
}

// ---------------------------------------------------------------------------
// Lifecycle

bool8 S9xSuperDiscIsBIOS (const uint8 *rom, uint32 size)
{
	if (size != SDISC_BIOS_SIZE)
		return (FALSE);
	for (int i = 0x7FC0; i < 0x7FE0; i++)
		if (rom[i] != 0xFF)
			return (FALSE);
	return (rom[0x7FEA] == 0xF8 && rom[0x7FEB] == 0x1F &&
			rom[0x7FEE] == 0xFC && rom[0x7FEF] == 0x1F &&
			rom[0x7FFC] == 0x00 && rom[0x7FFD] == 0x80) ? TRUE : FALSE;
}

bool8 S9xSuperDiscIsDiscImage (const char *path)
{
	if (!path || !*path)
		return (FALSE);
	if (HasExt(path, ".cue"))
		return (TRUE);

	const bool	iso = HasExt(path, ".iso");
	if (!iso && !HasExt(path, ".bin"))
		return (FALSE);

	// A plain .bin is usually a cart dump; only a raw CD image, sync mark
	// and ISO volume descriptor included, diverts.
	FILE	*f = fopen(path, "rb");
	if (!f)
		return (FALSE);

	uint8	head[16] = { 0 }, vd[8] = { 0 };
	bool	cd = false;
	if (fread(head, 1, sizeof head, f) == sizeof head)
	{
		if (!memcmp(head, SyncPattern, 12))
		{
			long	at = 16L * SECTOR_RAW + ((head[15] == 2) ? 24 : 16);
			cd = fseek(f, at, SEEK_SET) == 0 && fread(vd, 1, 6, f) == 6 && !memcmp(vd + 1, "CD001", 5);
		}
		else
		if (iso)
			cd = fseek(f, 16L * 2048, SEEK_SET) == 0 && fread(vd, 1, 6, f) == 6 && !memcmp(vd + 1, "CD001", 5);
	}
	fclose(f);
	return (cd ? TRUE : FALSE);
}

uint8 *S9xSuperDiscDRAM (void)
{
	return (SD.DRAM);
}

static void ResetChips (void)
{
	SD.Control = 0;
	SD.SRAMUnlockNext = 0;
	LockSRAM(TRUE);

	SD.MechReady = SD.MechNibble = 0;
	SD.MechPendingValid = 0;
	SD.MechDelay = 0;
	SD.MechPhase = MECH_IDLE;
	SD.MechCmdLen = SD.MechDataLen = SD.MechDataPos = 0;

	DecoderReset();
	SD.XAOld[0] = SD.XAOld[1] = SD.XAOlder[0] = SD.XAOlder[1] = 0;
	ClearAudio();
}

// "Super Disc boot ROM ver.0.95 ..." sits near the start of the BIOS.
static void ReadBIOSVersion (void)
{
	static const char	tag[] = "boot ROM ver.";
	strcpy(BIOSVersion, "?");
	for (uint32 i = 0; i + sizeof tag + 8 < SDISC_BIOS_SIZE; i++)
		if (!memcmp(Memory.ROM + i, tag, sizeof tag - 1))
		{
			const uint8	*v = Memory.ROM + i + sizeof tag - 1;
			int			n = 0;
			while (n < (int) sizeof BIOSVersion - 1 && (isdigit(v[n]) || v[n] == '.'))
			{
				BIOSVersion[n] = (char) v[n];
				n++;
			}
			BIOSVersion[n] = 0;
			if (!n)
				strcpy(BIOSVersion, "?");
			return;
		}
}

void S9xSuperDiscActivate (void)
{
	Active = TRUE;
	ReadBIOSVersion();
	memset(&SD, 0, sizeof SD);
	SD.DiscPresent = !Disc.tracks.empty();
	ResetChips();
	EnterIdleState();
}

void S9xSuperDiscDeactivate (void)
{
	if (Active)
		CPU.IRQExternal = FALSE;
	Active = FALSE;
	CloseDisc();
	ClearAudio();
}

void S9xSuperDiscPowerOn (void)
{
	if (!Active)
		return;
	ResetChips();
	SD.DoubleSpeed = 0;
	SD.AutoPause = 0;
	SD.SectorPhase = 0;
	SD.HeadLBA = 0;
	SD.DiscPresent = !Disc.tracks.empty();
	CloseTray();
	UpdateIRQ();
}

void S9xSuperDiscSoftReset (void)
{
	if (!Active)
		return;
	ResetChips();
	UpdateIRQ();
}

void S9xSuperDiscEndScanline (void)
{
	if (!Active)
		return;

	if (SD.MechPendingValid && --SD.MechDelay <= 0)
	{
		SD.MechPendingValid = 0;
		SD.MechNibble = SD.MechPending;
		SD.MechReady = 1;
		UpdateIRQ();
	}

	// Sectors come at 75Hz (150Hz double speed) of real time, whatever the
	// console's own clock.
	const uint32	master = Settings.PAL ? 21281370 : 21477272;
	SD.SectorPhase += (uint32) Timings.H_Max * (SD.DoubleSpeed ? 150 : 75);
	while (SD.SectorPhase >= master)
	{
		SD.SectorPhase -= master;
		DriveTick();
	}
}

bool8 S9xSuperDiscInsertDisc (const char *path)
{
	if (!OpenDisc(path))
	{
		SD.DiscPresent = 0;
		if (Active)
			EnterIdleState();
		return (FALSE);
	}
	ReadDiscName();
	if (!Active)
		return (TRUE);

	SD.DiscPresent = 1;
	ClearAudio();
	CloseTray();
	return (TRUE);
}

void S9xSuperDiscEjectDisc (void)
{
	CloseDisc();
	ClearAudio();
	if (!Active)
		return;
	SD.DiscPresent = 0;
	SD.DriveState = SDISC_TRAY_OPEN;
	SD.BusyTicks = 0;
	SD.PlayAfterSeek = 0;
}

bool8 S9xSuperDiscHasDisc (void)
{
	return (Disc.tracks.empty() ? FALSE : TRUE);
}

const char *S9xSuperDiscDiscPath (void)
{
	return (Disc.path.c_str());
}

const char *S9xSuperDiscTitle (void)
{
	static std::string	title;
	title = std::string("Super Disc (v") + BIOSVersion + ")";
	if (!Disc.name.empty())
		title += " - " + Disc.name;
	return (title.c_str());
}

// ---------------------------------------------------------------------------
// Savestates: "SDC!", version, payload size, then the SSuperDisc image. The
// disc itself stays whatever is in the drive.

#define SDISC_STATE_HEADER	12
#define SDISC_STATE_VERSION	1

size_t S9xSuperDiscStateSize (void)
{
	return (SDISC_STATE_HEADER + sizeof(SSuperDisc));
}

void S9xSuperDiscStateSave (uint8 *buf)
{
	const uint32	payload = (uint32) sizeof(SSuperDisc);
	memcpy(buf, "SDC!", 4);
	buf[4] = SDISC_STATE_VERSION;
	buf[5] = buf[6] = buf[7] = 0;
	buf[8]  = (uint8) payload;
	buf[9]  = (uint8) (payload >> 8);
	buf[10] = (uint8) (payload >> 16);
	buf[11] = (uint8) (payload >> 24);
	memcpy(buf + SDISC_STATE_HEADER, &SD, sizeof SD);
}

bool8 S9xSuperDiscStateLoad (const uint8 *buf, size_t size)
{
	if (size < SDISC_STATE_HEADER || memcmp(buf, "SDC!", 4) != 0 || buf[4] != SDISC_STATE_VERSION)
		return (FALSE);
	const size_t	payload = (size_t) buf[8] | ((size_t) buf[9] << 8) |
							  ((size_t) buf[10] << 16) | ((size_t) buf[11] << 24);
	if (payload != sizeof(SSuperDisc) || size < SDISC_STATE_HEADER + payload)
		return (FALSE);

	memcpy(&SD, buf + SDISC_STATE_HEADER, sizeof SD);

	// A state taken with a disc in the drive, restored without one, finds
	// the tray empty.
	if (SD.DiscPresent && Disc.tracks.empty())
	{
		SD.DiscPresent = 0;
		SD.DriveState = SDISC_NO_DISC;
	}

	ClearAudio();
	S9xSuperDiscRemapSRAM();
	UpdateIRQ();
	return (TRUE);
}
