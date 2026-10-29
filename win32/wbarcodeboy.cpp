#include "wbarcodeboy.h"
#include "win32_webcam.h"
#include "wlocale.h"
#include "rsrc/resource.h"

#include "../snes9x.h"
#include "../display.h"
#include "../sgb/sgb.h"
#include "../sgb/gb_barcodeboy.h"
#include "../sgb/gb_bardigun.h"

#define JXL_STATIC_DEFINE
#include <jxl/decode.h>

#include <commdlg.h>
#include <commctrl.h>
#include <shellapi.h>
#include <wincodec.h>
#include <tchar.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <random>
#include <string>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")

extern HINSTANCE g_hInst;

namespace {

// The link-port scanners share the dialog; each brings its cards and calls.
struct Device
{
	int          dialog;
	const TCHAR *name;
	const char  *osd;
	const SGB::BarcodeBoyCard *(*cards)(int *count);
	bool        (*scan)(const char *code);
	const char *(*lastCode)();
	const char *(*game)();
	bool         ean8;	// takes 8-digit codes too
};

const Device kBarcodeBoy = { IDD_BARCODE_BOY, TEXT("Barcode Boy"), "Barcode Boy", SGB::BarcodeBoyCards,
                             S9xSGBBarcodeBoyScan, S9xSGBBarcodeBoyLastCode, S9xSGBBarcodeBoyGame, false };
const Device kBardigun = { IDD_BARDIGUN, TEXT("Bardigun Card Reader"), "Bardigun Card Reader", SGB::BardigunCards,
                           S9xSGBBardigunScan, S9xSGBBardigunLastCode, S9xSGBBardigunGame, true };
const Device *g_device = &kBarcodeBoy;

const UINT_PTR kCameraTimer = 1;

bool                       g_cameraOn = false;
std::vector<unsigned char> g_frame;
int                        g_frameW = 0;
int                        g_frameH = 0;

// The shown card's photos from the barcodeboy folder beside the executable.
struct Photo { std::vector<unsigned char> bgrx; int w = 0, h = 0; };
std::wstring               g_photoDir;
const char                *g_photoStem = nullptr;
std::vector<Photo>         g_photos;
// Refilling the tree drops its selection; that isn't the user picking nothing.
bool                       g_filling = false;
// The whole code typed in: drawn in the preview when it has no photos.
std::string                g_code;

std::wstring Wide(const char *utf8)
{
	const int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
	std::wstring w(n > 0 ? n - 1 : 0, L'\0');
	if (n > 1)
		MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &w[0], n);
	return w;
}

std::wstring Lower(std::wstring s)
{
	if (!s.empty())
		CharLowerBuffW(&s[0], (DWORD)s.size());
	return s;
}

std::string DigitsOf(HWND hDlg)
{
	char text[64] = "";
	GetDlgItemTextA(hDlg, IDC_BARCODEBOY_EDIT, text, sizeof(text));
	std::string digits;
	for (const char *p = text; *p; p++)
		if (*p != ' ' && *p != '-')
			digits += *p;
	return digits;
}

// EAN-13 or EAN-8: the digit next to the check digit weighs 3.
int CheckDigit(const char *digits, int n)
{
	int sum = 0;
	for (int i = 0; i < n - 1; i++)
		sum += (digits[i] - '0') * (((n - 1 - i) & 1) ? 3 : 1);
	return (10 - sum % 10) % 10;
}

const SGB::BarcodeBoyCard *CardWithCode(const Device &device, const char *digits)
{
	int count = 0;
	const SGB::BarcodeBoyCard *cards = device.cards(&count);
	for (int i = 0; i < count; i++)
		if (strcmp(digits, cards[i].code) == 0)
			return &cards[i];
	return nullptr;
}

