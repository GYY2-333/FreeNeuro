/* -----------------------------------------------------------------------------

    BrainBay  -  OpenSource Biofeedback Software, contact: chris@shifz.org

  MODULE: DARKMODE.CPP:  global dark-theme support for Win32 dialogs

  See darkmode.h for the rationale.

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; See the
  GNU General Public License for more details.

-----------------------------------------------------------------------------*/

#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>
#include "brainBay.h"
#include "darkmode.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")

// text colors used across the dark dialogs
#define DM_TEXT_NORMAL   RGB(225, 225, 230)
#define DM_TEXT_DISABLED RGB(140, 140, 150)
#define DM_BK_DIALOG     RGB( 28,  28,  30)
#define DM_BK_EDIT       RGB( 44,  44,  48)

// subclass id (arbitrary but unique within this module)
#define DM_SUBCLASS_ID   0xDA2C

// ---------------------------------------------------------------------------
//  De-theme standard controls so WM_CTLCOLOR* takes effect and remove the
//  light classic-control frames. Called once per dialog on first paint.
// ---------------------------------------------------------------------------
static BOOL CALLBACK dm_theme_child(HWND hChild, LPARAM lParam)
{
	char cls[64];
	GetClassNameA(hChild, cls, sizeof(cls));

	// Buttons (push/check/radio/group) and combo boxes ignore WM_CTLCOLOR
	// while the visual style is active -> strip the theme from them.
	if (lstrcmpiA(cls, "Button")==0 ||
	    lstrcmpiA(cls, "ComboBox")==0 ||
	    lstrcmpiA(cls, "msctls_trackbar32")==0 ||
	    lstrcmpiA(cls, "ScrollBar")==0)
	{
		SetWindowTheme(hChild, L"", L"");
	}
	return TRUE;
}

