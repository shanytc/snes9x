/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

// File > Script > Lua: a RichEdit-based script editor. Colours go on through
// the Text Object Model with undo suspended, so highlighting never lands in
// the undo history.

#ifndef NOMINMAX
#define NOMINMAX // std::min/std::max, not the <windows.h> macros
#endif
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <richedit.h>
#include <richole.h>
#include <shellapi.h>
#include <tom.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <regex>
#include <string>
#include <vector>

#include "wlua.h"
#include "wsnes9x.h"
#include "wlocale.h"
#include "rsrc/resource.h"
#include "../snes9x.h"
#include "../memmap.h"
#include "../lua/lua_engine.h"

extern HINSTANCE g_hInst;

namespace {

const wchar_t kWindowClass[] = L"Snes9xLuaWindow";
const wchar_t kGutterClass[] = L"Snes9xLuaGutter";
const wchar_t kRichEditClass[] = L"RICHEDIT50W"; // Msftedit.dll
const int kIndentWidth = 2; // the LuaRocks style guide's two spaces
const IID kIIDTextDocument = { 0x8CC497C0, 0xA1DF, 0x11CE, { 0x80, 0x98, 0x00, 0xAA, 0x00, 0x47, 0xBE, 0x5D } };

enum
{
	IDC_TABS = 100, IDC_CONSOLE, IDC_TREE, IDC_SEARCH, IDC_STATUS, IDC_FIND, IDC_CLEAR, IDC_COMPLETION,
	IDC_BUTTON_NEW, IDC_BUTTON_OPEN, IDC_BUTTON_SAVE, IDC_BUTTON_RUN, IDC_BUTTON_PAUSE, IDC_BUTTON_STOP,
	IDC_BUTTON_CONTINUE, IDC_BUTTON_STEP_OVER, IDC_BUTTON_STEP_INTO, IDC_BUTTON_STEP_OUT,
	IDC_PANEL_TABS, IDC_VARIABLES, IDC_WATCH_EDIT, IDC_WATCH, IDC_CALLSTACK, IDC_BREAKPOINTS,
	IDC_BP_CONDITION, IDC_BP_HIT, IDC_BP_LOG,
	IDC_EDITOR = 1000
};

enum
{
	CMD_NEW = 40001, CMD_OPEN, CMD_SAVE, CMD_SAVE_AS, CMD_CLOSE_TAB, CMD_CLOSE_WINDOW,
	CMD_UNDO, CMD_REDO, CMD_CUT, CMD_COPY, CMD_PASTE, CMD_SELECT_ALL, CMD_TOGGLE_COMMENT,
	CMD_FIND, CMD_FIND_NEXT, CMD_FIND_PREVIOUS, CMD_GOTO,
	CMD_RUN, CMD_PAUSE, CMD_STOP, CMD_STOP_ALL,
	CMD_REFERENCE, CMD_CLEAR_CONSOLE, CMD_ZOOM_IN, CMD_ZOOM_OUT, CMD_ZOOM_RESET,
	CMD_GUIDE, CMD_MANUAL, CMD_PIL, CMD_RECENT_CLEAR,
	CMD_CONTINUE, CMD_BREAK, CMD_STEP_OVER, CMD_STEP_INTO, CMD_STEP_OUT,
	CMD_TOGGLE_BREAKPOINT, CMD_EDIT_BREAKPOINT, CMD_ENABLE_BREAKPOINT, CMD_REMOVE_BREAKPOINTS,
	CMD_BREAK_ON_ERROR, CMD_ADD_WATCH, CMD_REMOVE_WATCH,
	CMD_RECENT_FIRST = 41000, CMD_EXAMPLE_FIRST = 42000
};

enum { TIMER_POLL = 1, TIMER_HIGHLIGHT = 2, TIMER_MODAL = 3 };
const UINT WM_LUA_AFTER_CHAR = WM_APP + 41;
const UINT WM_LUA_POLL = WM_APP + 42;

const int kButtons = 10;
enum { PANEL_CONSOLE, PANEL_VARIABLES, PANEL_WATCH, PANEL_CALLSTACK, PANEL_BREAKPOINTS, PANEL_COUNT };

const COLORREF kKeyword = RGB(0, 51, 179), kBuiltin = RGB(0, 128, 128), kNumber = RGB(176, 80, 0);
const COLORREF kString = RGB(26, 128, 26), kComment = RGB(112, 112, 112), kText = RGB(0, 0, 0);
const COLORREF kErrorBack = RGB(246, 200, 200), kStoppedBack = RGB(255, 238, 140), kFrameBack = RGB(208, 236, 208);
const COLORREF kInfo = RGB(26, 95, 180), kWarning = RGB(160, 96, 0), kError = RGB(192, 16, 16), kQuiet = RGB(112, 112, 112);
const COLORREF kBreakpoint = RGB(200, 36, 36), kConditional = RGB(226, 132, 0), kLogpoint = RGB(36, 112, 200);

// A breakpoint rides a TOM range at its line's start, which follows the text as it's edited.
struct Breakpoint
{
	ITextRange *anchor = nullptr;
	bool enabled = true;
	std::wstring condition, hit, log;
};

struct Document
{
	HWND edit = nullptr;
	ITextDocument *tom = nullptr;
	std::wstring path;
	std::wstring untitled;
	int script = 0;
	bool paused = false;
	bool modified = false;
	int error_line = 0;
	std::vector<Breakpoint> breakpoints;
	int stopped_line = 0;	// where its script is stopped in the debugger
	int frame_line = 0;		// the call stack's selected frame, when it isn't the top one
};

struct Editor
{
	HWND wnd = nullptr, tabs = nullptr, gutter = nullptr, console = nullptr;
	HWND tree = nullptr, search = nullptr, status = nullptr, find = nullptr, clear = nullptr;
	HWND completion = nullptr, buttons[kButtons] = {};
	HWND panel_tabs = nullptr, variables = nullptr, watch_edit = nullptr, watch = nullptr;
	HWND callstack = nullptr, breakpoint_list = nullptr, bp_fields[3] = {}, bp_labels[3] = {};
	HFONT ui_font = nullptr, code_font = nullptr;
	HACCEL accel = nullptr;
	HMENU recent_menu = nullptr, menu = nullptr;
	std::vector<std::unique_ptr<Document>> docs;
	int active = -1;
	std::vector<std::pair<int, std::string>> names;
	int untitled_count = 0;
	int font_size = 10;
	bool show_reference = true;
	bool swallow_char = false;
	bool loading = false;
	int formatting = 0;		// colouring or a font change: RichEdit's EN_CHANGE isn't an edit
	int gutter_width = 40, marker_width = 16, line_height = 16;
	double scale = 1.0;
	int panel = PANEL_CONSOLE;
	int panel_height = 0;						// the bottom panel's, dragged; 0 for the default
	RECT splitter = {};							// the bar between the editor and the panel
	bool splitting = false;
	int split_grab = 0, split_y = 0, layout_top = 0, layout_bottom = 0;
	int close_spaces = 4;						// pad each tab title to make room for its close box
	int close_hover = -1, close_pressed = -1;	// tabs whose close box is under the mouse, pressed
	bool close_tracking = false;

	// Debugger
	int stopped = 0;							// the script stopped in the debugger
	std::wstring stop_text;
	std::vector<S9xLuaDebugFrame> frames;
	int frame = 0;								// the selected one
	struct Pending { int ref; HWND tree; HTREEITEM item; };
	std::vector<Pending> pending;				// variable requests, answered by ref
	std::vector<std::wstring> watches;
	int watch_generation = 0;
	std::vector<std::pair<Document *, size_t>> bp_rows;	// the breakpoints list
	bool filling_breakpoints = false;
	std::vector<HWND> disabled;					// windows switched off while stopped
	std::vector<MSG> deferred;					// their messages, for after
	bool poll_posted = false;
	bool quitting = false;
};

Editor ed;
std::vector<std::string> command_line_scripts;

/* ---- Text helpers ------------------------------------------------------- */

std::wstring Widen(const std::string &utf8)
{
	if (utf8.empty())
		return {};
	int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
	std::wstring out(n, 0);
	MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), &out[0], n);
	return out;
}

std::string Narrow(const std::wstring &wide)
{
	if (wide.empty())
		return {};
	int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
	std::string out(n, 0);
	WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), &out[0], n, nullptr, nullptr);
	return out;
}

int Scaled(int pixels)
{
	return (int)(pixels * ed.scale + 0.5);
}

// The whole text, with RichEdit's own '\r' paragraph ends.
std::wstring EditorText(HWND edit)
{
	GETTEXTLENGTHEX length = { GTL_NUMCHARS | GTL_PRECISE, 1200 };
	LONG n = (LONG)SendMessageW(edit, EM_GETTEXTLENGTHEX, (WPARAM)&length, 0);
	std::wstring text(n + 1, L'\0');
	GETTEXTEX get = { (DWORD)((n + 1) * sizeof(wchar_t)), GT_DEFAULT, 1200, nullptr, nullptr };
	LONG got = (LONG)SendMessageW(edit, EM_GETTEXTEX, (WPARAM)&get, (LPARAM)&text[0]);
	text.resize(std::max<LONG>(0, got));
	return text;
}

std::wstring TextRange(HWND edit, LONG first, LONG last)
{
	if (last <= first)
		return {};
	std::wstring text(last - first + 1, L'\0');
	TEXTRANGEW range = { { first, last }, &text[0] };
	LONG got = (LONG)SendMessageW(edit, EM_GETTEXTRANGE, 0, (LPARAM)&range);
	text.resize(std::max<LONG>(0, got));
	return text;
}

// A script as Lua sees it: '\n' line ends, UTF-8.
std::string ScriptText(Document *doc)
{
	std::wstring text = EditorText(doc->edit);
	std::replace(text.begin(), text.end(), L'\r', L'\n');
	return Narrow(text);
}

CHARRANGE Selection(HWND edit)
{
	CHARRANGE range = {};
	SendMessageW(edit, EM_EXGETSEL, 0, (LPARAM)&range);
	return range;
}

void Select(HWND edit, LONG first, LONG last)
{
	CHARRANGE range = { first, last };
	SendMessageW(edit, EM_EXSETSEL, 0, (LPARAM)&range);
	SendMessageW(edit, EM_SCROLLCARET, 0, 0);
}

void ReplaceSelection(HWND edit, const std::wstring &text)
{
	SendMessageW(edit, EM_REPLACESEL, TRUE, (LPARAM)text.c_str());
}

LONG LineOf(HWND edit, LONG position)
{
	return (LONG)SendMessageW(edit, EM_EXLINEFROMCHAR, 0, position);
}

LONG LineStart(HWND edit, LONG line)
{
	return (LONG)SendMessageW(edit, EM_LINEINDEX, line, 0);
}

LONG LineLength(HWND edit, LONG line)
{
	return (LONG)SendMessageW(edit, EM_LINELENGTH, LineStart(edit, line), 0);
}

std::wstring LineText(HWND edit, LONG line)
{
	LONG start = LineStart(edit, line);
	return TextRange(edit, start, start + LineLength(edit, line));
}

bool IsIdent(wchar_t c)
{
	return iswalnum(c) || c == L'_' || c == L'.';
}

std::wstring WordBefore(HWND edit, LONG position)
{
	LONG start = LineStart(edit, LineOf(edit, position));
	std::wstring before = TextRange(edit, start, position);
	size_t i = before.size();
	while (i > 0 && IsIdent(before[i - 1]))
		i--;
	return before.substr(i);
}

std::wstring WindowString(HWND hwnd)
{
	int length = GetWindowTextLengthW(hwnd);
	std::wstring text(length + 1, L'\0');
	GetWindowTextW(hwnd, &text[0], length + 1);
	text.resize(length);
	return text;
}

// Setting a control's text or state repaints it even when nothing changed; a
// script printing every frame would have the toolbar flicker.
void SetText(HWND hwnd, const std::wstring &text)
{
	if (WindowString(hwnd) != text)
		SetWindowTextW(hwnd, text.c_str());
}

void Enable(HWND hwnd, bool enabled)
{
	if ((IsWindowEnabled(hwnd) != FALSE) != enabled)
		EnableWindow(hwnd, enabled);
}

void SetStatus(const std::wstring &text)
{
	static std::wstring shown;
	if (text == shown)
		return;
	shown = text;
	SendMessageW(ed.status, SB_SETTEXTW, 0, (LPARAM)text.c_str());
}

std::wstring DocSummary(const S9xLuaApiDoc *doc)
{
	return Widen(std::string(doc->name) + doc->args + " - " + doc->summary);
}

/* ---- Documents ---------------------------------------------------------- */

Document *Active()
{
	return ed.active >= 0 && ed.active < (int)ed.docs.size() ? ed.docs[ed.active].get() : nullptr;
}

Document *ForScript(int id)
{
	if (!id)
		return nullptr;
	for (auto &doc : ed.docs)
		if (doc->script == id)
			return doc.get();
	return nullptr;
}

int IndexOf(Document *doc)
{
	for (size_t i = 0; i < ed.docs.size(); i++)
		if (ed.docs[i].get() == doc)
			return (int)i;
	return -1;
}

std::wstring DisplayName(const Document *doc)
{
	if (doc->path.empty())
		return doc->untitled;
	size_t slash = doc->path.find_last_of(L"\\/");
	return slash == std::wstring::npos ? doc->path : doc->path.substr(slash + 1);
}

/* ---- Breakpoints -------------------------------------------------------- */

void RefreshBreakpointList();

int BreakpointLine(Document *doc, const Breakpoint &b)
{
	long cp = 0;
	b.anchor->GetStart(&cp);
	return LineOf(doc->edit, cp) + 1;
}

Breakpoint *FindBreakpoint(Document *doc, int line)
{
	for (auto &b : doc->breakpoints)
		if (BreakpointLine(doc, b) == line)
			return &b;
	return nullptr;
}

void ReleaseBreakpoints(Document *doc)
{
	for (auto &b : doc->breakpoints)
		b.anchor->Release();
	doc->breakpoints.clear();
}

// Edits can fold two breakpoints onto one line: keep the first.
void MergeBreakpoints(Document *doc)
{
	std::vector<int> lines;
	for (size_t i = 0; i < doc->breakpoints.size();)
	{
		int line = BreakpointLine(doc, doc->breakpoints[i]);
		if (std::find(lines.begin(), lines.end(), line) != lines.end())
		{
			doc->breakpoints[i].anchor->Release();
			doc->breakpoints.erase(doc->breakpoints.begin() + i);
			continue;
		}
		lines.push_back(line);
		i++;
	}
}

// To the running script; its code is what was run, so lines are as they are now.
void SendBreakpoints(Document *doc)
{
	if (!doc->script)
		return;
	std::vector<S9xLuaBreakpoint> list;
	for (auto &b : doc->breakpoints)
	{
		S9xLuaBreakpoint bp;
		bp.line = BreakpointLine(doc, b);
		bp.enabled = b.enabled;
		bp.condition = Narrow(b.condition);
		bp.hit_condition = Narrow(b.hit);
		bp.log_message = Narrow(b.log);
		list.push_back(bp);
	}
	S9xLuaDebugSetBreakpoints(doc->script, list);
}

void BreakpointsChanged(Document *doc)
{
	MergeBreakpoints(doc);
	SendBreakpoints(doc);
	RefreshBreakpointList();
	InvalidateRect(ed.gutter, nullptr, TRUE);
}

void ToggleBreakpoint(Document *doc, int line)
{
	if (!doc->tom || line < 1)
		return;
	for (size_t i = 0; i < doc->breakpoints.size(); i++)
	{
		if (BreakpointLine(doc, doc->breakpoints[i]) == line)
		{
			doc->breakpoints[i].anchor->Release();
			doc->breakpoints.erase(doc->breakpoints.begin() + i);
			BreakpointsChanged(doc);
			return;
		}
	}
	LONG start = LineStart(doc->edit, line - 1);
	Breakpoint b;
	if (start < 0 || FAILED(doc->tom->Range(start, start, &b.anchor)) || !b.anchor)
		return;
	doc->breakpoints.push_back(b);
	BreakpointsChanged(doc);
}

void UpdateActions()
{
	Document *doc = Active();
	bool running = doc && doc->script;
	SetText(ed.buttons[3], running ? _L(L"Restart") : _L(L"Run"));
	SetText(ed.buttons[4], doc && doc->paused ? _L(L"Resume") : _L(L"Pause"));
	Enable(ed.buttons[3], doc != nullptr);
	Enable(ed.buttons[4], running);
	Enable(ed.buttons[5], running);
	for (int i = 6; i < kButtons; i++)
		Enable(ed.buttons[i], ed.stopped != 0);

	size_t count = ed.names.size();
	wchar_t text[128];
	if (ed.stopped)
		lstrcpynW(text, ed.stop_text.c_str(), 128);
	else if (count == 0)
		lstrcpynW(text, _L(L"No scripts running"), 128);
	else if (running)
		lstrcpynW(text, doc->paused ? _L(L"Paused") : _L(L"Running"), 128);
	else if (count == 1)
		lstrcpynW(text, _L(L"1 script running"), 128);
	else
		swprintf(text, 128, _L(L"%d scripts running"), (int)count);
	SetStatus(text);
}

void UpdateTab(Document *doc)
{
	int index = IndexOf(doc);
	if (index < 0)
		return;
	std::wstring title = DisplayName(doc);
	if (doc->modified)
		title += L"*";
	if (doc->script)
		title = std::wstring(doc->paused ? L"\x2016 " : L"\x25b6 ") + title;
	title += std::wstring(ed.close_spaces, L' ');

	wchar_t shown[MAX_PATH + 64] = {};
	TCITEMW item = {};
	item.mask = TCIF_TEXT;
	item.pszText = shown;
	item.cchTextMax = MAX_PATH + 64;
	if (!TabCtrl_GetItem(ed.tabs, index, &item) || title != shown)
	{
		item.pszText = &title[0];
		TabCtrl_SetItem(ed.tabs, index, &item);
	}
	UpdateActions();
}

