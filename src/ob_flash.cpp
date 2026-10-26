/* -----------------------------------------------------------------------------

    BrainBay  -  OpenSource Biofeedback Software, contact: chris@shifz.org

  MODULE: OB_FLASH.CPP:  the "Flash / SWF" object

  Plays a Flash movie inside a child window of the main window and writes the
  input port values into ActionScript variables of that movie. This is the
  interface used by the BioExplorer game models: "simpleAdapterLoader.swf" is
  loaded, it reads the real game from simpleadapterloader.ini and the game is
  steered by the variables in1 / in2 / in3 (one per threshold).

  The ActiveX control (Flash32_34_0_0_384.ocx, ProgID
  "ShockwaveFlash.ShockwaveFlash") is hosted through the ATL "AtlAxWin" window
  class. ATL is only used as container - the control itself is driven through
  plain IDispatch calls (GetIDsOfNames + Invoke), because the IShockwaveFlash
  interface has no header in the Windows SDK.

  Threading: the ActiveX control may only be touched by the thread that created
  it. incoming_data() and session_start() only store values and PostMessage(),
  every OLE call happens in FlashWndHandler(). Same rule that fixed the
  PhysioAmp reader-thread deadlock.

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; See the
  GNU General Public License for more details.

-----------------------------------------------------------------------------*/

#include <atlbase.h>
#include <atlwin.h>

#include "brainBay.h"
#include "ob_flash.h"

#pragma comment(lib, "atls.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "comdlg32.lib")


#define FLASH_PROGID        "ShockwaveFlash.ShockwaveFlash"
#define FLASH_WNDCLASS      "FlashClass"        // our own frame window
#define FLASH_AXWIN_CLASS   "AtlAxWin"          // ATL's ActiveX container class
#define FLASH_DEF_WIDTH     640
#define FLASH_DEF_HEIGHT    460
#define FLASH_PUSH_TIMER    1                   // window timer id
#define FLASH_PUSH_MS       30                  // push the variables at ~30 Hz


// ---------------------------------------------------------------------------
//  COM / ActiveX plumbing
// ---------------------------------------------------------------------------

static BOOL g_ole_ready = FALSE;
static BOOL g_ax_ready  = FALSE;

// ATL dereferences _pAtlModule without a NULL check in Lock()/Unlock(), and a
// plain Win32 application has no ATL module - hosting the control would crash
// with an access violation. This minimal module is created once and kept for
// the lifetime of the process.
class CFlashAtlModule : public ATL::CAtlModule
{
  public:
	HRESULT AddCommonRGSReplacements(::IRegistrarBase *) throw() { return S_OK; }
};

static void flash_ole_init(void)
{
	if (g_ole_ready) return;

	// S_FALSE means another object already initialised OLE - that is fine too,
	// we simply must not call OleUninitialize() in that case.
	if (OleInitialize(NULL) != RPC_E_CHANGED_MODE) g_ole_ready = TRUE;
	else write_logfile("Flash: OLE is running in a different apartment mode");
}

static BOOL flash_ax_init(void)
{
	if (g_ax_ready) return TRUE;

	if (ATL::_pAtlModule == NULL)
	{
		static CFlashAtlModule *module = new CFlashAtlModule();
		write_logfile("Flash: created a local ATL module (0x%p)", (void *) module);
	}

	// AtlAxWinInit() sets up the two ATL window messages and tries to register
	// the "AtlAxWin" window class. In a plain Win32 application the ATL base
	// module has no instance handle, so that registration may report a failure -
	// harmless, because AtlAxCreateControlEx() creates its own host object and
	// never uses that window class.
	ATL::AtlAxWinInit();

	g_ax_ready = TRUE;
	return TRUE;
}


// resolve a dispatch name into a DISPID
static BOOL flash_disp_id(IDispatch *disp, const OLECHAR *name, DISPID *id)
{
	LPOLESTR nm = (LPOLESTR) name;

	if (disp == NULL || id == NULL) return FALSE;

	return SUCCEEDED(disp->GetIDsOfNames(IID_NULL, &nm, 1, LOCALE_USER_DEFAULT, id));
}


// property put with one argument
static BOOL flash_put(IDispatch *disp, const OLECHAR *name, VARIANT *v)
{
	DISPID      id;
	DISPID      putid = DISPID_PROPERTYPUT;
	DISPPARAMS  dp;

	if (!flash_disp_id(disp, name, &id)) return FALSE;

	dp.rgvarg            = v;
	dp.rgdispidNamedArgs = &putid;
	dp.cArgs             = 1;
	dp.cNamedArgs        = 1;

	return SUCCEEDED(disp->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT,
	                              DISPATCH_PROPERTYPUT, &dp, NULL, NULL, NULL));
}


