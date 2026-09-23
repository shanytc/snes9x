/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// Cygwin's posix_spawn forks, which deadlocks a threaded parent: spawn the
// SGB child through the Win32 API there too.
#if defined(_WIN32) || defined(__CYGWIN__)
#define ACID_WIN32_SPAWN 1
#elif !defined(_GNU_SOURCE)
#define _GNU_SOURCE   // pipe2/kill under a strict -std= build
#endif

#include "acid.h"
#include "acid_report.h"
#include "sgb.h"
#include "gb_memory.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <functional>
#include <utility>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <memory>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif
#ifdef ACID_WIN32_SPAWN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#ifdef __CYGWIN__
#include <sys/cygwin.h>
#include <unistd.h>
#endif
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "stb_image.h"

// The process environment, for reporting inherited ACID_* timing overrides.
#ifdef _WIN32
extern "C" char **_environ;
#define ACID_ENVIRON _environ
#else
extern "C" char **environ;
#define ACID_ENVIRON environ
#endif


namespace AcidTests {

namespace {

constexpr int GB_W = 160;
constexpr int GB_H = 144;
// The shootout's compareImage(): grayscale, per-pixel tolerance of 50.
constexpr int TOLERANCE = 50;

std::string JoinPath(const std::string &dir, const std::string &rel)
{
	if (dir.empty()) return rel;
	std::string p = dir;
	if (p.back() != '/' && p.back() != '\\') p += '/';
	return p + rel;
}

// PIL Image.convert("L") luma — ITU-R 601-2, integer-truncated.
inline uint8_t Luma(int r, int g, int b)
{
	return static_cast<uint8_t>((r * 299 + g * 587 + b * 114) / 1000);
}

// Load a reference PNG as a 160x144 grayscale buffer.
bool LoadReferenceGray(const std::string &path, std::vector<uint8_t> &out,
                       std::string &err)
{
	int w = 0, h = 0, comp = 0;
	unsigned char *px = stbi_load(path.c_str(), &w, &h, &comp, 3);
	if (!px)
	{
		err = "cannot read " + path;
		return false;
	}
	if (w != GB_W || h != GB_H)
	{
		stbi_image_free(px);
		err = path + " is not 160x144";
		return false;
	}
	out.resize(GB_W * GB_H);
	for (int i = 0; i < GB_W * GB_H; ++i)
		out[i] = Luma(px[i * 3], px[i * 3 + 1], px[i * 3 + 2]);
	stbi_image_free(px);
	return true;
}

// Snapshot the current GB frame as grayscale. DMG/SGB/compat frames are
// 2-bit shade indices (0 = lightest); native-CGB frames come from the raw
// BGR555 color framebuffer with full-range 5→8 bit expansion.
void CaptureFrameGray(SGB::Emulator &emu, uint8_t out[GB_W * GB_H])
{
	if (emu.IsCgbRender())
	{
		const uint16_t *fb = emu.CgbColorFB();
		for (int i = 0; i < GB_W * GB_H; ++i)
		{
			const uint16_t c = fb[i];
			const int r5 = c & 0x1F, g5 = (c >> 5) & 0x1F, b5 = (c >> 10) & 0x1F;
			out[i] = Luma((r5 << 3) | (r5 >> 2),
			              (g5 << 3) | (g5 >> 2),
			              (b5 << 3) | (b5 >> 2));
		}
	}
	else
	{
		const SGB::FrameBuffer &fb = emu.GetFrameBuffer();
		for (int i = 0; i < GB_W * GB_H; ++i)
			out[i] = static_cast<uint8_t>((3 - (fb.pixels[i] & 3)) * 85);
	}
}

// The same frame in colour, for the UI. Native-CGB output is real BGR555;
// everything else is 2-bit shades, which the reference images also show as
// gray.
void CaptureFrameRgb(SGB::Emulator &emu, std::vector<uint8_t> &out)
{
	out.resize(GB_W * GB_H * 3);
	if (emu.IsCgbRender())
	{
		const uint16_t *fb = emu.CgbColorFB();
		for (int i = 0; i < GB_W * GB_H; ++i)
		{
			const uint16_t c = fb[i];
			const int r5 = c & 0x1F, g5 = (c >> 5) & 0x1F, b5 = (c >> 10) & 0x1F;
			out[i * 3 + 0] = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
			out[i * 3 + 1] = static_cast<uint8_t>((g5 << 3) | (g5 >> 2));
			out[i * 3 + 2] = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));
		}
	}
	else
	{
		const SGB::FrameBuffer &fb = emu.GetFrameBuffer();
		for (int i = 0; i < GB_W * GB_H; ++i)
		{
			const uint8_t g = static_cast<uint8_t>((3 - (fb.pixels[i] & 3)) * 85);
			out[i * 3 + 0] = out[i * 3 + 1] = out[i * 3 + 2] = g;
		}
	}
}

