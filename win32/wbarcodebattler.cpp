#include "wbarcodebattler.h"
#include "win32_webcam.h"
#include "wlocale.h"
#include "rsrc/resource.h"

#include "../snes9x.h"
#include "../display.h"
#include "../barcodebattler.h"

#include <commdlg.h>
#include <wincodec.h>
#include <tchar.h>
#include <stdio.h>
#include <string.h>
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

void CodeEdited(HWND hDlg)
{
	const std::string digits = DigitsOf(hDlg);
	const size_t n = digits.size();
	const bool allDigits = digits.find_first_not_of("0123456789") == std::string::npos;
	const bool lengthOk = allDigits && (n == 8 || n == 12 || n == 13);

	TCHAR status[256] = TEXT("");
	if (n == 0)
		;
	else if (!allDigits)
		_tcscpy(status, TEXT("A barcode holds only digits."));
	else if (!lengthOk)
		_stprintf(status, TEXT("%d digits; a barcode has 8, 12 or 13."), (int)n);
	else if (!S9xBarcodeChecksumValid(digits.c_str()))
		_tcscpy(status, TEXT("The check digit doesn't add up. A printed barcode would not read like this, but it can still be sent."));
	else
		_stprintf(status, TEXT("Valid %s barcode."),
		          n == 13 ? TEXT("EAN-13") : n == 12 ? TEXT("UPC-A") : TEXT("EAN-8"));

	SetDlgItemText(hDlg, IDC_BARCODE_STATUS, status);
	EnableWindow(GetDlgItem(hDlg, IDOK), lengthOk);
}

void Found(HWND hDlg, const char *code, const TCHAR *where)
{
	SetDlgItemTextA(hDlg, IDC_BARCODE_EDIT, code);
	TCHAR status[128];
	_stprintf(status, TEXT("Read %hs from the %s."), code, where);
	SetDlgItemText(hDlg, IDC_BARCODE_STATUS, status);
}

// Any picture WIC can open, as 8-bit grey.
bool LoadLuma(const wchar_t *path, std::vector<unsigned char> &luma, int &w, int &h)
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
	    SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat8bppGray, WICBitmapDitherTypeNone,
	                                    nullptr, 0.0, WICBitmapPaletteTypeCustom)))
	{
		UINT uw = 0, uh = 0;
		converter->GetSize(&uw, &uh);
		if (uw && uh)
		{
			w = (int)uw;
			h = (int)uh;
			luma.resize((size_t)uw * uh);
			ok = SUCCEEDED(converter->CopyPixels(nullptr, uw, (UINT)luma.size(), luma.data()));
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
	if (!GetOpenFileName(&ofn))
		return;

	std::vector<unsigned char> luma;
	int w = 0, h = 0;
	if (!LoadLuma(file, luma, w, h))
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

void DrawPreview(const DRAWITEMSTRUCT *dis)
{
	const RECT &rc = dis->rcItem;
	FillRect(dis->hDC, &rc, GetSysColorBrush(COLOR_BTNFACE));

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
			SendDlgItemMessage(hDlg, IDC_BARCODE_EDIT, EM_SETLIMITTEXT, 20, 0);
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
			return TRUE;
		}

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
	if (DialogBox(g_hInst, MAKEINTRESOURCE(IDD_BARCODE_BATTLER), parent, DlgBarcodeBattler) != IDOK)
		return false;

	char msg[64];
	snprintf(msg, sizeof(msg), "Barcode Battler: %s scanned", S9xBarcodeBattlerLastCode());
	S9xSetInfoString(msg);
	return true;
}