// Any picture WIC can open, converted to the given pixel format.
bool LoadPixels(const wchar_t *path, REFWICPixelFormatGUID format, int bytesPerPixel,
                std::vector<unsigned char> &pixels, int &w, int &h)
{
	const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	bool ok = false;

	IWICImagingFactory *factory = nullptr;
	IWICBitmapDecoder *decoder = nullptr;
	IWICBitmapFrameDecode *frame = nullptr;
	IWICFormatConverter *converter = nullptr;

	if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
	                               IID_PPV_ARGS(&factory))) &&
	    SUCCEEDED(factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ,
	                                                 WICDecodeMetadataCacheOnDemand, &decoder)) &&
	    SUCCEEDED(decoder->GetFrame(0, &frame)) &&
	    SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
	    SUCCEEDED(converter->Initialize(frame, format, WICBitmapDitherTypeNone,
	                                    nullptr, 0.0, WICBitmapPaletteTypeCustom)))
	{
		UINT uw = 0, uh = 0;
		converter->GetSize(&uw, &uh);
		if (uw && uh)
		{
			w = (int)uw;
			h = (int)uh;
			pixels.resize((size_t)uw * uh * bytesPerPixel);
			ok = SUCCEEDED(converter->CopyPixels(nullptr, uw * bytesPerPixel, (UINT)pixels.size(), pixels.data()));
		}
	}

	if (converter) converter->Release();
	if (frame) frame->Release();
	if (decoder) decoder->Release();
	if (factory) factory->Release();
	if (SUCCEEDED(init))
		CoUninitialize();
	return ok;
}

// A JPEG XL picture as top-down 32-bit BGRX, the layout StretchDIBits draws.
bool LoadJxl(const std::wstring &path, Photo &photo)
{
	FILE *f = _wfopen(path.c_str(), L"rb");
	if (!f)
		return false;
	std::vector<unsigned char> data;
	unsigned char chunk[65536];
	for (size_t n; (n = fread(chunk, 1, sizeof(chunk), f)) > 0; )
		data.insert(data.end(), chunk, chunk + n);
	fclose(f);

	JxlDecoder *dec = JxlDecoderCreate(nullptr);
	const JxlPixelFormat format = { 4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0 };
	bool ok = false;
	if (dec && JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE) == JXL_DEC_SUCCESS &&
	    JxlDecoderSetInput(dec, data.data(), data.size()) == JXL_DEC_SUCCESS)
	{
		JxlDecoderCloseInput(dec);
		for (;;)
		{
			const JxlDecoderStatus status = JxlDecoderProcessInput(dec);
			if (status == JXL_DEC_BASIC_INFO)
			{
				JxlBasicInfo info;
				if (JxlDecoderGetBasicInfo(dec, &info) != JXL_DEC_SUCCESS)
					break;
				photo.w = (int)info.xsize;
				photo.h = (int)info.ysize;
			}
			else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER)
			{
				size_t size = 0;
				if (JxlDecoderImageOutBufferSize(dec, &format, &size) != JXL_DEC_SUCCESS || size != (size_t)photo.w * photo.h * 4)
					break;
				photo.bgrx.resize(size);
				if (JxlDecoderSetImageOutBuffer(dec, &format, photo.bgrx.data(), size) != JXL_DEC_SUCCESS)
					break;
			}
			else if (status != JXL_DEC_FULL_IMAGE)
			{
				ok = status == JXL_DEC_SUCCESS && !photo.bgrx.empty();
				break;
			}
		}
	}
	JxlDecoderDestroy(dec);

	for (size_t i = 0; ok && i < photo.bgrx.size(); i += 4)
		std::swap(photo.bgrx[i], photo.bgrx[i + 2]);	// RGBA to BGRA
	return ok;
}

// The barcodeboy folder beside the executable, or "" when there is none.
std::wstring PhotoDir()
{
	wchar_t exe[MAX_PATH] = L"";
	std::wstring dir(exe, GetModuleFileNameW(nullptr, exe, MAX_PATH));
	dir.erase(dir.find_last_of(L"\\/") + 1);
	dir += L"barcodeboy\\";
	const DWORD attr = GetFileAttributesW(dir.c_str());
	return (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) ? dir : std::wstring();
}

// Puts a card's front and back photos in the preview; none for nullptr.
void ShowCard(HWND hDlg, const SGB::BarcodeBoyCard *card)
{
	const char *stem = card ? card->image : nullptr;
	if (stem == g_photoStem)
		return;

	g_photoStem = stem;
	g_photos.clear();
	if (stem && !g_photoDir.empty())
		for (const wchar_t *side : { L"-front.jxl", L"-back.jxl" })
		{
			std::wstring path = g_photoDir + Wide(stem) + side;
			std::replace(path.begin(), path.end(), L'/', L'\\');
			Photo photo;
			if (LoadJxl(path, photo))
				g_photos.push_back(std::move(photo));
		}

	InvalidateRect(GetDlgItem(hDlg, IDC_BARCODEBOY_PREVIEW), nullptr, TRUE);
}

