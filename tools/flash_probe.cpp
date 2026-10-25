// ---------------------------------------------------------------------------
//  flash_probe.cpp - standalone test for hosting the Flash ActiveX control
//
//  Reproduces exactly what the BrainBay "Flash" object does, but outside of
//  BrainBay, and writes every step to flash_probe.log so that a crash can be
//  located (the last line in the log is the call that killed the process).
//
//  build:
//    cmd /c ""<vcvarsall.bat>" x86 && cl /nologo /EHsc /MT flash_probe.cpp
//        /I"<msvc>\atlmfc\include" /link /libpath:"<msvc>\atlmfc\lib\x86"
//        atls.lib oleaut32.lib ole32.lib user32.lib gdi32.lib"
// ---------------------------------------------------------------------------

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <atlbase.h>
#include <atlwin.h>
#include <stdio.h>
#include <stdarg.h>

#pragma comment(lib, "atls.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

static FILE *g_log = NULL;

static void lg(const char *fmt, ...)
{
	va_list a;

	if (g_log == NULL) return;
	va_start(a, fmt);
	vfprintf(g_log, fmt, a);
	va_end(a);
	fputc('\n', g_log);
	fflush(g_log);
}

// minimal ATL module - ATL's ActiveX host object dereferences _pAtlModule in
// Lock()/Unlock(), which is NULL in a plain Win32 application
class CProbeAtlModule : public ATL::CAtlModule
{
public:
	HRESULT AddCommonRGSReplacements(::IRegistrarBase *) throw() { return S_OK; }
};

static LONG WINAPI probe_filter(EXCEPTION_POINTERS *ep)
{
	lg("!!! EXCEPTION code=0x%08x address=0x%p",
	   (unsigned int) ep->ExceptionRecord->ExceptionCode,
	   ep->ExceptionRecord->ExceptionAddress);
	return EXCEPTION_EXECUTE_HANDLER;
}

static BOOL disp_id(IDispatch *d, const OLECHAR *name, DISPID *id)
{
	LPOLESTR nm = (LPOLESTR) name;

	if (d == NULL) return FALSE;
	return SUCCEEDED(d->GetIDsOfNames(IID_NULL, &nm, 1, LOCALE_USER_DEFAULT, id));
}

static BOOL put_str(IDispatch *d, const OLECHAR *name, const wchar_t *val)
{
	DISPID id, putid = DISPID_PROPERTYPUT;
	VARIANT v;
	DISPPARAMS dp;
	BOOL ok;

	if (!disp_id(d, name, &id)) { lg("put %ls: name not found", name); return FALSE; }

	VariantInit(&v);
	v.vt = VT_BSTR;
	v.bstrVal = SysAllocString(val);

	dp.rgvarg = &v;
	dp.rgdispidNamedArgs = &putid;
	dp.cArgs = 1;
	dp.cNamedArgs = 1;

	HRESULT hr = d->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYPUT,
	                       &dp, NULL, NULL, NULL);
	ok = SUCCEEDED(hr);
	VariantClear(&v);
	lg("put %ls = %ls -> 0x%08x", name, val, (unsigned int) hr);
	return ok;
}

static BOOL put_bool(IDispatch *d, const OLECHAR *name, BOOL b)
{
	DISPID id, putid = DISPID_PROPERTYPUT;
	VARIANT v;
	DISPPARAMS dp;
	BOOL ok;

	if (!disp_id(d, name, &id)) { lg("put %ls: name not found", name); return FALSE; }

	VariantInit(&v);
	v.vt = VT_BOOL;
	v.boolVal = b ? VARIANT_TRUE : VARIANT_FALSE;

	dp.rgvarg = &v;
	dp.rgdispidNamedArgs = &putid;
	dp.cArgs = 1;
	dp.cNamedArgs = 1;

	HRESULT hr = d->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYPUT,
	                       &dp, NULL, NULL, NULL);
	ok = SUCCEEDED(hr);
	lg("put %ls = %d -> 0x%08x", name, b, (unsigned int) hr);
	return ok;
}