void FormatRange(ITextDocument *tom, long first, long last, COLORREF color, bool bold, bool italic)
{
	ITextRange *range = nullptr;
	if (FAILED(tom->Range(first, last, &range)) || !range)
		return;
	ITextFont *font = nullptr;
	if (SUCCEEDED(range->GetFont(&font)) && font)
	{
		font->SetForeColor(color);
		font->SetBold(bold ? tomTrue : tomFalse);
		font->SetItalic(italic ? tomTrue : tomFalse);
		font->Release();
	}
	range->Release();
}

void BackRange(ITextDocument *tom, long first, long last, long color)
{
	ITextRange *range = nullptr;
	if (FAILED(tom->Range(first, last, &range)) || !range)
		return;
	ITextFont *font = nullptr;
	if (SUCCEEDED(range->GetFont(&font)) && font)
	{
		font->SetBackColor(color);
		font->Release();
	}
	range->Release();
}

// RichEdit reports formatting as an edit (EN_CHANGE, sent before the call returns).
// Colouring marked every script modified and set off the next colouring pass.
struct Formatting
{
	Formatting() { ed.formatting++; }
	~Formatting() { ed.formatting--; }
};

void Highlight(Document *doc)
{
	if (!doc->tom)
		return;
	Formatting formatting;
	std::wstring text = EditorText(doc->edit);

	long freeze = 0;
	doc->tom->Freeze(&freeze);
	doc->tom->Undo(tomSuspend, nullptr);
	FormatRange(doc->tom, 0, (long)text.size() + 1, kText, false, false);
	BackRange(doc->tom, 0, (long)text.size() + 1, tomAutoColor);

	std::vector<S9xLuaToken> tokens;
	int state = 0, line = 1;
	for (size_t start = 0; start <= text.size(); line++)
	{
		size_t end = text.find(L'\r', start);
		if (end == std::wstring::npos)
			end = text.size();

		// UTF-8 for the tokenizer, and where each byte sits in UTF-16.
		std::wstring wline = text.substr(start, end - start);
		std::string bytes;
		std::vector<long> to_utf16;
		for (size_t i = 0; i < wline.size(); i++)
		{
			size_t count = 1;
			if (wline[i] >= 0xd800 && wline[i] < 0xdc00 && i + 1 < wline.size())
				count = 2;
			std::string piece = Narrow(wline.substr(i, count));
			for (size_t k = 0; k < piece.size(); k++)
				to_utf16.push_back((long)i);
			bytes += piece;
			i += count - 1;
		}
		to_utf16.push_back((long)wline.size());

		tokens.clear();
		state = S9xLuaTokenize(bytes.data(), (int)bytes.size(), state, tokens);
		for (auto &token : tokens)
		{
			COLORREF color;
			switch (token.kind)
			{
				case S9X_LUA_TOKEN_KEYWORD: color = kKeyword; break;
				case S9X_LUA_TOKEN_BUILTIN: color = kBuiltin; break;
				case S9X_LUA_TOKEN_NUMBER:  color = kNumber; break;
				case S9X_LUA_TOKEN_STRING:  color = kString; break;
				case S9X_LUA_TOKEN_COMMENT: color = kComment; break;
				default: continue;
			}
			long a = (long)start + to_utf16[token.start];
			long b = (long)start + to_utf16[std::min<size_t>(token.start + token.length, to_utf16.size() - 1)];
			FormatRange(doc->tom, a, b, color, token.kind == S9X_LUA_TOKEN_KEYWORD, token.kind == S9X_LUA_TOKEN_COMMENT);
		}

		if (line == doc->stopped_line)
			BackRange(doc->tom, (long)start, (long)end + 1, kStoppedBack);
		else if (line == doc->frame_line)
			BackRange(doc->tom, (long)start, (long)end + 1, kFrameBack);
		else if (line == doc->error_line)
			BackRange(doc->tom, (long)start, (long)end + 1, kErrorBack);
		if (end == text.size())
			break;
		start = end + 1;
	}

	doc->tom->Undo(tomResume, nullptr);
	doc->tom->Unfreeze(&freeze);
}

// One line's background as Highlight paints it, leaving the rest of the text alone.
void RefreshLineBack(Document *doc, int line)
{
	if (!doc->tom || line < 1)
		return;
	Formatting formatting;
	LONG start = LineStart(doc->edit, line - 1);
	if (start < 0)
		return;
	long color = line == doc->stopped_line ? (long)kStoppedBack : line == doc->frame_line ? (long)kFrameBack :
	             line == doc->error_line ? (long)kErrorBack : tomAutoColor;
	long freeze = 0;
	doc->tom->Freeze(&freeze);
	doc->tom->Undo(tomSuspend, nullptr);
	BackRange(doc->tom, start, start + LineLength(doc->edit, line - 1) + 1, color);
	doc->tom->Undo(tomResume, nullptr);
	doc->tom->Unfreeze(&freeze);
}

void SetErrorLine(Document *doc, int line)
{
	doc->error_line = line;
	Highlight(doc);
	InvalidateRect(ed.gutter, nullptr, TRUE);
}

void ApplyFont(HWND edit)
{
	Formatting formatting;
	CHARFORMAT2W format = {};
	format.cbSize = sizeof(format);
	format.dwMask = CFM_FACE | CFM_SIZE | CFM_CHARSET;
	lstrcpyW(format.szFaceName, L"Consolas");
	format.yHeight = ed.font_size * 20;
	format.bCharSet = DEFAULT_CHARSET;
	SendMessageW(edit, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&format);
	SendMessageW(edit, EM_SETCHARFORMAT, SCF_DEFAULT, (LPARAM)&format);
}

void MakeCodeFont()
{
	if (ed.code_font)
		DeleteObject(ed.code_font);
	HDC dc = GetDC(ed.wnd);
	int height = -MulDiv(ed.font_size, GetDeviceCaps(dc, LOGPIXELSY), 72);
	ed.code_font = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
	                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
	HGDIOBJ old = SelectObject(dc, ed.code_font);
	SIZE size = {};
	GetTextExtentPoint32W(dc, L"99999", 5, &size);
	TEXTMETRICW metrics = {};
	GetTextMetricsW(dc, &metrics);
	SelectObject(dc, old);
	ReleaseDC(ed.wnd, dc);
	ed.line_height = std::max<int>(metrics.tmHeight + metrics.tmExternalLeading, 8);
	ed.marker_width = std::max(Scaled(16), ed.line_height);
	ed.gutter_width = ed.marker_width + size.cx + Scaled(8);
}

void Layout();

void Activate(int index)
{
	if (index < 0 || index >= (int)ed.docs.size())
		index = ed.docs.empty() ? -1 : 0;
	ed.active = index;
	for (size_t i = 0; i < ed.docs.size(); i++)
		ShowWindow(ed.docs[i]->edit, (int)i == index ? SW_SHOW : SW_HIDE);
	if (index >= 0)
	{
		TabCtrl_SetCurSel(ed.tabs, index);
		SetFocus(ed.docs[index]->edit);
	}
	ShowWindow(ed.completion, SW_HIDE);
	Layout();
	InvalidateRect(ed.gutter, nullptr, TRUE);
	UpdateActions();
}