// How far off the closest reference is, for a failure report: a handful of
// pixels reads very differently from a screen that never got there.
int ClosestDiff(const uint8_t *frame,
                const std::vector<std::vector<uint8_t>> &refs)
{
	int best = -1;
	for (const auto &ref : refs)
	{
		int n = 0;
		for (int i = 0; i < GB_W * GB_H; ++i)
		{
			const int d = static_cast<int>(frame[i]) - static_cast<int>(ref[i]);
			if (d > TOLERANCE || d < -TOLERANCE) ++n;
		}
		if (best < 0 || n < best) best = n;
	}
	return best;
}

bool FramesMatch(const uint8_t *a, const uint8_t *b)
{
	for (int i = 0; i < GB_W * GB_H; ++i)
	{
		const int d = static_cast<int>(a[i]) - static_cast<int>(b[i]);
		if (d > TOLERANCE || d < -TOLERANCE) return false;
	}
	return true;
}

std::string FlattenName(const std::string &name)
{
	std::string s = name;
	for (char &c : s)
		if (c == '/' || c == '\\' || c == ' ' || c == '(' || c == ')' || c == ',')
			c = '_';
	return s;
}

void DumpGrayPpm(const std::string &path, const uint8_t *gray)
{
	FILE *f = fopen(path.c_str(), "wb");
	if (!f) return;
	fprintf(f, "P5\n%d %d\n255\n", GB_W, GB_H);
	fwrite(gray, 1, GB_W * GB_H, f);
	fclose(f);
}

// Serial capture — blargg ROMs print their diagnosis here; keeping it makes
// failure reports self-explaining.
void OnSerialByte(void *user, uint8_t b)
{
	std::string *sink = static_cast<std::string *>(user);
	if (sink && sink->size() < 4096)
		sink->push_back(static_cast<char>(b));
}

std::string OneLine(const std::string &s, size_t cap)
{
	std::string r;
	for (char c : s)
	{
		if (r.size() >= cap) { r += "..."; break; }
		r += (c == '\n' || c == '\r') ? ' ' : c;
	}
	return r;
}

char Lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

std::string LowerCopy(const std::string &s)
{
	std::string r = s;
	for (char &c : r) c = Lower(c);
	return r;
}

bool ContainsFold(const std::string &hay, const std::string &needle)
{
	return LowerCopy(hay).find(LowerCopy(needle)) != std::string::npos;
}

// "mooneye/acceptance/boot_div-S.gb" -> "mooneye".
std::string SuiteOfName(const std::string &name)
{
	const size_t slash = name.find_first_of("/\\");
	return slash == std::string::npos ? std::string("other") : name.substr(0, slash);
}

bool ReadWholeFile(const std::string &path, std::vector<uint8_t> &out)
{
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) return false;
	fseek(f, 0, SEEK_END);
	const long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n <= 0) { fclose(f); return false; }
	out.resize(static_cast<size_t>(n));
	const size_t rd = fread(out.data(), 1, out.size(), f);
	fclose(f);
	return rd == out.size();
}

} // anonymous

std::string RomPath(const std::string &acid_dir, const std::string &rom)
{
	return JoinPath(JoinPath(acid_dir, kRomDir), rom);
}

std::string BaselinePath(const std::string &acid_dir, const std::string &name)
{
	return JoinPath(JoinPath(acid_dir, kBaselineDir), name);
}

std::string ImagePath(const std::string &acid_dir, const std::string &baseline,
                      const std::string &image)
{
	return JoinPath(BaselinePath(acid_dir, baseline), image);
}

const char *ModelName(Model m)
{
	return m == Model::CGB ? "CGB" : m == Model::SGB ? "SGB"
	     : m == Model::SGB2 ? "SGB2" : "DMG";
}

const char *StatusName(Status s)
{
	switch (s)
	{
		case Status::Pass: return "PASS";
		case Status::Fail: return "FAIL";
		case Status::Info: return "INFO";
		default:           return "ERROR";
	}
}

const char *MatchName(Match m)
{
	switch (m)
	{
		case Match::Same:    return "same";
		case Match::Differs: return "differs";
		case Match::NoImage: return "missing";
		case Match::NoFrame: return "noframe";
		case Match::NoRef:   return "noref";
		default:             return "none";
	}
}

int DiffPixels(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b,
               int count)
{
	if ((int)a.size() < count * 3 || (int)b.size() < count * 3) return -1;
	int n = 0;
	for (int i = 0; i < count; ++i)
	{
		const int ga = Luma(a[i * 3], a[i * 3 + 1], a[i * 3 + 2]);
		const int gb = Luma(b[i * 3], b[i * 3 + 1], b[i * 3 + 2]);
		if (ga - gb > TOLERANCE || gb - ga > TOLERANCE) ++n;
	}
	return n;
}

// Suites match on a substring so upstream's own names work - "mealybug"
// finds mealybug-tearoom-tests.
bool Filter::Matches(const Test &t) const
{
	if (models && !(models & ModelBit(t.model))) return false;
	if (!text.empty() && !ContainsFold(t.name, text)) return false;
	if (suites.empty()) return true;
	for (const std::string &s : suites)
		if (ContainsFold(t.suite, s)) return true;
	return false;
}