static BOOL set_var(IDispatch *d, const wchar_t *name, const wchar_t *val)
{
	DISPID id;
	VARIANT args[2];
	DISPPARAMS dp;
	VARIANT res;
	HRESULT hr;

	if (!disp_id(d, L"SetVariable", &id)) { lg("SetVariable: name not found"); return FALSE; }

	VariantInit(&args[0]);
	args[0].vt = VT_BSTR;
	args[0].bstrVal = SysAllocString(val);

	VariantInit(&args[1]);
	args[1].vt = VT_BSTR;
	args[1].bstrVal = SysAllocString(name);

	dp.rgvarg = args;
	dp.rgdispidNamedArgs = NULL;
	dp.cArgs = 2;
	dp.cNamedArgs = 0;

	VariantInit(&res);
	hr = d->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD, &dp, &res, NULL, NULL);
	VariantClear(&res);
	VariantClear(&args[0]);
	VariantClear(&args[1]);

	// only log failures: one log line per sample would slow the loop down
	if (FAILED(hr)) lg("SetVariable(%ls, %ls) -> 0x%08x", name, val, (unsigned int) hr);
	return SUCCEEDED(hr);
}

static BOOL put_long(IDispatch *d, const OLECHAR *name, long val)
{
	DISPID id, putid = DISPID_PROPERTYPUT;
	VARIANT v;
	DISPPARAMS dp;
	HRESULT hr;

	if (!disp_id(d, name, &id)) { lg("put %ls: name not found", name); return FALSE; }

	VariantInit(&v);
	v.vt = VT_I4;
	v.lVal = val;

	dp.rgvarg = &v;
	dp.rgdispidNamedArgs = &putid;
	dp.cArgs = 1;
	dp.cNamedArgs = 1;

	hr = d->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYPUT,
	               &dp, NULL, NULL, NULL);
	lg("put %ls = %ld -> 0x%08x", name, val, (unsigned int) hr);
	return SUCCEEDED(hr);
}

// read a string property/method result
static void get_var(IDispatch *d, const wchar_t *name)
{
	DISPID id;
	VARIANT arg, res;
	DISPPARAMS dp;
	HRESULT hr;

	if (!disp_id(d, L"GetVariable", &id)) { lg("GetVariable: name not found"); return; }

	VariantInit(&arg);
	arg.vt = VT_BSTR;
	arg.bstrVal = SysAllocString(name);

	dp.rgvarg = &arg;
	dp.rgdispidNamedArgs = NULL;
	dp.cArgs = 1;
	dp.cNamedArgs = 0;

	VariantInit(&res);
	hr = d->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD, &dp, &res, NULL, NULL);
	lg("GetVariable(%ls) -> 0x%08x  value=%ls", name, (unsigned int) hr,
	   (SUCCEEDED(hr) && res.vt == VT_BSTR && res.bstrVal) ? res.bstrVal : L"<none>");
	VariantClear(&res);
	VariantClear(&arg);
}

// read a numeric property
static void get_long(IDispatch *d, const OLECHAR *name)
{
	DISPID id;
	DISPPARAMS dp;
	VARIANT res;
	HRESULT hr;

	if (!disp_id(d, name, &id)) { lg("get %ls: name not found", name); return; }

	dp.rgvarg = NULL;
	dp.rgdispidNamedArgs = NULL;
	dp.cArgs = 0;
	dp.cNamedArgs = 0;

	VariantInit(&res);
	hr = d->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET, &dp, &res, NULL, NULL);
	lg("get %ls -> 0x%08x  value=%ld", name, (unsigned int) hr,
	   (SUCCEEDED(hr) && res.vt == VT_I4) ? res.lVal : -1L);
	VariantClear(&res);
}

static void pump(int ms)
{
	DWORD end = GetTickCount() + ms;
	MSG   msg;

	while (GetTickCount() < end)
	{
		while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		Sleep(5);
	}
}

