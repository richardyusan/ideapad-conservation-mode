/*
 * Conservation Mode for Windows (Lenovo IdeaPad)
 *
 * Single self-contained .exe: Win32 + GDI+ only (both part of Windows),
 * statically linked, nothing to install, no admin rights needed.
 * Talks to the Lenovo "ACPI-Compliant Virtual Power Controller" driver
 * (\\.\EnergyDrv) exactly like Lenovo Vantage. The layout matches the
 * Linux build.
 *
 *   ConservationMode.exe                 open the app
 *   ConservationMode.exe status|on|off   command line
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <setupapi.h>
#include <batclass.h>
#include <gdiplus.h>
#include <stdio.h>
#include <wchar.h>

#define APP_VERSION L"1.2.0"

#define IOCTL_ENERGY_BATTERY_CHARGE_MODE 0x831020F8
#define CMD_READ  0xFF
#define CMD_ON    0x03
#define CMD_OFF   0x05
#define BIT_CONSERVATION 0x20

#define ID_SWITCH 101
#define ID_TIMER  1

/* Battery device class {72631e54-78a4-11d0-bcf7-00aa00b7b32a} */
static const GUID BATTERY_CLASS_GUID =
    { 0x72631e54, 0x78a4, 0x11d0, { 0xbc, 0xf7, 0x00, 0xaa, 0x00, 0xb7, 0xb3, 0x2a } };

/* ------------------------------------------------------------- backend -- */

static BOOL energy_call(DWORD in, DWORD *out, wchar_t *err, size_t errlen)
{
    HANDLE h = CreateFileW(L"\\\\.\\EnergyDrv", GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        swprintf(err, errlen,
                 L"Lenovo driver (EnergyDrv) not found, error %lu. "
                 L"Install Lenovo Vantage or Lenovo Energy Management.",
                 GetLastError());
        return FALSE;
    }
    DWORD o = 0, ret = 0;
    BOOL ok = DeviceIoControl(h, IOCTL_ENERGY_BATTERY_CHARGE_MODE, &in, sizeof in,
                              &o, sizeof o, &ret, NULL);
    if (!ok)
        swprintf(err, errlen, L"The Lenovo driver rejected the command, error %lu.", GetLastError());
    CloseHandle(h);
    if (out) *out = o;
    return ok;
}

/* 1 = on, 0 = off, -1 = error */
static int read_state(wchar_t *err, size_t errlen)
{
    DWORD v;
    if (!energy_call(CMD_READ, &v, err, errlen))
        return -1;
    return (v & BIT_CONSERVATION) ? 1 : 0;
}

static BOOL set_state(BOOL on, wchar_t *err, size_t errlen)
{
    return energy_call(on ? CMD_ON : CMD_OFF, NULL, err, errlen);
}

/* ------------------------------------------------------------- battery -- */

typedef struct {
    int present, level, health, cycles, plugged;
    const wchar_t *status;
} Battery;