std::string Filter::Describe() const
{
	if (Empty()) return "all tests";
	std::string d;
	auto add = [&](const std::string &s) { if (!d.empty()) d += ", "; d += s; };
	if (!suites.empty())
	{
		std::string list;
		for (const std::string &s : suites)
		{
			if (!list.empty()) list += "+";
			list += s;
		}
		add("suite " + list);
	}
	if (models)
	{
		std::string list;
		for (Model m : { Model::DMG, Model::CGB, Model::SGB, Model::SGB2 })
		{
			if (!(models & ModelBit(m))) continue;
			if (!list.empty()) list += "+";
			list += ModelName(m);
		}
		add("model " + list);
	}
	if (!text.empty()) add("name contains \"" + text + "\"");
	return d;
}

std::vector<std::string> SuitesOf(const std::vector<Test> &tests)
{
	std::vector<std::string> out;
	for (const Test &t : tests)
		if (std::find(out.begin(), out.end(), t.suite) == out.end())
			out.push_back(t.suite);
	return out;
}

bool LoadManifest(const char *acid_dir, std::vector<Test> &out, std::string &err)
{
	const std::string path = JoinPath(acid_dir ? acid_dir : "acid", "manifest.txt");
	FILE *f = fopen(path.c_str(), "rb");
	if (!f)
	{
		err = "cannot open " + path;
		return false;
	}

	char line[2048];
	while (fgets(line, sizeof line, f))
	{
		std::string s = line;
		while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
		if (s.empty() || s[0] == '#') continue;

		// name|model|runtime|rom|pass;pass|fail;fail
		std::vector<std::string> fields;
		size_t start = 0;
		for (size_t i = 0; i <= s.size(); ++i)
		{
			if (i == s.size() || s[i] == '|')
			{
				fields.push_back(s.substr(start, i - start));
				start = i + 1;
			}
		}
		if (fields.size() < 5)
		{
			fclose(f);
			err = "malformed manifest line: " + s;
			return false;
		}

		Test t;
		t.name = fields[0];
		t.model = fields[1] == "CGB" ? Model::CGB
		        : fields[1] == "SGB" ? Model::SGB : Model::DMG;
		t.runtime = atof(fields[2].c_str());
		t.rom = fields[3];
		auto split = [](const std::string &v, std::vector<std::string> &dst) {
			size_t p = 0;
			while (p < v.size())
			{
				size_t q = v.find(';', p);
				if (q == std::string::npos) q = v.size();
				if (q > p) dst.push_back(v.substr(p, q - p));
				p = q + 1;
			}
		};
		split(fields[4], t.pass_images);
		if (fields.size() >= 6) split(fields[5], t.fail_images);
		t.suite = SuiteOfName(t.name);
		// The shootout has one SGB model; we test both BIOSes.
		if (t.model == Model::SGB)
		{
			Test twin = t;
			twin.model = Model::SGB2;
			twin.name += kSgb2Suffix;
			out.push_back(std::move(t));
			out.push_back(std::move(twin));
		}
		else
			out.push_back(std::move(t));
	}
	fclose(f);

	if (out.empty())
	{
		err = "no tests in " + path;
		return false;
	}
	return true;
}

namespace {

// SNES frames per second, the SGB child's clock.
constexpr double kSnesFps = 60.0988;
// Longest the SGB BIOS may take to hand the cart the GB (SGB2's splash is ~6.5 s).
constexpr double kBiosBootSecs = 30.0;

// A process whose stdout we read: the SGB test child.
class ChildProcess
{
public:
	~ChildProcess() { Stop(); }
	bool Start(const std::vector<std::string> &args, std::string &err);
	bool Read(void *buf, size_t n);   // exactly n bytes; false at EOF
	void Kill();                      // from another thread: unblocks Read
	void Stop();

private:
#ifdef ACID_WIN32_SPAWN
	HANDLE proc_ = NULL, rd_ = NULL;
#else
	pid_t pid_ = -1;
	int   rd_  = -1;
#endif
};

#ifdef ACID_WIN32_SPAWN
// Paths here are UTF-8, as everywhere in the core.
std::wstring Widen(const std::string &s)
{
	const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int) s.size(), NULL, 0);
	std::wstring w(n, L'\0');
	if (n) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int) s.size(), &w[0], n);
	return w;
}

// CommandLineToArgv quoting: backslashes double only before a quote.
void AppendQuoted(std::wstring &cmd, const std::wstring &arg)
{
	if (!cmd.empty()) cmd += L' ';
	cmd += L'"';
	size_t slashes = 0;
	for (wchar_t c : arg)
	{
		if (c == L'\\') { ++slashes; continue; }
		cmd.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
		slashes = 0;
		cmd += c;
	}
	cmd.append(slashes * 2, L'\\');
	cmd += L'"';
}