void CodeEdited(HWND hDlg)
{
	const std::string digits = DigitsOf(hDlg);
	const size_t n = digits.size();
	const bool allDigits = digits.find_first_not_of("0123456789") == std::string::npos;
	const bool lengthOk = allDigits && (n == 13 || (g_device->ean8 && n == 8));
	const SGB::BarcodeBoyCard *card = lengthOk ? CardWithCode(*g_device, digits.c_str()) : nullptr;

	std::wstring status;
	if (n == 0)
		;
	else if (!allDigits)
		status = L"A barcode holds only digits.";
	else if (!lengthOk)
		status = std::to_wstring(n) + (g_device->ean8 ? L" digits; a barcode has 13, or 8 for EAN-8."
		                                              : L" digits; a Barcode Boy card has 13.");
	else if (CheckDigit(digits.c_str(), (int)n) != digits[n - 1] - '0')
		status = L"The check digit doesn't add up. A printed barcode would not read like this, but it can still be sent.";
	else if (card)
		status = Wide(card->name) + L"  " + Wide(card->kana) + L", from " + Wide(card->game) + L".";
	else if (n == 8)
		status = L"Valid EAN-8 barcode. It reads, but it is too weak to hatch an egg.";
	else
		status = L"Valid EAN-13 barcode.";

	SetDlgItemTextW(hDlg, IDC_BARCODEBOY_STATUS, status.c_str());
	EnableWindow(GetDlgItem(hDlg, IDOK), lengthOk);
	ShowCard(hDlg, card);

	const std::string code = lengthOk ? digits : std::string();
	if (code != g_code)
	{
		g_code = code;
		InvalidateRect(GetDlgItem(hDlg, IDC_BARCODEBOY_PREVIEW), nullptr, TRUE);
	}
}

void Found(HWND hDlg, const char *code, const TCHAR *where)
{
	SetDlgItemTextA(hDlg, IDC_BARCODEBOY_EDIT, code);
	TCHAR status[128];
	_stprintf(status, TEXT("Read %hs from the %s."), code, where);
	SetDlgItemText(hDlg, IDC_BARCODEBOY_STATUS, status);
}

void ReadImage(HWND hDlg, const TCHAR *file)
{
	std::vector<unsigned char> luma;
	int w = 0, h = 0;
	if (!LoadPixels(file, GUID_WICPixelFormat8bppGray, 1, luma, w, h))
	{
		MessageBox(hDlg, TEXT("This picture could not be opened."), g_device->name, MB_OK | MB_ICONWARNING);
		return;
	}

	char code[14];
	if (SGB::BarcodeBoyDecode(luma.data(), w, h, w, code))
		Found(hDlg, code, TEXT("picture"));
	else
		MessageBox(hDlg, TEXT("No EAN-13 barcode was found in this picture. Crop it closer to the barcode and keep the bars upright."),
		           g_device->name, MB_OK | MB_ICONINFORMATION);
}

void FromImage(HWND hDlg)
{
	TCHAR file[MAX_PATH] = TEXT("");
	OPENFILENAME ofn = {};
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = hDlg;
	ofn.lpstrFilter = TEXT("Images (*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif)\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff\0All Files (*.*)\0*.*\0\0");
	ofn.lpstrFile = file;
	ofn.nMaxFile = MAX_PATH;
	ofn.lpstrTitle = TEXT("Read Barcode from Image");
	ofn.Flags = OFN_HIDEREADONLY | OFN_FILEMUSTEXIST;
	if (GetOpenFileName(&ofn))
		ReadImage(hDlg, file);
}

// A picture dropped anywhere on the dialog reads like one picked with From Image.
void DroppedImage(HWND hDlg, HDROP drop)
{
	TCHAR file[MAX_PATH] = TEXT("");
	const bool got = DragQueryFile(drop, 0, file, MAX_PATH) > 0;
	DragFinish(drop);
	if (got)
		ReadImage(hDlg, file);
}

// One printed card: the run of entries from i sharing its photos.
int CardEnd(const SGB::BarcodeBoyCard *cards, int count, int i)
{
	int j = i + 1;
	while (j < count && !strcmp(cards[j].image, cards[i].image))
		j++;
	return j;
}

bool CardMatches(const SGB::BarcodeBoyCard &card, const std::wstring &find)
{
	return find.empty() || Lower(Wide(card.name)).find(find) != std::wstring::npos ||
	       Wide(card.kana).find(find) != std::wstring::npos ||
	       Lower(Wide(card.game)).find(find) != std::wstring::npos || Wide(card.code).find(find) != std::wstring::npos ||
	       (card.group && Lower(Wide(card.group)).find(find) != std::wstring::npos);
}