Document *AddDocument(const std::string &text, const std::wstring &path, const std::wstring &name)
{
	auto doc = std::make_unique<Document>();
	doc->path = path;
	if (name.empty())
	{
		wchar_t untitled[32];
		swprintf(untitled, 32, L"untitled-%d.lua", ++ed.untitled_count);
		doc->untitled = untitled;
	}
	else
		doc->untitled = name;

	doc->edit = CreateWindowExW(0, kRichEditClass, L"",
	                            WS_CHILD | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL |
	                            ES_AUTOHSCROLL | ES_NOHIDESEL | ES_WANTRETURN,
	                            0, 0, 10, 10, ed.wnd, (HMENU)(INT_PTR)(IDC_EDITOR + ed.docs.size()), g_hInst, nullptr);
	// The tab control frames the editors: below them, so painting it never covers them.
	SetWindowPos(ed.tabs, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	SendMessageW(doc->edit, EM_SETTARGETDEVICE, 0, 1); // no wrapping
	SendMessageW(doc->edit, EM_EXLIMITTEXT, 0, 16 * 1024 * 1024);
	SendMessageW(doc->edit, EM_SETUNDOLIMIT, 500, 0);
	ApplyFont(doc->edit);

	IRichEditOle *ole = nullptr;
	SendMessageW(doc->edit, EM_GETOLEINTERFACE, 0, (LPARAM)&ole);
	if (ole)
	{
		ole->QueryInterface(kIIDTextDocument, (void **)&doc->tom);
		ole->Release();
	}

	// RichEdit wants \r line ends; scripts come with \n or \r\n.
	std::wstring wide = Widen(text);
	std::wstring normalized;
	for (size_t i = 0; i < wide.size(); i++)
	{
		if (wide[i] == L'\r' && i + 1 < wide.size() && wide[i + 1] == L'\n')
			continue;
		normalized += wide[i] == L'\n' ? L'\r' : wide[i];
	}
	ed.loading = true;
	SETTEXTEX set = { ST_DEFAULT, 1200 };
	SendMessageW(doc->edit, EM_SETTEXTEX, (WPARAM)&set, (LPARAM)normalized.c_str());
	SendMessageW(doc->edit, EM_EMPTYUNDOBUFFER, 0, 0);
	SendMessageW(doc->edit, EM_SETEVENTMASK, 0,
	             ENM_CHANGE | ENM_KEYEVENTS | ENM_MOUSEEVENTS | ENM_SCROLL | ENM_SELCHANGE | ENM_UPDATE);
	Select(doc->edit, 0, 0);
	ed.loading = false;

	Document *raw = doc.get();
	ed.docs.push_back(std::move(doc));
	TCITEMW item = {};
	item.mask = TCIF_TEXT;
	std::wstring title = DisplayName(raw) + std::wstring(ed.close_spaces, L' ');
	item.pszText = &title[0];
	TabCtrl_InsertItem(ed.tabs, (int)ed.docs.size() - 1, &item);
	Highlight(raw);
	Activate((int)ed.docs.size() - 1);
	UpdateTab(raw);
	return raw;
}

void AddRecent(const std::wstring &path)
{
	const int count = sizeof(GUI.LuaRecentScripts) / sizeof(GUI.LuaRecentScripts[0]);
	std::vector<std::wstring> list = { path };
	for (int i = 0; i < count; i++)
		if (*GUI.LuaRecentScripts[i] && lstrcmpiW(GUI.LuaRecentScripts[i], path.c_str()))
			list.push_back(GUI.LuaRecentScripts[i]);
	for (int i = 0; i < count; i++)
		lstrcpynW(GUI.LuaRecentScripts[i], i < (int)list.size() ? list[i].c_str() : L"", MAX_PATH);

	size_t slash = path.find_last_of(L"\\/");
	if (slash != std::wstring::npos)
		lstrcpynW(GUI.LuaDir, path.substr(0, slash).c_str(), MAX_PATH);
}

bool ReadScript(const std::wstring &path, std::string &text)
{
	FILE *file = _wfopen(path.c_str(), L"rb");
	if (!file)
		return false;
	char buffer[16384];
	size_t n;
	text.clear();
	while ((n = fread(buffer, 1, sizeof(buffer), file)) > 0)
		text.append(buffer, n);
	fclose(file);
	if (text.compare(0, 3, "\xEF\xBB\xBF") == 0)
		text.erase(0, 3);
	return true;
}

void Run(Document *doc);

void OpenScript(const std::wstring &path, bool run)
{
	wchar_t full[MAX_PATH];
	if (!GetFullPathNameW(path.c_str(), MAX_PATH, full, nullptr))
		lstrcpynW(full, path.c_str(), MAX_PATH);

	for (size_t i = 0; i < ed.docs.size(); i++)
	{
		if (!lstrcmpiW(ed.docs[i]->path.c_str(), full))
		{
			Activate((int)i);
			if (run)
				Run(ed.docs[i].get());
			return;
		}
	}

	std::string text;
	if (!ReadScript(full, text))
	{
		std::wstring message = std::wstring(_L(L"Couldn't open ")) + full;
		MessageBoxW(ed.wnd, message.c_str(), _L(L"Lua Scripting"), MB_OK | MB_ICONERROR);
		return;
	}

	// A lone untouched new script makes way for the opened one.
	if (ed.docs.size() == 1 && ed.docs[0]->path.empty() && !ed.docs[0]->script && !ed.docs[0]->modified &&
	    ScriptText(ed.docs[0].get()) == S9xLuaNewScriptTemplate())
	{
		ReleaseBreakpoints(ed.docs[0].get());
		if (ed.docs[0]->tom)
			ed.docs[0]->tom->Release();
		DestroyWindow(ed.docs[0]->edit);
		ed.docs.clear();
		TabCtrl_DeleteAllItems(ed.tabs);
	}

	Document *doc = AddDocument(text, full, L"");
	AddRecent(full);
	if (run)
		Run(doc);
}

bool Save(Document *doc, bool save_as)
{
	std::wstring path = doc->path;
	if (path.empty() || save_as)
	{
		wchar_t file[MAX_PATH] = {};
		lstrcpynW(file, (doc->path.empty() ? doc->untitled : doc->path).c_str(), MAX_PATH);
		OPENFILENAMEW ofn = {};
		ofn.lStructSize = sizeof(ofn);
		ofn.hwndOwner = ed.wnd;
		ofn.lpstrFilter = L"Lua scripts (*.lua)\0*.lua\0All files (*.*)\0*.*\0";
		ofn.lpstrFile = file;
		ofn.nMaxFile = MAX_PATH;
		ofn.lpstrInitialDir = *GUI.LuaDir ? GUI.LuaDir : nullptr;
		ofn.lpstrDefExt = L"lua";
		ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
		if (!GetSaveFileNameW(&ofn))
			return false;
		path = file;
	}

	std::string text = ScriptText(doc);
	FILE *out = _wfopen(path.c_str(), L"wb");
	bool written = out && fwrite(text.data(), 1, text.size(), out) == text.size();
	if (out)
		fclose(out);
	if (!written)
	{
		std::wstring message = std::wstring(_L(L"Couldn't write ")) + path;
		MessageBoxW(ed.wnd, message.c_str(), _L(L"Lua Scripting"), MB_OK | MB_ICONERROR);
		return false;
	}

	doc->path = path;
	doc->modified = false;
	SendMessageW(doc->edit, EM_SETMODIFY, FALSE, 0);
	AddRecent(path);
	UpdateTab(doc);
	SetStatus(std::wstring(_L(L"Saved ")) + path);
	return true;
}

// True when the document can go: saved, discarded or unchanged.
bool AskSave(Document *doc)
{
	if (!doc->modified)
		return true;
	ShowWindow(ed.wnd, SW_SHOW);
	Activate(IndexOf(doc));
	std::wstring message = DisplayName(doc) + _L(L" has changes that aren't saved.\n\nSave them before closing?");
	int answer = MessageBoxW(ed.wnd, message.c_str(), _L(L"Lua Scripting"), MB_YESNOCANCEL | MB_ICONWARNING);
	if (answer == IDYES)
		return Save(doc, false);
	return answer == IDNO;
}

void Stop(Document *doc)
{
	if (!doc->script)
		return;
	S9xLuaStop(doc->script);
	doc->script = 0;
	doc->paused = false;
	UpdateTab(doc);
}

void Run(Document *doc)
{
	if (doc->script)
		Stop(doc);
	doc->script = S9xLuaNewScriptId();
	doc->paused = false;
	if (doc->error_line)
		SetErrorLine(doc, 0);
	S9xLuaRun(doc->script, ScriptText(doc), Narrow(DisplayName(doc)), Narrow(doc->path));
	SendBreakpoints(doc);
	UpdateTab(doc);
}

bool CloseDocument(Document *doc)
{
	if (!doc || !AskSave(doc))
		return false;
	if (doc->script)
		Stop(doc);
	int index = IndexOf(doc);
	TabCtrl_DeleteItem(ed.tabs, index);
	ReleaseBreakpoints(doc);
	if (doc->tom)
		doc->tom->Release();
	DestroyWindow(doc->edit);
	ed.docs.erase(ed.docs.begin() + index);
	Activate(std::min(index, (int)ed.docs.size() - 1));
	RefreshBreakpointList();
	return true;
}

void StopAll()
{
	S9xLuaStop(0);
	for (auto &doc : ed.docs)
	{
		doc->script = 0;
		doc->paused = false;
		UpdateTab(doc.get());
	}
}

/* ---- Console ------------------------------------------------------------ */

void AppendConsole(const std::string &utf8, COLORREF color)
{
	std::wstring text = Widen(utf8);
	std::replace(text.begin(), text.end(), L'\n', L'\r');
	GETTEXTLENGTHEX length = { GTL_NUMCHARS | GTL_PRECISE, 1200 };
	LONG end = (LONG)SendMessageW(ed.console, EM_GETTEXTLENGTHEX, (WPARAM)&length, 0);
	if (end > 0)
		text = L"\r" + text;

	Select(ed.console, end, end);
	CHARFORMAT2W format = {};
	format.cbSize = sizeof(format);
	format.dwMask = CFM_COLOR;
	format.crTextColor = color;
	SendMessageW(ed.console, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&format);
	SendMessageW(ed.console, EM_REPLACESEL, FALSE, (LPARAM)text.c_str());

	// Keep the newest 5000 lines.
	LONG lines = (LONG)SendMessageW(ed.console, EM_GETLINECOUNT, 0, 0);
	if (lines > 5000)
	{
		Select(ed.console, 0, LineStart(ed.console, lines - 5000));
		SendMessageW(ed.console, EM_REPLACESEL, FALSE, (LPARAM)L"");
	}
	SendMessageW(ed.console, WM_VSCROLL, SB_BOTTOM, 0);
}

void ClearConsole()
{
	SetWindowTextW(ed.console, L"");
}

void ConsoleDoubleClick(LPARAM position)
{
	POINTL point = { GET_X_LPARAM(position), GET_Y_LPARAM(position) };
	LONG at = (LONG)SendMessageW(ed.console, EM_CHARFROMPOS, 0, (LPARAM)&point);
	std::string line = Narrow(LineText(ed.console, LineOf(ed.console, at)));

	static const std::regex location("([^\\s:\"'\\[\\]]+\\.lua):(\\d+):");
	std::smatch match;
	if (!std::regex_search(line, match, location))
		return;
	for (size_t i = 0; i < ed.docs.size(); i++)
	{
		if (Narrow(DisplayName(ed.docs[i].get())) != match[1].str())
			continue;
		Activate((int)i);
		HWND edit = ed.docs[i]->edit;
		LONG start = LineStart(edit, std::stoi(match[2].str()) - 1);
		if (start >= 0)
			Select(edit, start, start);
		SetFocus(edit);
		return;
	}
}

void PollDebug();

void Poll()
{
	PollDebug();
	std::vector<S9xLuaEvent> events;
	if (!S9xLuaTakeEvents(events))
		return;

	auto name_of = [](int id) {
		for (auto &entry : ed.names)
			if (entry.first == id)
				return entry.second;
		return std::string();
	};

	for (auto &e : events)
	{
		Document *doc = ForScript(e.script);
		std::string prefix = ed.names.size() > 1 ? "[" + name_of(e.script) + "] " : "";
		switch (e.kind)
		{
			case S9X_LUA_EVENT_STARTED:
				if (name_of(e.script).empty())
					ed.names.push_back({ e.script, e.text });
				AppendConsole("\xE2\x96\xB6 " + e.text + " " + Narrow(_L(L"started")), kQuiet);
				break;
			case S9X_LUA_EVENT_PRINT:
				AppendConsole(prefix + e.text, kText);
				break;
			case S9X_LUA_EVENT_INFO:
				AppendConsole(prefix + e.text, kInfo);
				break;
			case S9X_LUA_EVENT_WARNING:
				AppendConsole(prefix + e.text, kWarning);
				break;
			case S9X_LUA_EVENT_ERROR:
				AppendConsole(prefix + e.text, kError);
				break;
			case S9X_LUA_EVENT_PAUSED:
			case S9X_LUA_EVENT_RESUMED:
				if (doc)
				{
					doc->paused = e.kind == S9X_LUA_EVENT_PAUSED;
					UpdateTab(doc);
				}
				AppendConsole(e.kind == S9X_LUA_EVENT_PAUSED ? "\xE2\x80\x96 " + e.text + " " + Narrow(_L(L"paused"))
				                                             : "\xE2\x96\xB6 " + e.text + " " + Narrow(_L(L"resumed")), kQuiet);
				break;
			case S9X_LUA_EVENT_ENDED:
			{
				std::string name = name_of(e.script);
				ed.names.erase(std::remove_if(ed.names.begin(), ed.names.end(),
				                              [&](const std::pair<int, std::string> &p) { return p.first == e.script; }),
				               ed.names.end());
				if (doc)
				{
					doc->script = 0;
					doc->paused = false;
					if (e.line > 0)
						SetErrorLine(doc, e.line);
					UpdateTab(doc);
				}
				if (e.text == "failed")
					AppendConsole("\xE2\x96\xA0 " + name + " " + Narrow(_L(L"stopped with an error")), kError);
				else if (e.text == "finished")
					AppendConsole("\xE2\x96\xA0 " + name + " " + Narrow(_L(L"finished")), kQuiet);
				else
					AppendConsole("\xE2\x96\xA0 " + name + " " + Narrow(_L(L"stopped")), kQuiet);
				break;
			}
			case S9X_LUA_EVENT_CLEAR:
				ClearConsole();
				break;
		}
	}
	UpdateActions();
}

/* ---- Debugger ----------------------------------------------------------- */

void CreateEditorWindow();

void ShowPanel(int panel)
{
	ed.panel = panel;
	TabCtrl_SetCurSel(ed.panel_tabs, panel);
	Layout();
}

std::wstring Trim(const std::wstring &text)
{
	size_t first = text.find_first_not_of(L" \t\r\n");
	if (first == std::wstring::npos)
		return {};
	return text.substr(first, text.find_last_not_of(L" \t\r\n") - first + 1);
}

HTREEITEM AddTreeItem(HWND tree, HTREEITEM parent, const std::wstring &text, int ref)
{
	TVINSERTSTRUCTW insert = {};
	insert.hParent = parent ? parent : TVI_ROOT;
	insert.hInsertAfter = TVI_LAST;
	insert.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN;
	insert.item.pszText = const_cast<wchar_t *>(text.c_str());
	insert.item.lParam = ref;
	insert.item.cChildren = ref ? 1 : 0;
	return TreeView_InsertItem(tree, &insert);
}

void SetTreeItem(HWND tree, HTREEITEM item, const std::wstring &text, int ref)
{
	while (HTREEITEM child = TreeView_GetChild(tree, item))
		TreeView_DeleteItem(tree, child);
	TVITEMW tv = {};
	tv.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN;
	tv.hItem = item;
	tv.pszText = const_cast<wchar_t *>(text.c_str());
	tv.lParam = ref;
	tv.cChildren = ref ? 1 : 0;
	TreeView_SetItem(tree, &tv);
}

void ClearTree(HWND tree)
{
	ed.pending.erase(std::remove_if(ed.pending.begin(), ed.pending.end(),
	                                [tree](const Editor::Pending &p) { return p.tree == tree; }),
	                 ed.pending.end());
	TreeView_DeleteAllItems(tree);
}

// A variable request whose answer fills `item` of `tree`.
void RequestChildren(HWND tree, HTREEITEM item, int ref)
{
	if (!ed.stopped || !ref)
		return;
	ed.pending.push_back({ ref, tree, item });
	S9xLuaDebugRequestVariables(ed.stopped, ref);
}

std::wstring VariableText(const S9xLuaDebugVariable &v)
{
	return Widen(v.name + " = " + v.value);
}

void FillVariables()
{
	SendMessageW(ed.variables, WM_SETREDRAW, FALSE, 0);
	ClearTree(ed.variables);
	if (ed.stopped && ed.frame < (int)ed.frames.size())
	{
		const wchar_t *scopes[3] = { L"Locals", L"Upvalues", L"Globals" };
		for (int scope = 0; scope < 3; scope++)
		{
			int ref = S9xLuaDebugScopeRef(ed.frame, scope);
			HTREEITEM item = AddTreeItem(ed.variables, nullptr, _L(scopes[scope]), ref);
			if (scope == 0)
				RequestChildren(ed.variables, item, ref);
		}
	}
	SendMessageW(ed.variables, WM_SETREDRAW, TRUE, 0);
}

HTREEITEM WatchItem(size_t index)
{
	HTREEITEM item = TreeView_GetRoot(ed.watch);
	while (item && index--)
		item = TreeView_GetNextSibling(ed.watch, item);
	return item;
}

// Every watch, evaluated again in the selected frame.
void FillWatches()
{
	SendMessageW(ed.watch, WM_SETREDRAW, FALSE, 0);
	ClearTree(ed.watch);
	ed.watch_generation++;
	for (size_t i = 0; i < ed.watches.size(); i++)
	{
		AddTreeItem(ed.watch, nullptr, ed.watches[i], 0);
		if (ed.stopped)
			S9xLuaDebugEvaluate(ed.stopped, ed.frame, Narrow(ed.watches[i]), ed.watch_generation * 1000 + (int)i);
	}
	SendMessageW(ed.watch, WM_SETREDRAW, TRUE, 0);
}

void AddWatch(const std::wstring &expression)
{
	std::wstring text = Trim(expression);
	if (text.empty() || ed.watches.size() >= 999)
		return;
	ed.watches.push_back(text);
	FillWatches();
}

void RemoveWatch(HTREEITEM item)
{
	if (!item || TreeView_GetParent(ed.watch, item))
		return;
	size_t index = 0;
	for (HTREEITEM i = TreeView_GetRoot(ed.watch); i && i != item; i = TreeView_GetNextSibling(ed.watch, i))
		index++;
	if (index < ed.watches.size())
	{
		ed.watches.erase(ed.watches.begin() + index);
		FillWatches();
	}
}

void FillCallStack()
{
	ListView_DeleteAllItems(ed.callstack);
	for (size_t i = 0; i < ed.frames.size(); i++)
	{
		const S9xLuaDebugFrame &f = ed.frames[i];
		std::wstring function = Widen(f.function);
		std::wstring location = f.line ? Widen(f.source + ":" + std::to_string(f.line)) : Widen(f.source.empty() ? "[C]" : f.source);
		LVITEMW item = {};
		item.mask = LVIF_TEXT;
		item.iItem = (int)i;
		item.pszText = const_cast<wchar_t *>(function.c_str());
		ListView_InsertItem(ed.callstack, &item);
		ListView_SetItemText(ed.callstack, (int)i, 1, const_cast<wchar_t *>(location.c_str()));
	}
}

// The document a frame's code is in, opening its file if it has to.
Document *FrameDocument(const S9xLuaDebugFrame &f)
{
	if (f.line <= 0)
		return nullptr;
	if (f.own)
		return ForScript(ed.stopped);
	if (f.path.empty())
		return nullptr;
	wchar_t full[MAX_PATH];
	if (!GetFullPathNameW(Widen(f.path).c_str(), MAX_PATH, full, nullptr))
		return nullptr;
	for (auto &doc : ed.docs)
		if (!lstrcmpiW(doc->path.c_str(), full))
			return doc.get();
	if (GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES)
		return nullptr;
	OpenScript(full, false);
	for (auto &doc : ed.docs)
		if (!lstrcmpiW(doc->path.c_str(), full))
			return doc.get();
	return nullptr;
}

void ClearStopLines()
{
	for (auto &doc : ed.docs)
	{
		if (!doc->stopped_line && !doc->frame_line)
			continue;
		int stopped = doc->stopped_line, frame = doc->frame_line;
		doc->stopped_line = doc->frame_line = 0;
		RefreshLineBack(doc.get(), stopped);
		RefreshLineBack(doc.get(), frame);
	}
}

void EnsureLineVisible(HWND edit, int line)
{
	LONG first = (LONG)SendMessageW(edit, EM_GETFIRSTVISIBLELINE, 0, 0);
	RECT client;
	GetClientRect(edit, &client);
	LONG visible = std::max<LONG>(1, client.bottom / ed.line_height);
	LONG target = line - 1;
	if (target < first || target >= first + visible - 1)
		SendMessageW(edit, EM_LINESCROLL, 0, target - first - visible / 3);
}

// Where the script is: the innermost frame with a line.
int TopFrame()
{
	for (size_t i = 0; i < ed.frames.size(); i++)
		if (ed.frames[i].line > 0)
			return (int)i;
	return 0;
}

void SelectFrame(int index)
{
	if (index < 0 || index >= (int)ed.frames.size())
		return;
	ed.frame = index;

	ClearStopLines();
	int top = TopFrame();
	if (Document *doc = FrameDocument(ed.frames[top]))
	{
		doc->stopped_line = ed.frames[top].line;
		RefreshLineBack(doc, doc->stopped_line);
	}
	Document *shown = FrameDocument(ed.frames[index]);
	if (shown && index != top)
	{
		shown->frame_line = ed.frames[index].line;
		RefreshLineBack(shown, shown->frame_line);
	}
	if (shown)
	{
		HWND focus = GetFocus();
		Activate(IndexOf(shown));
		EnsureLineVisible(shown->edit, ed.frames[index].line);
		if (focus == ed.callstack)
			SetFocus(focus);
	}
	InvalidateRect(ed.gutter, nullptr, TRUE);

	ListView_SetItemState(ed.callstack, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
	ListView_SetItemState(ed.callstack, index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
	FillVariables();
	FillWatches();
}

void OnStopped(const S9xLuaDebugEvent &e)
{
	ed.stopped = e.script;
	ed.frames = e.frames;
	int top = TopFrame();
	if (e.reason == "error")
		ed.stop_text = std::wstring(_L(L"Stopped on an error: ")) + Widen(e.message);
	else
	{
		const wchar_t *why = e.reason == "breakpoint" ? _L(L"breakpoint") : e.reason == "step" ? _L(L"step") : _L(L"break");
		wchar_t text[160];
		swprintf(text, 160, _L(L"Paused at line %d (%s)"), ed.frames.empty() ? 0 : ed.frames[top].line, why);
		ed.stop_text = text;
	}
	FillCallStack();
	if (ed.frames.empty())
	{
		FillVariables();
		FillWatches();
	}
	else
		SelectFrame(top);
	if (ed.panel == PANEL_CONSOLE)
		ShowPanel(PANEL_VARIABLES);
	UpdateActions();
}

void ClearDebugViews()
{
	ed.frames.clear();
	ed.pending.clear();
	ClearStopLines();
	ListView_DeleteAllItems(ed.callstack);
	ClearTree(ed.variables);
	FillWatches();
	InvalidateRect(ed.gutter, nullptr, TRUE);
	UpdateActions();
}

void OnVariables(const S9xLuaDebugEvent &e)
{
	for (size_t i = 0; i < ed.pending.size();)
	{
		if (ed.pending[i].ref != e.ref)
		{
			i++;
			continue;
		}
		HWND tree = ed.pending[i].tree;
		HTREEITEM item = ed.pending[i].item;
		ed.pending.erase(ed.pending.begin() + i);

		while (HTREEITEM child = TreeView_GetChild(tree, item))
			TreeView_DeleteItem(tree, child);
		for (auto &v : e.variables)
			AddTreeItem(tree, item, VariableText(v), v.ref);
		if (e.variables.empty())
		{
			TVITEMW tv = {};
			tv.mask = TVIF_CHILDREN;
			tv.hItem = item;
			tv.cChildren = 0;
			TreeView_SetItem(tree, &tv);
		}
		TreeView_Expand(tree, item, TVE_EXPAND);
	}
}

void OnEvaluated(const S9xLuaDebugEvent &e)
{
	int generation = e.request / 1000, index = e.request % 1000;
	if (generation != ed.watch_generation || index >= (int)ed.watches.size() || e.variables.empty())
		return;
	HTREEITEM item = WatchItem(index);
	if (!item)
		return;
	const S9xLuaDebugVariable &v = e.variables[0];
	std::wstring text = ed.watches[index] + L" = " + (e.failed ? std::wstring(_L(L"error: ")) : L"") + Widen(v.value);
	SetTreeItem(ed.watch, item, text, e.failed ? 0 : v.ref);
}

void PollDebug()
{
	ed.poll_posted = false;
	std::vector<S9xLuaDebugEvent> events;
	if (!S9xLuaDebugTakeEvents(events))
		return;
	bool resumed = false;
	for (auto &e : events)
	{
		switch (e.kind)
		{
			case S9X_LUA_DEBUG_STOPPED:
				resumed = false;
				OnStopped(e);
				break;
			case S9X_LUA_DEBUG_RESUMED:
				ed.stopped = 0;
				resumed = true;
				break;
			case S9X_LUA_DEBUG_VARIABLES:
				OnVariables(e);
				break;
			case S9X_LUA_DEBUG_EVALUATED:
				OnEvaluated(e);
				break;
		}
	}
	// A step stops again at once: only clear the views if it didn't.
	if (resumed && !ed.stopped)
		ClearDebugViews();
}

/* ---- Breakpoints list --------------------------------------------------- */

Breakpoint *SelectedBreakpoint(Document **owner)
{
	int row = ListView_GetNextItem(ed.breakpoint_list, -1, LVNI_SELECTED);
	if (row < 0 || row >= (int)ed.bp_rows.size())
		return nullptr;
	Document *doc = ed.bp_rows[row].first;
	size_t index = ed.bp_rows[row].second;
	if (IndexOf(doc) < 0 || index >= doc->breakpoints.size())
		return nullptr;
	if (owner)
		*owner = doc;
	return &doc->breakpoints[index];
}

void FillBreakpointFields()
{
	Breakpoint *b = SelectedBreakpoint(nullptr);
	const std::wstring *values[3] = { b ? &b->condition : nullptr, b ? &b->hit : nullptr, b ? &b->log : nullptr };
	for (int i = 0; i < 3; i++)
	{
		SetWindowTextW(ed.bp_fields[i], values[i] ? values[i]->c_str() : L"");
		EnableWindow(ed.bp_fields[i], b != nullptr);
	}
}

void RefreshBreakpointList()
{
	if (!ed.breakpoint_list)
		return;
	ed.filling_breakpoints = true;
	int selected = ListView_GetNextItem(ed.breakpoint_list, -1, LVNI_SELECTED);
	SendMessageW(ed.breakpoint_list, WM_SETREDRAW, FALSE, 0);
	ListView_DeleteAllItems(ed.breakpoint_list);
	ed.bp_rows.clear();
	for (auto &doc : ed.docs)
	{
		std::vector<std::pair<int, size_t>> order;
		for (size_t i = 0; i < doc->breakpoints.size(); i++)
			order.push_back({ BreakpointLine(doc.get(), doc->breakpoints[i]), i });
		std::sort(order.begin(), order.end());
		for (auto &entry : order)
		{
			Breakpoint &b = doc->breakpoints[entry.second];
			int row = (int)ed.bp_rows.size();
			ed.bp_rows.push_back({ doc.get(), entry.second });
			std::wstring location = DisplayName(doc.get()) + L":" + std::to_wstring(entry.first);
			LVITEMW item = {};
			item.mask = LVIF_TEXT;
			item.iItem = row;
			item.pszText = const_cast<wchar_t *>(location.c_str());
			ListView_InsertItem(ed.breakpoint_list, &item);
			ListView_SetItemText(ed.breakpoint_list, row, 1, const_cast<wchar_t *>(b.condition.c_str()));
			ListView_SetItemText(ed.breakpoint_list, row, 2, const_cast<wchar_t *>(b.hit.c_str()));
			ListView_SetItemText(ed.breakpoint_list, row, 3, const_cast<wchar_t *>(b.log.c_str()));
			ListView_SetCheckState(ed.breakpoint_list, row, b.enabled);
		}
	}
	if (selected >= 0 && selected < (int)ed.bp_rows.size())
		ListView_SetItemState(ed.breakpoint_list, selected, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
	SendMessageW(ed.breakpoint_list, WM_SETREDRAW, TRUE, 0);
	ed.filling_breakpoints = false;
	FillBreakpointFields();
}

void ApplyBreakpointFields()
{
	Document *doc = nullptr;
	Breakpoint *b = SelectedBreakpoint(&doc);
	if (!b)
		return;
	std::wstring condition = Trim(WindowString(ed.bp_fields[0]));
	std::wstring hit = Trim(WindowString(ed.bp_fields[1]));
	std::wstring log = WindowString(ed.bp_fields[2]);
	if (condition == b->condition && hit == b->hit && log == b->log)
		return;
	b->condition = condition;
	b->hit = hit;
	b->log = log;
	SendBreakpoints(doc);

	// The row in place: a rebuild here could pull the list out from under a click.
	int row = ListView_GetNextItem(ed.breakpoint_list, -1, LVNI_SELECTED);
	ListView_SetItemText(ed.breakpoint_list, row, 1, const_cast<wchar_t *>(b->condition.c_str()));
	ListView_SetItemText(ed.breakpoint_list, row, 2, const_cast<wchar_t *>(b->hit.c_str()));
	ListView_SetItemText(ed.breakpoint_list, row, 3, const_cast<wchar_t *>(b->log.c_str()));
	InvalidateRect(ed.gutter, nullptr, TRUE);
}

void SelectBreakpoint(Document *doc, int line)
{
	for (size_t row = 0; row < ed.bp_rows.size(); row++)
	{
		if (ed.bp_rows[row].first != doc || BreakpointLine(doc, doc->breakpoints[ed.bp_rows[row].second]) != line)
			continue;
		ListView_SetItemState(ed.breakpoint_list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
		ListView_SetItemState(ed.breakpoint_list, (int)row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
		ListView_EnsureVisible(ed.breakpoint_list, (int)row, FALSE);
		FillBreakpointFields();
		return;
	}
}

void EditBreakpoint(Document *doc, int line)
{
	if (!FindBreakpoint(doc, line))
		ToggleBreakpoint(doc, line);
	ShowPanel(PANEL_BREAKPOINTS);
	SelectBreakpoint(doc, line);
	SetFocus(ed.bp_fields[0]);
}

void RemoveBreakpointAt(int row)
{
	if (row < 0 || row >= (int)ed.bp_rows.size())
		return;
	Document *doc = ed.bp_rows[row].first;
	size_t index = ed.bp_rows[row].second;
	if (IndexOf(doc) < 0 || index >= doc->breakpoints.size())
		return;
	doc->breakpoints[index].anchor->Release();
	doc->breakpoints.erase(doc->breakpoints.begin() + index);
	BreakpointsChanged(doc);
}

void RemoveAllBreakpoints()
{
	for (auto &doc : ed.docs)
	{
		ReleaseBreakpoints(doc.get());
		SendBreakpoints(doc.get());
	}
	RefreshBreakpointList();
	InvalidateRect(ed.gutter, nullptr, TRUE);
}

void GoToBreakpoint(int row)
{
	if (row < 0 || row >= (int)ed.bp_rows.size())
		return;
	Document *doc = ed.bp_rows[row].first;
	size_t index = ed.bp_rows[row].second;
	if (IndexOf(doc) < 0 || index >= doc->breakpoints.size())
		return;
	Activate(IndexOf(doc));
	LONG start = LineStart(doc->edit, BreakpointLine(doc, doc->breakpoints[index]) - 1);
	if (start >= 0)
		Select(doc->edit, start, start);
	SetFocus(doc->edit);
}

/* ---- Stopping (the engine's side) --------------------------------------- */

// Other windows stay off while a script is stopped mid-frame, and messages that
// could act on the emulator wait for it to go on.
BOOL CALLBACK DisableWindow(HWND hwnd, LPARAM)
{
	if (hwnd != ed.wnd && hwnd != ed.completion && IsWindowVisible(hwnd) && IsWindowEnabled(hwnd))
	{
		EnableWindow(hwnd, FALSE);
		ed.disabled.push_back(hwnd);
	}
	return TRUE;
}

bool Deferrable(UINT message)
{
	return message == WM_COMMAND || message == WM_SYSCOMMAND || message == WM_HOTKEY || message == WM_DROPFILES ||
	       message == WM_CLOSE || (message >= WM_KEYFIRST && message <= WM_KEYLAST) || message >= WM_USER;
}

void DebuggerStopped(int script, bool stopped)
{
	if (stopped)
	{
		if (ed.quitting)
		{
			S9xLuaDebugCommand(script, S9X_LUA_DEBUG_STOP);
			return;
		}
		if (!ed.wnd)
			CreateEditorWindow();
		if (IsIconic(ed.wnd))
			ShowWindow(ed.wnd, SW_RESTORE);
		ShowWindow(ed.wnd, SW_SHOW);
		SetForegroundWindow(ed.wnd);
		S9xSetPause(PAUSE_LUA_DEBUGGER);
		ed.disabled.clear();
		EnumThreadWindows(GetCurrentThreadId(), DisableWindow, 0);
		ed.stopped = script;
		Poll();
		return;
	}

	for (HWND hwnd : ed.disabled)
		if (IsWindow(hwnd))
			EnableWindow(hwnd, TRUE);
	ed.disabled.clear();
	std::vector<MSG> deferred;
	deferred.swap(ed.deferred);
	for (auto &msg : deferred)
		PostMessageW(msg.hwnd, msg.message, msg.wParam, msg.lParam);
	S9xClearPause(PAUSE_LUA_DEBUGGER);
}

void DebuggerWait()
{
	MSG msg;
	if (GetMessageW(&msg, nullptr, 0, 0) <= 0)
	{
		// The emulator is closing: end the stopped script and let the quit through.
		ed.quitting = true;
		if (int stopped = S9xLuaDebugStoppedScript())
			S9xLuaDebugCommand(stopped, S9X_LUA_DEBUG_STOP);
		PostQuitMessage((int)msg.wParam);
		return;
	}
	if (WinLuaHandleMessage(&msg))
		return;
	if (msg.hwnd && Deferrable(msg.message))
	{
		ed.deferred.push_back(msg);
		return;
	}
	TranslateMessage(&msg);
	DispatchMessageW(&msg);
}

/* ---- Editing ------------------------------------------------------------ */

void HideCompletion()
{
	ShowWindow(ed.completion, SW_HIDE);
}

void UpdateCompletion(bool force)
{
	Document *doc = Active();
	if (!doc)
		return;
	CHARRANGE selection = Selection(doc->edit);
	std::string prefix = Narrow(WordBefore(doc->edit, selection.cpMax));
	if (!force && prefix.size() < 2)
	{
		HideCompletion();
		return;
	}

	SendMessageW(ed.completion, LB_RESETCONTENT, 0, 0);
	int count = 0;
	std::string only;
	for (auto &name : S9xLuaApiNames())
	{
		if (name.compare(0, prefix.size(), prefix) != 0)
			continue;
		SendMessageW(ed.completion, LB_ADDSTRING, 0, (LPARAM)Widen(name).c_str());
		only = name;
		if (++count >= 60)
			break;
	}
	if (count == 0 || (count == 1 && only == prefix))
	{
		HideCompletion();
		return;
	}
	SendMessageW(ed.completion, LB_SETCURSEL, 0, 0);

	POINTL point = {};
	SendMessageW(doc->edit, EM_POSFROMCHAR, (WPARAM)&point, selection.cpMax);
	int item = (int)SendMessageW(ed.completion, LB_GETITEMHEIGHT, 0, 0);
	POINT screen = { point.x, point.y + item + Scaled(2) };
	ClientToScreen(doc->edit, &screen);
	SetWindowPos(ed.completion, HWND_TOP, screen.x, screen.y, Scaled(280), std::min(count, 10) * item + Scaled(4),
	             SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void AcceptCompletion()
{
	Document *doc = Active();
	int index = (int)SendMessageW(ed.completion, LB_GETCURSEL, 0, 0);
	HideCompletion();
	if (!doc || index < 0)
		return;
	int length = (int)SendMessageW(ed.completion, LB_GETTEXTLEN, index, 0);
	std::wstring name(length + 1, L'\0');
	SendMessageW(ed.completion, LB_GETTEXT, index, (LPARAM)&name[0]);
	name.resize(length);

	CHARRANGE selection = Selection(doc->edit);
	LONG start = selection.cpMax - (LONG)WordBefore(doc->edit, selection.cpMax).size();
	Select(doc->edit, start, selection.cpMax);
	ReplaceSelection(doc->edit, name);
}

void ShowCallTip()
{
	Document *doc = Active();
	if (!doc)
		return;
	LONG caret = Selection(doc->edit).cpMax;
	std::string name = Narrow(WordBefore(doc->edit, caret - 1));
	HideCompletion();
	if (auto api = S9xLuaFindApiDoc(name))
		SetStatus(DocSummary(api));
}

void Group(Document *doc, bool begin)
{
	if (!doc->tom)
		return;
	if (begin)
		doc->tom->BeginEditCollection();
	else
		doc->tom->EndEditCollection();
}

void IndentLines(Document *doc, bool outdent)
{
	HWND edit = doc->edit;
	CHARRANGE selection = Selection(edit);
	LONG first = LineOf(edit, selection.cpMin), last = LineOf(edit, selection.cpMax);
	if (last > first && LineStart(edit, last) == selection.cpMax)
		last--;

	Group(doc, true);
	for (LONG line = first; line <= last; line++)
	{
		LONG start = LineStart(edit, line);
		std::wstring text = LineText(edit, line);
		if (outdent)
		{
			int n = 0;
			while (n < kIndentWidth && n < (int)text.size() && text[n] == L' ')
				n++;
			if (n == 0 && !text.empty() && text[0] == L'\t')
				n = 1;
			Select(edit, start, start + n);
			ReplaceSelection(edit, L"");
		}
		else if (!text.empty())
		{
			Select(edit, start, start);
			ReplaceSelection(edit, std::wstring(kIndentWidth, L' '));
		}
	}
	Group(doc, false);
	Select(edit, LineStart(edit, first), LineStart(edit, last) + LineLength(edit, last));
}

void ToggleComment(Document *doc)
{
	HWND edit = doc->edit;
	CHARRANGE selection = Selection(edit);
	LONG first = LineOf(edit, selection.cpMin), last = LineOf(edit, selection.cpMax);
	if (last > first && LineStart(edit, last) == selection.cpMax)
		last--;

	bool all_commented = true;
	size_t indent = std::wstring::npos;
	for (LONG line = first; line <= last; line++)
	{
		std::wstring text = LineText(edit, line);
		size_t code = text.find_first_not_of(L" \t");
		if (code == std::wstring::npos)
			continue;
		all_commented = all_commented && text.compare(code, 2, L"--") == 0;
		indent = std::min(indent, code);
	}
	if (indent == std::wstring::npos)
		return;

	Group(doc, true);
	for (LONG line = first; line <= last; line++)
	{
		std::wstring text = LineText(edit, line);
		size_t code = text.find_first_not_of(L" \t");
		if (code == std::wstring::npos)
			continue;
		LONG start = LineStart(edit, line);
		if (all_commented)
		{
			int length = text.compare(code, 3, L"-- ") == 0 ? 3 : 2;
			Select(edit, start + (LONG)code, start + (LONG)code + length);
			ReplaceSelection(edit, L"");
		}
		else
		{
			Select(edit, start + (LONG)indent, start + (LONG)indent);
			ReplaceSelection(edit, L"-- ");
		}
	}
	Group(doc, false);
}

bool OpensBlock(const std::wstring &line)
{
	std::string code = Narrow(line);
	while (!code.empty() && isspace((unsigned char)code.back()))
		code.pop_back();
	size_t start = code.find_first_not_of(" \t");
	if (start == std::string::npos || code.compare(start, 2, "--") == 0)
		return false;
	static const std::regex opener("(\\bthen|\\bdo|\\belse|\\brepeat|[{(]|\\bfunction\\b[^)]*\\))$");
	return std::regex_search(code, opener);
}

// EN_MSGFILTER for an editor: true swallows the message.
bool EditorKey(Document *doc, MSGFILTER *filter)
{
	HWND edit = doc->edit;
	bool control = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
	bool completing = IsWindowVisible(ed.completion) != FALSE;

	if (filter->msg == WM_KEYDOWN)
	{
		WPARAM key = filter->wParam;
		if (completing)
		{
			int index = (int)SendMessageW(ed.completion, LB_GETCURSEL, 0, 0);
			int count = (int)SendMessageW(ed.completion, LB_GETCOUNT, 0, 0);
			switch (key)
			{
				case VK_DOWN:
					SendMessageW(ed.completion, LB_SETCURSEL, std::min(index + 1, count - 1), 0);
					return true;
				case VK_UP:
					SendMessageW(ed.completion, LB_SETCURSEL, std::max(index - 1, 0), 0);
					return true;
				case VK_RETURN:
				case VK_TAB:
					AcceptCompletion();
					ed.swallow_char = true;
					return true;
				case VK_ESCAPE:
					HideCompletion();
					return true;
				case VK_BACK:
					PostMessageW(ed.wnd, WM_LUA_AFTER_CHAR, 0, 0);
					return false;
				default:
					break;
			}
		}

		if (key == VK_SPACE && control)
		{
			UpdateCompletion(true);
			ed.swallow_char = true;
			return true;
		}
		if ((key == 'V' && control && !alt) || (key == VK_INSERT && shift))
		{
			SendMessageW(edit, EM_PASTESPECIAL, CF_UNICODETEXT, 0); // never rich text
			return true;
		}
		if (key == VK_TAB && !control && !alt)
		{
			CHARRANGE selection = Selection(edit);
			if (shift || LineOf(edit, selection.cpMin) != LineOf(edit, selection.cpMax))
				IndentLines(doc, shift);
			else
			{
				LONG column = selection.cpMin - LineStart(edit, LineOf(edit, selection.cpMin));
				ReplaceSelection(edit, std::wstring(kIndentWidth - column % kIndentWidth, L' '));
			}
			ed.swallow_char = true;
			return true;
		}
		if (key == VK_RETURN && !control && !alt && !shift)
		{
			// Keep the line's indent, one level deeper after a block opener.
			CHARRANGE selection = Selection(edit);
			LONG start = LineStart(edit, LineOf(edit, selection.cpMin));
			std::wstring before = TextRange(edit, start, selection.cpMin);
			size_t code = before.find_first_not_of(L" \t");
			std::wstring indent = before.substr(0, code == std::wstring::npos ? before.size() : code);
			if (OpensBlock(before))
				indent += std::wstring(kIndentWidth, L' ');
			ReplaceSelection(edit, L"\r" + indent);
			ed.swallow_char = true;
			return true;
		}
		return false;
	}

	if (filter->msg == WM_CHAR)
	{
		wchar_t c = (wchar_t)filter->wParam;
		if (ed.swallow_char && (c == L'\t' || c == L'\r' || c == L' ' || c == L'\n'))
		{
			ed.swallow_char = false;
			return true;
		}
		ed.swallow_char = false;
		if (IsIdent(c) || c == L'(')
			PostMessageW(ed.wnd, WM_LUA_AFTER_CHAR, c, 0);
		else
			HideCompletion();
		return false;
	}

	if (filter->msg == WM_LBUTTONDOWN || filter->msg == WM_KILLFOCUS)
		HideCompletion();
	return false;
}

void Find(bool backward)
{
	Document *doc = Active();
	int length = GetWindowTextLengthW(ed.find);
	if (!doc || length == 0)
		return;
	std::wstring needle(length + 1, L'\0');
	GetWindowTextW(ed.find, &needle[0], length + 1);
	needle.resize(length);

	// ":42" in the find box goes to line 42.
	if (needle[0] == L':')
	{
		int line = _wtoi(needle.c_str() + 1);
		LONG start = LineStart(doc->edit, std::max(line, 1) - 1);
		if (start >= 0)
		{
			Select(doc->edit, start, start);
			SetFocus(doc->edit);
		}
		return;
	}

	CHARRANGE selection = Selection(doc->edit);
	FINDTEXTEXW find = {};
	find.lpstrText = needle.c_str();
	find.chrg = backward ? CHARRANGE{ selection.cpMin, 0 } : CHARRANGE{ selection.cpMax, -1 };
	WPARAM flags = backward ? 0 : FR_DOWN;
	LONG found = (LONG)SendMessageW(doc->edit, EM_FINDTEXTEXW, flags, (LPARAM)&find);
	if (found < 0) // wrap around once
	{
		find.chrg = backward ? CHARRANGE{ -1, 0 } : CHARRANGE{ 0, -1 };
		found = (LONG)SendMessageW(doc->edit, EM_FINDTEXTEXW, flags, (LPARAM)&find);
	}
	if (found < 0)
	{
		SetStatus(std::wstring(L"\"") + needle + L"\" " + _L(L"not found"));
		return;
	}
	Select(doc->edit, find.chrgText.cpMin, find.chrgText.cpMax);
}

void Zoom(int steps)
{
	ed.font_size = steps == 0 ? 10 : std::clamp(ed.font_size + steps, 6, 40);
	MakeCodeFont();
	for (auto &doc : ed.docs)
	{
		ApplyFont(doc->edit);
		Highlight(doc.get());
	}
	SendMessageW(ed.console, WM_SETFONT, (WPARAM)ed.code_font, TRUE);
	ApplyFont(ed.console);
	Layout();
}

/* ---- Reference ---------------------------------------------------------- */

void FillReference()
{
	int length = GetWindowTextLengthW(ed.search);
	std::wstring filter(length + 1, L'\0');
	GetWindowTextW(ed.search, &filter[0], length + 1);
	filter.resize(length);
	std::string needle = Narrow(filter);
	std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return (char)tolower(c); });

	SendMessageW(ed.tree, WM_SETREDRAW, FALSE, 0);
	TreeView_DeleteAllItems(ed.tree);
	size_t count;
	const S9xLuaApiDoc *docs = S9xLuaApiDocs(&count);
	HTREEITEM parent = nullptr;
	std::string module;
	for (size_t i = 0; i < count; i++)
	{
		std::string name = docs[i].name;
		std::string haystack = name + " " + docs[i].summary;
		std::transform(haystack.begin(), haystack.end(), haystack.begin(), [](unsigned char c) { return (char)tolower(c); });
		if (!needle.empty() && haystack.find(needle) == std::string::npos)
			continue;

		std::string this_module = name.substr(0, name.find('.'));
		if (this_module != module || !parent)
		{
			module = this_module;
			std::wstring label = Widen(module);
			TVINSERTSTRUCTW insert = {};
			insert.hParent = TVI_ROOT;
			insert.hInsertAfter = TVI_LAST;
			insert.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_STATE;
			insert.item.pszText = &label[0];
			insert.item.lParam = -1;
			insert.item.state = insert.item.stateMask = TVIS_BOLD;
			parent = TreeView_InsertItem(ed.tree, &insert);
		}

		std::wstring label = Widen(name.substr(module.size() + 1) + docs[i].args);
		TVINSERTSTRUCTW insert = {};
		insert.hParent = parent;
		insert.hInsertAfter = TVI_LAST;
		insert.item.mask = TVIF_TEXT | TVIF_PARAM;
		insert.item.pszText = &label[0];
		insert.item.lParam = (LPARAM)i;
		TreeView_InsertItem(ed.tree, &insert);
		if (!needle.empty())
			TreeView_Expand(ed.tree, parent, TVE_EXPAND);
	}
	SendMessageW(ed.tree, WM_SETREDRAW, TRUE, 0);
	InvalidateRect(ed.tree, nullptr, TRUE);
}

const S9xLuaApiDoc *TreeDoc(HTREEITEM item)
{
	TVITEMW tv = {};
	tv.mask = TVIF_PARAM;
	tv.hItem = item;
	if (!item || !TreeView_GetItem(ed.tree, &tv) || tv.lParam < 0)
		return nullptr;
	size_t count;
	const S9xLuaApiDoc *docs = S9xLuaApiDocs(&count);
	return (size_t)tv.lParam < count ? &docs[tv.lParam] : nullptr;
}

/* ---- Window ------------------------------------------------------------- */

void Layout()
{
	if (!ed.wnd)
		return;
	RECT client;
	GetClientRect(ed.wnd, &client);
	int width = client.right, height = client.bottom;

	SendMessageW(ed.status, WM_SIZE, 0, 0);
	RECT status;
	GetWindowRect(ed.status, &status);
	int status_height = status.bottom - status.top;
	int parts[2] = { width - Scaled(140), -1 };
	SendMessageW(ed.status, SB_SETPARTS, 2, (LPARAM)parts);

	int pad = Scaled(4), button_w = Scaled(66), button_h = Scaled(26);
	int x = pad;
	for (int i = 0; i < kButtons; i++)
	{
		if (i == 3 || i == 6)
			x += Scaled(10);
		MoveWindow(ed.buttons[i], x, pad, button_w, button_h, TRUE);
		x += button_w + pad;
	}
	int find_w = std::max(Scaled(80), std::min(Scaled(200), width - pad - x));
	MoveWindow(ed.find, width - pad - find_w, pad + Scaled(2), find_w, button_h - Scaled(4), TRUE);

	int top = pad * 2 + button_h, bottom = height - status_height;
	int reference_w = ed.show_reference ? Scaled(270) : 0;
	int left_w = width - pad * 2 - (reference_w ? reference_w + pad : 0);
	ShowWindow(ed.search, ed.show_reference ? SW_SHOW : SW_HIDE);
	ShowWindow(ed.tree, ed.show_reference ? SW_SHOW : SW_HIDE);
	if (ed.show_reference)
	{
		int x = width - pad - reference_w;
		MoveWindow(ed.search, x, top, reference_w, Scaled(22), TRUE);
		MoveWindow(ed.tree, x, top + Scaled(26), reference_w, bottom - top - Scaled(26) - pad, TRUE);
	}

	// The panel's height is the splitter's, kept so neither side gets too small.
	int header_h = Scaled(24), split = Scaled(6);
	int console_h = ed.panel_height > 0 ? ed.panel_height : std::max(Scaled(110), (bottom - top) * 3 / 10);
	int most = std::max(Scaled(60), bottom - top - header_h - split - Scaled(100));
	console_h = std::clamp(console_h, Scaled(60), most);
	if (ed.panel_height > 0)
		ed.panel_height = console_h;
	int editor_bottom = bottom - console_h - header_h - split;
	RECT splitter = { pad, editor_bottom, pad + left_w, editor_bottom + split };
	if (!EqualRect(&splitter, &ed.splitter))
	{
		InvalidateRect(ed.wnd, &ed.splitter, TRUE);
		InvalidateRect(ed.wnd, &splitter, TRUE);
		ed.splitter = splitter;
	}
	ed.layout_top = top;
	ed.layout_bottom = bottom;
	MoveWindow(ed.tabs, pad, top, left_w, editor_bottom - top, TRUE);
	RECT inner = { pad, top, pad + left_w, editor_bottom };
	TabCtrl_AdjustRect(ed.tabs, FALSE, &inner);
	MoveWindow(ed.gutter, inner.left, inner.top, ed.gutter_width, inner.bottom - inner.top, TRUE);
	for (auto &doc : ed.docs)
		MoveWindow(doc->edit, inner.left + ed.gutter_width, inner.top, inner.right - inner.left - ed.gutter_width,
		           inner.bottom - inner.top, TRUE);

	// The bottom panel: console, variables, watches, call stack, breakpoints.
	MoveWindow(ed.panel_tabs, pad, editor_bottom + split, left_w - Scaled(70), header_h, TRUE);
	MoveWindow(ed.clear, pad + left_w - Scaled(64), editor_bottom + split, Scaled(64), Scaled(22), TRUE);
	ShowWindow(ed.clear, ed.panel == PANEL_CONSOLE ? SW_SHOW : SW_HIDE);

	int px = pad, py = editor_bottom + split + header_h, pw = left_w, ph = console_h - pad, row = Scaled(22);
	auto place = [](HWND hwnd, bool show, int x0, int y0, int w0, int h0) {
		if (show)
			MoveWindow(hwnd, x0, y0, std::max(w0, 1), std::max(h0, 1), TRUE);
		ShowWindow(hwnd, show ? SW_SHOW : SW_HIDE);
	};
	place(ed.console, ed.panel == PANEL_CONSOLE, px, py, pw, ph);
	place(ed.variables, ed.panel == PANEL_VARIABLES, px, py, pw, ph);
	place(ed.watch_edit, ed.panel == PANEL_WATCH, px, py, pw, row);
	place(ed.watch, ed.panel == PANEL_WATCH, px, py + row + pad, pw, ph - row - pad);
	place(ed.callstack, ed.panel == PANEL_CALLSTACK, px, py, pw, ph);

	bool breakpoints = ed.panel == PANEL_BREAKPOINTS;
	place(ed.breakpoint_list, breakpoints, px, py, pw, ph - row - pad);
	int label_w[3] = { Scaled(64), Scaled(58), Scaled(80) };
	int fields_w = pw - label_w[0] - label_w[1] - label_w[2] - pad * 2;
	int field_w[3] = { fields_w * 36 / 100, fields_w * 18 / 100, 0 };
	field_w[2] = fields_w - field_w[0] - field_w[1];
	int fx = px, fy = py + ph - row;
	for (int i = 0; i < 3; i++)
	{
		place(ed.bp_labels[i], breakpoints, fx, fy + Scaled(3), label_w[i] - Scaled(4), row - Scaled(3));
		fx += label_w[i];
		place(ed.bp_fields[i], breakpoints, fx, fy, field_w[i], row);
		fx += field_w[i] + pad;
	}
	InvalidateRect(ed.gutter, nullptr, TRUE);
}

// The dotted bar that follows the mouse while the splitter is dragged, inverted
// over the window: drawing it a second time takes it away.
void DrawSplitTracker(int y)
{
	static HBRUSH halftone = [] {
		WORD pattern[8];
		for (int i = 0; i < 8; i++)
			pattern[i] = (WORD)(0x5555 << (i & 1));
		HBITMAP bitmap = CreateBitmap(8, 8, 1, 1, pattern);
		HBRUSH brush = CreatePatternBrush(bitmap);
		DeleteObject(bitmap);
		return brush;
	}();
	HDC dc = GetDCEx(ed.wnd, nullptr, DCX_WINDOW | DCX_CACHE | DCX_LOCKWINDOWUPDATE);
	if (!dc)
		return;
	POINT origin = { 0, 0 };
	ClientToScreen(ed.wnd, &origin);
	RECT window;
	GetWindowRect(ed.wnd, &window);
	HGDIOBJ old = SelectObject(dc, halftone);
	PatBlt(dc, origin.x - window.left + ed.splitter.left, origin.y - window.top + y,
	       ed.splitter.right - ed.splitter.left, ed.splitter.bottom - ed.splitter.top, PATINVERT);
	SelectObject(dc, old);
	ReleaseDC(ed.wnd, dc);
}

void EndSplit()
{
	ed.splitting = false;
	DrawSplitTracker(ed.split_y);
	LockWindowUpdate(nullptr);
}

// Every pane moved with drawing off, then the window painted once: no flicker, no trails.
void LayoutAndRedraw()
{
	SendMessageW(ed.wnd, WM_SETREDRAW, FALSE, 0);
	Layout();
	SendMessageW(ed.wnd, WM_SETREDRAW, TRUE, 0);
	RedrawWindow(ed.wnd, nullptr, nullptr, RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN);
}

void DrawBreakpoint(HDC dc, const RECT &box, const Breakpoint &b)
{
	COLORREF color = !b.log.empty() ? kLogpoint : (!b.condition.empty() || !b.hit.empty()) ? kConditional : kBreakpoint;
	HPEN pen = CreatePen(PS_SOLID, std::max(1, Scaled(1)), color);
	HBRUSH brush = b.enabled ? CreateSolidBrush(color) : nullptr;
	HGDIOBJ old_pen = SelectObject(dc, pen);
	HGDIOBJ old_brush = SelectObject(dc, brush ? (HGDIOBJ)brush : GetStockObject(NULL_BRUSH));
	if (!b.log.empty())
	{
		// A logpoint: a diamond.
		int cx = (box.left + box.right) / 2, cy = (box.top + box.bottom) / 2;
		POINT points[4] = { { cx, box.top }, { box.right, cy }, { cx, box.bottom }, { box.left, cy } };
		Polygon(dc, points, 4);
	}
	else
		Ellipse(dc, box.left, box.top, box.right, box.bottom);
	SelectObject(dc, old_brush);
	SelectObject(dc, old_pen);
	DeleteObject(pen);
	if (brush)
		DeleteObject(brush);
}

void DrawArrow(HDC dc, const RECT &box, COLORREF fill)
{
	int h = box.bottom - box.top, cy = (box.top + box.bottom) / 2, mid = box.left + (box.right - box.left) / 2;
	POINT points[7] = {
		{ box.left, cy - h / 5 }, { mid, cy - h / 5 }, { mid, box.top }, { box.right, cy },
		{ mid, box.bottom }, { mid, cy + h / 5 }, { box.left, cy + h / 5 }
	};
	HPEN pen = CreatePen(PS_SOLID, 1, RGB(96, 80, 0));
	HBRUSH brush = CreateSolidBrush(fill);
	HGDIOBJ old_pen = SelectObject(dc, pen);
	HGDIOBJ old_brush = SelectObject(dc, brush);
	Polygon(dc, points, 7);
	SelectObject(dc, old_brush);
	SelectObject(dc, old_pen);
	DeleteObject(pen);
	DeleteObject(brush);
}

// The line under a gutter y, 1-based; 0 below the last one.
int GutterLine(Document *doc, int y)
{
	POINTL point = { 1, y };
	LONG at = (LONG)SendMessageW(doc->edit, EM_CHARFROMPOS, 0, (LPARAM)&point);
	LONG line = LineOf(doc->edit, at);
	POINTL top = {};
	SendMessageW(doc->edit, EM_POSFROMCHAR, (WPARAM)&top, LineStart(doc->edit, line));
	return y < top.y + ed.line_height ? line + 1 : 0;
}

void GutterMenu(Document *doc, int line, POINT screen)
{
	Breakpoint *b = FindBreakpoint(doc, line);
	HMENU menu = CreatePopupMenu();
	AppendMenuW(menu, MF_STRING, CMD_TOGGLE_BREAKPOINT, b ? _L(L"Remove Breakpoint") : _L(L"Add Breakpoint"));
	AppendMenuW(menu, MF_STRING, CMD_EDIT_BREAKPOINT, b ? _L(L"Edit Breakpoint...") : _L(L"Add Conditional Breakpoint..."));
	if (b)
		AppendMenuW(menu, MF_STRING, CMD_ENABLE_BREAKPOINT, b->enabled ? _L(L"Disable Breakpoint") : _L(L"Enable Breakpoint"));
	AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
	AppendMenuW(menu, MF_STRING | (ed.bp_rows.empty() ? MF_GRAYED : 0), CMD_REMOVE_BREAKPOINTS, _L(L"Remove All Breakpoints"));
	UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, ed.wnd, nullptr);
	DestroyMenu(menu);

	switch (command)
	{
		case CMD_TOGGLE_BREAKPOINT:		ToggleBreakpoint(doc, line); break;
		case CMD_EDIT_BREAKPOINT:		EditBreakpoint(doc, line); break;
		case CMD_REMOVE_BREAKPOINTS:	RemoveAllBreakpoints(); break;
		case CMD_ENABLE_BREAKPOINT:
			if ((b = FindBreakpoint(doc, line)) != nullptr)
			{
				b->enabled = !b->enabled;
				BreakpointsChanged(doc);
			}
			break;
	}
}

LRESULT CALLBACK GutterProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONUP)
	{
		Document *doc = Active();
		int line = doc ? GutterLine(doc, GET_Y_LPARAM(lparam)) : 0;
		if (!line)
			return 0;
		if (msg == WM_LBUTTONDOWN)
			ToggleBreakpoint(doc, line);
		else
		{
			POINT screen = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
			ClientToScreen(hwnd, &screen);
			GutterMenu(doc, line, screen);
		}
		return 0;
	}
	if (msg == WM_ERASEBKGND)
		return 1;	// the paint covers it all
	if (msg != WM_PAINT)
		return DefWindowProcW(hwnd, msg, wparam, lparam);

	// Drawn off screen and copied in one go, so it never shows half painted.
	PAINTSTRUCT ps;
	HDC target = BeginPaint(hwnd, &ps);
	RECT client;
	GetClientRect(hwnd, &client);
	HDC dc = CreateCompatibleDC(target);
	HBITMAP bitmap = CreateCompatibleBitmap(target, std::max<LONG>(client.right, 1), std::max<LONG>(client.bottom, 1));
	HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
	FillRect(dc, &client, GetSysColorBrush(COLOR_3DFACE));

	Document *doc = Active();
	if (doc)
	{
		std::vector<std::pair<int, const Breakpoint *>> marks;
		for (auto &b : doc->breakpoints)
			marks.push_back({ BreakpointLine(doc, b), &b });

		HGDIOBJ old = SelectObject(dc, ed.code_font);
		SetBkMode(dc, TRANSPARENT);
		LONG first = (LONG)SendMessageW(doc->edit, EM_GETFIRSTVISIBLELINE, 0, 0);
		LONG count = (LONG)SendMessageW(doc->edit, EM_GETLINECOUNT, 0, 0);
		LONG current = LineOf(doc->edit, Selection(doc->edit).cpMax);
		int size = std::min(ed.marker_width, ed.line_height) - Scaled(4);
		for (LONG line = first; line < count; line++)
		{
			POINTL point = {};
			SendMessageW(doc->edit, EM_POSFROMCHAR, (WPARAM)&point, LineStart(doc->edit, line));
			if (point.y > client.bottom)
				break;

			RECT box = { Scaled(3), point.y + (ed.line_height - size) / 2, 0, 0 };
			box.right = box.left + size;
			box.bottom = box.top + size;
			for (auto &mark : marks)
				if (mark.first == line + 1)
					DrawBreakpoint(dc, box, *mark.second);
			if (line + 1 == doc->stopped_line)
				DrawArrow(dc, box, RGB(255, 214, 0));
			else if (line + 1 == doc->frame_line)
				DrawArrow(dc, box, RGB(96, 184, 96));

			COLORREF color = GetSysColor(COLOR_GRAYTEXT);
			if (line + 1 == doc->error_line)
				color = kError;
			else if (line == current)
				color = GetSysColor(COLOR_WINDOWTEXT);
			SetTextColor(dc, color);
			wchar_t number[16];
			swprintf(number, 16, L"%ld", line + 1);
			RECT row = { ed.marker_width, point.y, client.right - Scaled(4), point.y + Scaled(40) };
			DrawTextW(dc, number, -1, &row, DT_RIGHT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
		}
		SelectObject(dc, old);
	}
	BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
	SelectObject(dc, old_bitmap);
	DeleteObject(bitmap);
	DeleteDC(dc);
	EndPaint(hwnd, &ps);
	return 0;
}

void FillRecentMenu()
{
	while (GetMenuItemCount(ed.recent_menu) > 0)
		DeleteMenu(ed.recent_menu, 0, MF_BYPOSITION);
	int count = sizeof(GUI.LuaRecentScripts) / sizeof(GUI.LuaRecentScripts[0]);
	int shown = 0;
	for (int i = 0; i < count; i++)
	{
		if (!*GUI.LuaRecentScripts[i])
			continue;
		wchar_t label[MAX_PATH + 8];
		swprintf(label, MAX_PATH + 8, L"&%d: %s", i, GUI.LuaRecentScripts[i]);
		AppendMenuW(ed.recent_menu, MF_STRING, CMD_RECENT_FIRST + i, label);
		shown++;
	}
	if (!shown)
		AppendMenuW(ed.recent_menu, MF_STRING | MF_GRAYED, 0, _L(L"(none)"));
	AppendMenuW(ed.recent_menu, MF_SEPARATOR, 0, nullptr);
	AppendMenuW(ed.recent_menu, MF_STRING, CMD_RECENT_CLEAR, _L(L"Clear Recent Scripts"));
}

HMENU BuildMenu()
{
	HMENU bar = CreateMenu();
	auto popup = [bar](const wchar_t *label) {
		HMENU menu = CreatePopupMenu();
		AppendMenuW(bar, MF_POPUP, (UINT_PTR)menu, _L(label));
		return menu;
	};
	auto item = [](HMENU menu, UINT id, const wchar_t *label) { AppendMenuW(menu, MF_STRING, id, _L(label)); };
	auto separator = [](HMENU menu) { AppendMenuW(menu, MF_SEPARATOR, 0, nullptr); };

	HMENU file = popup(L"&File");
	item(file, CMD_NEW, L"&New Script\tCtrl+N");
	item(file, CMD_OPEN, L"&Open...\tCtrl+O");
	ed.recent_menu = CreatePopupMenu();
	AppendMenuW(file, MF_POPUP, (UINT_PTR)ed.recent_menu, _L(L"Open &Recent"));
	separator(file);
	item(file, CMD_SAVE, L"&Save\tCtrl+S");
	item(file, CMD_SAVE_AS, L"Save &As...\tCtrl+Shift+S");
	separator(file);
	item(file, CMD_CLOSE_TAB, L"&Close Script\tCtrl+W");
	item(file, CMD_CLOSE_WINDOW, L"Close &Window");

	HMENU edit = popup(L"&Edit");
	item(edit, CMD_UNDO, L"&Undo\tCtrl+Z");
	item(edit, CMD_REDO, L"&Redo\tCtrl+Y");
	separator(edit);
	item(edit, CMD_CUT, L"Cu&t\tCtrl+X");
	item(edit, CMD_COPY, L"&Copy\tCtrl+C");
	item(edit, CMD_PASTE, L"&Paste\tCtrl+V");
	item(edit, CMD_SELECT_ALL, L"Select &All\tCtrl+A");
	separator(edit);
	item(edit, CMD_TOGGLE_COMMENT, L"Toggle C&omment\tCtrl+/");
	item(edit, CMD_FIND, L"&Find...\tCtrl+F");
	item(edit, CMD_FIND_NEXT, L"Find &Next\tF3");
	item(edit, CMD_FIND_PREVIOUS, L"Find Pre&vious\tShift+F3");
	item(edit, CMD_GOTO, L"&Go to Line...\tCtrl+G");

	HMENU script = popup(L"&Script");
	item(script, CMD_RUN, L"&Run\tF5");
	item(script, CMD_PAUSE, L"&Pause / Resume\tF6");
	item(script, CMD_STOP, L"&Stop\tShift+F5");
	separator(script);
	item(script, CMD_STOP_ALL, L"Stop &All Scripts");

	HMENU debug = popup(L"&Debug");
	item(debug, CMD_CONTINUE, L"&Continue\tF5");
	item(debug, CMD_BREAK, L"&Break\tF8");
	item(debug, CMD_STEP_OVER, L"Step &Over\tF10");
	item(debug, CMD_STEP_INTO, L"Step &Into\tF11");
	item(debug, CMD_STEP_OUT, L"Step O&ut\tShift+F11");
	separator(debug);
	item(debug, CMD_TOGGLE_BREAKPOINT, L"&Toggle Breakpoint\tF9");
	item(debug, CMD_EDIT_BREAKPOINT, L"&Edit Breakpoint...");
	item(debug, CMD_REMOVE_BREAKPOINTS, L"&Remove All Breakpoints\tCtrl+Shift+F9");
	item(debug, CMD_ADD_WATCH, L"Add &Watch\tShift+F9");
	separator(debug);
	item(debug, CMD_BREAK_ON_ERROR, L"Break on &Errors");

	HMENU view = popup(L"&View");
	item(view, CMD_REFERENCE, L"API &Reference\tF1");
	item(view, CMD_CLEAR_CONSOLE, L"C&lear Console\tCtrl+L");
	separator(view);
	item(view, CMD_ZOOM_IN, L"Zoom &In\tCtrl+=");
	item(view, CMD_ZOOM_OUT, L"Zoom &Out\tCtrl+-");
	item(view, CMD_ZOOM_RESET, L"&Reset Zoom\tCtrl+0");

	HMENU help = popup(L"&Help");
	HMENU examples = CreatePopupMenu();
	size_t count;
	const S9xLuaExample *list = S9xLuaExamples(&count);
	for (size_t i = 0; i < count; i++)
		AppendMenuW(examples, MF_STRING, CMD_EXAMPLE_FIRST + i, Widen(list[i].title).c_str());
	AppendMenuW(help, MF_POPUP, (UINT_PTR)examples, _L(L"&Examples"));
	separator(help);
	item(help, CMD_GUIDE, L"Snes9x &Scripting Guide");
	item(help, CMD_MANUAL, L"Lua 5.4 &Manual");
	item(help, CMD_PIL, L"&Programming in Lua");
	return bar;
}

void OpenDialog()
{
	std::vector<wchar_t> buffer(32 * 1024, L'\0');
	OPENFILENAMEW ofn = {};
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = ed.wnd;
	ofn.lpstrFilter = L"Lua scripts (*.lua)\0*.lua\0All files (*.*)\0*.*\0";
	ofn.lpstrFile = buffer.data();
	ofn.nMaxFile = (DWORD)buffer.size();
	ofn.lpstrInitialDir = *GUI.LuaDir ? GUI.LuaDir : nullptr;
	ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_NOCHANGEDIR;
	if (!GetOpenFileNameW(&ofn))
		return;

	// One file: the full path. Several: the folder, then each name.
	std::wstring first = buffer.data();
	const wchar_t *next = buffer.data() + first.size() + 1;
	if (!*next)
	{
		OpenScript(first, false);
		return;
	}
	for (; *next; next += lstrlenW(next) + 1)
		OpenScript(first + L"\\" + next, false);
}

LRESULT CALLBACK FindProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR)
{
	if (msg == WM_KEYDOWN && wparam == VK_RETURN)
	{
		Find(GetKeyState(VK_SHIFT) < 0);
		return 0;
	}
	if (msg == WM_KEYDOWN && wparam == VK_ESCAPE)
	{
		if (Document *doc = Active())
			SetFocus(doc->edit);
		return 0;
	}
	if (msg == WM_CHAR && (wparam == L'\r' || wparam == 27))
		return 0;
	return DefSubclassProc(hwnd, msg, wparam, lparam);
}

// Enter in the watch box adds it; in a breakpoint field, applies it.
LRESULT CALLBACK DebugFieldProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR)
{
	if (msg == WM_KEYDOWN && wparam == VK_RETURN)
	{
		if (hwnd == ed.watch_edit)
		{
			AddWatch(WindowString(hwnd));
			SetWindowTextW(hwnd, L"");
		}
		else
			ApplyBreakpointFields();
		return 0;
	}
	if (msg == WM_CHAR && wparam == L'\r')
		return 0;
	return DefSubclassProc(hwnd, msg, wparam, lparam);
}

/* ---- Tab close boxes ---------------------------------------------------- */

// The close box in the room the padded title leaves at the tab's right.
RECT CloseBox(int tab)
{
	RECT item = {};
	TabCtrl_GetItemRect(ed.tabs, tab, &item);
	int size = Scaled(14), cy = (item.top + item.bottom) / 2;
	return { item.right - Scaled(5) - size, cy - size / 2, item.right - Scaled(5), cy - size / 2 + size };
}

int CloseBoxAt(POINT point)
{
	for (int tab = 0; tab < TabCtrl_GetItemCount(ed.tabs); tab++)
	{
		RECT box = CloseBox(tab);
		if (PtInRect(&box, point))
			return tab;
	}
	return -1;
}

void DrawCloseBoxes(HWND hwnd)
{
	HDC dc = GetDC(hwnd);
	for (int tab = 0; tab < TabCtrl_GetItemCount(hwnd); tab++)
	{
		RECT box = CloseBox(tab);
		bool hot = tab == ed.close_hover || tab == ed.close_pressed;
		if (hot)
		{
			HBRUSH back = CreateSolidBrush(tab == ed.close_pressed ? RGB(176, 32, 32) : RGB(224, 64, 64));
			FillRect(dc, &box, back);
			DeleteObject(back);
		}
		int inset = Scaled(4);
		HPEN pen = CreatePen(PS_SOLID, std::max(1, Scaled(1)), hot ? RGB(255, 255, 255) : RGB(96, 96, 96));
		HGDIOBJ old = SelectObject(dc, pen);
		MoveToEx(dc, box.left + inset, box.top + inset, nullptr);
		LineTo(dc, box.right - inset + 1, box.bottom - inset + 1);
		MoveToEx(dc, box.right - inset, box.top + inset, nullptr);
		LineTo(dc, box.left + inset - 1, box.bottom - inset + 1);
		SelectObject(dc, old);
		DeleteObject(pen);
	}
	ReleaseDC(hwnd, dc);
}

// Only the box: repainting the whole tab control would repaint the editor it frames.
void RedrawCloseBox(int tab)
{
	if (tab < 0 || tab >= TabCtrl_GetItemCount(ed.tabs))
		return;
	RECT box = CloseBox(tab);
	InvalidateRect(ed.tabs, &box, FALSE);
}

// A tab's close box (or a middle click anywhere on it) closes it as Ctrl+W does.
LRESULT CALLBACK TabsProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR)
{
	POINT point = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
	switch (msg)
	{
		case WM_PAINT:
		{
			LRESULT result = DefSubclassProc(hwnd, msg, wparam, lparam);
			DrawCloseBoxes(hwnd);
			return result;
		}

		case WM_MOUSEMOVE:
		{
			int tab = ed.close_pressed >= 0 ? (CloseBoxAt(point) == ed.close_pressed ? ed.close_pressed : -1) : CloseBoxAt(point);
			if (tab != ed.close_hover)
			{
				RedrawCloseBox(ed.close_hover);
				ed.close_hover = tab;
				RedrawCloseBox(tab);
			}
			if (!ed.close_tracking)
			{
				TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, hwnd, 0 };
				ed.close_tracking = TrackMouseEvent(&track) != FALSE;
			}
			break;
		}

		case WM_MOUSELEAVE:
			ed.close_tracking = false;
			if (ed.close_hover >= 0)
			{
				RedrawCloseBox(ed.close_hover);
				ed.close_hover = -1;
			}
			break;

		case WM_LBUTTONDOWN:
		{
			int tab = CloseBoxAt(point);
			if (tab < 0)
				break;
			ed.close_pressed = tab;
			SetCapture(hwnd);
			RedrawCloseBox(tab);
			return 0;
		}

		case WM_LBUTTONUP:
		{
			if (ed.close_pressed < 0)
				break;
			int tab = ed.close_pressed;
			ed.close_pressed = -1;
			ReleaseCapture();
			RedrawCloseBox(tab);
			if (CloseBoxAt(point) == tab && tab < (int)ed.docs.size())
				CloseDocument(ed.docs[tab].get());
			ed.close_hover = -1;
			return 0;
		}

		case WM_CAPTURECHANGED:
			if (ed.close_pressed >= 0)
			{
				RedrawCloseBox(ed.close_pressed);
				ed.close_pressed = -1;
			}
			break;

		case WM_MBUTTONUP:
		{
			TCHITTESTINFO hit = { point, 0 };
			int tab = TabCtrl_HitTest(hwnd, &hit);
			if (tab >= 0 && tab < (int)ed.docs.size())
				CloseDocument(ed.docs[tab].get());
			ed.close_hover = -1;
			return 0;
		}
	}
	return DefSubclassProc(hwnd, msg, wparam, lparam);
}