bool ChildProcess::Start(const std::vector<std::string> &args, std::string &err)
{
	SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
	HANDLE wr = NULL;
	if (!CreatePipe(&rd_, &wr, &sa, 1 << 16))
	{
		rd_ = NULL;
		err = "cannot create a pipe";
		return false;
	}
	SetHandleInformation(rd_, HANDLE_FLAG_INHERIT, 0);
	HANDLE nul = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE,
	                         FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);

	// Only this child's own handles are inherited: a parallel spawn must not
	// pick up another child's pipe end and hold it open.
	HANDLE inherit[2] = { wr, nul };
	SIZE_T attr_size = 0;
	InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
	std::vector<char> attr_buf(attr_size);
	LPPROC_THREAD_ATTRIBUTE_LIST attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
	const bool have_attrs = InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) != FALSE;
	bool ok = have_attrs && nul != INVALID_HANDLE_VALUE &&
	          UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
	                                    inherit, sizeof inherit, NULL, NULL);

	STARTUPINFOEXW si = {};
	si.StartupInfo.cb         = sizeof si;
	si.StartupInfo.dwFlags    = STARTF_USESTDHANDLES;
	si.StartupInfo.hStdInput  = nul;
	si.StartupInfo.hStdOutput = wr;
	si.StartupInfo.hStdError  = nul;
	si.lpAttributeList        = attrs;
	std::vector<std::wstring> argw;
	for (const std::string &a : args) argw.push_back(Widen(a));
	const wchar_t *cwd = NULL;
#ifdef __CYGWIN__
	// Win32 sees neither cygwin paths nor cygwin's working directory.
	std::wstring cwd_w;
	auto to_win = [](const char *posix) {
		wchar_t buf[4096] = {};
		return cygwin_conv_path(CCP_POSIX_TO_WIN_W, posix, buf, sizeof buf) == 0
		       ? std::wstring(buf) : Widen(posix);
	};
	argw[0] = to_win(args[0].c_str());
	char here[4096];
	if (getcwd(here, sizeof here)) { cwd_w = to_win(here); cwd = cwd_w.c_str(); }
#endif
	std::wstring cmd;
	for (const std::wstring &a : argw) AppendQuoted(cmd, a);
	std::vector<wchar_t> cmd_buf(cmd.begin(), cmd.end());
	cmd_buf.push_back(L'\0');
	// No application name: CreateProcess then finds the program from the
	// command line and supplies a missing ".exe".
	PROCESS_INFORMATION pi = {};
	ok = ok && CreateProcessW(NULL, cmd_buf.data(), NULL, NULL, TRUE,
	                          CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
	                          NULL, cwd, &si.StartupInfo, &pi);
	if (have_attrs) DeleteProcThreadAttributeList(attrs);
	CloseHandle(wr);
	if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
	if (!ok)
	{
		CloseHandle(rd_);
		rd_ = NULL;
		err = "cannot start " + args[0];
		return false;
	}
	CloseHandle(pi.hThread);
	proc_ = pi.hProcess;
	return true;
}

bool ChildProcess::Read(void *buf, size_t n)
{
	char *p = static_cast<char *>(buf);
	while (n)
	{
		DWORD got = 0;
		if (!ReadFile(rd_, p, (DWORD) n, &got, NULL) || !got) return false;
		p += got;
		n -= got;
	}
	return true;
}

void ChildProcess::Kill()
{
	if (proc_) TerminateProcess(proc_, 1);
}

void ChildProcess::Stop()
{
	// Closing our end first lets a still-running child's next write fail.
	if (rd_) { CloseHandle(rd_); rd_ = NULL; }
	if (proc_)
	{
		if (WaitForSingleObject(proc_, 0) == WAIT_TIMEOUT)
			TerminateProcess(proc_, 1);
		WaitForSingleObject(proc_, 5000);
		CloseHandle(proc_);
		proc_ = NULL;
	}
}
#else
bool ChildProcess::Start(const std::vector<std::string> &args, std::string &err)
{
	// Close-on-exec, so a parallel spawn never inherits this pipe; the dup2
	// onto the child's stdout clears the flag for that one copy.
	int fds[2];
	if (pipe2(fds, O_CLOEXEC) != 0)
	{
		err = "cannot create a pipe";
		return false;
	}
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
	posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
	std::vector<char *> av;
	for (const std::string &a : args) av.push_back(const_cast<char *>(a.c_str()));
	av.push_back(nullptr);
	const int rc = posix_spawn(&pid_, args[0].c_str(), &fa, nullptr, av.data(), ACID_ENVIRON);
	posix_spawn_file_actions_destroy(&fa);
	close(fds[1]);
	if (rc != 0)
	{
		close(fds[0]);
		pid_ = -1;
		err = "cannot start " + args[0];
		return false;
	}
	rd_ = fds[0];
	return true;
}

bool ChildProcess::Read(void *buf, size_t n)
{
	char *p = static_cast<char *>(buf);
	while (n)
	{
		const ssize_t got = read(rd_, p, n);
		if (got <= 0) return false;
		p += got;
		n -= (size_t) got;
	}
	return true;
}