static BOOL flash_put_string(IDispatch *disp, const OLECHAR *name, const char *value)
{
	VARIANT v;
	BOOL    ok;
	wchar_t wide[FLASH_PATHLEN * 2];

	if (value == NULL) value = "";
	MultiByteToWideChar(CP_ACP, 0, value, -1, wide, FLASH_PATHLEN * 2);

	VariantInit(&v);
	v.vt      = VT_BSTR;
	v.bstrVal = SysAllocString(wide);
	if (v.bstrVal == NULL) return FALSE;

	ok = flash_put(disp, name, &v);
	VariantClear(&v);
	return ok;
}


static BOOL flash_put_bool(IDispatch *disp, const OLECHAR *name, BOOL value)
{
	VARIANT v;

	VariantInit(&v);
	v.vt      = VT_BOOL;
	v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;

	return flash_put(disp, name, &v);
}


static BOOL flash_put_long(IDispatch *disp, const OLECHAR *name, long value)
{
	VARIANT v;

	VariantInit(&v);
	v.vt   = VT_I4;
	v.lVal = value;

	return flash_put(disp, name, &v);
}


// method call; arguments must be passed in reverse order (last parameter first)
static BOOL flash_invoke(IDispatch *disp, const OLECHAR *name, VARIANT *args, UINT nargs)
{
	DISPID      id;
	DISPPARAMS  dp;
	VARIANT     result;
	HRESULT     hr;

	if (!flash_disp_id(disp, name, &id)) return FALSE;

	dp.rgvarg            = args;
	dp.rgdispidNamedArgs = NULL;
	dp.cArgs             = nargs;
	dp.cNamedArgs        = 0;

	VariantInit(&result);
	hr = disp->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT,
	                  DISPATCH_METHOD, &dp, &result, NULL, NULL);
	VariantClear(&result);

	return SUCCEEDED(hr);
}


// read a numeric property of the control (ReadyState, TotalFrames, ...)
static BOOL flash_get_long(IDispatch *disp, const OLECHAR *name, long *out)
{
	DISPID      id;
	DISPPARAMS  dp;
	VARIANT     result;
	HRESULT     hr;

	if (out) *out = -1;
	if (disp == NULL) return FALSE;
	if (!flash_disp_id(disp, name, &id)) return FALSE;

	memset(&dp, 0, sizeof(dp));
	VariantInit(&result);
	hr = disp->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT,
	                  DISPATCH_PROPERTYGET, &dp, &result, NULL, NULL);
	if (SUCCEEDED(hr) && out)
	{
		if (result.vt == VT_I4)     *out = result.lVal;
		else if (result.vt == VT_I2) *out = result.iVal;
		else if (result.vt == VT_R8) *out = (long) result.dblVal;
	}
	VariantClear(&result);

	return SUCCEEDED(hr);
}


// SetVariable("name", "value") - the actual data path into the movie
static BOOL flash_set_variable(IDispatch *disp, const char *name, float value)
{
	VARIANT args[2];
	BOOL    ok;
	char    text[32];
	wchar_t wname[FLASH_VARLEN * 2];
	wchar_t wval[64];

	if (disp == NULL || name == NULL || name[0] == 0) return FALSE;

	sprintf(text, "%.4f", value);
	MultiByteToWideChar(CP_ACP, 0, name, -1, wname, FLASH_VARLEN * 2);
	MultiByteToWideChar(CP_ACP, 0, text, -1, wval, 64);

	VariantInit(&args[0]);
	args[0].vt      = VT_BSTR;
	args[0].bstrVal = SysAllocString(wval);

	VariantInit(&args[1]);
	args[1].vt      = VT_BSTR;
	args[1].bstrVal = SysAllocString(wname);

	ok = flash_invoke(disp, L"SetVariable", args, 2);

	VariantClear(&args[0]);
	VariantClear(&args[1]);

	return ok;
}


// ---------------------------------------------------------------------------
//  object helpers - all of them run on the thread that owns displayWnd
// ---------------------------------------------------------------------------

static FLASHOBJ *flash_find(HWND hWnd)
{
	int t;
	FLASHOBJ *st = NULL;

	for (t = 0; (t < GLOBAL.objects) && (st == NULL); t++)
		if (objects[t]->type == OB_FLASH)
		{
			st = (FLASHOBJ *) objects[t];
			if (st->displayWnd != hWnd) st = NULL;
		}

	return st;
}


static void flash_store_geometry(FLASHOBJ *st)
{
	RECT rc;

	if (st->displayWnd == NULL) return;

	GetWindowRect(st->displayWnd, &rc);
	MapWindowPoints(NULL, ghWndMain, (LPPOINT) &rc, 2);

	st->left   = rc.left;
	st->top    = rc.top;
	st->right  = rc.right;
	st->bottom = rc.bottom;
}


