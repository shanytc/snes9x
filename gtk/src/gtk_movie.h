/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#pragma once

#include <cstdint>
#include <string>

/* The win32 "Play Movie" and "Record Movie" dialogs (File->Movie Play... and
 * File->Movie Record...). Each returns false when cancelled. */

struct MoviePlayChoice
{
    std::string path;
    bool read_only = true;
};

struct MovieRecordChoice
{
    std::string path;
    std::wstring metadata;
    uint8_t controllers_mask = 1;
    bool from_reset = false;
    bool clear_sram = false;
};

bool S9xPlayMovieDialog(MoviePlayChoice &choice);
/* sram_exists: whether the game has a battery save on disk, the thing the
 * "Clear SRAM" option would delete. */
bool S9xRecordMovieDialog(bool sram_exists, MovieRecordChoice &choice);

/* The message for a movie.cpp result code, as win32 words it. */
std::string S9xMovieErrorString(int result, bool brief);