static void save_client_bmp(HWND hWnd, const char *path)
{
	RECT   rc;
	POINT  pt = { 0, 0 };
	int    w, h, stride, imgsize;
	HDC    hScreen, hMem;
	HBITMAP hBmp, hOld;
	BITMAPFILEHEADER fh;
	BITMAPINFOHEADER ih;
	BYTE  *bits;
	FILE  *f;

	GetClientRect(hWnd, &rc);
	w = rc.right;
	h = rc.bottom;
	if (w < 8 || h < 8) { lg("capture: window too small"); return; }

	ClientToScreen(hWnd, &pt);
	hScreen = GetDC(NULL);
	hMem    = CreateCompatibleDC(hScreen);
	hBmp    = CreateCompatibleBitmap(hScreen, w, h);
	hOld    = (HBITMAP) SelectObject(hMem, hBmp);

	// PrintWindow renders the window itself, independent of what is on top of it;
	// the screen copy is only a fallback
	if (!PrintWindow(hWnd, hMem, 2 /* PW_RENDERFULLCONTENT */))
		BitBlt(hMem, 0, 0, w, h, hScreen, pt.x, pt.y, SRCCOPY);

	stride  = ((w * 3 + 3) / 4) * 4;
	imgsize = stride * h;
	bits    = (BYTE *) malloc(imgsize);

	memset(&ih, 0, sizeof(ih));
	ih.biSize        = sizeof(ih);
	ih.biWidth       = w;
	ih.biHeight      = h;
	ih.biPlanes      = 1;
	ih.biBitCount    = 24;
	ih.biCompression = BI_RGB;

	GetDIBits(hMem, hBmp, 0, h, bits, (BITMAPINFO *) &ih, DIB_RGB_COLORS);

	memset(&fh, 0, sizeof(fh));
	fh.bfType    = 0x4D42;
	fh.bfOffBits = sizeof(fh) + sizeof(ih);
	fh.bfSize    = fh.bfOffBits + imgsize;

	f = fopen(path, "wb");
	if (f)
	{
		fwrite(&fh, sizeof(fh), 1, f);
		fwrite(&ih, sizeof(ih), 1, f);
		fwrite(bits, imgsize, 1, f);
		fclose(f);
		lg("captured %dx%d to %s", w, h, path);
	}

	free(bits);
	SelectObject(hMem, hOld);
	DeleteObject(hBmp);
	DeleteDC(hMem);
	ReleaseDC(NULL, hScreen);
}

static BOOL CALLBACK enum_child(HWND h, LPARAM lp)
{
	char cls[128];
	RECT rc;

	GetClassName(h, cls, sizeof(cls));
	GetClientRect(h, &rc);
	lg("  child 0x%p class=%s size=%dx%d", (void *) h, cls, rc.right, rc.bottom);
	*(HWND *) lp = h;
	return TRUE;
}