static void flash_release(FLASHOBJ *st)
{
	if (st->ax_ctrl != NULL)
	{
		((IDispatch *) st->ax_ctrl)->Release();
		st->ax_ctrl = NULL;
	}

	if (st->ax_host != NULL)
	{
		DestroyWindow((HWND) st->ax_host);
		st->ax_host = NULL;
	}

	// the client site has to outlive the control, so it is released last
	if (st->ax_container != NULL)
	{
		((IUnknown *) st->ax_container)->Release();
		st->ax_container = NULL;
	}
}


static BOOL flash_ensure_control(FLASHOBJ *st)
{
	RECT       rc;
	HWND       hAx;
	IUnknown  *pUnknown   = NULL;
	IUnknown  *pContainer = NULL;
	IDispatch *pDispatch  = NULL;
	HRESULT    hr;

	if (st->ax_ctrl != NULL) return TRUE;
	if (st->displayWnd == NULL) return FALSE;

	flash_ole_init();
	if (!flash_ax_init()) return FALSE;

	GetClientRect(st->displayWnd, &rc);
	if (rc.right  < 16) rc.right  = FLASH_DEF_WIDTH;
	if (rc.bottom < 16) rc.bottom = FLASH_DEF_HEIGHT;

	// A plain child window is enough as container: the ATL host object created
	// by AtlAxCreateControlEx() activates the control in place inside it and
	// returns the control's IUnknown.
	hAx = CreateWindow("STATIC", NULL,
	                   WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
	                   0, 0, rc.right, rc.bottom,
	                   st->displayWnd, NULL, hInst, NULL);
	if (hAx == NULL)
	{
		write_logfile("Flash: could not create the control container (error %d)", GetLastError());
		return FALSE;
	}

	hr = ATL::AtlAxCreateControlEx(L"ShockwaveFlash.ShockwaveFlash", hAx, NULL,
	                               &pContainer, &pUnknown, IID_NULL, NULL);
	if (FAILED(hr) || pUnknown == NULL)
	{
		write_logfile("Flash: cannot create '" FLASH_PROGID "' (0x%08x) - is the Flash "
		              "ActiveX control installed?", (unsigned int) hr);
		DestroyWindow(hAx);
		return FALSE;
	}

	hr = pUnknown->QueryInterface(IID_IDispatch, (void **) &pDispatch);
	pUnknown->Release();

	if (FAILED(hr) || pDispatch == NULL)
	{
		write_logfile("Flash: the Flash control has no IDispatch interface (0x%08x)", (unsigned int) hr);
		DestroyWindow(hAx);
		return FALSE;
	}

	st->ax_host      = (void *) hAx;
	st->ax_container = (void *) pContainer;		// keep the client site alive
	st->ax_ctrl      = (void *) pDispatch;

	MoveWindow(hAx, 0, 0, rc.right, rc.bottom, TRUE);
	write_logfile("Flash: ActiveX control created");

	return TRUE;
}


static void flash_apply_settings(FLASHOBJ *st)
{
	IDispatch *disp = (IDispatch *) st->ax_ctrl;

	if (disp == NULL) return;

	flash_put_string(disp, L"BGColor",   "#000000");
	flash_put_long  (disp, L"Quality",   st->quality);		// Flash enum: 0..5
	flash_put_long  (disp, L"ScaleMode", st->scalemode);	// Flash enum: 0..3
	flash_put_bool  (disp, L"Loop",      st->loop_movie);
	flash_put_bool  (disp, L"Menu",      st->showmenu);

	if (st->swfpath[0] == 0)
	{
		write_logfile("Flash: no movie selected, use the Flash toolbox to choose a .swf file");
		return;
	}

	// "Movie" is the standard dispatch name of IShockwaveFlash::put_Movie
	if (!flash_put_string(disp, L"Movie", st->swfpath))
	{
		if (!flash_put_string(disp, L"Src", st->swfpath))
		{
			write_logfile("Flash: loading '%s' failed", st->swfpath);
			return;
		}
	}

	flash_invoke   (disp, L"Rewind",  NULL, 0);
	flash_put_bool (disp, L"Playing", TRUE);
	write_logfile("Flash: loading '%s'", st->swfpath);
}


// find the input port that carries the given SWF variable name (case
// insensitive) - falls back to a fixed port number
static int flash_drive_channel(FLASHOBJ *st, const char *name, int fallback)
{
	int i;

	for (i = 0; i < st->channels; i++)
		if (lstrcmpi(st->varnames[i], name) == 0) return i;

	if (fallback >= st->channels) fallback = st->channels - 1;
	if (fallback < 0) fallback = 0;
	return fallback;
}


