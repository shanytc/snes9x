#include "CDebuggerSplitter.h"

static const TCHAR *kClassName = TEXT("S9xDebuggerSplitter");
static const int    kBarSize   = 4;

struct SplitterState
{
	SplitterOrient orient;
	HWND  childA;
	HWND  childB;
	float ratio;
	bool  dragging;
};

static SplitterState *GetState(HWND h)
{
	return (SplitterState *)GetWindowLongPtr(h, GWLP_USERDATA);
}

static void Layout(HWND h, SplitterState *s)
{
	RECT rc;
	GetClientRect(h, &rc);
	const int w = rc.right - rc.left;
	const int hh = rc.bottom - rc.top;

	if (s->orient == SPLIT_HORZ)
	{
		int leftW = (int)(w * s->ratio);
		if (leftW < 32) leftW = 32;
		if (leftW > w - 32 - kBarSize) leftW = w - 32 - kBarSize;
		if (s->childA) MoveWindow(s->childA, 0, 0, leftW, hh, TRUE);
		if (s->childB) MoveWindow(s->childB, leftW + kBarSize, 0, w - leftW - kBarSize, hh, TRUE);
	}
	else
	{
		int topH = (int)(hh * s->ratio);
		if (topH < 32) topH = 32;
		if (topH > hh - 32 - kBarSize) topH = hh - 32 - kBarSize;
		if (s->childA) MoveWindow(s->childA, 0, 0, w, topH, TRUE);
		if (s->childB) MoveWindow(s->childB, 0, topH + kBarSize, w, hh - topH - kBarSize, TRUE);
	}
}

static LRESULT CALLBACK SplitterProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg)
	{
		case WM_CREATE:
		{
			SplitterState *s = new SplitterState();
			s->orient   = SPLIT_HORZ;
			s->childA   = NULL;
			s->childB   = NULL;
			s->ratio    = 0.5f;
			s->dragging = false;
			SetWindowLongPtr(h, GWLP_USERDATA, (LONG_PTR)s);
			return 0;
		}

		case WM_DESTROY:
		{
			SplitterState *s = GetState(h);
			if (s) delete s;
			return 0;
		}

		case WM_SIZE:
		{
			SplitterState *s = GetState(h);
			if (s) Layout(h, s);
			return 0;
		}

		case WM_LBUTTONDOWN:
		{
			SplitterState *s = GetState(h);
			if (!s) return 0;
			s->dragging = true;
			SetCapture(h);
			return 0;
		}

		case WM_LBUTTONUP:
		{
			SplitterState *s = GetState(h);
			if (s) s->dragging = false;
			ReleaseCapture();
			return 0;
		}

		case WM_MOUSEMOVE:
		{
			SplitterState *s = GetState(h);
			if (!s || !s->dragging) return 0;
			RECT rc; GetClientRect(h, &rc);
			const int x = LOWORD(lp);
			const int y = HIWORD(lp);
			const int w  = rc.right - rc.left;
			const int hh = rc.bottom - rc.top;
			float r = s->ratio;
			if (s->orient == SPLIT_HORZ && w > 0)
				r = (float)x / (float)w;
			else if (s->orient == SPLIT_VERT && hh > 0)
				r = (float)y / (float)hh;
			if (r < 0.05f) r = 0.05f;
			if (r > 0.95f) r = 0.95f;
			s->ratio = r;
			Layout(h, s);
			InvalidateRect(h, NULL, TRUE);
			return 0;
		}

		case WM_SETCURSOR:
		{
			SplitterState *s = GetState(h);
			if (!s) break;
			if ((HWND)wp != h)
				return FALSE;
			SetCursor(LoadCursor(NULL, s->orient == SPLIT_HORZ ? IDC_SIZEWE : IDC_SIZENS));
			return TRUE;
		}

		case WM_ERASEBKGND:
			return 1;

		case WM_PAINT:
		{
			PAINTSTRUCT ps;
			HDC dc = BeginPaint(h, &ps);
			RECT rc; GetClientRect(h, &rc);
			HBRUSH br = GetSysColorBrush(COLOR_BTNFACE);
			FillRect(dc, &rc, br);
			EndPaint(h, &ps);
			return 0;
		}
	}

	return DefWindowProc(h, msg, wp, lp);
}

void SplitterRegisterClass(HINSTANCE hInst)
{
	WNDCLASSEX wc = {};
	wc.cbSize        = sizeof(wc);
	wc.style         = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc   = SplitterProc;
	wc.hInstance     = hInst;
	wc.hCursor       = NULL;
	wc.hbrBackground = NULL;
	wc.lpszClassName = kClassName;
	RegisterClassEx(&wc);
}

HWND SplitterCreate(HWND parent, SplitterOrient orient, int id)
{
	HWND h = CreateWindowEx(0, kClassName, TEXT(""),
	                        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
	                        0, 0, 0, 0,
	                        parent, (HMENU)(LONG_PTR)id, GetModuleHandle(NULL), NULL);
	if (h)
	{
		SplitterState *s = GetState(h);
		if (s) s->orient = orient;
	}
	return h;
}

void SplitterSetChildren(HWND splitter, HWND a, HWND b)
{
	SplitterState *s = GetState(splitter);
	if (!s) return;
	s->childA = a;
	s->childB = b;
	if (a) SetParent(a, splitter);
	if (b) SetParent(b, splitter);
	Layout(splitter, s);
}

void SplitterSetRatio(HWND splitter, float ratio)
{
	SplitterState *s = GetState(splitter);
	if (!s) return;
	if (ratio < 0.05f) ratio = 0.05f;
	if (ratio > 0.95f) ratio = 0.95f;
	s->ratio = ratio;
	Layout(splitter, s);
}

float SplitterGetRatio(HWND splitter)
{
	SplitterState *s = GetState(splitter);
	return s ? s->ratio : 0.5f;
}
