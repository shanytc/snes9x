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
	CMD_RECENT_FIRST = 41000, CMD_EXAMPLE_FIRST = 42000
};

enum { TIMER_POLL = 1, TIMER_HIGHLIGHT = 2 };
const UINT WM_LUA_AFTER_CHAR = WM_APP + 41;

const COLORREF kKeyword = RGB(0, 51, 179), kBuiltin = RGB(0, 128, 128), kNumber = RGB(176, 80, 0);
const COLORREF kString = RGB(26, 128, 26), kComment = RGB(112, 112, 112), kText = RGB(0, 0, 0);
const COLORREF kErrorBack = RGB(246, 200, 200);
const COLORREF kInfo = RGB(26, 95, 180), kWarning = RGB(160, 96, 0), kError = RGB(192, 16, 16), kQuiet = RGB(112, 112, 112);

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
};

struct Editor
{
	HWND wnd = nullptr, tabs = nullptr, gutter = nullptr, console = nullptr, console_label = nullptr;
	HWND tree = nullptr, search = nullptr, status = nullptr, find = nullptr, clear = nullptr;
	HWND completion = nullptr, buttons[6] = {};
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
	int gutter_width = 40;
	double scale = 1.0;
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

void SetStatus(const std::wstring &text)
{
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

void UpdateActions()
{
	Document *doc = Active();
	bool running = doc && doc->script;
	SetWindowTextW(ed.buttons[3], running ? _L(L"Restart") : _L(L"Run"));
	SetWindowTextW(ed.buttons[4], doc && doc->paused ? _L(L"Resume") : _L(L"Pause"));
	EnableWindow(ed.buttons[3], doc != nullptr);
	EnableWindow(ed.buttons[4], running);
	EnableWindow(ed.buttons[5], running);

	size_t count = ed.names.size();
	wchar_t text[128];
	if (count == 0)
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
	TCITEMW item = {};
	item.mask = TCIF_TEXT;
	item.pszText = &title[0];
	TabCtrl_SetItem(ed.tabs, index, &item);
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

void Highlight(Document *doc)
{
	if (!doc->tom)
		return;
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

		if (line == doc->error_line)
			BackRange(doc->tom, (long)start, (long)end + 1, kErrorBack);
		if (end == text.size())
			break;
		start = end + 1;
	}

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
	SelectObject(dc, old);
	ReleaseDC(ed.wnd, dc);
	ed.gutter_width = size.cx + Scaled(8);
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
	std::wstring title = DisplayName(raw);
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
	std::wstring message = std::wstring(_L(L"Save changes to ")) + DisplayName(doc) + L"?";
	int answer = MessageBoxW(ed.wnd, message.c_str(), _L(L"Lua Scripting"), MB_YESNOCANCEL | MB_ICONQUESTION);
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
	if (doc->tom)
		doc->tom->Release();
	DestroyWindow(doc->edit);
	ed.docs.erase(ed.docs.begin() + index);
	Activate(std::min(index, (int)ed.docs.size() - 1));
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

void Poll()
{
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

	int pad = Scaled(4), button_w = Scaled(72), button_h = Scaled(26);
	for (int i = 0; i < 6; i++)
		MoveWindow(ed.buttons[i], pad + i * (button_w + pad) + (i >= 3 ? Scaled(12) : 0), pad, button_w, button_h, TRUE);
	MoveWindow(ed.find, width - pad - Scaled(200), pad + Scaled(2), Scaled(200), button_h - Scaled(4), TRUE);

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

	int console_h = std::max(Scaled(110), (bottom - top) * 3 / 10);
	int header_h = Scaled(24);
	int editor_bottom = bottom - console_h - header_h - pad;
	MoveWindow(ed.tabs, pad, top, left_w, editor_bottom - top, TRUE);
	RECT inner = { pad, top, pad + left_w, editor_bottom };
	TabCtrl_AdjustRect(ed.tabs, FALSE, &inner);
	MoveWindow(ed.gutter, inner.left, inner.top, ed.gutter_width, inner.bottom - inner.top, TRUE);
	for (auto &doc : ed.docs)
		MoveWindow(doc->edit, inner.left + ed.gutter_width, inner.top, inner.right - inner.left - ed.gutter_width,
		           inner.bottom - inner.top, TRUE);

	MoveWindow(ed.console_label, pad + Scaled(2), editor_bottom + pad + Scaled(4), Scaled(120), Scaled(18), TRUE);
	MoveWindow(ed.clear, pad + left_w - Scaled(64), editor_bottom + pad, Scaled(64), Scaled(22), TRUE);
	MoveWindow(ed.console, pad, editor_bottom + header_h + pad, left_w, console_h - pad, TRUE);
	InvalidateRect(ed.gutter, nullptr, TRUE);
}

LRESULT CALLBACK GutterProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	if (msg != WM_PAINT)
		return DefWindowProcW(hwnd, msg, wparam, lparam);

	PAINTSTRUCT ps;
	HDC dc = BeginPaint(hwnd, &ps);
	RECT client;
	GetClientRect(hwnd, &client);
	FillRect(dc, &client, GetSysColorBrush(COLOR_3DFACE));

	Document *doc = Active();
	if (doc)
	{
		HGDIOBJ old = SelectObject(dc, ed.code_font);
		SetBkMode(dc, TRANSPARENT);
		LONG first = (LONG)SendMessageW(doc->edit, EM_GETFIRSTVISIBLELINE, 0, 0);
		LONG count = (LONG)SendMessageW(doc->edit, EM_GETLINECOUNT, 0, 0);
		LONG current = LineOf(doc->edit, Selection(doc->edit).cpMax);
		for (LONG line = first; line < count; line++)
		{
			POINTL point = {};
			SendMessageW(doc->edit, EM_POSFROMCHAR, (WPARAM)&point, LineStart(doc->edit, line));
			if (point.y > client.bottom)
				break;
			COLORREF color = GetSysColor(COLOR_GRAYTEXT);
			if (line + 1 == doc->error_line)
				color = kError;
			else if (line == current)
				color = GetSysColor(COLOR_WINDOWTEXT);
			SetTextColor(dc, color);
			wchar_t number[16];
			swprintf(number, 16, L"%ld", line + 1);
			RECT row = { 0, point.y, client.right - Scaled(4), point.y + Scaled(40) };
			DrawTextW(dc, number, -1, &row, DT_RIGHT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
		}
		SelectObject(dc, old);
	}
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
		case CMD_RUN:			if (doc) Run(doc); break;
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

		case WM_GETMINMAXINFO:
		{
			auto info = (MINMAXINFO *)lparam;
			info->ptMinTrackSize = { Scaled(560), Scaled(380) };
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
				if (code == EN_CHANGE && !ed.loading)
				{
					if (!doc->modified)
					{
						doc->modified = true;
						UpdateTab(doc);
					}
					doc->error_line = 0;
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
			switch (id)
			{
				case IDC_BUTTON_NEW:	Command(CMD_NEW); break;
				case IDC_BUTTON_OPEN:	Command(CMD_OPEN); break;
				case IDC_BUTTON_SAVE:	Command(CMD_SAVE); break;
				case IDC_BUTTON_RUN:	Command(CMD_RUN); break;
				case IDC_BUTTON_PAUSE:	Command(CMD_PAUSE); break;
				case IDC_BUTTON_STOP:	Command(CMD_STOP); break;
				case IDC_CLEAR:			ClearConsole(); break;
				default:				Command(id); break;
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

		case WM_TIMER:
			if (wparam == TIMER_POLL)
				Poll();
			else if (wparam == TIMER_HIGHLIGHT)
			{
				KillTimer(hwnd, TIMER_HIGHLIGHT);
				if (Document *doc = Active())
					Highlight(doc);
			}
			return 0;

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
			// Closing only hides the editor; scripts keep running.
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
	MakeCodeFont();

	auto child = [](const wchar_t *cls, const wchar_t *text, DWORD style, int id, DWORD ex = 0) {
		HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, ed.wnd,
		                         (HMENU)(INT_PTR)id, g_hInst, nullptr);
		SendMessageW(h, WM_SETFONT, (WPARAM)ed.ui_font, FALSE);
		return h;
	};

	const wchar_t *labels[6] = { L"New", L"Open", L"Save", L"Run", L"Pause", L"Stop" };
	for (int i = 0; i < 6; i++)
		ed.buttons[i] = child(L"BUTTON", _L(labels[i]), BS_PUSHBUTTON | WS_TABSTOP, IDC_BUTTON_NEW + i);
	ed.find = child(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, IDC_FIND, WS_EX_CLIENTEDGE);
	SendMessageW(ed.find, EM_SETCUEBANNER, TRUE, (LPARAM)_L(L"Find (or :line)"));
	SetWindowSubclass(ed.find, FindProc, 1, 0);

	ed.tabs = child(WC_TABCONTROLW, L"", WS_CLIPSIBLINGS | TCS_FOCUSNEVER, IDC_TABS);
	ed.gutter = child(kGutterClass, L"", 0, 0);
	ed.console_label = child(L"STATIC", _L(L"Console"), SS_LEFT, 0);
	ed.clear = child(L"BUTTON", _L(L"Clear"), BS_PUSHBUTTON, IDC_CLEAR);
	ed.console = child(kRichEditClass, L"", WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, IDC_CONSOLE,
	                   WS_EX_CLIENTEDGE);
	SendMessageW(ed.console, EM_SETEVENTMASK, 0, ENM_MOUSEEVENTS);
	ApplyFont(ed.console);

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
		{ FCONTROL | FVIRTKEY, '0', CMD_ZOOM_RESET }
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
	S9xLuaSetFrontend(frontend);
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
			if (doc->tom)
				doc->tom->Release();
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
	// The editor's keys never reach the emulator's accelerators or hotkeys.
	if (!TranslateAcceleratorW(ed.wnd, ed.accel, msg))
	{
		TranslateMessage(msg);
		DispatchMessageW(msg);
	}
	return true;
}
