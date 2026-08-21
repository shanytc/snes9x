/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifdef UNZIP_SUPPORT

#include <assert.h>
#include <ctype.h>
#include "snes9x.h"
#ifdef SYSTEM_ZIP
#include <minizip/unzip.h>
#else
#include "unzip/unzip.h"
#endif
#include "memmap.h"


// Extensions that mark an entry as an actual cartridge dump. MSU-1 packs ship
// the ROM next to .pcm tracks that are tens of megabytes yet still under
// MAX_ROM_SIZE, so "largest entry wins" hands back an audio track and the game
// loads as garbage. A recognized ROM extension outranks raw size whenever the
// archive has one.
static bool is_rom_entry (const char *name)
{
	static const char	*rom_ext[] = { ".smc", ".sfc", ".swc", ".fig", ".078",
	                                   ".bin", ".gd3", ".bs", ".st", ".gb",
	                                   ".gbc", ".sgb", NULL };
	int	len = strlen(name);

	for (int i = 0; rom_ext[i] != NULL; i++)
	{
		int	l = strlen(rom_ext[i]);

		if (len > l && strcasecmp(name + len - l, rom_ext[i]) == 0)
			return (true);
	}

	return (false);
}

bool8 LoadZip (const char *zipname, uint32 *TotalFileSize, uint8 *buffer,
               uint32 buffer_size)
{
	*TotalFileSize = 0;

	unzFile	file = unzOpen(zipname);
	if (file == NULL)
		return (FALSE);

	// find the largest file with a ROM extension, else the largest file in the
	// zip file (under MAX_ROM_SIZE), or a file with extension .1, or a file
	// named program.rom
	char	filename[132];
	uint32	filesize = 0;
	char	rom_filename[132];
	uint32	rom_filesize = 0;
	bool	forced = false;
	int		port = unzGoToFirstFile(file);

	unz_file_info	info;

	while (port == UNZ_OK)
	{
		char	name[132];
		unzGetCurrentFileInfo(file, &info, name, 128, NULL, 0, NULL, 0);

		if (info.uncompressed_size > CMemory::MAX_ROM_SIZE + 512)
		{
			port = unzGoToNextFile(file);
			continue;
		}

		if (info.uncompressed_size > filesize)
		{
			strcpy(filename, name);
			filesize = info.uncompressed_size;
		}

		if (is_rom_entry(name) && info.uncompressed_size > rom_filesize)
		{
			strcpy(rom_filename, name);
			rom_filesize = info.uncompressed_size;
		}

		int	len = strlen(name);
		if (len > 2 && name[len - 2] == '.' && name[len - 1] == '1')
		{
			strcpy(filename, name);
			filesize = info.uncompressed_size;
			forced = true;
			break;
		}

		if (strncasecmp(name, "program.rom", 11) == 0)
		{
			strcpy(filename, name);
			filesize = info.uncompressed_size;
			forced = true;
			break;
		}

		port = unzGoToNextFile(file);
	}

	// split dumps and program.rom pick themselves; otherwise a ROM extension
	// wins over a merely bigger entry
	if (!forced && rom_filesize != 0)
	{
		strcpy(filename, rom_filename);
		filesize = rom_filesize;
	}

	int len = strlen(zipname);
	bool	msu1_pack = (len > 5 && strcasecmp(zipname + len - 5, ".msu1") == 0);

	// A .msu1 pack must hold the cartridge as well as the audio: bsnes-style
	// packs name it program.rom, but accept a plain ROM extension too so a
	// downloaded MSU-1 .zip only needs renaming, not repacking.
	if (!(port == UNZ_END_OF_LIST_OF_FILE || port == UNZ_OK) || filesize == 0 ||
		(msu1_pack && strcasecmp(filename, "program.rom") != 0 && !is_rom_entry(filename)))
	{
		if (unzClose(file) != UNZ_OK)
			assert(FALSE);
		return (FALSE);
	}

	// find extension
	char	tmp[2] = { 0, 0 };
	char	*ext = strrchr(filename, '.');
	if (ext)
		ext++;
	else
		ext = tmp;

	uint8	*ptr = buffer;
	bool8	more = FALSE;

	unzLocateFile(file, filename, 0);
	unzGetCurrentFileInfo(file, &info, filename, 128, NULL, 0, NULL, 0);

	if (unzOpenCurrentFile(file) != UNZ_OK)
	{
		unzClose(file);
		return (FALSE);
	}

	do
	{
		assert(info.uncompressed_size <= CMemory::MAX_ROM_SIZE + 512);

		uint32 FileSize = info.uncompressed_size;
		uint32 used = ptr - buffer;
		if (used > buffer_size || FileSize > buffer_size - used)
		{
			unzCloseCurrentFile(file);
			unzClose(file);
			return (FALSE);
		}
		int	l = unzReadCurrentFile(file, ptr, FileSize);

		if (unzCloseCurrentFile(file) == UNZ_CRCERROR)
		{
			unzClose(file);
			return (FALSE);
		}

		if (l <= 0 || l != (int) FileSize)
		{
			unzClose(file);
			return (FALSE);
		}

		FileSize = Memory.HeaderRemove(FileSize, ptr);
		ptr += FileSize;
		*TotalFileSize += FileSize;

		int	len;

		if (ptr - buffer < buffer_size && (isdigit(ext[0]) && ext[1] == 0 && ext[0] < '9'))
		{
			more = TRUE;
			ext[0]++;
		}
		else
		if (ptr - buffer < buffer_size)
		{
			if (ext == tmp)
				len = strlen(filename);
			else
				len = ext - filename - 1;

			if ((len == 7 || len == 8) && strncasecmp(filename, "sf", 2) == 0 &&
				isdigit(filename[2]) && isdigit(filename[3]) && isdigit(filename[4]) &&
				isdigit(filename[5]) && isalpha(filename[len - 1]))
			{
				more = TRUE;
				filename[len - 1]++;
			}
		}
		else
			more = FALSE;

		if (more)
		{
			if (unzLocateFile(file, filename, 0) != UNZ_OK ||
				unzGetCurrentFileInfo(file, &info, filename, 128, NULL, 0, NULL, 0) != UNZ_OK ||
				unzOpenCurrentFile(file) != UNZ_OK)
				break;
		}
	} while (more);

	unzClose(file);

	return (TRUE);
}

#endif