// the score gate is open while the gate port delivers a value above 0.5 -
// BrainBay's AND object reports TRUE_VALUE (512) and INVALID_VALUE (-32767)
static int flash_gate_open(FLASHOBJ *st)
{
	int p = st->gateport;

	if (p < 0 || p >= st->channels) return 0;
	if (st->last_value[p] == INVALID_VALUE) return 0;
	return (st->last_value[p] > 0.5f);
}


static void flash_push_values(FLASHOBJ *st)
{
	int   i;
	int   ok = 0;
	int   scorech, clockch;
	float v;
	DWORD now;

	if (st->ax_ctrl == NULL) return;

	if (st->drive)
	{
		if (st->clockms < 50)  st->clockms = 50;
		if (st->scorems < 100) st->scorems = 100;

		scorech = flash_drive_channel(st, st->scorevar, 0);
		clockch = flash_drive_channel(st, st->clockvar, 1);

		now = GetTickCount();

		// keep alive clock: always runs, so In1In2InputModel never pauses
		if (st->clocktick == 0 || now < st->clocktick) st->clocktick = now;
		while ((LONG) (now - st->clocktick) >= st->clockms)
		{
			st->clocktick += (DWORD) st->clockms;
			st->heartbeat += 1.0;
		}

		// score: only grows while the gate port reports success
		if (flash_gate_open(st))
		{
			if (st->scoretick == 0 || now < st->scoretick) st->scoretick = now;
			while ((LONG) (now - st->scoretick) >= st->scorems)
			{
				st->scoretick += (DWORD) st->scorems;
				st->score += 1.0;
			}
		}
		else
		{
			// gate closed: do not bank the elapsed time, restart the interval
			st->scoretick = now;
		}
	}

	for (i = 0; i < st->channels; i++)
	{
		v = st->last_value[i];

		if (st->drive)
		{
			if (i == scorech)      v = (float) st->score;
			else if (i == clockch) v = (float) st->heartbeat;
		}

		if (flash_set_variable((IDispatch *) st->ax_ctrl, st->varnames[i], v))
			ok++;
	}

	if (ok > 0)
	{
		if (!st->var_ok)
			write_logfile("Flash: SetVariable() works, the movie is under control");

		st->var_ok     = 1;
		st->sent_count++;

		// a few samples of the load state: TotalFrames == 0 means the movie did
		// not really render (wrong loader build, game swf missing, ...)
		if (st->sent_count == 2 || st->sent_count == 100 || st->sent_count == 400)
		{
			long ready = -1, frames = -1, pct = -1;
			flash_get_long((IDispatch *) st->ax_ctrl, L"ReadyState",    &ready);
			flash_get_long((IDispatch *) st->ax_ctrl, L"PercentLoaded", &pct);
			flash_get_long((IDispatch *) st->ax_ctrl, L"TotalFrames",   &frames);
			write_logfile("Flash: ReadyState=%ld PercentLoaded=%ld TotalFrames=%ld",
			              ready, pct, frames);
		}
	}
}


// ---------------------------------------------------------------------------
//  flash window
// ---------------------------------------------------------------------------