static void read_battery(Battery *b)
{
    ZeroMemory(b, sizeof *b);
    b->level = b->health = b->cycles = -1;
    b->status = L"Unknown";

    SYSTEM_POWER_STATUS ps;
    if (GetSystemPowerStatus(&ps)) {
        b->plugged = ps.ACLineStatus == 1;
        if (ps.BatteryLifePercent <= 100 && !(ps.BatteryFlag & 128)) {
            b->present = 1;
            b->level = ps.BatteryLifePercent;
        }
    }
    if (!b->present)
        return;

    HDEVINFO dev = SetupDiGetClassDevsW(&BATTERY_CLASS_GUID, NULL, NULL,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (dev == INVALID_HANDLE_VALUE)
        return;
    SP_DEVICE_INTERFACE_DATA did;
    ZeroMemory(&did, sizeof did);
    did.cbSize = sizeof did;
    if (SetupDiEnumDeviceInterfaces(dev, NULL, &BATTERY_CLASS_GUID, 0, &did)) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(dev, &did, NULL, 0, &need, NULL);
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail = LocalAlloc(LPTR, need);
        if (detail) {
            detail->cbSize = sizeof *detail;
            if (SetupDiGetDeviceInterfaceDetailW(dev, &did, detail, need, NULL, NULL)) {
                HANDLE h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
                if (h != INVALID_HANDLE_VALUE) {
                    DWORD wait = 0, ret;
                    BATTERY_QUERY_INFORMATION q = { 0 };
                    if (DeviceIoControl(h, IOCTL_BATTERY_QUERY_TAG, &wait, sizeof wait,
                                        &q.BatteryTag, sizeof q.BatteryTag, &ret, NULL) &&
                        q.BatteryTag) {
                        BATTERY_INFORMATION bi = { 0 };
                        q.InformationLevel = BatteryInformation;
                        if (DeviceIoControl(h, IOCTL_BATTERY_QUERY_INFORMATION, &q, sizeof q,
                                            &bi, sizeof bi, &ret, NULL)) {
                            if (bi.DesignedCapacity > 0 && bi.FullChargedCapacity > 0)
                                b->health = MulDiv(100, bi.FullChargedCapacity, bi.DesignedCapacity);
                            if (bi.CycleCount > 0)
                                b->cycles = bi.CycleCount;
                        }
                        BATTERY_WAIT_STATUS ws = { 0 };
                        BATTERY_STATUS bs = { 0 };
                        ws.BatteryTag = q.BatteryTag;
                        if (DeviceIoControl(h, IOCTL_BATTERY_QUERY_STATUS, &ws, sizeof ws,
                                            &bs, sizeof bs, &ret, NULL)) {
                            if (bs.PowerState & BATTERY_POWER_ON_LINE) b->plugged = 1;
                            if (bs.PowerState & BATTERY_CHARGING)          b->status = L"Charging";
                            else if (bs.PowerState & BATTERY_DISCHARGING)  b->status = L"Discharging";
                        }
                    }
                    CloseHandle(h);
                }
            }
            LocalFree(detail);
        }
    }
    SetupDiDestroyDeviceInfoList(dev);

    if (!wcscmp(b->status, L"Unknown"))
        b->status = !b->plugged ? L"Discharging" : b->level >= 100 ? L"Full" : L"Not charging";
}

/* ----------------------------------------------------------------- CLI -- */

static int run_cli(const wchar_t *arg)
{
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }
    wchar_t err[512];
    if (!_wcsicmp(arg, L"--version")) {
        wprintf(L"\nConservationMode %ls\n", APP_VERSION);
        return 0;
    }
    if (!_wcsicmp(arg, L"on") || !_wcsicmp(arg, L"off")) {
        if (!set_state(!_wcsicmp(arg, L"on"), err, 512)) {
            fwprintf(stderr, L"\nError: %ls\n", err);
            return 1;
        }
        Sleep(300);
    } else if (_wcsicmp(arg, L"status")) {
        fwprintf(stderr, L"\nUsage: ConservationMode.exe [status|on|off|--version]\n");
        return 2;
    }
    int s = read_state(err, 512);
    if (s < 0) {
        fwprintf(stderr, L"\nError: %ls\n", err);
        return 1;
    }
    wprintf(L"\nConservation Mode: %ls\n", s ? L"ON" : L"OFF");
    return 0;
}

/* ----------------------------------------------------------------- GUI -- */

/* Colors (Adwaita, same as the GTK build) */
#define C_WINDOW  RGB(0xf6, 0xf5, 0xf4)
#define C_CARD    RGB(0xff, 0xff, 0xff)
#define C_BORDER  RGB(0xd6, 0xd1, 0xcd)
#define C_TEXT    RGB(0x2e, 0x34, 0x36)
#define C_DIM     RGB(0x77, 0x76, 0x7b)
#define C_ERROR   RGB(0xc0, 0x1c, 0x28)
#define ARGB(c) (0xff000000u | (GetRValue(c) << 16) | (GetGValue(c) << 8) | GetBValue(c))
#define A_GREEN   0xff2ec27eu
#define A_GREEN_D 0xff26a269u
#define A_BLUE    0xff3584e4u
#define A_OFF     0xffe1dedbu
#define A_OFF_B   0xffcdc7c2u