static void click_child(HWND hParent, int x, int y)
{
	HWND hTarget = NULL;

	EnumChildWindows(hParent, enum_child, (LPARAM) &hTarget);
	if (hTarget == NULL) { lg("click: no child window"); return; }

	PostMessage(hTarget, WM_MOUSEMOVE,  0,          MAKELPARAM(x, y));
	PostMessage(hTarget, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
	PostMessage(hTarget, WM_LBUTTONUP,   0,          MAKELPARAM(x, y));
	lg("clicked 0x%p at %d,%d", (void *) hTarget, x, y);
}

static void real_click(HWND hFrame, int cx, int cy)
{
	POINT pt;
	INPUT in[2];

	SetForegroundWindow(hFrame);
	Sleep(150);

	pt.x = cx;
	pt.y = cy;
	ClientToScreen(hFrame, &pt);
	SetCursorPos(pt.x, pt.y);
	Sleep(80);

	memset(in, 0, sizeof(in));
	in[0].type = INPUT_MOUSE; in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
	in[1].type = INPUT_MOUSE; in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
	SendInput(2, in, sizeof(INPUT));
	lg("real click at client %d,%d (screen %ld,%ld)", cx, cy, pt.x, pt.y);
}

int main(int argc, char **argv)
{
	const wchar_t *movie = (argc > 1) ? NULL : L"C:\\Program Files (x86)\\BioExplorer\\Flash\\simpleAdapterLoader.swf";
	wchar_t wmovie[MAX_PATH];
	HWND hFrame, hHost;
	IUnknown  *pContainer = NULL, *pControl = NULL;
	IDispatch *pDisp = NULL;
	HRESULT hr;
	int i;

	g_log = fopen("flash_probe.log", "w");
	if (g_log == NULL) return 2;

	SetUnhandledExceptionFilter(probe_filter);

	if (argc > 1) MultiByteToWideChar(CP_ACP, 0, argv[1], -1, wmovie, MAX_PATH);
	else          wcscpy(wmovie, movie);

	lg("probe start, movie = %ls", wmovie);

	hr = OleInitialize(NULL);
	lg("OleInitialize -> 0x%08x", (unsigned int) hr);

	lg("_pAtlModule before = 0x%p", (void *) ATL::_pAtlModule);
	if (ATL::_pAtlModule == NULL)
	{
		// a plain Win32 application has no ATL module - ATL's host object crashes
		// in Lock()/Unlock() without one, so create (and intentionally leak) one
		static CProbeAtlModule *module = new CProbeAtlModule();
		lg("created ATL module 0x%p", (void *) module);
	}
	lg("_pAtlModule now = 0x%p", (void *) ATL::_pAtlModule);

	BOOL ax = AtlAxWinInit();
	lg("AtlAxWinInit -> %d, _pAtlModule now = 0x%p", ax, (void *) ATL::_pAtlModule);

	hFrame = CreateWindow("STATIC", "probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
	                      50, 50, 700, 520, NULL, NULL, NULL, NULL);
	lg("frame window = 0x%p", (void *) hFrame);
	if (hFrame == NULL) { lg("no frame -> abort"); fclose(g_log); return 3; }

	ShowWindow(hFrame, SW_SHOW);
	SetWindowPos(hFrame, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
	SetForegroundWindow(hFrame);

	hHost = CreateWindow("STATIC", NULL, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
	                     0, 0, 680, 460, hFrame, NULL, NULL, NULL);
	lg("host window = 0x%p", (void *) hHost);
	if (hHost == NULL) { lg("no host -> abort"); fclose(g_log); return 3; }

	hr = AtlAxCreateControlEx(L"ShockwaveFlash.ShockwaveFlash", hHost, NULL,
	                          &pContainer, &pControl, IID_NULL, NULL);
	lg("AtlAxCreateControlEx -> 0x%08x container=0x%p control=0x%p",
	   (unsigned int) hr, (void *) pContainer, (void *) pControl);
	if (pControl == NULL) { lg("no control -> abort"); fclose(g_log); return 4; }

	lg("QueryInterface(IDispatch) -> 0x%08x",
	   (unsigned int) pControl->QueryInterface(IID_IDispatch, (void **) &pDisp));
	if (pDisp == NULL) { lg("no dispatch -> abort"); fclose(g_log); return 5; }

	put_str (pDisp, L"BGColor",   L"#000000");
	put_long(pDisp, L"Quality",   4);      // 4 = High
	put_long(pDisp, L"ScaleMode", 0);      // 0 = ShowAll
	put_bool(pDisp, L"Loop",      FALSE);
	put_bool(pDisp, L"Menu",      FALSE);

	put_str (pDisp, L"Movie", wmovie);
	put_bool(pDisp, L"Playing", TRUE);

	lg("pumping messages for 3000 ms ...");
	pump(3000);
	lg("pump done");

	save_client_bmp(hFrame, "shot_base.bmp");

	// the In1In2InputModel pauses the movie when the input does not change
	// (MAX_UNCHANGED_COUNT_BEFORE_PAUSE) - feed a changing value and capture the
	// sequence; the credits roll of the intro is long, so run about 110 seconds
	for (i = 0; i < 1800; i++)
	{
		set_var(pDisp, L"in1", (i & 1) ? L"1.0000" : L"0.0000");
		if (i & 2) set_var(pDisp, L"in2", L"0.0000");
		else       set_var(pDisp, L"in2", L"1.0000");
		pump(60);

		if (i == 500)  save_client_bmp(hFrame, "shot_t030.bmp");
		if (i == 900)  save_client_bmp(hFrame, "shot_t054.bmp");
		if (i == 1300) save_client_bmp(hFrame, "shot_t078.bmp");
		if (i == 1700) save_client_bmp(hFrame, "shot_t102.bmp");
	}
	save_client_bmp(hFrame, "shot_t108.bmp");

	get_long(pDisp, L"TotalFrames");
	get_long(pDisp, L"PercentLoaded");
	get_long(pDisp, L"ReadyState");

	// drive one variable at a time and capture the screen after each step
	set_var(pDisp, L"in1", L"1.0000");
	pump(1400);
	save_client_bmp(hFrame, "shot_in1.bmp");

	set_var(pDisp, L"in1", L"0.0000");
	set_var(pDisp, L"in2", L"1.0000");
	pump(1400);
	save_client_bmp(hFrame, "shot_in2.bmp");

	set_var(pDisp, L"in2", L"0.0000");
	set_var(pDisp, L"in3", L"1.0000");
	pump(1400);
	save_client_bmp(hFrame, "shot_in3.bmp");

	set_var(pDisp, L"in3", L"0.0000");

	// read the variables back - this proves the movie received them
	get_var(pDisp, L"in1");
	get_var(pDisp, L"in2");
	get_var(pDisp, L"in3");

	lg("done - no crash");
	fclose(g_log);

	pDisp->Release();
	if (pControl) pControl->Release();
	if (pContainer) pContainer->Release();
	DestroyWindow(hFrame);
	OleUninitialize();
	return 0;
}