LRESULT CALLBACK FlashWndHandler(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	FLASHOBJ *st = flash_find(hWnd);

	if (st == NULL) return DefWindowProc(hWnd, message, wParam, lParam);

	switch (message)
	{
	  case WM_MOUSEACTIVATE:
		actobject = st;
		InvalidateRect(ghWndDesign, NULL, TRUE);
		break;

	  case WM_SIZE:
	  case WM_MOVE:
		{
			WINDOWPLACEMENT wndpl;

			if ((message == WM_SIZE) && (st->ax_host != NULL))
			{
				RECT rc;
				GetClientRect(hWnd, &rc);
				MoveWindow((HWND) st->ax_host, 0, 0, rc.right, rc.bottom, TRUE);
			}

			if (GLOBAL.locksession)
			{
				wndpl.length = sizeof(wndpl);
				GetWindowPlacement(hWnd, &wndpl);
				SetWindowLong(hWnd, GWL_STYLE, GetWindowLong(hWnd, GWL_STYLE) & ~WS_SIZEBOX);
			}
			else
			{
				SetWindowLong(hWnd, GWL_STYLE, GetWindowLong(hWnd, GWL_STYLE) | WS_SIZEBOX);
				flash_store_geometry(st);
			}
		}
		break;

	  case WM_FLASH_CMD:
		switch (wParam)
		{
		  case FLASH_CMD_RELOAD:
			st->reload_req = 0;
			st->var_ok     = 0;
		// Never destroy and re-create the control here: the Flash ActiveX control
		// does not survive a destroy/create cycle reliably and answers with a
		// "the necessary components could not be loaded - repair Adobe Flash"
		// page. Setting the Movie property again is enough to restart the film.
		if (st->ax_ctrl == NULL)
		{
			if (flash_ensure_control(st)) flash_apply_settings(st);
		}
		else flash_apply_settings(st);
		break;

	  case FLASH_CMD_PLAY:
		if (st->ax_ctrl == NULL)
		{
			st->reload_req = 0;
			if (flash_ensure_control(st)) flash_apply_settings(st);
		}
		else flash_put_bool((IDispatch *) st->ax_ctrl, L"Playing", TRUE);
		flash_push_values(st);
		break;

	  case FLASH_CMD_STOP:
		if (st->ax_ctrl != NULL)
			flash_put_bool((IDispatch *) st->ax_ctrl, L"Playing", FALSE);
		break;

	  case FLASH_CMD_UPDATE:
		InterlockedExchange(&st->pending, 0);
		flash_push_values(st);
		break;
	}
	return 0;

	  case WM_TIMER:
	// 30 Hz is plenty for the movie and keeps the number of cross-apartment
	// SetVariable() calls low
	if (wParam == FLASH_PUSH_TIMER)
	{
		if (st->ax_ctrl != NULL && st->active) flash_push_values(st);
		return 0;
	}
	break;

	  case WM_DESTROY:
		KillTimer(hWnd, FLASH_PUSH_TIMER);
	  default:
		return DefWindowProc(hWnd, message, wParam, lParam);
	}

	return 0;
}


// ---------------------------------------------------------------------------
//  toolbox dialog
// ---------------------------------------------------------------------------

static void flash_dlg_apply(HWND hDlg, FLASHOBJ *st)
{
	int  i;
	char text[FLASH_PATHLEN];

	GetDlgItemText(hDlg, IDC_FL_PATH, text, FLASH_PATHLEN);
	strncpy(st->swfpath, text, FLASH_PATHLEN - 1);
	st->swfpath[FLASH_PATHLEN - 1] = 0;

	st->channels  = GetDlgItemInt(hDlg, IDC_FL_CHANNELS, NULL, FALSE);
	if (st->channels < 1) st->channels = 1;
	if (st->channels > FLASH_MAXVAR) st->channels = FLASH_MAXVAR;

	st->autoplay   = (IsDlgButtonChecked(hDlg, IDC_FL_AUTOPLAY) == BST_CHECKED);
	st->loop_movie = (IsDlgButtonChecked(hDlg, IDC_FL_LOOP)     == BST_CHECKED);
	st->showmenu   = (IsDlgButtonChecked(hDlg, IDC_FL_MENU)     == BST_CHECKED);

	st->quality   = (int) SendDlgItemMessage(hDlg, IDC_FL_QUALITY,   CB_GETCURSEL, 0, 0);
	st->scalemode = (int) SendDlgItemMessage(hDlg, IDC_FL_SCALEMODE, CB_GETCURSEL, 0, 0);
	if (st->quality   < 0) st->quality   = FLASH_QUALITY_HIGH;
	if (st->scalemode < 0) st->scalemode = FLASH_SCALE_SHOWALL;

	st->drive    = (IsDlgButtonChecked(hDlg, IDC_FL_DRIVE) == BST_CHECKED);
	st->gateport = GetDlgItemInt(hDlg, IDC_FL_GATEPORT, NULL, TRUE);
	st->scorems  = GetDlgItemInt(hDlg, IDC_FL_SCOREMS,  NULL, FALSE);
	st->clockms  = GetDlgItemInt(hDlg, IDC_FL_CLOCKMS,  NULL, FALSE);
	if (st->gateport < 0) st->gateport = 0;
	if (st->scorems < 100) st->scorems = 1000;
	if (st->clockms < 50)  st->clockms = 500;

	for (i = 0; i < FLASH_MAXVAR; i++)
	{
		GetDlgItemText(hDlg, IDC_FL_VAR1 + i, text, FLASH_VARLEN);
		if (text[0] == 0) sprintf(text, "In%d", i + 1);
		strncpy(st->varnames[i], text, FLASH_VARLEN - 1);
		st->varnames[i][FLASH_VARLEN - 1] = 0;
	}

	st->update_inports();
	update_dimensions();
}