/* Layout in 96-DPI units (scaled with S()) */
#define CW 420
#define CH 452
#define M  22

typedef struct {
    HWND hwnd, sw;
    HFONT font, bold, title;
    int dpi, mode, syncing;
    Battery bat;
    wchar_t msg[256];
    int msg_error;
} App;

static App A = { .mode = -1 };

static int S(int v) { return MulDiv(v, A.dpi, 96); }

static GpPath *round_path(float x, float y, float w, float h, float r)
{
    GpPath *p;
    float d;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    d = 2 * r;
    GdipCreatePath(FillModeAlternate, &p);
    GdipAddPathArc(p, x + w - d, y, d, d, 270, 90);
    GdipAddPathArc(p, x + w - d, y + h - d, d, d, 0, 90);
    GdipAddPathArc(p, x, y + h - d, d, d, 90, 90);
    GdipAddPathArc(p, x, y, d, d, 180, 90);
    GdipClosePathFigure(p);
    return p;
}

static void fill_round(GpGraphics *g, ARGB color, float x, float y, float w, float h, float r)
{
    GpSolidFill *br;
    GpPath *p = round_path(x, y, w, h, r);
    GdipCreateSolidFill(color, &br);
    GdipFillPath(g, (GpBrush *)br, p);
    GdipDeleteBrush((GpBrush *)br);
    GdipDeletePath(p);
}

/* Battery picture: same geometry as the Linux build (1024-unit design). */
static void draw_battery(GpGraphics *g, float ox, float oy, float size)
{
    float s = size / 1024.0f;
#define X(v) (ox + (v) * s)
#define Y(v) (oy + (v) * s)
#define L(v) ((v) * s)
    ARGB fg = ARGB(C_TEXT);
    fill_round(g, fg, X(432), Y(90), L(160), L(100), L(40));

    GpPath *outer = round_path(X(262), Y(170), L(500), L(760), L(90));
    GpPath *inner = round_path(X(302), Y(210), L(420), L(680), L(60));
    GdipAddPathPath(outer, inner, FALSE);
    GpSolidFill *br;
    GdipCreateSolidFill(fg, &br);
    GdipFillPath(g, (GpBrush *)br, outer);
    GdipDeleteBrush((GpBrush *)br);
    GdipDeletePath(outer);
    GdipDeletePath(inner);

    int lvl = A.bat.level < 0 ? 0 : A.bat.level > 100 ? 100 : A.bat.level;
    float top = 860 - 620.0f * lvl / 100.0f;
    if (lvl > 0)
        fill_round(g, A.mode == 1 ? A_GREEN : A_BLUE, X(332), Y(top), L(360), L(860 - top), L(40));
    if (A.mode == 1)
        fill_round(g, A_GREEN_D, X(322), Y(860 - 620 * 0.6f - 30), L(380), L(20), L(10));
#undef X
#undef Y
#undef L
}

static void text(HDC dc, HFONT f, COLORREF c, const wchar_t *s, int x, int y, int w, int h, UINT fmt)
{
    RECT r = { S(x), S(y), S(x + w), S(y + h) };
    SelectObject(dc, f);
    SetTextColor(dc, c);
    DrawTextW(dc, s, -1, &r, fmt | DT_NOPREFIX);
}