// A card with several barcodes is named after them.
std::wstring CardLabel(const SGB::BarcodeBoyCard *cards, int i, int end)
{
	std::wstring label;
	for (int k = i; k < end; k++)
		label += (k > i ? L" / " : L"") + Wide(cards[k].name);
	return label;
}

// The known cards with a game, name or code holding the search text, one
// branch per game, per group within it and per card with several barcodes;
// with nothing to search for, only the loaded game's branch is open.
void FillCards(HWND hDlg)
{
	HWND tree = GetDlgItem(hDlg, IDC_BARCODEBOY_CARDS);
	wchar_t text[64] = L"";
	GetDlgItemTextW(hDlg, IDC_BARCODEBOY_SEARCH, text, 64);
	const std::wstring find = Lower(text);
	const char *loaded = g_device->game();

	SendMessage(tree, WM_SETREDRAW, FALSE, 0);
	g_filling = true;
	TreeView_DeleteAllItems(tree);

	int count = 0;
	const SGB::BarcodeBoyCard *cards = g_device->cards(&count);
	const char *game = nullptr, *group = nullptr;
	HTREEITEM branch = nullptr, groupItem = nullptr;
	int inGame = 0, inGroup = 0;

	auto setCountLabel = [&](HTREEITEM hItem, const char *name, int n) {
		std::wstring label = Wide(name) + L" (" + std::to_wstring(n) + L")";
		TVITEMW item = {};
		item.mask = TVIF_TEXT;
		item.hItem = hItem;
		item.pszText = &label[0];
		SendMessage(tree, TVM_SETITEMW, 0, (LPARAM)&item);
	};
	auto closeGroup = [&]() {
		if (!groupItem)
			return;
		setCountLabel(groupItem, group, inGroup);
		TreeView_Expand(tree, groupItem, TVE_EXPAND);
		groupItem = nullptr;
	};
	auto closeGame = [&]() {
		closeGroup();
		if (!branch)
			return;
		setCountLabel(branch, game, inGame);
		if (!find.empty() || (loaded && !strcmp(loaded, game)))
			TreeView_Expand(tree, branch, TVE_EXPAND);
	};

	for (int i = 0, end; i < count; i = end)
	{
		end = CardEnd(cards, count, i);
		bool hit = false;
		for (int k = i; k < end && !hit; k++)
			hit = CardMatches(cards[k], find);
		if (!hit)
			continue;

		TVINSERTSTRUCTW ins = {};
		ins.hInsertAfter = TVI_LAST;
		ins.item.mask = TVIF_TEXT | TVIF_PARAM;

		if (!game || strcmp(game, cards[i].game))
		{
			closeGame();
			game = cards[i].game;
			inGame = 0;
			std::wstring label = Wide(game);
			ins.hParent = TVI_ROOT;
			ins.item.pszText = &label[0];
			ins.item.lParam = -1;
			branch = (HTREEITEM)SendMessage(tree, TVM_INSERTITEMW, 0, (LPARAM)&ins);
		}

		// lParam: the entry for a barcode, -2 - first entry for a card's branch
		HTREEITEM under = branch;
		if (cards[i].group)
		{
			if (!groupItem || strcmp(group, cards[i].group))
			{
				closeGroup();
				group = cards[i].group;
				inGroup = 0;
				std::wstring label = Wide(group);
				ins.hParent = branch;
				ins.item.pszText = &label[0];
				ins.item.lParam = -1;
				groupItem = (HTREEITEM)SendMessage(tree, TVM_INSERTITEMW, 0, (LPARAM)&ins);
			}
			under = groupItem;
			inGroup++;
		}
		else
			closeGroup();

		HTREEITEM parent = under;
		if (end - i > 1)
		{
			std::wstring label = CardLabel(cards, i, end);
			ins.hParent = under;
			ins.item.pszText = &label[0];
			ins.item.lParam = -2 - i;
			parent = (HTREEITEM)SendMessage(tree, TVM_INSERTITEMW, 0, (LPARAM)&ins);
		}

		for (int k = i; k < end; k++)
		{
			std::wstring label = Wide(cards[k].name) + L"  " + Wide(cards[k].code);
			ins.hParent = parent;
			ins.item.pszText = &label[0];
			ins.item.lParam = k;
			SendMessage(tree, TVM_INSERTITEMW, 0, (LPARAM)&ins);
		}

		if (parent != under && !find.empty())
			TreeView_Expand(tree, parent, TVE_EXPAND);
		inGame++;
	}
	closeGame();
	g_filling = false;

	SendMessage(tree, WM_SETREDRAW, TRUE, 0);
	InvalidateRect(tree, nullptr, TRUE);
}

