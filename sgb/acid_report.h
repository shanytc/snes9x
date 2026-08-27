/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _SGB_ACID_REPORT_H_
#define _SGB_ACID_REPORT_H_

// Exports for an Acid Tests run, in the three formats the GB Emulator
// Shootout produces: its plain results table, the test/result JSON its
// build.py consumes, and a standalone HTML report with the captured frames
// embedded. Shared by the win32 dialog's Export button and the headless CLI.

#include <string>
#include <vector>

#include "acid.h"

namespace AcidTests {

enum class Format { Text, Json, Html };

// A test and how it went. `result` is null when the test was listed but
// never ran - filtered out, or dropped by a cancel.
struct ReportRow
{
	const Test   *test   = nullptr;
	const Result *result = nullptr;
};

struct ReportInfo
{
	// Emulator named in the header, and in the JSON's "emulator" field.
	std::string title = "SuperSnes9x SGB/GB/GBC core";
	std::string generated;      // timestamp; taken from the clock when empty
	std::string env;            // EnvOverrides(), warned about when set
	std::string filter = "all tests";       // Filter::Describe()
	std::string source;         // acid directory, for the header
	double      seconds   = 0.0;
	int         threads   = 1;
	bool        cancelled = false;
};

// Render a finished run. Returned as a string rather than written, so the
// caller can use whatever file API its platform needs for the path.
std::string RenderReport(Format fmt, const std::vector<ReportRow> &rows,
                         const ReportInfo &info);

// Render and write to `path`. False with a message in `err` on failure.
bool WriteReport(const char *path, Format fmt, const std::vector<ReportRow> &rows,
                 const ReportInfo &info, std::string &err);

// .json / .html by extension, Text otherwise.
Format FormatFromPath(const char *path);

// A 160x144 RGB frame as a PNG, for embedding a screenshot in a report.
std::vector<uint8_t> EncodePng(const uint8_t *rgb, int w, int h);

} // namespace AcidTests

#endif