static void paint(HDC dc)
{
    RECT rc;
    GetClientRect(A.hwnd, &rc);
    HBRUSH bg = CreateSolidBrush(C_WINDOW);
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);

    GpGraphics *g;
    GdipCreateFromHDC(dc, &g);
    GdipSetSmoothingMode(g, SmoothingModeAntiAlias);

    /* hero */
    draw_battery(g, (float)S((CW - 72) / 2), (float)S(M), (float)S(72));
    text(dc, A.title, C_TEXT,
         A.mode < 0 ? L"Not available" : A.mode ? L"Conservation Mode is ON" : L"Conservation Mode is OFF",
         M, 104, CW - 2 * M, 32, DT_CENTER | DT_SINGLELINE);
    text(dc, A.font, C_DIM,
         L"When ON, the battery stops charging at about 60%\n"
         L"to extend its lifespan. Same setting as Lenovo Vantage.",
         M, 142, CW - 2 * M, 44, DT_CENTER);

    /* card */
    fill_round(g, ARGB(C_BORDER), (float)S(M), (float)S(204), (float)S(CW - 2 * M), (float)S(68), (float)S(12));
    fill_round(g, ARGB(C_CARD), (float)S(M) + 1, (float)S(204) + 1,
               (float)S(CW - 2 * M) - 2, (float)S(68) - 2, (float)S(12) - 1);
    GdipDeleteGraphics(g);

    text(dc, A.bold, C_TEXT, L"Conservation Mode", M + 16, 216, 260, 22, DT_SINGLELINE);
    text(dc, A.font, C_DIM,
         A.mode < 0 ? L"Lenovo driver not found" : A.mode ? L"Battery is held at about 60%"
                                                          : L"Battery charges to 100%",
         M + 16, 239, 260, 22, DT_SINGLELINE);

    /* battery details */
    text(dc, A.bold, C_TEXT, L"Battery", M, 292, 200, 22, DT_SINGLELINE);
    wchar_t level[32], state[96], health[96];
    if (A.bat.present) {
        swprintf(level, 32, L"%d%%", A.bat.level);
        swprintf(state, 96, L"%ls%ls", A.bat.status,
                 !A.bat.plugged ? L""
                 : !wcscmp(A.bat.status, L"Not charging") ? L" (plugged in, holding level)"
                                                          : L" (plugged in)");
        if (A.bat.health < 0)
            wcscpy(health, L"Unknown");
        else if (A.bat.cycles > 0)
            swprintf(health, 96, L"%d%% of original capacity \x00B7 %d cycles", A.bat.health, A.bat.cycles);
        else
            swprintf(health, 96, L"%d%% of original capacity", A.bat.health);
    } else {
        wcscpy(level, L"No battery found");
        wcscpy(state, L"-");
        wcscpy(health, L"-");
    }
    const wchar_t *keys[] = { L"Level", L"Status", L"Health" };
    const wchar_t *vals[] = { level, state, health };
    for (int i = 0; i < 3; i++) {
        text(dc, A.font, C_DIM, keys[i], M, 322 + i * 26, 70, 22, DT_SINGLELINE);
        text(dc, A.font, C_TEXT, vals[i], M + 78, 322 + i * 26, CW - 2 * M - 78, 22, DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    if (A.msg[0])
        text(dc, A.font, A.msg_error ? C_ERROR : C_TEXT, A.msg, M, 404, CW - 2 * M, 44,
             DT_WORDBREAK | DT_END_ELLIPSIS);
}

static void draw_switch(DRAWITEMSTRUCT *di)
{
    HDC dc = di->hDC;
    RECT r = di->rcItem;
    HBRUSH bg = CreateSolidBrush(C_CARD);
    FillRect(dc, &r, bg);
    DeleteObject(bg);

    GpGraphics *g;
    GdipCreateFromHDC(dc, &g);
    GdipSetSmoothingMode(g, SmoothingModeAntiAlias);

    float w = (float)(r.right - r.left), h = (float)(r.bottom - r.top);
    float tw = (float)S(48), th = (float)S(26);
    float x = (w - tw) / 2, y = (h - th) / 2;
    int on = A.mode == 1;
    int enabled = A.mode >= 0;

    if (di->itemState & ODS_FOCUS)
        fill_round(g, 0x803584e4u, x - S(3), y - S(3), tw + S(6), th + S(6), (th + S(6)) / 2);
    if (on) {
        fill_round(g, enabled ? A_BLUE : 0x803584e4u, x, y, tw, th, th / 2);
    } else {
        fill_round(g, A_OFF_B, x, y, tw, th, th / 2);
        fill_round(g, A_OFF, x + 1, y + 1, tw - 2, th - 2, th / 2 - 1);
    }
    float k = th - S(4);
    float kx = on ? x + tw - S(2) - k : x + S(2);
    fill_round(g, 0x30000000u, kx, y + S(2) + 1, k, k, k / 2);  /* soft shadow */
    fill_round(g, enabled ? 0xffffffffu : 0xfff6f5f4u, kx, y + S(2), k, k, k / 2);
    GdipDeleteGraphics(g);
}

static void refresh(BOOL show_errors)
{
    wchar_t err[512] = L"";
    A.mode = read_state(err, 512);
    read_battery(&A.bat);
    EnableWindow(A.sw, A.mode >= 0);
    if (A.mode < 0 && show_errors) {
        wcsncpy(A.msg, err, 255);
        A.msg_error = 1;
    }
    InvalidateRect(A.hwnd, NULL, FALSE);
    InvalidateRect(A.sw, NULL, FALSE);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        A.hwnd = hwnd;
        A.sw = CreateWindowExW(0, L"BUTTON", L"Conservation Mode",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                               S(CW - M - 16 - 56), S(204 + 34 - 17), S(56), S(34),
                               hwnd, (HMENU)(INT_PTR)ID_SWITCH, GetModuleHandleW(NULL), NULL);
        SetTimer(hwnd, ID_TIMER, 5000, NULL);
        return 0;

    case WM_COMMAND:
        if (LOWORD(wp) == ID_SWITCH && HIWORD(wp) == BN_CLICKED && A.mode >= 0) {
            BOOL want = A.mode != 1;
            wchar_t err[512];
            if (set_state(want, err, 512)) {
                Sleep(300);
                refresh(FALSE);
                if (A.mode == (int)want) {
                    wcscpy(A.msg, want ? L"Conservation Mode turned ON." : L"Conservation Mode turned OFF.");
                    A.msg_error = 0;
                } else {
                    wcscpy(A.msg, L"The laptop did not accept the change.");
                    A.msg_error = 1;
                }
            } else {
                wcsncpy(A.msg, err, 255);
                A.msg_error = 1;
                refresh(FALSE);
            }
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_DRAWITEM:
        if (wp == ID_SWITCH) {
            draw_switch((DRAWITEMSTRUCT *)lp);
            return TRUE;
        }
        break;

    case WM_TIMER:
        refresh(FALSE);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        paint(mem);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, ID_TIMER);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HFONT make_font(int pt, int weight)
{
    return CreateFontW(-MulDiv(pt, A.dpi, 72), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH, L"Segoe UI");
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    (void)prev; (void)cmd;
    int argc;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1)
        return run_cli(argv[1]);

    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    GdiplusStartupInput gsi = { 1, NULL, FALSE, FALSE };
    ULONG_PTR token;
    GdiplusStartup(&token, &gsi, NULL);

    HDC sdc = GetDC(NULL);
    A.dpi = GetDeviceCaps(sdc, LOGPIXELSY);
    ReleaseDC(NULL, sdc);
    A.font  = make_font(10, FW_NORMAL);
    A.bold  = make_font(10, FW_BOLD);
    A.title = make_font(17, FW_BOLD);

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"ConservationModeWnd";
    RegisterClassW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r = { 0, 0, S(CW), S(CH) };
    AdjustWindowRect(&r, style, FALSE);
    int w = r.right - r.left, h = r.bottom - r.top;
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Conservation Mode", style,
                              (GetSystemMetrics(SM_CXSCREEN) - w) / 2,
                              (GetSystemMetrics(SM_CYSCREEN) - h) / 2,
                              w, h, NULL, NULL, inst, NULL);
    refresh(TRUE);
    ShowWindow(hwnd, show);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    GdiplusShutdown(token);
    return 0;
}