// The entry under a tree item: a barcode, or with branch set the first
// barcode of a card's branch; nullptr otherwise.
const SGB::BarcodeBoyCard *CardOf(HWND tree, HTREEITEM hItem, bool branch = false)
{
	TVITEMW item = {};
	item.mask = TVIF_PARAM;
	item.hItem = hItem;
	if (!hItem || !SendMessage(tree, TVM_GETITEMW, 0, (LPARAM)&item))
		return nullptr;

	int count = 0;
	const SGB::BarcodeBoyCard *cards = g_device->cards(&count);
	const LPARAM entry = branch ? -2 - item.lParam : item.lParam;
	return entry >= 0 && entry < count ? &cards[entry] : nullptr;
}

void StopCamera(HWND hDlg)
{
	if (!g_cameraOn)
		return;

	g_cameraOn = false;
	KillTimer(hDlg, kCameraTimer);
	SetDlgItemText(hDlg, IDC_BARCODEBOY_CAMERA_TOGGLE, TEXT("Start &Camera"));
	g_frame.clear();
	InvalidateRect(GetDlgItem(hDlg, IDC_BARCODEBOY_PREVIEW), nullptr, TRUE);

	// Back to what Display > Game Boy Image asks for.
	GBCameraStop();
	if (Settings.GBVideoCamera)
		GBCameraStart(Settings.GBVideoCameraIndex);
}

void ToggleCamera(HWND hDlg)
{
	if (g_cameraOn)
	{
		StopCamera(hDlg);
		return;
	}

	const int index = (int)SendDlgItemMessage(hDlg, IDC_BARCODEBOY_CAMERA, CB_GETCURSEL, 0, 0);
	if (index < 0 || !GBCameraStart(index))
	{
		SetDlgItemText(hDlg, IDC_BARCODEBOY_STATUS, TEXT("The camera could not be started."));
		return;
	}

	g_cameraOn = true;
	SetDlgItemText(hDlg, IDC_BARCODEBOY_CAMERA_TOGGLE, TEXT("Stop &Camera"));
	SetTimer(hDlg, kCameraTimer, 100, nullptr);
}

void PollCamera(HWND hDlg)
{
	if (!GBCameraGetFrame(g_frame, g_frameW, g_frameH))
		return;

	InvalidateRect(GetDlgItem(hDlg, IDC_BARCODEBOY_PREVIEW), nullptr, FALSE);

	char code[14];
	if (SGB::BarcodeBoyDecode(g_frame.data(), g_frameW, g_frameH, g_frameW, code) && DigitsOf(hDlg) != code)
	{
		Found(hDlg, code, TEXT("camera"));
		MessageBeep(MB_OK);
	}
}

// The shown card's photos side by side, each fitted to an equal share of the box.
void DrawPhotos(const DRAWITEMSTRUCT *dis)
{
	const RECT &rc = dis->rcItem;
	const int n = (int)g_photos.size(), gap = 6;
	const int slotW = (rc.right - rc.left - gap * (n + 1)) / n, slotH = rc.bottom - rc.top - gap * 2;

	SetStretchBltMode(dis->hDC, HALFTONE);
	for (int i = 0; i < n; i++)
	{
		const Photo &p = g_photos[i];
		int w = slotW, h = slotW * p.h / p.w;
		if (h > slotH)
		{
			h = slotH;
			w = slotH * p.w / p.h;
		}
		const int x = rc.left + gap + i * (slotW + gap) + (slotW - w) / 2;
		const int y = rc.top + (rc.bottom - rc.top - h) / 2;

		BITMAPINFOHEADER bi = { sizeof(bi), p.w, -p.h, 1, 32, BI_RGB };	// top-down
		StretchDIBits(dis->hDC, x, y, w, h, 0, 0, p.w, p.h, p.bgrx.data(), (BITMAPINFO *)&bi, DIB_RGB_COLORS, SRCCOPY);
	}
}