void ChildProcess::Kill()
{
	if (pid_ > 0) kill(pid_, SIGKILL);
}

void ChildProcess::Stop()
{
	if (rd_ >= 0) { close(rd_); rd_ = -1; }
	if (pid_ > 0)
	{
		kill(pid_, SIGKILL);
		waitpid(pid_, nullptr, 0);
		pid_ = -1;
	}
}
#endif

// A worker's running SGB child, as the runner's main loop sees it: killed on
// cancel, or when it goes kChildStallSecs without a record.
constexpr double kChildStallSecs = 30.0;

struct ChildWatch
{
	std::mutex    mu;
	ChildProcess *child = nullptr;
	std::chrono::steady_clock::time_point last;
	bool          stalled = false;

	void Touch()
	{
		std::lock_guard<std::mutex> lk(mu);
		last = std::chrono::steady_clock::now();
	}
};

// Where a test's frames come from.
class FrameSource
{
public:
	virtual ~FrameSource() {}
	virtual bool Next() = 0;            // one more frame; false = gave up (see error)
	virtual bool Booting() const = 0;   // boot ROM / BIOS still owns the GB
	virtual void Gray(uint8_t *out) const = 0;
	virtual void Rgb(std::vector<uint8_t> &out) const = 0;
	std::string error;
};

// The worker's own GB core.
class CoreSource : public FrameSource
{
public:
	explicit CoreSource(SGB::Emulator &emu) : emu_(emu) {}
	bool Next() override { emu_.RunFrame(); return true; }
	bool Booting() const override { return emu_.BootROMMapped(); }
	void Gray(uint8_t *out) const override { CaptureFrameGray(emu_, out); }
	void Rgb(std::vector<uint8_t> &out) const override { CaptureFrameRgb(emu_, out); }

private:
	SGB::Emulator &emu_;
};

// An SGB child running the real BIOS; see acidsgb.h for the record stream.
class ChildSource : public FrameSource
{
public:
	ChildSource(ChildProcess &child, ChildWatch &watch, std::string &serial)
		: child_(child), watch_(watch), serial_(serial) {}

	bool Next() override
	{
		for (;;)
		{
			uint8_t tag = 0;
			if (!child_.Read(&tag, 1))
				return Stop("the SGB child exited early");
			watch_.Touch();
			if (tag == kChildFrame)
			{
				uint8_t flags = 0;
				if (!child_.Read(&flags, 1) || !child_.Read(px_, sizeof px_))
					return Stop("the SGB child exited mid-frame");
				booting_ = (flags & kChildBooting) != 0;
				return true;
			}
			if (tag == kChildSerial)
			{
				uint8_t b = 0;
				if (!child_.Read(&b, 1)) return Stop("the SGB child exited early");
				OnSerialByte(&serial_, b);
				continue;
			}
			if (tag == kChildError)
			{
				uint8_t len[2] = {};
				std::string msg;
				if (child_.Read(len, 2))
				{
					msg.resize(len[0] | (len[1] << 8));
					if (!msg.empty() && !child_.Read(&msg[0], msg.size())) msg.clear();
				}
				return Stop(msg.empty() ? "the SGB child failed" : msg);
			}
			return Stop("the SGB child sent an unknown record");
		}
	}
	bool Booting() const override { return booting_; }
	void Gray(uint8_t *out) const override
	{
		for (int i = 0; i < GB_W * GB_H; ++i)
			out[i] = static_cast<uint8_t>((3 - (px_[i] & 3)) * 85);
	}
	void Rgb(std::vector<uint8_t> &out) const override
	{
		out.resize(GB_W * GB_H * 3);
		for (int i = 0; i < GB_W * GB_H; ++i)
			out[i * 3 + 0] = out[i * 3 + 1] = out[i * 3 + 2] =
				static_cast<uint8_t>((3 - (px_[i] & 3)) * 85);
	}

private:
	bool Stop(const std::string &why) { error = why; return false; }

	ChildProcess &child_;
	ChildWatch   &watch_;
	std::string  &serial_;
	uint8_t       px_[GB_W * GB_H] = {};
	bool          booting_ = true;
};