static void flash_dlg_browse(HWND hDlg, FLASHOBJ *st)
{
	OPENFILENAME ofn;
	char  file[FLASH_PATHLEN];
	DWORD attrs;

	file[0] = 0;
	GetDlgItemText(hDlg, IDC_FL_PATH, file, FLASH_PATHLEN);

	memset(&ofn, 0, sizeof(ofn));
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner   = hDlg;
	ofn.lpstrFilter = "Flash movies (*.swf)\0*.swf\0All files (*.*)\0*.*\0";
	ofn.lpstrFile   = file;
	ofn.nMaxFile    = FLASH_PATHLEN;
	ofn.lpstrDefExt = "swf";
	ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;

	attrs = GetFileAttributes(file);
	if (attrs != INVALID_FILE_ATTRIBUTES)
	{
		// start in the directory of the current file
		char *p = strrchr(file, '\\');
		if (p) { *(p + 1) = 0; ofn.lpstrInitialDir = file; file[0] = 0; }
	}

	if (GetOpenFileName(&ofn))
	{
		SetDlgItemText(hDlg, IDC_FL_PATH, file);
		flash_dlg_apply(hDlg, st);
		st->request_reload();
	}
}


LRESULT CALLBACK FlashDlgHandler(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
	FLASHOBJ *st;
	int i;

	st = (FLASHOBJ *) actobject;
	if ((st == NULL) || (st->type != OB_FLASH)) return FALSE;

	switch (message)
	{
	  case WM_INITDIALOG:
		SetDlgItemText(hDlg, IDC_FL_PATH, st->swfpath);
		SetDlgItemInt(hDlg, IDC_FL_CHANNELS, st->channels, FALSE);

		CheckDlgButton(hDlg, IDC_FL_AUTOPLAY, st->autoplay   ? BST_CHECKED : BST_UNCHECKED);
		CheckDlgButton(hDlg, IDC_FL_LOOP,     st->loop_movie ? BST_CHECKED : BST_UNCHECKED);
		CheckDlgButton(hDlg, IDC_FL_MENU,     st->showmenu   ? BST_CHECKED : BST_UNCHECKED);

		SendDlgItemMessage(hDlg, IDC_FL_QUALITY, CB_ADDSTRING, 0, (LPARAM) "Low");
		SendDlgItemMessage(hDlg, IDC_FL_QUALITY, CB_ADDSTRING, 0, (LPARAM) "AutoLow");
		SendDlgItemMessage(hDlg, IDC_FL_QUALITY, CB_ADDSTRING, 0, (LPARAM) "AutoHigh");
		SendDlgItemMessage(hDlg, IDC_FL_QUALITY, CB_ADDSTRING, 0, (LPARAM) "Medium");
		SendDlgItemMessage(hDlg, IDC_FL_QUALITY, CB_ADDSTRING, 0, (LPARAM) "High");
		SendDlgItemMessage(hDlg, IDC_FL_QUALITY, CB_ADDSTRING, 0, (LPARAM) "Best");
		SendDlgItemMessage(hDlg, IDC_FL_QUALITY, CB_SETCURSEL, st->quality, 0);

		SendDlgItemMessage(hDlg, IDC_FL_SCALEMODE, CB_ADDSTRING, 0, (LPARAM) "ShowAll");
		SendDlgItemMessage(hDlg, IDC_FL_SCALEMODE, CB_ADDSTRING, 0, (LPARAM) "NoBorder");
		SendDlgItemMessage(hDlg, IDC_FL_SCALEMODE, CB_ADDSTRING, 0, (LPARAM) "ExactFit");
		SendDlgItemMessage(hDlg, IDC_FL_SCALEMODE, CB_ADDSTRING, 0, (LPARAM) "NoScale");
		SendDlgItemMessage(hDlg, IDC_FL_SCALEMODE, CB_SETCURSEL, st->scalemode, 0);

		for (i = 0; i < FLASH_MAXVAR; i++)
			SetDlgItemText(hDlg, IDC_FL_VAR1 + i, st->varnames[i]);

		CheckDlgButton(hDlg, IDC_FL_DRIVE, st->drive ? BST_CHECKED : BST_UNCHECKED);
		SetDlgItemInt(hDlg, IDC_FL_GATEPORT, st->gateport, TRUE);
		SetDlgItemInt(hDlg, IDC_FL_SCOREMS,  st->scorems,  FALSE);
		SetDlgItemInt(hDlg, IDC_FL_CLOCKMS,  st->clockms,  FALSE);

		SetDlgItemText(hDlg, IDC_FL_STATUS,
		               st->swfpath[0] ? "press Reload to (re)start the movie"
		                              : "no movie loaded - choose simpleAdapterLoader.swf or a game .swf");
		return TRUE;

	  case WM_CLOSE:
		EndDialog(hDlg, LOWORD(wParam));
		return TRUE;

	  case WM_COMMAND:
		switch (LOWORD(wParam))
		{
		  case IDC_FL_BROWSE:
			flash_dlg_browse(hDlg, st);
			break;

		  case IDC_FL_RELOAD:
			flash_dlg_apply(hDlg, st);
			st->request_reload();
			break;

		  case IDC_FL_PATH:
		  case IDC_FL_CHANNELS:
		  case IDC_FL_AUTOPLAY:
		  case IDC_FL_LOOP:
		  case IDC_FL_MENU:
		  case IDC_FL_QUALITY:
		  case IDC_FL_SCALEMODE:
		  case IDC_FL_DRIVE:
		  case IDC_FL_GATEPORT:
		  case IDC_FL_SCOREMS:
		  case IDC_FL_CLOCKMS:
		  case IDC_FL_VAR1: case IDC_FL_VAR2: case IDC_FL_VAR3: case IDC_FL_VAR4:
		  case IDC_FL_VAR5: case IDC_FL_VAR6: case IDC_FL_VAR7: case IDC_FL_VAR8:
			flash_dlg_apply(hDlg, st);
			break;

		  case IDOK:
			flash_dlg_apply(hDlg, st);
			EndDialog(hDlg, IDOK);
			break;
		}
		return TRUE;
	}

	return FALSE;
}