// A code with no photos as its printed barcode: bars on a white label, the
// guard bars running down between the digits.
bool DrawBarcode(const DRAWITEMSTRUCT *dis)
{
	bool bars[95];
	const int modules = SGB::EanBars(g_code.c_str(), bars);
	if (!modules)
		return false;

	const bool ean13 = modules == 95;
	const int quietL = ean13 ? 11 : 7, quietR = 7, total = quietL + modules + quietR;
	const RECT &rc = dis->rcItem;
	const int boxW = rc.right - rc.left, boxH = rc.bottom - rc.top;
	const int m = boxW * 85 / 100 / total > 1 ? boxW * 85 / 100 / total : 1;	// pixels per module
	const int digitH = 9 * m, pad = 4 * m;
	const int barH = 55 * m < boxH - digitH - 2 * pad - 8 ? 55 * m : boxH - digitH - 2 * pad - 8;
	const int w = total * m, h = pad + barH + digitH + pad;
	const int x0 = rc.left + (boxW - w) / 2, y0 = rc.top + (boxH - h) / 2;
	const int barsX = x0 + quietL * m, barTop = y0 + pad;

	RECT label = { x0, y0, x0 + w, y0 + h };
	FillRect(dis->hDC, &label, (HBRUSH)GetStockObject(WHITE_BRUSH));

	const int half = ean13 ? 6 : 4, mid = 3 + 7 * half;
	for (int i = 0; i < modules; i++)
	{
		if (!bars[i])
			continue;
		const bool guard = i < 3 || i >= modules - 3 || (i >= mid && i < mid + 5);
		RECT bar = { barsX + i * m, barTop, barsX + (i + 1) * m, barTop + barH + (guard ? digitH / 2 : 0) };
		FillRect(dis->hDC, &bar, (HBRUSH)GetStockObject(BLACK_BRUSH));
	}

	HFONT font = CreateFontW(-digitH, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
	                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
	HGDIOBJ old = SelectObject(dis->hDC, font);
	SetBkMode(dis->hDC, TRANSPARENT);
	SetTextColor(dis->hDC, RGB(0, 0, 0));

	// Each digit under its own seven modules; an EAN-13's first one in the quiet zone.
	auto digit = [&](char c, int left) {
		RECT r = { left, barTop + barH, left + 7 * m, barTop + barH + digitH + m };
		DrawTextA(dis->hDC, &c, 1, &r, DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOCLIP);
	};
	const char *code = g_code.c_str();
	int k = 0;
	if (ean13)
		digit(code[k++], barsX - 9 * m);
	for (int i = 0; i < half; i++)
		digit(code[k++], barsX + (3 + 7 * i) * m);
	for (int i = 0; i < half; i++)
		digit(code[k++], barsX + (mid + 5 + 7 * i) * m);

	SelectObject(dis->hDC, old);
	DeleteObject(font);
	return true;
}

void PaintPreview(const DRAWITEMSTRUCT *dis)
{
	const RECT &rc = dis->rcItem;
	FillRect(dis->hDC, &rc, GetSysColorBrush(COLOR_BTNFACE));

	if (!g_cameraOn && !g_photos.empty())
	{
		DrawPhotos(dis);
		return;
	}
	if (!g_cameraOn && DrawBarcode(dis))
		return;

	if (g_frame.empty() || g_frameW <= 0 || g_frameH <= 0)
	{
		RECT text = rc;
		InflateRect(&text, -8, -8);
		SetBkMode(dis->hDC, TRANSPARENT);
		SelectObject(dis->hDC, (HFONT)SendMessage(dis->hwndItem, WM_GETFONT, 0, 0));
		DrawText(dis->hDC, g_cameraOn ? TEXT("Waiting for the camera...")
		                              : TEXT("Hold the barcode up to the camera, straight and in focus."),
		         -1, &text, DT_CENTER | DT_WORDBREAK);
		return;
	}

	struct { BITMAPINFOHEADER h; RGBQUAD pal[256]; } bmi = {};
	bmi.h.biSize = sizeof(bmi.h);
	bmi.h.biWidth = g_frameW;
	bmi.h.biHeight = -g_frameH;	// top-down
	bmi.h.biPlanes = 1;
	bmi.h.biBitCount = 8;
	bmi.h.biCompression = BI_RGB;
	bmi.h.biClrUsed = 256;
	for (int i = 0; i < 256; i++)
		bmi.pal[i].rgbRed = bmi.pal[i].rgbGreen = bmi.pal[i].rgbBlue = (BYTE)i;

	// DIB rows are DWORD aligned; widths that aren't need a padded copy.
	const int stride = (g_frameW + 3) & ~3;
	std::vector<unsigned char> padded;
	const unsigned char *bits = g_frame.data();
	if (stride != g_frameW)
	{
		padded.resize((size_t)stride * g_frameH);
		for (int y = 0; y < g_frameH; y++)
			memcpy(&padded[(size_t)y * stride], &g_frame[(size_t)y * g_frameW], g_frameW);
		bits = padded.data();
	}

	const int boxW = rc.right - rc.left, boxH = rc.bottom - rc.top;
	int w = boxW, h = boxW * g_frameH / g_frameW;
	if (h > boxH)
	{
		h = boxH;
		w = boxH * g_frameW / g_frameH;
	}
	const int x = rc.left + (boxW - w) / 2, y = rc.top + (boxH - h) / 2;

	// Mirrored, as a webcam preview is expected to look; the decoder reads
	// both directions, so it gets the frame as captured.
	SetStretchBltMode(dis->hDC, HALFTONE);
	StretchDIBits(dis->hDC, x + w, y, -w, h, 0, 0, g_frameW, g_frameH, bits,
	              (BITMAPINFO *)&bmi, DIB_RGB_COLORS, SRCCOPY);
}

// Painted off-screen and copied in one go: on screen, every camera frame would
// first flash the background fill.
void DrawPreview(const DRAWITEMSTRUCT *dis)
{
	const RECT &rc = dis->rcItem;
	const int w = rc.right - rc.left, h = rc.bottom - rc.top;
	HDC dc = CreateCompatibleDC(dis->hDC);
	HBITMAP bmp = CreateCompatibleBitmap(dis->hDC, w, h);
	HGDIOBJ old = SelectObject(dc, bmp);

	DRAWITEMSTRUCT off = *dis;
	off.hDC = dc;
	OffsetRect(&off.rcItem, -rc.left, -rc.top);
	PaintPreview(&off);
	BitBlt(dis->hDC, rc.left, rc.top, w, h, dc, 0, 0, SRCCOPY);

	SelectObject(dc, old);
	DeleteObject(bmp);
	DeleteDC(dc);
}

// The games seed their characters from any 13 digits; made-up codes carry
// JAN's old flag 49, as most of the cards do.
void RandomCode(char out[14])
{
	static std::mt19937 rng(std::random_device{}());
	std::uniform_int_distribution<int> digit(0, 9);
	out[0] = '4';
	out[1] = '9';
	for (int i = 2; i < 12; i++)
		out[i] = (char)('0' + digit(rng));
	out[12] = (char)('0' + CheckDigit(out, 13));
	out[13] = 0;
}

INT_PTR CALLBACK DlgBarcodeBoy(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
		case WM_INITDIALOG:
		{
			LocalizeDialog(hDlg);
			g_photoDir = PhotoDir();
			g_photoStem = nullptr;
			g_photos.clear();
			g_code.clear();
			SendDlgItemMessage(hDlg, IDC_BARCODEBOY_EDIT, EM_SETLIMITTEXT, 20, 0);
			SendDlgItemMessage(hDlg, IDC_BARCODEBOY_EDIT, EM_SETCUEBANNER, TRUE, (LPARAM)L"Type your barcode");
			SetDlgItemTextA(hDlg, IDC_BARCODEBOY_EDIT, g_device->lastCode());

			std::vector<std::wstring> names;
			GBCameraEnumerate(names);
			for (auto &name : names)
				SendDlgItemMessage(hDlg, IDC_BARCODEBOY_CAMERA, CB_ADDSTRING, 0, (LPARAM)name.c_str());
			if (names.empty())
			{
				SendDlgItemMessage(hDlg, IDC_BARCODEBOY_CAMERA, CB_ADDSTRING, 0, (LPARAM)TEXT("No video camera found"));
				EnableWindow(GetDlgItem(hDlg, IDC_BARCODEBOY_CAMERA), FALSE);
				EnableWindow(GetDlgItem(hDlg, IDC_BARCODEBOY_CAMERA_TOGGLE), FALSE);
			}
			SendDlgItemMessage(hDlg, IDC_BARCODEBOY_CAMERA, CB_SETCURSEL,
			                   Settings.GBVideoCameraIndex < names.size() ? Settings.GBVideoCameraIndex : 0, 0);

			CodeEdited(hDlg);
			DragAcceptFiles(hDlg, TRUE);
			SendDlgItemMessage(hDlg, IDC_BARCODEBOY_SEARCH, EM_SETCUEBANNER, TRUE, (LPARAM)L"Name, game or barcode");
			FillCards(hDlg);
			return TRUE;
		}

		case WM_NOTIFY:
		{
			const NMHDR *nm = (const NMHDR *)lParam;
			if (nm->idFrom != IDC_BARCODEBOY_CARDS)
				break;

			// Picking a barcode types its code; a card's branch shows its
			// photos with no code; a double-click on a barcode scans it.
			if (nm->code == TVN_SELCHANGEDW && !g_filling)
			{
				const HTREEITEM picked = ((const NMTREEVIEWW *)lParam)->itemNew.hItem;
				if (const SGB::BarcodeBoyCard *card = CardOf(nm->hwndFrom, picked))
					SetDlgItemTextA(hDlg, IDC_BARCODEBOY_EDIT, card->code);
				else
				{
					SetDlgItemTextA(hDlg, IDC_BARCODEBOY_EDIT, "");
					ShowCard(hDlg, CardOf(nm->hwndFrom, picked, true));
				}
				return TRUE;
			}
			if (nm->code == NM_DBLCLK && CardOf(nm->hwndFrom, TreeView_GetSelection(nm->hwndFrom)))
			{
				PostMessage(hDlg, WM_COMMAND, IDOK, 0);
				SetWindowLongPtr(hDlg, DWLP_MSGRESULT, TRUE);
				return TRUE;
			}
			break;
		}

		case WM_DROPFILES:
			DroppedImage(hDlg, (HDROP)wParam);
			return TRUE;

		case WM_DRAWITEM:
			if (wParam == IDC_BARCODEBOY_PREVIEW)
			{
				DrawPreview((const DRAWITEMSTRUCT *)lParam);
				return TRUE;
			}
			break;

		case WM_TIMER:
			if (wParam == kCameraTimer)
				PollCamera(hDlg);
			return TRUE;

		case WM_COMMAND:
			switch (LOWORD(wParam))
			{
				case IDC_BARCODEBOY_EDIT:
					if (HIWORD(wParam) == EN_CHANGE)
						CodeEdited(hDlg);
					return TRUE;

				case IDC_BARCODEBOY_SEARCH:
					if (HIWORD(wParam) == EN_CHANGE)
						FillCards(hDlg);
					return TRUE;

				case IDC_BARCODEBOY_RANDOM:
				{
					char code[14];
					RandomCode(code);
					SetDlgItemTextA(hDlg, IDC_BARCODEBOY_EDIT, code);
					return TRUE;
				}

				case IDC_BARCODEBOY_IMAGE:
					FromImage(hDlg);
					return TRUE;

				case IDC_BARCODEBOY_CAMERA_TOGGLE:
					ToggleCamera(hDlg);
					return TRUE;

				case IDC_BARCODEBOY_CAMERA:
					if (HIWORD(wParam) == CBN_SELCHANGE && g_cameraOn)
					{
						StopCamera(hDlg);
						ToggleCamera(hDlg);
					}
					return TRUE;

				case IDOK:
					if (!g_device->scan(DigitsOf(hDlg).c_str()))
						return TRUE;
					StopCamera(hDlg);
					EndDialog(hDlg, IDOK);
					return TRUE;

				case IDCANCEL:
					StopCamera(hDlg);
					EndDialog(hDlg, IDCANCEL);
					return TRUE;
			}
			break;
	}

	return FALSE;
}

bool ScannerDialog(HWND parent, const Device &device)
{
	INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_TREEVIEW_CLASSES };
	InitCommonControlsEx(&icc);

	g_device = &device;
	return DialogBox(g_hInst, MAKEINTRESOURCE(device.dialog), parent, DlgBarcodeBoy) == IDOK;
}

void Scanned(const Device &device)
{
	const char *code = device.lastCode();
	const SGB::BarcodeBoyCard *card = CardWithCode(device, code);
	char msg[128];
	snprintf(msg, sizeof(msg), "%s: %s scanned", device.osd, card ? card->name : code);
	S9xSetInfoString(msg);
}

} // namespace

bool WinBarcodeBoyDialog(HWND parent)
{
	if (!ScannerDialog(parent, kBarcodeBoy))
		return false;
	WinBarcodeBoyScanned();
	return true;
}

void WinBarcodeBoyScanned() { Scanned(kBarcodeBoy); }

bool WinBardigunDialog(HWND parent)
{
	if (!ScannerDialog(parent, kBardigun))
		return false;
	WinBardigunScanned();
	return true;
}

void WinBardigunScanned() { Scanned(kBardigun); }