// ---------------------------------------------------------------------------
//  Central subclass procedure: answer all WM_CTLCOLOR* with dark brushes.
// ---------------------------------------------------------------------------
static LRESULT CALLBACK DarkSubclassProc(HWND hWnd, UINT msg, WPARAM wParam,
                                         LPARAM lParam, UINT_PTR uId, DWORD_PTR ref)
{
	switch (msg)
	{
	case WM_CTLCOLORDLG:
		return (LRESULT)(DRAW.brush_dlg_bg ? DRAW.brush_dlg_bg :
		                 (HBRUSH)GetStockObject(BLACK_BRUSH));

	case WM_CTLCOLORSTATIC:
		{
			HDC hdc = (HDC)wParam;
			HWND ctl = (HWND)lParam;
			SetBkMode(hdc, TRANSPARENT);
			// dim the text of disabled controls (e.g. read-only status fields)
			BOOL enabled = IsWindowEnabled(ctl);
			SetTextColor(hdc, enabled ? DM_TEXT_NORMAL : DM_TEXT_DISABLED);
			SetBkColor(hdc, DM_BK_DIALOG);
			return (LRESULT)(DRAW.brush_dlg_bg ? DRAW.brush_dlg_bg :
			                 (HBRUSH)GetStockObject(BLACK_BRUSH));
		}

	case WM_CTLCOLOREDIT:
	case WM_CTLCOLORLISTBOX:
		{
			HDC hdc = (HDC)wParam;
			SetTextColor(hdc, DM_TEXT_NORMAL);
			SetBkColor(hdc, DM_BK_EDIT);
			return (LRESULT)(DRAW.brush_dlg_edit ? DRAW.brush_dlg_edit :
			                 (HBRUSH)GetStockObject(BLACK_BRUSH));
		}

	case WM_CTLCOLORBTN:
		{
			HDC hdc = (HDC)wParam;
			SetTextColor(hdc, DM_TEXT_NORMAL);
			SetBkColor(hdc, DM_BK_EDIT);
			return (LRESULT)(DRAW.brush_dlg_btn ? DRAW.brush_dlg_btn :
			                 (HBRUSH)GetStockObject(BLACK_BRUSH));
		}

	case WM_CTLCOLORSCROLLBAR:
		return (LRESULT)(DRAW.brush_dlg_edit ? DRAW.brush_dlg_edit :
		                 (HBRUSH)GetStockObject(BLACK_BRUSH));

	case WM_CLOSE:
		{
			/*  A modeless object settings dialog is only destroyed by its own
			    EndDialog(). Nearly every object dialog starts with
			        if ((st==NULL)||(st->type != OB_XXX)) return FALSE;
			    so as soon as actobject points somewhere else (another object
			    window was clicked, a design was loaded, an object was pasted,
			    ...) the dialog ignores WM_CLOSE and the title bar X does
			    nothing - the window can then never be closed again.

			    Give the dialog procedure a chance first; only if it really
			    ignored the message (returned FALSE) close it here. */
			LRESULT r = DefSubclassProc(hWnd, msg, wParam, lParam);

			if ((r == FALSE) && IsWindow(hWnd))
			{
				write_logfile("dialog: WM_CLOSE was ignored by the dialog procedure, closing it");
				EndDialog(hWnd, IDCANCEL);
			}
			return TRUE;
		}

	case WM_NCDESTROY:
		RemoveWindowSubclass(hWnd, DarkSubclassProc, uId);
		break;
	}
	return DefSubclassProc(hWnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
//  Object display frames.
//
//  The display windows of the signal objects (threshold meters, band power
//  panels, the Flash movie ...) are plain WS_CHILD windows with a caption and
//  a 3D sizing border, so Windows paints them in the classic light look. On
//  the dark canvas that shows up as a light grey title bar plus a light frame
//  around every panel - and the panels themselves drew another light 2px
//  rectangle inside the client area, which looked like a double border.
//
//  Here the whole non-client area is painted in theme colours: a flat dark
//  band, the caption text in dim grey and a one pixel outline that hugs the
//  client area. Only GetWindowDC() is used; the client area stays untouched so
//  child windows (Flash ActiveX!) are never covered.
// ---------------------------------------------------------------------------
#define DM_FRAME_BACK   RGB( 22, 22, 26)
#define DM_FRAME_LINE   RGB( 76, 78, 90)
#define DM_FRAME_TEXT   RGB(156,158,170)

void PaintDisplayFrame(HWND hWnd, BOOL draw_caption)
{
	HDC    hdc;
	RECT   wr, cr, r;
	HBRUSH br;
	POINT  pt;
	int    W, H, x0, y0, x1, y1;

	if ((hWnd==NULL)||(!IsWindow(hWnd))) return;

	hdc=GetWindowDC(hWnd);
	if (hdc==NULL) return;

	GetWindowRect(hWnd,&wr);
	W=wr.right-wr.left;
	H=wr.bottom-wr.top;

	// client rectangle in window coordinates
	GetClientRect(hWnd,&cr);
	pt.x=0; pt.y=0;
	ClientToScreen(hWnd,&pt);
	OffsetRect(&cr,pt.x-wr.left,pt.y-wr.top);

	if ((W<4)||(H<4)||((cr.right-cr.left)<=0)||((cr.bottom-cr.top)<=0))
	{
		ReleaseDC(hWnd,hdc);
		return;
	}

	// ---- flat dark frame (everything that is not the client area) ------
	br=CreateSolidBrush(DM_FRAME_BACK);
	if (cr.top>0)    { SetRect(&r,0,0,W,cr.top);                   FillRect(hdc,&r,br); }
	if (cr.bottom<H) { SetRect(&r,0,cr.bottom,W,H);                FillRect(hdc,&r,br); }
	if (cr.left>0)   { SetRect(&r,0,cr.top,cr.left,cr.bottom);     FillRect(hdc,&r,br); }
	if (cr.right<W)  { SetRect(&r,cr.right,cr.top,W,cr.bottom);    FillRect(hdc,&r,br); }
	DeleteObject(br);

	// ---- one pixel outline around the client area ----------------------
	x0=(cr.left>0)  ? cr.left-1 : 0;
	y0=(cr.top>0)   ? cr.top-1  : 0;
	x1=(cr.right<W) ? cr.right  : W-1;
	y1=(cr.bottom<H)? cr.bottom : H-1;

	br=CreateSolidBrush(DM_FRAME_LINE);
	if (cr.top>0)    { SetRect(&r,x0,y0,x1+1,y0+1); FillRect(hdc,&r,br); }
	if (cr.bottom<H) { SetRect(&r,x0,y1,x1+1,y1+1); FillRect(hdc,&r,br); }
	if (cr.left>0)   { SetRect(&r,x0,y0,x0+1,y1+1); FillRect(hdc,&r,br); }
	if (cr.right<W)  { SetRect(&r,x1,y0,x1+1,y1+1); FillRect(hdc,&r,br); }
	DeleteObject(br);

	// ---- caption text, dim grey on the dark band -----------------------
	// Only when the window really has a caption bar. A window that only has
	// WS_THICKFRAME (the session manager, the meters) gets an 8 pixel frame and
	// no caption band at all - drawing the text there clipped it in half and
	// left a cut off title line above the panel.
	if (draw_caption)
	{
		long style=GetWindowLong(hWnd,GWL_STYLE);
		int  caph;                                  // height of the caption band

		caph=GetSystemMetrics(SM_CYCAPTION);
		if ((style & WS_CAPTION) && (caph>0) && (cr.top>=caph))
		{
			char text[128];

			text[0]=0;
			GetWindowText(hWnd,text,sizeof(text)-1);
			text[sizeof(text)-1]=0;

			if (text[0]!=0)
			{
				HFONT ofont;

				ofont=(HFONT)SelectObject(hdc,GetStockObject(DEFAULT_GUI_FONT));
				SetBkMode(hdc,TRANSPARENT);
				SetTextColor(hdc,DM_FRAME_TEXT);

				SetRect(&r,6,0,W-6,cr.top);
				DrawText(hdc,text,-1,&r,
				         DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS|DT_NOPREFIX);

				SelectObject(hdc,ofont);
			}
		}
	}

	ReleaseDC(hWnd,hdc);
}

// ---------------------------------------------------------------------------
//  Public: install the dark subclass on a dialog window.
// ---------------------------------------------------------------------------
void InstallDarkSubclass(HWND hWnd)
{
	if (!hWnd) return;

	// avoid installing twice
	DWORD_PTR ref = 0;
	if (GetWindowSubclass(hWnd, DarkSubclassProc, DM_SUBCLASS_ID, &ref))
		return;

	SetWindowSubclass(hWnd, DarkSubclassProc, DM_SUBCLASS_ID, 0);

	// strip themes from buttons / combos / sliders so they honour our colors
	EnumChildWindows(hWnd, dm_theme_child, 0);

	// match the title bar to the dark client area
	EnableDarkTitleBar(hWnd);

	InvalidateRect(hWnd, NULL, TRUE);
}

// ---------------------------------------------------------------------------
//  Public: enable the DWM dark title bar (Windows 10 1809+).
//  Loaded dynamically so we don't hard-depend on dwmapi.lib / new SDK values.
// ---------------------------------------------------------------------------
void EnableDarkTitleBar(HWND hWnd)
{
	if (!hWnd) return;

	typedef HRESULT (WINAPI *PFN_DwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);
	static PFN_DwmSetWindowAttribute pDwmSet = NULL;
	static int tried = 0;

	if (!tried)
	{
		tried = 1;
		HMODULE h = LoadLibraryA("dwmapi.dll");
		if (h) pDwmSet = (PFN_DwmSetWindowAttribute)GetProcAddress(h, "DwmSetWindowAttribute");
	}
	if (!pDwmSet) return;

	BOOL dark = TRUE;
	// 20 = DWMWA_USE_IMMERSIVE_DARK_MODE (Win10 1903+), 19 = pre-1903 build.
	if (FAILED(pDwmSet(hWnd, 20, &dark, sizeof(dark))))
		pDwmSet(hWnd, 19, &dark, sizeof(dark));
}
