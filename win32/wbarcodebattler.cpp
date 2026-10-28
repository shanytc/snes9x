#include "wbarcodebattler.h"
#include "win32_webcam.h"
#include "wlocale.h"
#include "rsrc/resource.h"

#include "../snes9x.h"
#include "../display.h"
#include "../barcodebattler.h"

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
#include <string>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")

extern HINSTANCE g_hInst;

namespace {

const UINT_PTR kCameraTimer = 1;

bool                       g_cameraOn = false;
std::vector<unsigned char> g_frame;
int                        g_frameW = 0;
int                        g_frameH = 0;

// The shown card's photos from the battler folder beside the executable.
struct Photo { std::vector<unsigned char> bgrx; int w = 0, h = 0; };
std::wstring               g_photoDir;
const S9xBarcodeCard      *g_photoCard = nullptr;
std::vector<Photo>         g_photos;

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
	GetDlgItemTextA(hDlg, IDC_BARCODE_EDIT, text, sizeof(text));
	std::string digits;
	for (const char *p = text; *p; p++)
		if (*p != ' ' && *p != '-')
			digits += *p;
	return digits;
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

// The battler folder beside the executable, or "" when there is none.
std::wstring PhotoDir()
{
	wchar_t exe[MAX_PATH] = L"";
	std::wstring dir(exe, GetModuleFileNameW(nullptr, exe, MAX_PATH));
	dir.erase(dir.find_last_of(L"\\/") + 1);
	dir += L"battler\\";
	const DWORD attr = GetFileAttributesW(dir.c_str());
	return (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) ? dir : std::wstring();
}

// Puts a card's front, back and fluorescent photos in the preview; none for nullptr.
void ShowCard(HWND hDlg, const S9xBarcodeCard *card)
{
	if (card == g_photoCard)
		return;

	g_photoCard = card;
	g_photos.clear();
	if (card && *card->image && !g_photoDir.empty())
		for (const wchar_t *side : { L"-front.jxl", L"-back.jxl", L"-fluorescent.jxl" })
		{
			std::wstring path = g_photoDir + Wide(card->image) + side;
			for (wchar_t &c : path)
				if (c == L'/')
					c = L'\\';
			Photo photo;
			if (LoadJxl(path, photo))
				g_photos.push_back(std::move(photo));
		}

	InvalidateRect(GetDlgItem(hDlg, IDC_BARCODE_PREVIEW), nullptr, TRUE);
}

void CodeEdited(HWND hDlg)
{
	const std::string digits = DigitsOf(hDlg);
	const size_t n = digits.size();
	const bool allDigits = digits.find_first_not_of("0123456789") == std::string::npos;
	const bool lengthOk = allDigits && (n == 8 || n == 12 || n == 13);
	const S9xBarcodeCard *card = lengthOk ? S9xBarcodeCardFind(digits.c_str()) : nullptr;

	TCHAR status[256] = TEXT("");
	if (n == 0)
		;
	else if (!allDigits)
		_tcscpy(status, TEXT("A barcode holds only digits."));
	else if (!lengthOk)
		_stprintf(status, TEXT("%d digits; a barcode has 8, 12 or 13."), (int)n);
	else if (!S9xBarcodeChecksumValid(digits.c_str()))
		_tcscpy(status, TEXT("The check digit doesn't add up. A printed barcode would not read like this, but it can still be sent."));
	else if (card)
		swprintf(status, 256, L"%ls, from %ls.", Wide(card->name).c_str(), Wide(card->set).c_str());
	else
		_stprintf(status, TEXT("Valid %s barcode."),
		          n == 13 ? TEXT("EAN-13") : n == 12 ? TEXT("UPC-A") : TEXT("EAN-8"));

	SetDlgItemText(hDlg, IDC_BARCODE_STATUS, status);
	EnableWindow(GetDlgItem(hDlg, IDOK), lengthOk);
	ShowCard(hDlg, card);
}

void Found(HWND hDlg, const char *code, const TCHAR *where)
{
	SetDlgItemTextA(hDlg, IDC_BARCODE_EDIT, code);
	TCHAR status[128];
	_stprintf(status, TEXT("Read %hs from the %s."), code, where);
	SetDlgItemText(hDlg, IDC_BARCODE_STATUS, status);
}

void ReadImage(HWND hDlg, const TCHAR *file)
{
	std::vector<unsigned char> luma;
	int w = 0, h = 0;
	if (!LoadPixels(file, GUID_WICPixelFormat8bppGray, 1, luma, w, h))
	{
		MessageBox(hDlg, TEXT("This picture could not be opened."), TEXT("Barcode Battler II"), MB_OK | MB_ICONWARNING);
		return;
	}

	char code[14];
	if (S9xBarcodeDecode(luma.data(), w, h, w, code))
		Found(hDlg, code, TEXT("picture"));
	else
		MessageBox(hDlg, TEXT("No EAN-13, UPC-A or EAN-8 barcode was found in this picture. Crop it closer to the barcode and keep the bars upright."),
		           TEXT("Barcode Battler II"), MB_OK | MB_ICONINFORMATION);
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

// One printed card: the run of entries from i sharing its set and photo stem
// (a card with several barcodes); a single entry when it has no photos.
int CardEnd(const S9xBarcodeCard *cards, int count, int i)
{
	int j = i + 1;
	if (*cards[i].image)
		while (j < count && !strcmp(cards[j].set, cards[i].set) && !strcmp(cards[j].image, cards[i].image))
			j++;
	return j;
}

bool CardMatches(const S9xBarcodeCard &card, const std::wstring &find)
{
	return find.empty() || Lower(Wide(card.name)).find(find) != std::wstring::npos ||
	       Lower(Wide(card.set)).find(find) != std::wstring::npos || Wide(card.code).find(find) != std::wstring::npos;
}

// A card with several barcodes is named after them, less their positions.
std::wstring CardLabel(const S9xBarcodeCard *cards, int i, int end)
{
	std::vector<std::wstring> names;
	for (int k = i; k < end; k++)
	{
		std::wstring name = Wide(cards[k].name);
		for (const wchar_t *where : { L" (Up)", L" (Down)", L" (Left)", L" (Right)", L" (Front)", L" (Back)" })
		{
			const size_t n = wcslen(where);
			if (name.size() > n && !name.compare(name.size() - n, n, where))
			{
				name.erase(name.size() - n);
				break;
			}
		}
		if (std::find(names.begin(), names.end(), name) == names.end())
			names.push_back(name);
	}

	std::wstring label;
	for (const std::wstring &name : names)
		label += (label.empty() ? L"" : L" / ") + name;
	return label;
}

// The known cards with a set, name or code holding the search text, one
// branch per set and one per card with several barcodes; all folded when
// there's nothing to search for.
void FillCards(HWND hDlg)
{
	HWND tree = GetDlgItem(hDlg, IDC_BARCODE_CARDS);
	wchar_t text[64] = L"";
	GetDlgItemTextW(hDlg, IDC_BARCODE_SEARCH, text, 64);
	const std::wstring find = Lower(text);

	SendMessage(tree, WM_SETREDRAW, FALSE, 0);
	TreeView_DeleteAllItems(tree);

	int count = 0;
	const S9xBarcodeCard *cards = S9xBarcodeCards(&count);
	const char *set = nullptr;
	HTREEITEM branch = nullptr;
	int inSet = 0;

	auto closeSet = [&]() {
		if (!branch)
			return;
		std::wstring label = Wide(set) + L" (" + std::to_wstring(inSet) + L")";
		TVITEMW item = {};
		item.mask = TVIF_TEXT;
		item.hItem = branch;
		item.pszText = &label[0];
		TreeView_SetItem(tree, &item);
		if (!find.empty())
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

		if (!set || strcmp(set, cards[i].set))
		{
			closeSet();
			set = cards[i].set;
			inSet = 0;
			std::wstring label = Wide(set);
			ins.hParent = TVI_ROOT;
			ins.item.pszText = &label[0];
			ins.item.lParam = -1;
			branch = (HTREEITEM)SendMessage(tree, TVM_INSERTITEMW, 0, (LPARAM)&ins);
		}

		// lParam: the entry for a barcode, -2 - first entry for a card's branch
		HTREEITEM parent = branch;
		if (end - i > 1)
		{
			std::wstring label = CardLabel(cards, i, end);
			ins.hParent = branch;
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

		if (parent != branch && !find.empty())
			TreeView_Expand(tree, parent, TVE_EXPAND);
		inSet++;
	}
	closeSet();

	SendMessage(tree, WM_SETREDRAW, TRUE, 0);
	InvalidateRect(tree, nullptr, TRUE);
}

// The entry under a tree item: a barcode, or with branch set the first
// barcode of a card's branch; nullptr otherwise.
const S9xBarcodeCard *CardOf(HWND tree, HTREEITEM hItem, bool branch = false)
{
	TVITEMW item = {};
	item.mask = TVIF_PARAM;
	item.hItem = hItem;
	if (!hItem || !TreeView_GetItem(tree, &item))
		return nullptr;

	int count = 0;
	const S9xBarcodeCard *cards = S9xBarcodeCards(&count);
	const LPARAM entry = branch ? -2 - item.lParam : item.lParam;
	return entry >= 0 && entry < count ? &cards[entry] : nullptr;
}

void StopCamera(HWND hDlg)
{
	if (!g_cameraOn)
		return;

	g_cameraOn = false;
	KillTimer(hDlg, kCameraTimer);
	SetDlgItemText(hDlg, IDC_BARCODE_CAMERA_TOGGLE, TEXT("Start &Camera"));
	g_frame.clear();
	InvalidateRect(GetDlgItem(hDlg, IDC_BARCODE_PREVIEW), nullptr, TRUE);

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

	const int index = (int)SendDlgItemMessage(hDlg, IDC_BARCODE_CAMERA, CB_GETCURSEL, 0, 0);
	if (index < 0 || !GBCameraStart(index))
	{
		SetDlgItemText(hDlg, IDC_BARCODE_STATUS, TEXT("The camera could not be started."));
		return;
	}

	g_cameraOn = true;
	SetDlgItemText(hDlg, IDC_BARCODE_CAMERA_TOGGLE, TEXT("Stop &Camera"));
	SetTimer(hDlg, kCameraTimer, 100, nullptr);
}

void PollCamera(HWND hDlg)
{
	if (!GBCameraGetFrame(g_frame, g_frameW, g_frameH))
		return;

	InvalidateRect(GetDlgItem(hDlg, IDC_BARCODE_PREVIEW), nullptr, FALSE);

	char code[14];
	if (S9xBarcodeDecode(g_frame.data(), g_frameW, g_frameH, g_frameW, code) && DigitsOf(hDlg) != code)
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

void DrawPreview(const DRAWITEMSTRUCT *dis)
{
	const RECT &rc = dis->rcItem;
	FillRect(dis->hDC, &rc, GetSysColorBrush(COLOR_BTNFACE));

	if (!g_cameraOn && !g_photos.empty())
	{
		DrawPhotos(dis);
		return;
	}

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

INT_PTR CALLBACK DlgBarcodeBattler(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
		case WM_INITDIALOG:
		{
			LocalizeDialog(hDlg);
			g_photoDir = PhotoDir();
			g_photoCard = nullptr;
			g_photos.clear();
			SendDlgItemMessage(hDlg, IDC_BARCODE_EDIT, EM_SETLIMITTEXT, 20, 0);
			SendDlgItemMessage(hDlg, IDC_BARCODE_EDIT, EM_SETCUEBANNER, TRUE, (LPARAM)L"Type your barcode");
			SetDlgItemTextA(hDlg, IDC_BARCODE_EDIT, S9xBarcodeBattlerLastCode());

			std::vector<std::wstring> names;
			GBCameraEnumerate(names);
			for (auto &name : names)
				SendDlgItemMessage(hDlg, IDC_BARCODE_CAMERA, CB_ADDSTRING, 0, (LPARAM)name.c_str());
			if (names.empty())
			{
				SendDlgItemMessage(hDlg, IDC_BARCODE_CAMERA, CB_ADDSTRING, 0, (LPARAM)TEXT("No video camera found"));
				EnableWindow(GetDlgItem(hDlg, IDC_BARCODE_CAMERA), FALSE);
				EnableWindow(GetDlgItem(hDlg, IDC_BARCODE_CAMERA_TOGGLE), FALSE);
			}
			SendDlgItemMessage(hDlg, IDC_BARCODE_CAMERA, CB_SETCURSEL,
			                   Settings.GBVideoCameraIndex < names.size() ? Settings.GBVideoCameraIndex : 0, 0);

			CodeEdited(hDlg);
			DragAcceptFiles(hDlg, TRUE);
			SendDlgItemMessage(hDlg, IDC_BARCODE_SEARCH, EM_SETCUEBANNER, TRUE, (LPARAM)L"Name, set or barcode");
			FillCards(hDlg);
			return TRUE;
		}

		case WM_NOTIFY:
		{
			const NMHDR *nm = (const NMHDR *)lParam;
			if (nm->idFrom != IDC_BARCODE_CARDS)
				break;

			// Picking a card types its code; a double-click on one scans it.
			if (nm->code == TVN_SELCHANGEDW)
			{
				const HTREEITEM picked = ((const NMTREEVIEWW *)lParam)->itemNew.hItem;
				if (const S9xBarcodeCard *card = CardOf(nm->hwndFrom, picked))
				{
					SetDlgItemTextA(hDlg, IDC_BARCODE_EDIT, card->code);
					ShowCard(hDlg, card);	// this set's printing, not the first card with the code
				}
				else
				{
					// a set or a card's branch: no barcode until one is picked; a card shows its photos
					SetDlgItemTextA(hDlg, IDC_BARCODE_EDIT, "");
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
			if (wParam == IDC_BARCODE_PREVIEW)
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
				case IDC_BARCODE_EDIT:
					if (HIWORD(wParam) == EN_CHANGE)
						CodeEdited(hDlg);
					return TRUE;

				case IDC_BARCODE_SEARCH:
					if (HIWORD(wParam) == EN_CHANGE)
						FillCards(hDlg);
					return TRUE;

				case IDC_BARCODE_RANDOM:
				{
					char code[14];
					S9xBarcodeRandom(code);
					SetDlgItemTextA(hDlg, IDC_BARCODE_EDIT, code);
					return TRUE;
				}

				case IDC_BARCODE_IMAGE:
					FromImage(hDlg);
					return TRUE;

				case IDC_BARCODE_CAMERA_TOGGLE:
					ToggleCamera(hDlg);
					return TRUE;

				case IDC_BARCODE_CAMERA:
					if (HIWORD(wParam) == CBN_SELCHANGE && g_cameraOn)
					{
						StopCamera(hDlg);
						ToggleCamera(hDlg);
					}
					return TRUE;

				case IDOK:
					if (!S9xBarcodeBattlerScan(DigitsOf(hDlg).c_str()))
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

} // namespace

bool WinBarcodeBattlerDialog(HWND parent)
{
	INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_TREEVIEW_CLASSES };
	InitCommonControlsEx(&icc);

	if (DialogBox(g_hInst, MAKEINTRESOURCE(IDD_BARCODE_BATTLER), parent, DlgBarcodeBattler) != IDOK)
		return false;

	char msg[64];
	snprintf(msg, sizeof(msg), "Barcode Battler: %s scanned", S9xBarcodeBattlerLastCode());
	S9xSetInfoString(msg);
	return true;
}