// One test, start to finish, on one core (or one SGB child). Everything it
// touches is its own, so several of these run in parallel.
Result RunOneTest(SGB::Emulator &emu, const std::string &dir, const Test &t,
                  const std::vector<uint8_t> &boot_rom, const RunOptions &opts,
                  const std::atomic<bool> &cancel,
                  const std::function<void(int, int)> &tick, ChildWatch &watch,
                  bool &aborted)
{
	aborted = false;
	Result r;
	std::string serial;
	std::vector<std::vector<uint8_t>> pass_refs, fail_refs;
	bool ref_error = false;
	// Pass and fail screens are the suite's own verdict, so they always
	// come from baseline/default however many other baselines exist.
	for (const std::string &img : t.pass_images)
	{
		std::vector<uint8_t> g;
		if (!LoadReferenceGray(ImagePath(dir, kDefaultBaseline, img), g, r.detail))
		{ ref_error = true; break; }
		pass_refs.push_back(std::move(g));
	}
	for (const std::string &img : t.fail_images)
	{
		if (ref_error) break;
		std::vector<uint8_t> g;
		if (!LoadReferenceGray(ImagePath(dir, kDefaultBaseline, img), g, r.detail))
		{ ref_error = true; break; }
		fail_refs.push_back(std::move(g));
	}

	std::vector<uint8_t> rom;
	if (!ref_error && !ReadWholeFile(RomPath(dir, t.rom), rom))
	{
		ref_error = true;
		r.detail = "cannot read ROM " + t.rom;
	}
	if (ref_error)
	{
		r.status = Status::Error;
		return r;
	}

	const bool sgb2 = t.model == Model::SGB2;
	const std::string &bios = sgb2 ? opts.sgb2_bios : opts.sgb1_bios;
	const bool via_child = IsSgbModel(t.model) && !opts.sgb_child.empty() && !bios.empty();

	struct SinkGuard
	{
		SGB::Emulator *e = nullptr;
		~SinkGuard() { if (e) e->SetSerialSink(nullptr, nullptr); }
	} sink_guard;
	ChildProcess child;
	struct WatchGuard
	{
		ChildWatch &w;
		~WatchGuard() { std::lock_guard<std::mutex> lk(w.mu); w.child = nullptr; }
	} watch_guard{ watch };
	std::unique_ptr<FrameSource> src;
	// A BIOS run's budget starts when the BIOS hands the cart the GB.
	int frames_total = 0, boot_cap = 0;
	if (via_child)
	{
		frames_total = static_cast<int>(std::ceil((t.runtime + 6.0) * kSnesFps));
		boot_cap     = static_cast<int>(kBiosBootSecs * kSnesFps);
		const std::string &gb_boot = sgb2 ? opts.sgb2_boot : opts.sgb1_boot;
		std::string err;
		if (!child.Start({ opts.sgb_child, kSgbChildFlag, sgb2 ? "2" : "1",
		                   RomPath(dir, t.rom), bios, gb_boot.empty() ? "-" : gb_boot,
		                   opts.suppress_nrx ? "1" : "0",
		                   std::to_string(frames_total + boot_cap) }, err))
		{
			r.status = Status::Error;
			r.detail = err;
			return r;
		}
		src.reset(new ChildSource(child, watch, serial));
		std::lock_guard<std::mutex> lk(watch.mu);
		watch.child   = &child;
		watch.last    = std::chrono::steady_clock::now();
		watch.stalled = false;
	}
	else
	{
		// Model setup must precede LoadROM — LoadROM cold-resets with it.
		emu.SetForceModel(t.model == Model::CGB ? 2 : IsSgbModel(t.model) ? 3 : 1);
		emu.SetRunMode(t.model == Model::SGB ? SGB::RunMode::SGB
		             : sgb2 ? SGB::RunMode::SGB2 : SGB::RunMode::DMG);
		emu.SetClockMultiplier(1.0f);

		// Staged before LoadROM as well: its cold reset is what maps it.
		if (!emu.LoadBootROM(boot_rom.empty() ? nullptr : boot_rom.data(), boot_rom.size()))
		{
			r.status = Status::Error;
			r.detail = "core rejected the boot ROM";
			return r;
		}

		if (!emu.LoadROM(rom.data(), rom.size(), nullptr))
		{
			r.status = Status::Error;
			r.detail = "core rejected ROM " + t.rom;
			return r;
		}

		// SGB1 pushes ~61.2 GB frames per second; everything else 59.73.
		const double fps = (t.model == Model::SGB) ? 61.2 : 59.7275;
		// Match the shootout's wall clock: runtime + startup_time (1s) + 5s, plus
		// the boot ROM's own run when one is staged (DMG ~5.6s, CGB ~3.1s).
		const double boot_secs = boot_rom.empty() ? 0.0 : (t.model == Model::CGB ? 3.5 : 6.0);
		frames_total = static_cast<int>(std::ceil((t.runtime + 6.0 + boot_secs) * fps));

		emu.SetSerialSink(&OnSerialByte, &serial);
		sink_guard.e = &emu;
		src.reset(new CoreSource(emu));
	}

	uint8_t frame[GB_W * GB_H];
	r.status = pass_refs.empty() ? Status::Info : Status::Fail;
	int counted = 0, boot_frames = 0;
	while (counted < frames_total)
	{
		if (!src->Next())
		{
			if (cancel.load())
			{
				aborted = true;
				return r;
			}
			r.status = Status::Error;
			std::lock_guard<std::mutex> lk(watch.mu);
			r.detail = watch.stalled ? "the SGB child stopped sending frames" : src->error;
			break;
		}
		++r.frames;
		const bool booting = src->Booting();
		if (via_child && booting)
		{
			if (++boot_frames > boot_cap)
			{
				r.status = Status::Error;
				r.detail = "the SGB BIOS never handed the cart the Game Boy";
				break;
			}
		}
		else
			++counted;

		// The boot ROM's logo frames are nobody's reference.
		if (!booting && (!pass_refs.empty() || !fail_refs.empty()))
		{
			src->Gray(frame);
			bool decided = false;
			for (const auto &ref : pass_refs)
			{
				if (FramesMatch(frame, ref.data()))
				{
					r.status = Status::Pass;
					decided = true;
					break;
				}
			}
			if (!decided)
			{
				for (const auto &ref : fail_refs)
				{
					if (FramesMatch(frame, ref.data()))
					{
						r.status = Status::Fail;
						r.detail = "matched fail image";
						decided = true;
						break;
					}
				}
			}
			if (decided) break;
		}

		if ((r.frames & 63) == 63)
		{
			if (cancel.load(std::memory_order_relaxed))
			{
				aborted = true;
				return r;   // being cancelled; this one has no verdict
			}
			tick(counted, frames_total);
			while (opts.pause && opts.pause->load(std::memory_order_relaxed) &&
			       !cancel.load(std::memory_order_relaxed))
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
	}

	if (r.status == Status::Fail && r.detail.empty())
	{
		src->Gray(frame);
		const int off = ClosestDiff(frame, pass_refs);
		r.detail = "timeout after " + std::to_string(r.frames) + " frames";
		if (off >= 0)
			r.detail += "; " + std::to_string(off) + " px off the closest reference";
		if (!serial.empty())
			r.detail += "; serial: \"" + OneLine(serial, 160) + "\"";
	}
	if (r.status == Status::Fail && opts.dump_failures)
	{
		src->Gray(frame);
		DumpGrayPpm(JoinPath(dir, "_failures/" + FlattenName(t.name) + ".ppm"), frame);
	}
	if (r.frames > 0) src->Rgb(r.shot);
	return r;
}

} // anonymous