void Command(UINT id)
{
	Document *doc = Active();
	auto send = [doc](UINT msg, WPARAM wparam = 0) {
		if (doc)
			SendMessageW(doc->edit, msg, wparam, 0);
	};

	if (id >= CMD_RECENT_FIRST && id < CMD_RECENT_FIRST + 100)
	{
		std::wstring path = GUI.LuaRecentScripts[id - CMD_RECENT_FIRST];
		if (!path.empty())
			OpenScript(path, false);
		return;
	}
	if (id >= CMD_EXAMPLE_FIRST && id < CMD_EXAMPLE_FIRST + 100)
	{
		size_t count;
		const S9xLuaExample *list = S9xLuaExamples(&count);
		if (id - CMD_EXAMPLE_FIRST < count)
			AddDocument(list[id - CMD_EXAMPLE_FIRST].code, L"", Widen(list[id - CMD_EXAMPLE_FIRST].filename));
		return;
	}

	switch (id)
	{
		case CMD_NEW:			AddDocument(S9xLuaNewScriptTemplate(), L"", L""); break;
		case CMD_OPEN:			OpenDialog(); break;
		case CMD_SAVE:			if (doc) Save(doc, false); break;
		case CMD_SAVE_AS:		if (doc) Save(doc, true); break;
		case CMD_CLOSE_TAB:		if (doc) CloseDocument(doc); break;
		case CMD_CLOSE_WINDOW:	SendMessageW(ed.wnd, WM_CLOSE, 0, 0); break;
		case CMD_UNDO:			send(EM_UNDO); break;
		case CMD_REDO:			send(EM_REDO); break;
		case CMD_CUT:			send(WM_CUT); break;
		case CMD_COPY:			send(WM_COPY); break;
		case CMD_PASTE:			send(EM_PASTESPECIAL, CF_UNICODETEXT); break;
		case CMD_SELECT_ALL:	if (doc) Select(doc->edit, 0, -1); break;
		case CMD_TOGGLE_COMMENT: if (doc) ToggleComment(doc); break;
		case CMD_FIND:
			SetFocus(ed.find);
			SendMessageW(ed.find, EM_SETSEL, 0, -1);
			break;
		case CMD_FIND_NEXT:		Find(false); break;
		case CMD_FIND_PREVIOUS:	Find(true); break;
		case CMD_GOTO:
			SetWindowTextW(ed.find, L":");
			SetFocus(ed.find);
			SendMessageW(ed.find, EM_SETSEL, 1, 1);
			SetStatus(_L(L"Type a line number after the colon and press Enter"));
			break;
		case CMD_RUN:
			// While a script is stopped, F5 lets it go on.
			if (ed.stopped)
				S9xLuaDebugCommand(ed.stopped, S9X_LUA_DEBUG_CONTINUE);
			else if (doc)
				Run(doc);
			break;
		case CMD_CONTINUE:		if (ed.stopped) S9xLuaDebugCommand(ed.stopped, S9X_LUA_DEBUG_CONTINUE); break;
		case CMD_STEP_OVER:		if (ed.stopped) S9xLuaDebugCommand(ed.stopped, S9X_LUA_DEBUG_STEP_OVER); break;
		case CMD_STEP_INTO:		if (ed.stopped) S9xLuaDebugCommand(ed.stopped, S9X_LUA_DEBUG_STEP_INTO); break;
		case CMD_STEP_OUT:		if (ed.stopped) S9xLuaDebugCommand(ed.stopped, S9X_LUA_DEBUG_STEP_OUT); break;
		case CMD_BREAK:
			if (!ed.stopped && doc && doc->script)
			{
				S9xLuaDebugCommand(doc->script, S9X_LUA_DEBUG_BREAK);
				SetStatus(_L(L"Stopping at the script's next line"));
			}
			break;
		case CMD_TOGGLE_BREAKPOINT:
			if (doc)
				ToggleBreakpoint(doc, LineOf(doc->edit, Selection(doc->edit).cpMax) + 1);
			break;
		case CMD_EDIT_BREAKPOINT:
			if (doc)
				EditBreakpoint(doc, LineOf(doc->edit, Selection(doc->edit).cpMax) + 1);
			break;
		case CMD_REMOVE_BREAKPOINTS:	RemoveAllBreakpoints(); break;
		case CMD_BREAK_ON_ERROR:
			GUI.LuaBreakOnError = !GUI.LuaBreakOnError;
			S9xLuaDebugSetBreakOnError(GUI.LuaBreakOnError);
			break;
		case CMD_ADD_WATCH:
			if (doc)
			{
				// The selection, or the name under the caret.
				CHARRANGE selection = Selection(doc->edit);
				std::wstring text = TextRange(doc->edit, selection.cpMin, selection.cpMax);
				if (Trim(text).empty())
				{
					LONG start = LineStart(doc->edit, LineOf(doc->edit, selection.cpMax));
					std::wstring line = LineText(doc->edit, LineOf(doc->edit, selection.cpMax));
					size_t at = (size_t)(selection.cpMax - start), first = at, last = at;
					while (first > 0 && IsIdent(line[first - 1]))
						first--;
					while (last < line.size() && IsIdent(line[last]))
						last++;
					text = line.substr(first, last - first);
				}
				AddWatch(text);
				ShowPanel(PANEL_WATCH);
			}
			break;
		case CMD_REMOVE_WATCH:	RemoveWatch(TreeView_GetSelection(ed.watch)); break;
		case CMD_PAUSE:			if (doc && doc->script) S9xLuaPause(doc->script, !doc->paused); break;
		case CMD_STOP:			if (doc) Stop(doc); break;
		case CMD_STOP_ALL:		StopAll(); break;
		case CMD_REFERENCE:
			ed.show_reference = !ed.show_reference;
			Layout();
			break;
		case CMD_CLEAR_CONSOLE:	ClearConsole(); break;
		case CMD_ZOOM_IN:		Zoom(1); break;
		case CMD_ZOOM_OUT:		Zoom(-1); break;
		case CMD_ZOOM_RESET:	Zoom(0); break;
		case CMD_GUIDE:
			ShellExecuteW(nullptr, L"open", L"https://github.com/shanytc/snes9x/blob/master/docs/lua-scripting.md", nullptr, nullptr, SW_SHOWNORMAL);
			break;
		case CMD_MANUAL:
			ShellExecuteW(nullptr, L"open", L"https://www.lua.org/manual/5.4/", nullptr, nullptr, SW_SHOWNORMAL);
			break;
		case CMD_PIL:
			ShellExecuteW(nullptr, L"open", L"https://www.lua.org/pil/contents.html", nullptr, nullptr, SW_SHOWNORMAL);
			break;
		case CMD_RECENT_CLEAR:
			for (auto &path : GUI.LuaRecentScripts)
				*path = L'\0';
			break;
	}
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	switch (msg)
	{
		case WM_SIZE:
			Layout();
			return 0;

		// The splitter between the editor and the panel: drag it, double-click to reset.
		case WM_SETCURSOR:
			if ((HWND)wparam == hwnd && LOWORD(lparam) == HTCLIENT)
			{
				POINT point;
				GetCursorPos(&point);
				ScreenToClient(hwnd, &point);
				if (ed.splitting || PtInRect(&ed.splitter, point))
				{
					SetCursor(LoadCursor(nullptr, IDC_SIZENS));
					return TRUE;
				}
			}
			break;

		case WM_LBUTTONDOWN:
		{
			POINT point = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
			if (!PtInRect(&ed.splitter, point))
				break;
			ed.splitting = true;
			ed.split_grab = point.y - ed.splitter.top;
			ed.split_y = ed.splitter.top;
			SetCapture(hwnd);
			LockWindowUpdate(hwnd);	// nothing paints over the tracker while it's dragged
			DrawSplitTracker(ed.split_y);
			return 0;
		}

		case WM_MOUSEMOVE:
			if (ed.splitting)
			{
				// Only the tracker moves; the panes follow once, on release.
				int y = std::clamp(GET_Y_LPARAM(lparam) - ed.split_grab, ed.layout_top + Scaled(100),
				                   std::max(ed.layout_top + Scaled(100), ed.layout_bottom - Scaled(60) - Scaled(24) - Scaled(6)));
				if (y != ed.split_y)
				{
					DrawSplitTracker(ed.split_y);
					ed.split_y = y;
					DrawSplitTracker(ed.split_y);
				}
				return 0;
			}
			break;

		case WM_LBUTTONUP:
			if (ed.splitting)
			{
				EndSplit();
				ed.panel_height = std::max(1, ed.layout_bottom - ed.split_y - Scaled(24) - Scaled(6));
				ReleaseCapture();
				LayoutAndRedraw();
				return 0;
			}
			break;

		case WM_CAPTURECHANGED:
			if (ed.splitting)
				EndSplit();	// taken away mid-drag: the panes stay as they were
			break;

		case WM_LBUTTONDBLCLK:
		{
			POINT point = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
			if (PtInRect(&ed.splitter, point))
			{
				ed.panel_height = 0;
				LayoutAndRedraw();
				return 0;
			}
			break;
		}

		case WM_PAINT:
		{
			// A grip of dots in the middle of the splitter.
			PAINTSTRUCT ps;
			HDC dc = BeginPaint(hwnd, &ps);
			int cx = (ed.splitter.left + ed.splitter.right) / 2, cy = (ed.splitter.top + ed.splitter.bottom) / 2;
			int dot = std::max(2, Scaled(2));
			for (int i = -3; i <= 3; i++)
			{
				RECT r = { cx + i * dot * 3 - dot / 2, cy - dot / 2, cx + i * dot * 3 - dot / 2 + dot, cy - dot / 2 + dot };
				FillRect(dc, &r, GetSysColorBrush(COLOR_3DSHADOW));
			}
			EndPaint(hwnd, &ps);
			return 0;
		}

		case WM_GETMINMAXINFO:
		{
			auto info = (MINMAXINFO *)lparam;
			info->ptMinTrackSize = { Scaled(800), Scaled(420) };
			return 0;
		}

		case WM_COMMAND:
		{
			HWND from = (HWND)lparam;
			WORD code = HIWORD(wparam), id = LOWORD(wparam);
			if (from == ed.completion)
			{
				if (code == LBN_DBLCLK)
					AcceptCompletion();
				return 0;
			}
			if (from && id >= IDC_EDITOR)
			{
				Document *doc = nullptr;
				for (auto &d : ed.docs)
					if (d->edit == from)
						doc = d.get();
				if (!doc)
					return 0;
				if (code == EN_CHANGE && !ed.loading && !ed.formatting)
				{
					if (!doc->modified)
					{
						doc->modified = true;
						UpdateTab(doc);
					}
					doc->error_line = 0;
					if (!doc->breakpoints.empty())
					{
						// Lines moved: the list shows where they are now.
						MergeBreakpoints(doc);
						RefreshBreakpointList();
					}
					SetTimer(ed.wnd, TIMER_HIGHLIGHT, 150, nullptr);
					InvalidateRect(ed.gutter, nullptr, TRUE);
				}
				else if (code == EN_VSCROLL || code == EN_UPDATE)
					InvalidateRect(ed.gutter, nullptr, TRUE);
				return 0;
			}
			if (from == ed.search && code == EN_CHANGE)
			{
				FillReference();
				return 0;
			}
			if (from && (from == ed.bp_fields[0] || from == ed.bp_fields[1] || from == ed.bp_fields[2] ||
			             from == ed.watch_edit))
			{
				if (code == EN_KILLFOCUS && from != ed.watch_edit)
					ApplyBreakpointFields();
				return 0;
			}
			switch (id)
			{
				case IDC_BUTTON_NEW:		Command(CMD_NEW); break;
				case IDC_BUTTON_OPEN:		Command(CMD_OPEN); break;
				case IDC_BUTTON_SAVE:		Command(CMD_SAVE); break;
				case IDC_BUTTON_RUN:		if (Document *doc = Active()) Run(doc); break;	// a restart, even when stopped
				case IDC_BUTTON_PAUSE:		Command(CMD_PAUSE); break;
				case IDC_BUTTON_STOP:		Command(CMD_STOP); break;
				case IDC_BUTTON_CONTINUE:	Command(CMD_CONTINUE); break;
				case IDC_BUTTON_STEP_OVER:	Command(CMD_STEP_OVER); break;
				case IDC_BUTTON_STEP_INTO:	Command(CMD_STEP_INTO); break;
				case IDC_BUTTON_STEP_OUT:	Command(CMD_STEP_OUT); break;
				case IDC_CLEAR:				ClearConsole(); break;
				default:					Command(id); break;
			}
			return 0;
		}

		case WM_NOTIFY:
		{
			auto header = (NMHDR *)lparam;
			if (header->hwndFrom == ed.tabs && header->code == TCN_SELCHANGE)
			{
				Activate(TabCtrl_GetCurSel(ed.tabs));
				return 0;
			}
			if (header->hwndFrom == ed.panel_tabs && header->code == TCN_SELCHANGE)
			{
				ed.panel = TabCtrl_GetCurSel(ed.panel_tabs);
				Layout();
				return 0;
			}
			if (header->hwndFrom == ed.variables || header->hwndFrom == ed.watch)
			{
				if (header->code == TVN_ITEMEXPANDINGW)
				{
					auto view = (NMTREEVIEWW *)lparam;
					if ((view->action & TVE_EXPAND) && !TreeView_GetChild(header->hwndFrom, view->itemNew.hItem))
						RequestChildren(header->hwndFrom, view->itemNew.hItem, (int)view->itemNew.lParam);
				}
				else if (header->code == TVN_SELCHANGEDW)
				{
					// Long values are cut off in the tree; the status bar has room.
					wchar_t text[1024] = {};
					TVITEMW tv = {};
					tv.mask = TVIF_TEXT;
					tv.hItem = ((NMTREEVIEWW *)lparam)->itemNew.hItem;
					tv.pszText = text;
					tv.cchTextMax = 1024;
					if (tv.hItem && TreeView_GetItem(header->hwndFrom, &tv))
						SetStatus(text);
				}
				else if (header->hwndFrom == ed.watch && header->code == TVN_KEYDOWN && ((NMTVKEYDOWN *)lparam)->wVKey == VK_DELETE)
					RemoveWatch(TreeView_GetSelection(ed.watch));
				else if (header->hwndFrom == ed.watch && header->code == NM_RCLICK)
				{
					POINT screen;
					GetCursorPos(&screen);
					TVHITTESTINFO hit = { screen };
					ScreenToClient(ed.watch, &hit.pt);
					if (HTREEITEM item = TreeView_HitTest(ed.watch, &hit))
						TreeView_SelectItem(ed.watch, item);
					HMENU menu = CreatePopupMenu();
					AppendMenuW(menu, MF_STRING | (TreeView_GetSelection(ed.watch) ? 0 : MF_GRAYED), CMD_REMOVE_WATCH, _L(L"Remove Watch"));
					TrackPopupMenu(menu, TPM_RIGHTBUTTON, screen.x, screen.y, 0, ed.wnd, nullptr);
					DestroyMenu(menu);
					return 1;
				}
				return 0;
			}
			if (header->hwndFrom == ed.callstack && header->code == LVN_ITEMCHANGED)
			{
				auto view = (NMLISTVIEW *)lparam;
				if ((view->uChanged & LVIF_STATE) && (view->uNewState & LVIS_SELECTED) && !(view->uOldState & LVIS_SELECTED) &&
				    view->iItem != ed.frame)
					SelectFrame(view->iItem);
				return 0;
			}
			if (header->hwndFrom == ed.breakpoint_list)
			{
				if (header->code == LVN_ITEMCHANGED && !ed.filling_breakpoints)
				{
					auto view = (NMLISTVIEW *)lparam;
					if ((view->uChanged & LVIF_STATE) && ((view->uNewState ^ view->uOldState) & LVIS_STATEIMAGEMASK) &&
					    view->iItem >= 0 && view->iItem < (int)ed.bp_rows.size())
					{
						Document *doc = ed.bp_rows[view->iItem].first;
						size_t index = ed.bp_rows[view->iItem].second;
						bool enabled = ListView_GetCheckState(ed.breakpoint_list, view->iItem) != FALSE;
						if (IndexOf(doc) >= 0 && index < doc->breakpoints.size() && doc->breakpoints[index].enabled != enabled)
						{
							doc->breakpoints[index].enabled = enabled;
							SendBreakpoints(doc);
							InvalidateRect(ed.gutter, nullptr, TRUE);
						}
					}
					if ((view->uChanged & LVIF_STATE) && ((view->uNewState ^ view->uOldState) & LVIS_SELECTED))
						FillBreakpointFields();
				}
				else if (header->code == NM_DBLCLK)
					GoToBreakpoint(((NMITEMACTIVATE *)lparam)->iItem);
				else if (header->code == LVN_KEYDOWN && ((NMLVKEYDOWN *)lparam)->wVKey == VK_DELETE)
					RemoveBreakpointAt(ListView_GetNextItem(ed.breakpoint_list, -1, LVNI_SELECTED));
				return 0;
			}
			if (header->hwndFrom == ed.tree)
			{
				if (header->code == TVN_SELCHANGEDW)
				{
					if (auto doc = TreeDoc(((NMTREEVIEWW *)lparam)->itemNew.hItem))
						SetStatus(DocSummary(doc));
				}
				else if (header->code == NM_DBLCLK)
				{
					auto api = TreeDoc(TreeView_GetSelection(ed.tree));
					if (Document *doc = Active(); api && doc)
					{
						ReplaceSelection(doc->edit, Widen(api->name));
						SetFocus(doc->edit);
					}
					return 1;
				}
				else if (header->code == TVN_GETINFOTIPW)
				{
					auto tip = (NMTVGETINFOTIPW *)lparam;
					if (auto api = TreeDoc(tip->hItem))
						lstrcpynW(tip->pszText, Widen(api->summary).c_str(), tip->cchTextMax);
				}
				return 0;
			}
			if (header->code == EN_MSGFILTER)
			{
				auto filter = (MSGFILTER *)lparam;
				if (header->hwndFrom == ed.console)
				{
					if (filter->msg == WM_LBUTTONDBLCLK)
					{
						ConsoleDoubleClick(filter->lParam);
						return 1;
					}
					return 0;
				}
				for (auto &doc : ed.docs)
					if (doc->edit == header->hwndFrom)
						return EditorKey(doc.get(), filter) ? 1 : 0;
				return 0;
			}
			if (header->code == EN_SELCHANGE)
			{
				if (Document *doc = Active(); doc && header->hwndFrom == doc->edit)
				{
					CHARRANGE selection = Selection(doc->edit);
					LONG line = LineOf(doc->edit, selection.cpMax);
					wchar_t text[64];
					swprintf(text, 64, _L(L"Line %ld, Col %ld"), line + 1, selection.cpMax - LineStart(doc->edit, line) + 1);
					SendMessageW(ed.status, SB_SETTEXTW, 1, (LPARAM)text);
					InvalidateRect(ed.gutter, nullptr, TRUE);
				}
				return 0;
			}
			break;
		}

		case WM_LUA_AFTER_CHAR:
			if (wparam == L'(')
				ShowCallTip();
			else
				UpdateCompletion(false);
			return 0;

		case WM_LUA_POLL:
			Poll();
			return 0;

		case WM_TIMER:
			if (wparam == TIMER_POLL)
				Poll();
			else if (wparam == TIMER_HIGHLIGHT)
			{
				KillTimer(hwnd, TIMER_HIGHLIGHT);
				if (Document *doc = Active())
					Highlight(doc);
			}
			else if (wparam == TIMER_MODAL)
				WinRunFrameInModalLoop();
			return 0;

		// A menu open or the window being moved or sized: Windows runs its own
		// loop till it's done, so the editor's timer keeps the game going meanwhile.
		case WM_ENTERMENULOOP:
		case WM_ENTERSIZEMOVE:
			SetTimer(hwnd, TIMER_MODAL, 1, nullptr);
			break;
		case WM_EXITMENULOOP:
		case WM_EXITSIZEMOVE:
			KillTimer(hwnd, TIMER_MODAL);
			break;

		case WM_INITMENUPOPUP:
			if ((HMENU)wparam == ed.recent_menu)
				FillRecentMenu();
			else
			{
				Document *doc = Active();
				EnableMenuItem(ed.menu, CMD_PAUSE, doc && doc->script ? MF_ENABLED : MF_GRAYED);
				EnableMenuItem(ed.menu, CMD_STOP, doc && doc->script ? MF_ENABLED : MF_GRAYED);
				EnableMenuItem(ed.menu, CMD_STOP_ALL, !ed.names.empty() ? MF_ENABLED : MF_GRAYED);
				CheckMenuItem(ed.menu, CMD_REFERENCE, ed.show_reference ? MF_CHECKED : MF_UNCHECKED);
				for (UINT command : { CMD_CONTINUE, CMD_STEP_OVER, CMD_STEP_INTO, CMD_STEP_OUT })
					EnableMenuItem(ed.menu, command, ed.stopped ? MF_ENABLED : MF_GRAYED);
				EnableMenuItem(ed.menu, CMD_BREAK, !ed.stopped && doc && doc->script ? MF_ENABLED : MF_GRAYED);
				EnableMenuItem(ed.menu, CMD_REMOVE_BREAKPOINTS, !ed.bp_rows.empty() ? MF_ENABLED : MF_GRAYED);
				CheckMenuItem(ed.menu, CMD_BREAK_ON_ERROR, GUI.LuaBreakOnError ? MF_CHECKED : MF_UNCHECKED);
			}
			return 0;

		case WM_DROPFILES:
		{
			HDROP drop = (HDROP)wparam;
			UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
			for (UINT i = 0; i < count; i++)
			{
				wchar_t path[MAX_PATH];
				if (DragQueryFileW(drop, i, path, MAX_PATH))
					OpenScript(path, false);
			}
			DragFinish(drop);
			return 0;
		}

		case WM_ACTIVATE:
			if (LOWORD(wparam) == WA_INACTIVE)
				HideCompletion();
			else if (Document *doc = Active())
				SetFocus(doc->edit);
			return 0;

		case WM_CLOSE:
		{
			// Closing only hides the editor; scripts keep running, a stopped one too.
			if (ed.stopped)
				S9xLuaDebugCommand(ed.stopped, S9X_LUA_DEBUG_CONTINUE);
			RECT r;
			if (!IsIconic(hwnd) && !IsZoomed(hwnd) && GetWindowRect(hwnd, &r))
			{
				GUI.LuaWindowX = r.left;
				GUI.LuaWindowY = r.top;
				GUI.LuaWindowWidth = r.right - r.left;
				GUI.LuaWindowHeight = r.bottom - r.top;
			}
			HideCompletion();
			ShowWindow(hwnd, SW_HIDE);
			return 0;
		}
	}
	return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void CreateEditorWindow()
{
	static bool registered = false;
	if (!registered)
	{
		LoadLibraryW(L"Msftedit.dll");
		INITCOMMONCONTROLSEX controls = { sizeof(controls), ICC_TAB_CLASSES | ICC_TREEVIEW_CLASSES | ICC_BAR_CLASSES };
		InitCommonControlsEx(&controls);

		WNDCLASSEXW wc = { sizeof(wc) };
		wc.lpfnWndProc = WindowProc;
		wc.style = CS_DBLCLKS;	// a double-click resets the splitter
		wc.hInstance = g_hInst;
		wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
		wc.hbrBackground = (HBRUSH)(COLOR_3DFACE + 1);
		wc.lpszClassName = kWindowClass;
		wc.hIcon = (HICON)SendMessageW(GUI.hWnd, WM_GETICON, ICON_BIG, 0);
		wc.hIconSm = (HICON)SendMessageW(GUI.hWnd, WM_GETICON, ICON_SMALL, 0);
		RegisterClassExW(&wc);

		WNDCLASSEXW gutter = { sizeof(gutter) };
		gutter.lpfnWndProc = GutterProc;
		gutter.hInstance = g_hInst;
		gutter.hCursor = LoadCursor(nullptr, IDC_ARROW);
		gutter.lpszClassName = kGutterClass;
		RegisterClassExW(&gutter);
		registered = true;
	}

	HDC screen = GetDC(nullptr);
	ed.scale = GetDeviceCaps(screen, LOGPIXELSY) / 96.0;
	ReleaseDC(nullptr, screen);
	ed.font_size = GUI.LuaFontSize > 0 ? GUI.LuaFontSize : 10;
	ed.show_reference = GUI.LuaShowReference;
	ed.panel_height = std::max(0, GUI.LuaPanelHeight);

	int w = GUI.LuaWindowWidth > 0 ? GUI.LuaWindowWidth : Scaled(960);
	int h = GUI.LuaWindowHeight > 0 ? GUI.LuaWindowHeight : Scaled(680);
	int x = GUI.LuaWindowWidth > 0 ? GUI.LuaWindowX : CW_USEDEFAULT;
	int y = GUI.LuaWindowWidth > 0 ? GUI.LuaWindowY : CW_USEDEFAULT;
	ed.menu = BuildMenu();
	ed.wnd = CreateWindowExW(WS_EX_ACCEPTFILES, kWindowClass, _L(L"Lua Scripting"),
	                         WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w, h, GUI.hWnd, ed.menu, g_hInst, nullptr);

	NONCLIENTMETRICSW metrics = { sizeof(metrics) };
	SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
	ed.ui_font = CreateFontIndirectW(&metrics.lfMessageFont);
	{
		// Enough trailing spaces after a tab's title for its close box.
		HDC dc = GetDC(ed.wnd);
		HGDIOBJ old = SelectObject(dc, ed.ui_font);
		SIZE space = {};
		GetTextExtentPoint32W(dc, L" ", 1, &space);
		SelectObject(dc, old);
		ReleaseDC(ed.wnd, dc);
		ed.close_spaces = std::max(1, (int)((Scaled(20) + space.cx - 1) / std::max<LONG>(space.cx, 1)));
	}
	MakeCodeFont();

	auto child = [](const wchar_t *cls, const wchar_t *text, DWORD style, int id, DWORD ex = 0) {
		HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, ed.wnd,
		                         (HMENU)(INT_PTR)id, g_hInst, nullptr);
		SendMessageW(h, WM_SETFONT, (WPARAM)ed.ui_font, FALSE);
		return h;
	};

	const wchar_t *labels[kButtons] = { L"New", L"Open", L"Save", L"Run", L"Pause", L"Stop",
	                                    L"Continue", L"Step Over", L"Step Into", L"Step Out" };
	for (int i = 0; i < kButtons; i++)
		ed.buttons[i] = child(L"BUTTON", _L(labels[i]), BS_PUSHBUTTON | WS_TABSTOP, IDC_BUTTON_NEW + i);
	ed.find = child(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, IDC_FIND, WS_EX_CLIENTEDGE);
	SendMessageW(ed.find, EM_SETCUEBANNER, TRUE, (LPARAM)_L(L"Find (or :line)"));
	SetWindowSubclass(ed.find, FindProc, 1, 0);

	ed.tabs = child(WC_TABCONTROLW, L"", WS_CLIPSIBLINGS | TCS_FOCUSNEVER, IDC_TABS);
	SetWindowSubclass(ed.tabs, TabsProc, 4, 0);
	ed.gutter = child(kGutterClass, L"", 0, 0);
	SetWindowPos(ed.tabs, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	ed.panel_tabs = child(WC_TABCONTROLW, L"", WS_CLIPSIBLINGS | TCS_FOCUSNEVER, IDC_PANEL_TABS);
	const wchar_t *panels[PANEL_COUNT] = { L"Console", L"Variables", L"Watch", L"Call Stack", L"Breakpoints" };
	for (int i = 0; i < PANEL_COUNT; i++)
	{
		std::wstring label = _L(panels[i]);
		TCITEMW item = {};
		item.mask = TCIF_TEXT;
		item.pszText = &label[0];
		TabCtrl_InsertItem(ed.panel_tabs, i, &item);
	}
	ed.clear = child(L"BUTTON", _L(L"Clear"), BS_PUSHBUTTON, IDC_CLEAR);
	ed.console = child(kRichEditClass, L"", WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, IDC_CONSOLE,
	                   WS_EX_CLIENTEDGE);
	SendMessageW(ed.console, EM_SETEVENTMASK, 0, ENM_MOUSEEVENTS);
	ApplyFont(ed.console);

	// The debugger's panels.
	DWORD tree_style = TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS | WS_TABSTOP;
	ed.variables = child(WC_TREEVIEWW, L"", tree_style, IDC_VARIABLES, WS_EX_CLIENTEDGE);
	ed.watch_edit = child(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, IDC_WATCH_EDIT, WS_EX_CLIENTEDGE);
	SendMessageW(ed.watch_edit, EM_SETCUEBANNER, TRUE, (LPARAM)_L(L"Add a watch: type an expression and press Enter"));
	SetWindowSubclass(ed.watch_edit, DebugFieldProc, 2, 0);
	ed.watch = child(WC_TREEVIEWW, L"", tree_style, IDC_WATCH, WS_EX_CLIENTEDGE);

	auto columns = [](HWND list, std::initializer_list<std::pair<const wchar_t *, int>> list_columns) {
		int index = 0;
		for (auto &column : list_columns)
		{
			std::wstring label = _L(column.first);
			LVCOLUMNW lv = {};
			lv.mask = LVCF_TEXT | LVCF_WIDTH;
			lv.cx = Scaled(column.second);
			lv.pszText = &label[0];
			ListView_InsertColumn(list, index++, &lv);
		}
	};
	DWORD list_style = LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER | WS_TABSTOP;
	ed.callstack = child(WC_LISTVIEWW, L"", list_style, IDC_CALLSTACK, WS_EX_CLIENTEDGE);
	ListView_SetExtendedListViewStyle(ed.callstack, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
	columns(ed.callstack, { { L"Function", 260 }, { L"Location", 260 } });
	ed.breakpoint_list = child(WC_LISTVIEWW, L"", list_style, IDC_BREAKPOINTS, WS_EX_CLIENTEDGE);
	ListView_SetExtendedListViewStyle(ed.breakpoint_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_CHECKBOXES);
	columns(ed.breakpoint_list, { { L"Location", 170 }, { L"Condition", 170 }, { L"Hit Count", 80 }, { L"Log Message", 230 } });

	const wchar_t *field_labels[3] = { L"Condition:", L"Hit count:", L"Log message:" };
	const wchar_t *field_cues[3] = { L"lives < 3", L"5, >= 5 or % 5", L"x is {x}" };
	for (int i = 0; i < 3; i++)
	{
		ed.bp_labels[i] = child(L"STATIC", _L(field_labels[i]), SS_RIGHT, 0);
		ed.bp_fields[i] = child(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, IDC_BP_CONDITION + i, WS_EX_CLIENTEDGE);
		SendMessageW(ed.bp_fields[i], EM_SETCUEBANNER, TRUE, (LPARAM)field_cues[i]);
		SetWindowSubclass(ed.bp_fields[i], DebugFieldProc, 3, 0);
		EnableWindow(ed.bp_fields[i], FALSE);
	}

	ed.search = child(L"EDIT", L"", ES_AUTOHSCROLL, IDC_SEARCH, WS_EX_CLIENTEDGE);
	SendMessageW(ed.search, EM_SETCUEBANNER, TRUE, (LPARAM)_L(L"Search functions"));
	ed.tree = child(WC_TREEVIEWW, L"", TVS_HASBUTTONS | TVS_LINESATROOT | TVS_SHOWSELALWAYS | TVS_INFOTIP | WS_TABSTOP,
	                IDC_TREE, WS_EX_CLIENTEDGE);
	ed.status = child(STATUSCLASSNAMEW, L"", SBARS_SIZEGRIP, IDC_STATUS);

	ed.completion = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"LISTBOX", L"",
	                                WS_POPUP | WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_HASSTRINGS,
	                                0, 0, 10, 10, ed.wnd, nullptr, g_hInst, nullptr);
	SendMessageW(ed.completion, WM_SETFONT, (WPARAM)ed.code_font, FALSE);

	ACCEL keys[] =
	{
		{ FCONTROL | FVIRTKEY, 'N', CMD_NEW }, { FCONTROL | FVIRTKEY, 'O', CMD_OPEN },
		{ FCONTROL | FVIRTKEY, 'S', CMD_SAVE }, { FCONTROL | FSHIFT | FVIRTKEY, 'S', CMD_SAVE_AS },
		{ FCONTROL | FVIRTKEY, 'W', CMD_CLOSE_TAB }, { FCONTROL | FVIRTKEY, VK_OEM_2, CMD_TOGGLE_COMMENT },
		{ FCONTROL | FVIRTKEY, 'F', CMD_FIND }, { FVIRTKEY, VK_F3, CMD_FIND_NEXT },
		{ FSHIFT | FVIRTKEY, VK_F3, CMD_FIND_PREVIOUS }, { FCONTROL | FVIRTKEY, 'G', CMD_GOTO },
		{ FVIRTKEY, VK_F5, CMD_RUN }, { FVIRTKEY, VK_F6, CMD_PAUSE }, { FSHIFT | FVIRTKEY, VK_F5, CMD_STOP },
		{ FVIRTKEY, VK_F1, CMD_REFERENCE }, { FCONTROL | FVIRTKEY, 'L', CMD_CLEAR_CONSOLE },
		{ FCONTROL | FVIRTKEY, VK_OEM_PLUS, CMD_ZOOM_IN }, { FCONTROL | FVIRTKEY, VK_OEM_MINUS, CMD_ZOOM_OUT },
		{ FCONTROL | FVIRTKEY, '0', CMD_ZOOM_RESET },
		{ FVIRTKEY, VK_F8, CMD_BREAK }, { FVIRTKEY, VK_F9, CMD_TOGGLE_BREAKPOINT },
		{ FSHIFT | FVIRTKEY, VK_F9, CMD_ADD_WATCH }, { FCONTROL | FSHIFT | FVIRTKEY, VK_F9, CMD_REMOVE_BREAKPOINTS },
		{ FVIRTKEY, VK_F10, CMD_STEP_OVER }, { FVIRTKEY, VK_F11, CMD_STEP_INTO }, { FSHIFT | FVIRTKEY, VK_F11, CMD_STEP_OUT }
	};
	ed.accel = CreateAcceleratorTableW(keys, sizeof(keys) / sizeof(keys[0]));

	FillReference();
	SetTimer(ed.wnd, TIMER_POLL, 40, nullptr);

	int count = sizeof(GUI.LuaOpenScripts) / sizeof(GUI.LuaOpenScripts[0]);
	for (int i = 0; i < count; i++)
		if (*GUI.LuaOpenScripts[i] && GetFileAttributesW(GUI.LuaOpenScripts[i]) != INVALID_FILE_ATTRIBUTES)
			OpenScript(GUI.LuaOpenScripts[i], false);

	// Scripts started from the command line get a tab too.
	for (auto &info : S9xLuaScripts())
	{
		if (ForScript(info.id) || info.path.empty())
			continue;
		OpenScript(Widen(info.path), false);
		if (Document *doc = Active(); doc && !doc->script)
		{
			doc->script = info.id;
			doc->paused = info.state == S9X_LUA_PAUSED;
			ed.names.push_back({ info.id, info.name });
			UpdateTab(doc);
		}
	}

	if (ed.docs.empty())
		AddDocument(S9xLuaNewScriptTemplate(), L"", L"");
	Layout();
}

void SaveSession()
{
	int count = sizeof(GUI.LuaOpenScripts) / sizeof(GUI.LuaOpenScripts[0]);
	int n = 0;
	for (auto &doc : ed.docs)
		if (!doc->path.empty() && n < count)
			lstrcpynW(GUI.LuaOpenScripts[n++], doc->path.c_str(), MAX_PATH);
	for (; n < count; n++)
		*GUI.LuaOpenScripts[n] = L'\0';
	GUI.LuaFontSize = ed.font_size;
	GUI.LuaShowReference = ed.show_reference;
	GUI.LuaPanelHeight = ed.panel_height;
}

void CALLBACK IdleTick(HWND, UINT, UINT_PTR, DWORD)
{
	// While nothing is emulated scripts still get their emu.yield() turns.
	if (Settings.Paused || Settings.StopEmulation || Settings.ForcedPause)
		S9xLuaIdle();
}

} // namespace