// ---------------------------------------------------------------------------
//  object implementation
// ---------------------------------------------------------------------------

FLASHOBJ::FLASHOBJ(int num) : BASE_CL()
{
	int i;

	swfpath[0] = 0;
	wndcaption[0] = 0;
	strcpy(wndcaption, "Flash");

	channels = 3;
	for (i = 0; i < FLASH_MAXVAR; i++)
	{
		// the NeuroFlasher movies read upper case In1..In8 - ActionScript variable
		// names are case sensitive, lower case in1 never reached the loader
		sprintf(varnames[i], "In%d", i + 1);
		last_value[i] = 0.0f;
	}

	autoplay    = 1;
	loop_movie  = 0;
	showmenu    = 0;
	quality     = FLASH_QUALITY_HIGH;
	scalemode   = FLASH_SCALE_SHOWALL;

	// raw pass through by default - the NeuroFlasher designs switch drive on
	drive      = 0;
	gateport   = 0;
	scorems    = 1000;
	clockms    = 500;
	strcpy(scorevar, "In1");
	strcpy(clockvar, "In2");
	score      = 0.0;
	heartbeat  = 0.0;
	scoretick  = 0;
	clocktick  = 0;

	ax_host      = NULL;
	ax_container = NULL;
	ax_ctrl      = NULL;
	pending     = 0;
	active      = 0;
	reload_req  = 1;
	var_ok      = 0;
	sent_count  = 0;

	outports = 0;
	width    = 90;

	left   = 40;
	top    = 40;
	right  = left + FLASH_DEF_WIDTH;
	bottom = top + FLASH_DEF_HEIGHT;

	update_inports();

	if (!(displayWnd = CreateWindow(FLASH_WNDCLASS, wndcaption,
	                                WS_CLIPSIBLINGS | WS_CHILD | WS_CAPTION |
	                                WS_THICKFRAME | WS_VISIBLE,
	                                left, top, right - left, bottom - top,
	                                ghWndMain, NULL, hInst, NULL)))
		report_error("can't create Flash Window");
	else
	{
		ShowWindow(displayWnd, TRUE);
		UpdateWindow(displayWnd);
		SetTimer(displayWnd, FLASH_PUSH_TIMER, FLASH_PUSH_MS, NULL);
	}
}


FLASHOBJ::~FLASHOBJ(void)
{
	flash_release(this);

	if (displayWnd != NULL)
	{
		DestroyWindow(displayWnd);
		displayWnd = NULL;
	}
}


void FLASHOBJ::update_inports(void)
{
	int i;

	if (channels < 1) channels = 1;
	if (channels > FLASH_MAXVAR) channels = FLASH_MAXVAR;

	inports = channels;

	for (i = 0; i < inports; i++)
	{
		strncpy(in_ports[i].in_name, varnames[i], 19);
		in_ports[i].in_name[19] = 0;
		sprintf(in_ports[i].in_desc, "SWF variable %s", varnames[i]);
		in_ports[i].in_min     = 0.0f;
		in_ports[i].in_max     = 1.0f;
		strcpy(in_ports[i].in_dim, "ratio");
		in_ports[i].get_range  = 0;
	}

	height = CON_START + inports * CON_HEIGHT + 5;
}


void FLASHOBJ::request_reload(void)
{
	reload_req = 1;
	if (displayWnd != NULL) PostMessage(displayWnd, WM_FLASH_CMD, FLASH_CMD_RELOAD, 0);
}