std::string EnvOverrides()
{
	std::string out;
	char **env = ACID_ENVIRON;
	for (; env && *env; ++env)
	{
		if (std::strncmp(*env, "ACID_", 5) != 0) continue;
		if (!out.empty()) out += ' ';
		out += *env;
	}
	return out;
}

int DefaultThreadCount()
{
	const unsigned hw = std::thread::hardware_concurrency();
	return hw ? static_cast<int>(hw) : 1;
}

Summary Run(const RunOptions &opts)
{
	Summary sum;
	const std::string dir = opts.acid_dir ? opts.acid_dir : "acid";

	std::vector<Test> tests;
	std::string err;
	if (!LoadManifest(dir.c_str(), tests, err))
	{
		fprintf(stderr, "acid: %s\n", err.c_str());
		return sum;
	}
	if (!opts.filter.Empty())
	{
		std::vector<Test> kept;
		for (auto &t : tests)
			if (opts.filter.Matches(t)) kept.push_back(std::move(t));
		tests.swap(kept);
	}
	return RunTests(tests, opts);
}

Summary RunTests(const std::vector<Test> &tests, const RunOptions &opts,
                 std::vector<Result> *out_results)
{
	Summary sum;
	const std::string dir = opts.acid_dir ? opts.acid_dir : "acid";
	if (tests.empty()) return sum;

	const auto wall_start = std::chrono::steady_clock::now();

	if (opts.dump_failures)
	{
#ifdef _WIN32
		_mkdir(JoinPath(dir, "_failures").c_str());
#else
		mkdir(JoinPath(dir, "_failures").c_str(), 0755);
#endif
	}

	int nthreads = opts.threads > 0 ? opts.threads : DefaultThreadCount();
	if (nthreads > static_cast<int>(tests.size())) nthreads = static_cast<int>(tests.size());
	if (nthreads < 1) nthreads = 1;

	// Longest test first: the slowest ROM decides when the last core goes
	// idle, so starting it early keeps the tail short.
	std::vector<size_t> order(tests.size());
	for (size_t i = 0; i < order.size(); ++i) order[i] = i;
	std::stable_sort(order.begin(), order.end(),
	                 [&](size_t a, size_t b) { return tests[a].runtime > tests[b].runtime; });

	std::vector<Result> results(tests.size());
	std::vector<char>   ran(tests.size(), 0);   // false for cancelled-away tests
	std::atomic<size_t> next_test{0};
	std::atomic<bool>   cancel{false};
	std::atomic<int>    live{nthreads};
	std::mutex          done_mu;
	// What the workers have to say, in the order they said it.
	struct Event { size_t index; int kind; int done; int total; };
	enum { EvStart, EvRunning, EvFinished };
	std::vector<Event> events;

	// Every worker owns a core of its own; the scoped bind keeps the GB
	// side's host hooks pointed at it instead of the singleton.
	std::vector<ChildWatch> watches(nthreads);
	auto worker = [&](int w) {
		SGB::Emulator emu;
		SGB::ScopedActiveEmulator bind(emu);
		emu.SetSuppressNrxGlitches(opts.suppress_nrx ? 1 : 0);
		// Pinned, not read live: the emulator keeps running beside the suite.
		emu.SetHostBiosMode(0);
		emu.SetHostMute(1);
		if (emu.Init())
		{
			for (;;)
			{
				const size_t slot = next_test.fetch_add(1);
				if (slot >= order.size() || cancel.load()) break;
				const size_t i = order[slot];
				{
					std::lock_guard<std::mutex> lk(done_mu);
					events.push_back({ i, EvStart, 0, 0 });
				}
				auto tick = [&](int done, int total) {
					std::lock_guard<std::mutex> lk(done_mu);
					events.push_back({ i, EvRunning, done, total });
				};
				static const std::vector<uint8_t> kNoBoot;
				const std::vector<uint8_t> &boot =
					tests[i].model == Model::DMG ? opts.dmg_boot :
					tests[i].model == Model::CGB ? opts.cgb_boot : kNoBoot;
				bool aborted = false;
				Result r = RunOneTest(emu, dir, tests[i], boot, opts, cancel, tick,
				                      watches[w], aborted);
				if (aborted) break;
				std::lock_guard<std::mutex> lk(done_mu);
				results[i] = std::move(r);
				ran[i] = 1;
				events.push_back({ i, EvFinished, results[i].frames, results[i].frames });
			}
		}
		live.fetch_sub(1);
	};

	std::vector<std::thread> pool;
	pool.reserve(nthreads);
	for (int i = 0; i < nthreads; ++i) pool.emplace_back(worker, i);

	// Results are reported from THIS thread, so a UI caller's callbacks
	// stay on the thread that owns its windows.
	size_t reported = 0;
	size_t last_seen = 0;
	for (;;)
	{
		std::vector<Event> batch;
		{
			std::lock_guard<std::mutex> lk(done_mu);
			batch.swap(events);
		}
		for (const auto &ev : batch)
		{
			const size_t idx = ev.index;
			last_seen = idx;
			if (ev.kind == EvStart)
			{
				if (opts.on_start)
					opts.on_start(opts.user, static_cast<int>(idx), tests[idx]);
				continue;
			}
			if (ev.kind == EvRunning)
			{
				if (opts.on_running)
					opts.on_running(opts.user, static_cast<int>(idx), ev.done, ev.total);
				continue;
			}
			const Result &r = results[idx];
			switch (r.status)
			{
				case Status::Pass:  ++sum.passed; break;
				case Status::Fail:  ++sum.failed; break;
				case Status::Info:  ++sum.info;   break;
				case Status::Error: ++sum.errors; break;
			}
			++sum.total;
			++reported;
			if (opts.on_result)
				opts.on_result(opts.user, static_cast<int>(idx), tests[idx], r);
		}
		if (opts.progress && !batch.empty() &&
		    !opts.progress(opts.user, static_cast<int>(reported),
		                   static_cast<int>(tests.size()), tests[last_seen],
		                   static_cast<int>(reported), static_cast<int>(tests.size())))
			cancel.store(true);

		// A child blocks its worker in a pipe read: cancel and stalls end it here.
		{
			const auto now = std::chrono::steady_clock::now();
			const bool paused = opts.pause && opts.pause->load();
			for (ChildWatch &w : watches)
			{
				std::lock_guard<std::mutex> lk(w.mu);
				if (!w.child) continue;
				if (paused)
					w.last = now;   // a paused worker reads nothing
				else if (cancel.load())
					w.child->Kill();
				else if (std::chrono::duration<double>(now - w.last).count() > kChildStallSecs)
				{
					w.stalled = true;
					w.child->Kill();
				}
			}
		}

		if (live.load() == 0)
		{
			std::lock_guard<std::mutex> lk(done_mu);
			if (events.empty()) break;
			continue;
		}
		if (batch.empty())
		{
			if (opts.progress &&
			    !opts.progress(opts.user, static_cast<int>(reported),
			                   static_cast<int>(tests.size()), tests[last_seen],
			                   static_cast<int>(reported), static_cast<int>(tests.size())))
				cancel.store(true);
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
	}
	for (auto &th : pool) th.join();

	const bool cancelled = cancel.load();
	sum.cancelled = cancelled ? 1 : 0;

	// The report goes out in manifest order, whichever worker produced it.
	if (opts.report && *opts.report)
	{
		ReportInfo info;
		info.env       = EnvOverrides();
		info.filter    = opts.filter.Describe();
		info.source    = dir;
		info.threads   = nthreads;
		info.cancelled = cancelled;
		info.seconds   = std::chrono::duration<double>(
		                     std::chrono::steady_clock::now() - wall_start).count();
		std::vector<ReportRow> rows;
		rows.reserve(tests.size());
		for (size_t i = 0; i < tests.size(); ++i)
			rows.push_back({ &tests[i], ran[i] ? &results[i] : nullptr });
		const std::string path = JoinPath(dir, opts.report);
		std::string err;
		WriteReport(path.c_str(), FormatFromPath(opts.report), rows, info, err);
	}
	if (out_results) *out_results = std::move(results);
	return sum;
}

} // namespace AcidTests