void WinLuaInit()
{
	S9xLuaFrontend frontend;
	frontend.set_paused = [](bool paused) {
		Settings.Paused = paused;
		Settings.FrameAdvance = false;
	};
	frontend.reset = [](bool power) {
		// As Emulation > Soft Reset / Hard Reset, between frames.
		PostMessage(GUI.hWnd, WM_COMMAND, power ? ID_EMULATION_HARD_RESET : ID_EMULATION_SOFT_RESET, 0);
	};
	frontend.wake_ui = [] {
		if (ed.wnd && !ed.poll_posted)
		{
			ed.poll_posted = true;
			PostMessageW(ed.wnd, WM_LUA_POLL, 0, 0);
		}
	};
	frontend.default_dir = [] {
		// The emulator's own folder.
		wchar_t path[MAX_PATH];
		DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
		std::wstring dir(path, n < MAX_PATH ? n : 0);
		size_t slash = dir.find_last_of(L"\\/");
		return slash == std::wstring::npos ? std::string() : Narrow(dir.substr(0, slash + 1));
	};
	frontend.debug_break = DebuggerStopped;
	frontend.debug_wait = DebuggerWait;
	S9xLuaSetFrontend(frontend);
	S9xLuaDebugSetBreakOnError(GUI.LuaBreakOnError);
	SetTimer(nullptr, 0, 15, IdleTick);
}