void FLASHOBJ::request_play(void)
{
	if (displayWnd != NULL) PostMessage(displayWnd, WM_FLASH_CMD, FLASH_CMD_PLAY, 0);
}


void FLASHOBJ::request_stop(void)
{
	if (displayWnd != NULL) PostMessage(displayWnd, WM_FLASH_CMD, FLASH_CMD_STOP, 0);
}


void FLASHOBJ::make_dialog(void)
{
	display_toolbox(hDlg = CreateDialog(hInst, (LPCTSTR) IDD_FLASHBOX,
	                                    ghWndStatusbox, (DLGPROC) FlashDlgHandler));
}


void FLASHOBJ::load(HANDLE hFile)
{
	int  i;
	char key[16];

	load_object_basics(this);

	load_property("swfpath",   P_STRING, swfpath);
	load_property("channels",  P_INT,    &channels);
	load_property("wndcaption",P_STRING, wndcaption);
	load_property("autoplay",  P_INT,    &autoplay);
	load_property("loop",      P_INT,    &loop_movie);
	load_property("showmenu",  P_INT,    &showmenu);
	load_property("quality",   P_INT,    &quality);
	load_property("scalemode", P_INT,    &scalemode);
	load_property("drive",     P_INT,    &drive);
	load_property("gateport",  P_INT,    &gateport);
	load_property("scorems",   P_INT,    &scorems);
	load_property("clockms",   P_INT,    &clockms);
	load_property("scorevar",  P_STRING, scorevar);
	load_property("clockvar",  P_STRING, clockvar);

	for (i = 0; i < FLASH_MAXVAR; i++)
	{
		sprintf(key, "var%d", i + 1);
		load_property(key, P_STRING, varnames[i]);
	}

	load_property("left",   P_INT, &left);
	load_property("top",    P_INT, &top);
	load_property("right",  P_INT, &right);
	load_property("bottom", P_INT, &bottom);

	update_inports();

	if (displayWnd != NULL)
	{
		SetWindowText(displayWnd, wndcaption);
		MoveWindow(displayWnd, left, top, right - left, bottom - top, TRUE);
	}

	request_reload();
}


void FLASHOBJ::save(HANDLE hFile)
{
	int  i;
	char key[16];

	save_object_basics(hFile, this);

	save_property(hFile, "swfpath",   P_STRING, swfpath);
	save_property(hFile, "channels",  P_INT,    &channels);
	save_property(hFile, "wndcaption",P_STRING, wndcaption);
	save_property(hFile, "autoplay",  P_INT,    &autoplay);
	save_property(hFile, "loop",      P_INT,    &loop_movie);
	save_property(hFile, "showmenu",  P_INT,    &showmenu);
	save_property(hFile, "quality",   P_INT,    &quality);
	save_property(hFile, "scalemode", P_INT,    &scalemode);
	save_property(hFile, "drive",     P_INT,    &drive);
	save_property(hFile, "gateport",  P_INT,    &gateport);
	save_property(hFile, "scorems",   P_INT,    &scorems);
	save_property(hFile, "clockms",   P_INT,    &clockms);
	save_property(hFile, "scorevar",  P_STRING, scorevar);
	save_property(hFile, "clockvar",  P_STRING, clockvar);

	for (i = 0; i < FLASH_MAXVAR; i++)
	{
		sprintf(key, "var%d", i + 1);
		save_property(hFile, key, P_STRING, varnames[i]);
	}

	save_property(hFile, "left",   P_INT, &left);
	save_property(hFile, "top",    P_INT, &top);
	save_property(hFile, "right",  P_INT, &right);
	save_property(hFile, "bottom", P_INT, &bottom);
}


void FLASHOBJ::incoming_data(int port, float value)
{
	if (port < 0 || port >= FLASH_MAXVAR) return;

	// the timer thread only stores the value - the ActiveX control is written by
	// the window timer in FlashWndHandler(), i.e. on the thread that owns it
	last_value[port] = value;
}


void FLASHOBJ::work(void)
{
	// nothing to do - the values are pushed from FlashWndHandler()
}


void FLASHOBJ::session_start(void)
{
	active = 1;

	score     = 0.0;
	heartbeat = 0.0;
	scoretick = 0;
	clocktick = 0;

	if (reload_req || ax_ctrl == NULL)
	{
		reload_req = 1;
		request_reload();
	}
	else if (autoplay)
		request_play();
}


void FLASHOBJ::session_stop(void)
{
	active = 0;

	request_stop();
}


void FLASHOBJ::session_reset(void)
{
	active = 0;

	score     = 0.0;
	heartbeat = 0.0;
	scoretick = 0;
	clocktick = 0;

	request_stop();
}