void WinShowLuaWindow()
{
	if (!ed.wnd)
		CreateEditorWindow();
	ShowWindow(ed.wnd, SW_SHOW);
	SetForegroundWindow(ed.wnd);
	if (Document *doc = Active())
		SetFocus(doc->edit);
}

void WinLuaAddCommandLineScript(const char *utf8_path)
{
	command_line_scripts.push_back(utf8_path);
}

void WinLuaRunCommandLineScripts()
{
	for (auto &path : command_line_scripts)
	{
		if (ed.wnd)
			OpenScript(Widen(path), true);
		else
			S9xLuaRunFile(S9xLuaNewScriptId(), path);
	}
	command_line_scripts.clear();
}

bool WinLuaConfirmClose()
{
	for (auto &doc : ed.docs)
		if (!AskSave(doc.get()))
			return false;
	return true;
}

void WinLuaShutdown()
{
	if (ed.wnd)
	{
		SaveSession();
		for (auto &doc : ed.docs)
		{
			ReleaseBreakpoints(doc.get());
			if (doc->tom)
				doc->tom->Release();
		}
		ed.docs.clear();
		DestroyWindow(ed.completion);
		DestroyWindow(ed.wnd);
		ed.wnd = nullptr;
	}
	S9xLuaShutdown();
	if (ed.code_font)
		DeleteObject(ed.code_font);
	if (ed.ui_font)
		DeleteObject(ed.ui_font);
	ed.code_font = ed.ui_font = nullptr;
}

bool WinLuaHandleMessage(MSG *msg)
{
	if (!ed.wnd || !(msg->hwnd == ed.wnd || IsChild(ed.wnd, msg->hwnd) || msg->hwnd == ed.completion))
		return false;
	// F10 arrives as a system key that would open the menu bar: here it steps over.
	if ((msg->message == WM_SYSKEYDOWN || msg->message == WM_SYSKEYUP) && msg->wParam == VK_F10 && GetKeyState(VK_MENU) >= 0)
	{
		if (msg->message == WM_SYSKEYDOWN)
			Command(CMD_STEP_OVER);
		return true;
	}
	// The editor's keys never reach the emulator's accelerators or hotkeys.
	if (!TranslateAcceleratorW(ed.wnd, ed.accel, msg))
	{
		TranslateMessage(msg);
		DispatchMessageW(msg);
	}
	return true;
}
