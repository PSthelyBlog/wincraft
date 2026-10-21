/*
 * wcshell - the WinCraft OS desktop shell.
 *
 * A native Win32 program providing what explorer.exe provides on Windows:
 * the desktop (wallpaper and icons), the taskbar (Start button, running
 * windows, clock) and the Start menu (installed programs, search, power).
 *
 *   wcshell.exe             run the shell
 *   wcshell.exe /setup      create the default shortcuts, then exit (build time)
 *   wcshell.exe /download   "Download a Program": fetch an installer and run it
 *   wcshell.exe /install    "Install WinCraft OS": copy a live system to a disk
 *   wcshell.exe /features   "WinCraft Features": the features added to this computer
 *
 * The shell owns no application logic: programs are whatever the Start Menu
 * and Desktop folders contain, exactly as on Windows, so installers "just
 * work" - their shortcuts show up here.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define _WIN32_WINNT 0x0A00
#define COBJMACROS
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <commctrl.h>
#include <urlmon.h>
#include <stdio.h>
#include <wchar.h>

#define WC_VERSION      L"0.7.18"

/* ---- metrics ---- */
#define BAR_H           g_bar_h
#define START_W         56
#define TASK_MAX_W      210
#define TASK_MIN_W      48
#define CLOCK_W         92
#define PEEK_W          10
#define ICON_CELL_W     104
#define ICON_CELL_H     100
#define MENU_W          400
#define MENU_HEADER     64
#define MENU_FOOTER     58
#define MENU_ROW        44

/* ---- palette: dark, or light when Windows' "light system" setting is on ---- */
struct palette {
    COLORREF bar, bar_edge, hot, active, accent, text, dim, menu, menu_alt;
};
static const struct palette dark_palette = {
    RGB(22, 26, 38), RGB(48, 54, 74), RGB(44, 50, 70), RGB(58, 66, 92), RGB(74, 165, 240),
    RGB(236, 239, 244), RGB(150, 158, 176), RGB(28, 32, 46), RGB(36, 41, 58),
};
static const struct palette light_palette = {
    RGB(238, 240, 244), RGB(206, 211, 222), RGB(222, 226, 234), RGB(204, 211, 226), RGB(0, 103, 192),
    RGB(24, 28, 36), RGB(96, 104, 120), RGB(246, 247, 250), RGB(232, 235, 241),
};
static struct palette g_pal;
#define C_BAR           g_pal.bar
#define C_BAR_EDGE      g_pal.bar_edge
#define C_HOT           g_pal.hot
#define C_ACTIVE        g_pal.active
#define C_ACCENT        g_pal.accent
#define C_TEXT          g_pal.text
#define C_DIM           g_pal.dim
#define C_MENU          g_pal.menu
#define C_MENU_ALT      g_pal.menu_alt
#define C_WALL_A        RGB(11, 21, 48)
#define C_WALL_B        RGB(24, 86, 138)

/* ---- private messages / ids ---- */
#define WM_TOGGLE_START (WM_APP + 1)
#define WM_SHELL_CMD    (WM_APP + 2)
#define WM_WINDOWS_CHANGED (WM_APP + 3)
#define WM_PREFS_CHANGED (WM_APP + 4)
#define WM_SWITCHER     (WM_APP + 5)    /* wParam: one of SWITCH_* below */
enum { CMD_RUN = 1, CMD_FILES, CMD_SHOW_DESKTOP };
enum { TIMER_TASKS = 1, TIMER_CLOCK, TIMER_ICONS, TIMER_EVENT, TIMER_AUTOHIDE, TIMER_PREFS, TIMER_BALLOON };
enum { HK_RUN = 1, HK_FILES, HK_DESKTOP, HK_START };
enum { IDM_RESTORE = 100, IDM_MINIMIZE, IDM_MAXIMIZE, IDM_CLOSE,
       IDM_REFRESH, IDM_OPEN_DESKTOP, IDM_RUN, IDM_ABOUT, IDM_SETTINGS };

static HINSTANCE g_inst;
static HWND g_desktop, g_bar, g_menu;
static HFONT g_font, g_font_bold, g_font_small, g_font_large;
static int g_sw, g_sh;
static int g_bar_h = 44;     /* WINCRAFT_TASKBAR: must match the session's work area */
static BOOL g_bar_top;       /* the taskbar at the top (WinCraft\Shell: TaskbarPosition) */
/* the taskbar's place, and the part of the screen it leaves free */
#define BAR_Y           (g_bar_top ? 0 : g_sh - BAR_H)
#define AREA_TOP        (g_bar_top ? BAR_H : 0)
#define AREA_BOTTOM     (g_bar_top ? g_sh : g_sh - BAR_H)
#define BAR_INNER_Y     (g_bar_top ? BAR_H : g_sh - BAR_H)      /* where it meets the desktop */
#define BAR_POPUP_ALIGN (g_bar_top ? TPM_TOPALIGN : TPM_BOTTOMALIGN)
static HHOOK g_kbd_hook;
/* "Install WinCraft OS" is for a session started from the installation
   medium; on an installed system its shortcuts are not shown. */
static BOOL g_hide_installer;
#define INSTALLER_LINK  L"Install WinCraft OS.lnk"

static BOOL read_native_file(const WCHAR *unix_path, char *buf, DWORD size);

/* ======================================================================
 * Drawing helpers
 * ====================================================================== */

static void fill(HDC dc, const RECT *rc, COLORREF color)
{
    SetBkColor(dc, color);
    ExtTextOutW(dc, 0, 0, ETO_OPAQUE, rc, NULL, 0, NULL);
}

static void fill_xywh(HDC dc, int x, int y, int w, int h, COLORREF color)
{
    RECT rc = { x, y, x + w, y + h };
    fill(dc, &rc, color);
}

/* Blend a solid colour over a rectangle. */
static void blend(HDC dc, const RECT *rc, COLORREF color, BYTE alpha)
{
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, alpha, 0 };
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, 1, 1);
    HGDIOBJ old = SelectObject(mem, bmp);

    SetPixel(mem, 0, 0, color);
    AlphaBlend(dc, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top,
               mem, 0, 0, 1, 1, bf);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

static void text(HDC dc, const WCHAR *s, RECT *rc, UINT fmt, HFONT font, COLORREF color)
{
    HGDIOBJ old = SelectObject(dc, font);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, s, -1, rc, fmt | DT_NOPREFIX);
    SelectObject(dc, old);
}

static HFONT make_font(int pt, int weight)
{
    return CreateFontW(-MulDiv(pt, 96, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Liberation Sans");
}

/* The WinCraft logo: an isometric block, drawn as three faces. */
static void logo_faces(int cx, int cy, int r, POINT top[4], POINT left[4], POINT right[4])
{
    int dx = r * 866 / 1000, dy = r / 2;
    POINT n = { cx, cy - r }, ne = { cx + dx, cy - dy }, se = { cx + dx, cy + dy };
    POINT s = { cx, cy + r }, sw = { cx - dx, cy + dy }, nw = { cx - dx, cy - dy };
    POINT c = { cx, cy };

    top[0] = n;  top[1] = ne;  top[2] = c;    top[3] = nw;
    left[0] = nw; left[1] = c;  left[2] = s;   left[3] = sw;
    right[0] = c; right[1] = ne; right[2] = se; right[3] = s;
}

static void poly(HDC dc, const POINT *pts, COLORREF color)
{
    HBRUSH br = CreateSolidBrush(color);
    HGDIOBJ old_br = SelectObject(dc, br), old_pen = SelectObject(dc, GetStockObject(NULL_PEN));

    Polygon(dc, pts, 4);
    SelectObject(dc, old_br);
    SelectObject(dc, old_pen);
    DeleteObject(br);
}

/* Draw the logo antialiased (4x supersampled) on a known background. */
static void draw_logo(HDC dc, int x, int y, int size, COLORREF bg)
{
    int big = size * 4;
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, big, big);
    HGDIOBJ old = SelectObject(mem, bmp);
    RECT rc = { 0, 0, big, big };
    POINT top[4], left[4], right[4];

    fill(mem, &rc, bg);
    logo_faces(big / 2, big / 2, big / 2 - 2, top, left, right);
    poly(mem, top, RGB(143, 208, 255));
    poly(mem, left, RGB(74, 165, 240));
    poly(mem, right, RGB(43, 120, 197));
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, NULL);
    StretchBlt(dc, x, y, size, size, mem, 0, 0, big, big, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

/* A small arrow centred on (x, y): pointing right when closed, down when open. */
static void draw_chevron(HDC dc, int x, int y, BOOL open, COLORREF color)
{
    HPEN pen = CreatePen(PS_SOLID, 2, color);
    HGDIOBJ old = SelectObject(dc, pen);

    if (open) {
        MoveToEx(dc, x - 5, y - 2, NULL);
        LineTo(dc, x, y + 3);
        LineTo(dc, x + 6, y - 3);
    } else {
        MoveToEx(dc, x - 2, y - 5, NULL);
        LineTo(dc, x + 3, y);
        LineTo(dc, x - 3, y + 6);
    }
    SelectObject(dc, old);
    DeleteObject(pen);
}

static HICON extract_icon(const WCHAR *file, int index, int size)
{
    WCHAR path[MAX_PATH];
    HICON icon = NULL;
    UINT id;

    if (!ExpandEnvironmentStringsW(file, path, MAX_PATH))
        lstrcpynW(path, file, MAX_PATH);
    if (PrivateExtractIconsW(path, index, size, size, &icon, &id, 1, 0) >= 1 && icon)
        return icon;
    return NULL;
}

/*
 * Icon of a file at an exact pixel size. Shortcuts are resolved here rather
 * than by SHGetFileInfo so the result is the target's own artwork at the
 * requested size, without an overlay.
 */
static HICON file_icon(const WCHAR *path, int size)
{
    const WCHAR *ext = PathFindExtensionW(path);
    WCHAR sys[MAX_PATH];
    SHFILEINFOW sfi;
    HICON icon = NULL;

    if (!lstrcmpiW(ext, L".lnk")) {
        IShellLinkW *link;
        IPersistFile *pf;

        if (SUCCEEDED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                                       &IID_IShellLinkW, (void **)&link))) {
            if (SUCCEEDED(IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&pf))) {
                if (SUCCEEDED(IPersistFile_Load(pf, path, STGM_READ))) {
                    WCHAR loc[MAX_PATH] = L"";
                    int index = 0;

                    if (SUCCEEDED(IShellLinkW_GetIconLocation(link, loc, MAX_PATH, &index)) && loc[0])
                        icon = extract_icon(loc, index, size);
                    if (!icon && SUCCEEDED(IShellLinkW_GetPath(link, loc, MAX_PATH, NULL, 0)) &&
                        loc[0] && lstrcmpiW(PathFindExtensionW(loc), L".lnk"))
                        icon = file_icon(loc, size);
                }
                IPersistFile_Release(pf);
            }
            IShellLinkW_Release(link);
        }
        if (icon)
            return icon;
    } else if (!lstrcmpiW(ext, L".exe") || !lstrcmpiW(ext, L".ico") || !lstrcmpiW(ext, L".scr")) {
        icon = extract_icon(path, 0, size);
        if (icon)
            return icon;
    } else {
        ZeroMemory(&sfi, sizeof(sfi));
        SHGetFileInfoW(path, 0, &sfi, sizeof(sfi),
                       SHGFI_ICON | (size <= 16 ? SHGFI_SMALLICON : SHGFI_LARGEICON));
        if (sfi.hIcon)
            return sfi.hIcon;
    }
    /* generic program icon */
    GetSystemDirectoryW(sys, MAX_PATH);
    PathAppendW(sys, L"shell32.dll");
    return extract_icon(sys, -3, size);
}

/* ======================================================================
 * Actions
 * ====================================================================== */

static void open_path(const WCHAR *path, const WCHAR *args)
{
    WCHAR dir[MAX_PATH];
    HINSTANCE r;

    lstrcpynW(dir, path, MAX_PATH);
    PathRemoveFileSpecW(dir);
    r = ShellExecuteW(NULL, NULL, path, args, dir[0] ? dir : NULL, SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) {
        WCHAR msg[MAX_PATH + 64];

        wsprintfW(msg, L"Cannot open:\n%s", path);
        MessageBoxW(NULL, msg, L"WinCraft", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
    }
}

static void system_tool(const WCHAR *name, const WCHAR *args)
{
    WCHAR path[MAX_PATH];

    if (!lstrcmpiW(name, L"explorer.exe")) {
        GetWindowsDirectoryW(path, MAX_PATH);
    } else {
        GetSystemDirectoryW(path, MAX_PATH);
    }
    PathAppendW(path, name);
    open_path(path, args);
}

/*
 * shell32's standard Run dialog (exported by ordinal on every Windows).
 * A thread-local hook moves it above the Start button as it appears.
 */
static LRESULT CALLBACK run_dialog_hook(int code, WPARAM wp, LPARAM lp)
{
    static DWORD placed_tid;

    if (code == HCBT_ACTIVATE && placed_tid != GetCurrentThreadId()) {
        HWND hwnd = (HWND)wp;
        RECT rc;

        placed_tid = GetCurrentThreadId();
        if (GetWindowRect(hwnd, &rc))
            SetWindowPos(hwnd, NULL, 8, g_bar_top ? BAR_INNER_Y + 8 : BAR_INNER_Y - (rc.bottom - rc.top) - 8,
                         0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

static DWORD WINAPI run_dialog_thread(void *arg)
{
    typedef void (WINAPI *RunFileDlg_t)(HWND, HICON, LPCWSTR, LPCWSTR, LPCWSTR, DWORD);
    RunFileDlg_t run = (RunFileDlg_t)(void *)GetProcAddress(GetModuleHandleW(L"shell32.dll"),
                                                           MAKEINTRESOURCEA(61));
    HHOOK hook = SetWindowsHookExW(WH_CBT, run_dialog_hook, NULL, GetCurrentThreadId());

    if (run)
        run(NULL, LoadIconW(g_inst, MAKEINTRESOURCEW(1)), NULL, NULL, NULL, 0);
    if (hook)
        UnhookWindowsHookEx(hook);
    return 0;
}

static void run_dialog(void)
{
    static HANDLE thread;

    if (thread && WaitForSingleObject(thread, 0) == WAIT_TIMEOUT)
        return;     /* already open */
    if (thread)
        CloseHandle(thread);
    thread = CreateThread(NULL, 0, run_dialog_thread, NULL, 0, NULL);
}

/* Start a native (host) program; used for the few things only the OS can do. */
static BOOL run_native(const WCHAR *cmdline, BOOL wait)
{
    static const WCHAR *prefixes[] = { L"\\\\?\\unix", L"Z:" };
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    WCHAR buf[512];
    unsigned i;

    for (i = 0; i < ARRAYSIZE(prefixes); i++) {
        WCHAR *p;

        wsprintfW(buf, L"%s%s", prefixes[i], cmdline);
        for (p = buf; *p && *p != L' '; p++)
            if (*p == L'/')
                *p = L'\\';
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        if (CreateProcessW(NULL, buf, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            if (wait)
                WaitForSingleObject(pi.hProcess, 20000);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            return TRUE;
        }
    }
    return FALSE;
}

static DWORD WINAPI power_thread(void *arg)
{
    BOOL restart = (BOOL)(INT_PTR)arg;

    if (MessageBoxW(NULL,
                    restart ? L"Restart WinCraft OS now?\n\nOpen programs will be asked to close."
                            : L"Shut down WinCraft OS now?\n\nOpen programs will be asked to close.",
                    restart ? L"Restart" : L"Shut down",
                    MB_OKCANCEL | MB_ICONQUESTION | MB_SETFOREGROUND | MB_TOPMOST) != IDOK)
        return 0;
    /*
     * Ending the session closes every Windows program, this one included,
     * so the sequence is run by a native script that outlives us.
     */
    if (!run_native(restart ? L"/usr/lib/wincraft/end-session reboot"
                            : L"/usr/lib/wincraft/end-session poweroff", FALSE))
        MessageBoxW(NULL, L"The system service did not respond.", L"WinCraft",
                    MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    return 0;
}

static void power(BOOL restart)
{
    HANDLE th = CreateThread(NULL, 0, power_thread, (void *)(INT_PTR)restart, 0, NULL);

    if (th)
        CloseHandle(th);
}

static DWORD WINAPI about_thread(void *arg)
{
    typedef const char *(CDECL *wine_version_t)(void);
    typedef void (CDECL *host_version_t)(const char **, const char **);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    wine_version_t wine_version = (wine_version_t)(void *)GetProcAddress(ntdll, "wine_get_version");
    host_version_t host_version = (host_version_t)(void *)GetProcAddress(ntdll, "wine_get_host_version");
    const char *sys = "?", *rel = "?";
    WCHAR msg[512];

    if (host_version)
        host_version(&sys, &rel);
    wsprintfW(msg,
              L"WinCraft OS %s\n\n"
              L"A lightweight, open-source operating system that runs Windows programs.\n\n"
              L"Kernel:\t\t%S %S\n"
              L"Windows runtime:\tWine %S\n"
              L"Shell:\t\twcshell %s\n\n"
              L"WinCraft components are MIT licensed. Linux is GPL-2.0, Wine is LGPL-2.1.",
              WC_VERSION, sys, rel, wine_version ? wine_version() : "?", WC_VERSION);
    MessageBoxW(NULL, msg, L"About WinCraft OS", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
    return 0;
}

static void about(void)
{
    HANDLE th = CreateThread(NULL, 0, about_thread, NULL, 0, NULL);

    if (th)
        CloseHandle(th);
}

/*
 * Show a shell context menu. A menu only gets the keyboard, and only closes
 * on a click elsewhere, when its owner is the foreground window - so the
 * taskbar takes the foreground for the duration and hands it back after.
 */
static int popup_menu(HMENU menu, UINT flags, int x, int y)
{
    HWND prev = GetForegroundWindow();
    int cmd;

    SetForegroundWindow(g_bar);
    cmd = TrackPopupMenu(menu, TPM_RETURNCMD | flags, x, y, 0, g_bar, NULL);
    PostMessageW(g_bar, WM_NULL, 0, 0);
    if (prev && prev != g_bar && IsWindow(prev))
        SetForegroundWindow(prev);
    return cmd;
}

/* ======================================================================
 * Preferences: Windows' own settings where Windows has one, so that the
 * assistant, regedit and programs that change them (SystemParametersInfo)
 * all work; WinCraft's own values only where Windows keeps the setting in
 * a binary blob. Read at start and again whenever these keys change.
 * ====================================================================== */

#define KEY_DESKTOP     L"Control Panel\\Desktop"
#define KEY_ADVANCED    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced"
#define KEY_PERSONALIZE L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"
#define KEY_DWM         L"Software\\Microsoft\\Windows\\DWM"
#define KEY_WINCRAFT    L"Software\\WinCraft\\Shell"

static struct prefs {
    WCHAR wallpaper[MAX_PATH];  /* Control Panel\Desktop: Wallpaper (a picture) */
    int style;                  /* WallpaperStyle: 0 centre, 2 stretch, 6 fit, 10 fill, 22 span */
    BOOL tile;                  /* TileWallpaper */
    BOOL has_background;        /* Control Panel\Colors: Background, when not WinCraft's own */
    COLORREF background;
    BOOL light;                 /* Personalize: SystemUsesLightTheme */
    BOOL has_accent;            /* DWM: AccentColor */
    COLORREF accent;
    BOOL seconds;               /* Explorer\Advanced: ShowSecondsInSystemClock */
    BOOL single_click;          /* WinCraft\Shell: SingleClickOpen */
    BOOL autohide;              /* WinCraft\Shell: TaskbarAutoHide */
    BOOL bar_top;               /* WinCraft\Shell: TaskbarPosition 1 (Windows' number for the top edge) */
    BOOL focus_follows_mouse;   /* Control Panel\Mouse: ActiveWindowTracking */
    DWORD focus_delay;          /* Control Panel\Desktop: ActiveWndTrkTimeout, ms */
} g_prefs;

/* A number stored as a dword, or as text as Windows often does ("10"). */
static DWORD reg_number(const WCHAR *key, const WCHAR *name, DWORD def, BOOL *found)
{
    BYTE data[64];
    DWORD type, size = sizeof(data) - sizeof(WCHAR), value = def;
    HKEY k;

    if (found)
        *found = FALSE;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_READ, &k))
        return def;
    if (!RegQueryValueExW(k, name, NULL, &type, data, &size)) {
        if (type == REG_DWORD && size >= sizeof(DWORD)) {
            value = *(DWORD *)data;
            if (found) *found = TRUE;
        } else if (type == REG_SZ || type == REG_EXPAND_SZ) {
            ((WCHAR *)data)[size / sizeof(WCHAR)] = 0;
            if (((WCHAR *)data)[0]) {
                value = (DWORD)_wtoi((WCHAR *)data);
                if (found) *found = TRUE;
            }
        }
    }
    RegCloseKey(k);
    return value;
}

static void reg_text(const WCHAR *key, const WCHAR *name, WCHAR *out, DWORD chars)
{
    DWORD type, size = (chars - 1) * sizeof(WCHAR);
    HKEY k;

    out[0] = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_READ, &k))
        return;
    if (RegQueryValueExW(k, name, NULL, &type, (BYTE *)out, &size) ||
        (type != REG_SZ && type != REG_EXPAND_SZ))
        size = 0;
    out[size / sizeof(WCHAR)] = 0;
    RegCloseKey(k);
}

static void read_prefs(struct prefs *p)
{
    WCHAR buf[64];
    int r, g, b;
    BOOL found;
    DWORD accent;

    ZeroMemory(p, sizeof(*p));
    reg_text(KEY_DESKTOP, L"Wallpaper", p->wallpaper, ARRAYSIZE(p->wallpaper));
    p->style = (int)reg_number(KEY_DESKTOP, L"WallpaperStyle", 10, NULL);
    p->tile = reg_number(KEY_DESKTOP, L"TileWallpaper", 0, NULL) != 0;
    reg_text(L"Control Panel\\Colors", L"Background", buf, ARRAYSIZE(buf));
    if (swscanf(buf, L"%d %d %d", &r, &g, &b) == 3 && RGB(r, g, b) != C_WALL_A) {
        p->has_background = TRUE;
        p->background = RGB(r, g, b);
    }
    p->light = reg_number(KEY_PERSONALIZE, L"SystemUsesLightTheme", 0, NULL) != 0;
    accent = reg_number(KEY_DWM, L"AccentColor", 0, &found);
    if (found) {
        p->has_accent = TRUE;
        p->accent = RGB(accent & 0xff, (accent >> 8) & 0xff, (accent >> 16) & 0xff);   /* 0xAABBGGRR */
    }
    p->seconds = reg_number(KEY_ADVANCED, L"ShowSecondsInSystemClock", 0, NULL) != 0;
    p->single_click = reg_number(KEY_WINCRAFT, L"SingleClickOpen", 0, NULL) != 0;
    p->autohide = reg_number(KEY_WINCRAFT, L"TaskbarAutoHide", 0, NULL) != 0;
    p->bar_top = reg_number(KEY_WINCRAFT, L"TaskbarPosition", 3, NULL) == 1;      /* 3: the bottom */
    p->focus_follows_mouse = reg_number(L"Control Panel\\Mouse", L"ActiveWindowTracking", 0, NULL) != 0;
    p->focus_delay = reg_number(KEY_DESKTOP, L"ActiveWndTrkTimeout", 0, NULL);
}

static void place_bar(BOOL hidden);
static void autohide_later(void);

static void set_palette(void)
{
    g_pal = g_prefs.light ? light_palette : dark_palette;
    if (g_prefs.has_accent)
        g_pal.accent = g_prefs.accent;
}

/* What the window manager (wcwm, a Linux program) needs of these, as a small
 * file in the session's runtime folder, which it reads when it changes. */
static void write_wm_prefs(void)
{
    WCHAR dir[MAX_PATH], path[MAX_PATH], tmp[MAX_PATH], *p;
    char text[160];
    HANDLE h;
    DWORD done;
    int n;

    /* Wine passes XDG_* variables on to Windows programs renamed WINE_HOST_XDG_* */
    if ((!GetEnvironmentVariableW(L"WINE_HOST_XDG_RUNTIME_DIR", dir, ARRAYSIZE(dir)) &&
         !GetEnvironmentVariableW(L"XDG_RUNTIME_DIR", dir, ARRAYSIZE(dir))) || dir[0] != '/')
        return;
    for (p = dir; *p; p++)
        if (*p == '/') *p = '\\';
    _snwprintf(path, ARRAYSIZE(path), L"\\\\?\\unix%ls\\wcwm.conf", dir);
    _snwprintf(tmp, ARRAYSIZE(tmp), L"\\\\?\\unix%ls\\wcwm.conf.new", dir);
    path[ARRAYSIZE(path) - 1] = tmp[ARRAYSIZE(tmp) - 1] = 0;
    n = snprintf(text, sizeof(text),
                 "taskbar=%d\nposition=%s\nautohide=%d\nfocus_follows_mouse=%d\nfocus_delay=%lu\n",
                 g_bar_h, g_bar_top ? "top" : "bottom", g_prefs.autohide, g_prefs.focus_follows_mouse,
                 (unsigned long)g_prefs.focus_delay);
    h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    WriteFile(h, text, (DWORD)n, &done, NULL);
    CloseHandle(h);
    MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING);
}

/* Wake the taskbar when one of the keys the preferences come from changes.
 * The assistant writes the registry without telling anyone, so watching the
 * keys matters as much as WM_SETTINGCHANGE does. */
static DWORD WINAPI watch_prefs(void *arg)
{
    static const WCHAR *keys[] = {
        L"Control Panel", L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
        KEY_PERSONALIZE, KEY_DWM, KEY_WINCRAFT,
    };
    HANDLE events[ARRAYSIZE(keys)];
    HKEY handles[ARRAYSIZE(keys)];
    unsigned i, n = 0;

    for (i = 0; i < ARRAYSIZE(keys); i++) {
        if (RegCreateKeyExW(HKEY_CURRENT_USER, keys[i], 0, NULL, 0, KEY_NOTIFY, NULL, &handles[n], NULL))
            continue;
        events[n] = CreateEventW(NULL, FALSE, FALSE, NULL);
        RegNotifyChangeKeyValue(handles[n], TRUE, REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET,
                                events[n], TRUE);
        n++;
    }
    while (n) {
        DWORD w = WaitForMultipleObjects(n, events, FALSE, INFINITE);

        if (w >= WAIT_OBJECT_0 + n)
            break;
        i = w - WAIT_OBJECT_0;
        RegNotifyChangeKeyValue(handles[i], TRUE, REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET,
                                events[i], TRUE);
        if (g_bar)
            PostMessageW(g_bar, WM_PREFS_CHANGED, 0, 0);
    }
    return 0;
}

/* ---- pictures, through Windows' GDI+ (gdiplus.dll), declared here for C ---- */
typedef struct { UINT32 version; void *debug_callback; BOOL no_thread; BOOL no_codecs; } gdip_input;
int WINAPI GdiplusStartup(ULONG_PTR *token, const gdip_input *input, void *output);
int WINAPI GdipLoadImageFromFile(const WCHAR *file, void **image);
int WINAPI GdipGetImageWidth(void *image, UINT *width);
int WINAPI GdipGetImageHeight(void *image, UINT *height);
int WINAPI GdipCreateFromHDC(HDC dc, void **graphics);
int WINAPI GdipSetInterpolationMode(void *graphics, int mode);
int WINAPI GdipDrawImageRectI(void *graphics, void *image, INT x, INT y, INT width, INT height);
int WINAPI GdipDeleteGraphics(void *graphics);
int WINAPI GdipDisposeImage(void *image);

/* Draw the picture at path over the whole screen the way Windows' styles
 * do; FALSE when it cannot be read. A Linux path ("/home/...") works too. */
static BOOL draw_picture(HDC dc, const WCHAR *path, int style, BOOL tile)
{
    static ULONG_PTR token;
    WCHAR file[MAX_PATH + 16];
    void *image = NULL, *graphics = NULL;
    UINT iw = 0, ih = 0;
    int x, y, w, h;
    BOOL ok = FALSE;

    if (!token) {
        gdip_input input = { 1, NULL, FALSE, FALSE };

        if (GdiplusStartup(&token, &input, NULL))
            return FALSE;
    }
    if (path[0] == '/') {
        WCHAR *p;

        _snwprintf(file, ARRAYSIZE(file), L"\\\\?\\unix%ls", path);
        file[ARRAYSIZE(file) - 1] = 0;
        for (p = file + 8; *p; p++)
            if (*p == '/') *p = '\\';
    } else {
        ExpandEnvironmentStringsW(path, file, ARRAYSIZE(file));
    }
    if (GdipLoadImageFromFile(file, &image) || !image)
        return FALSE;
    GdipGetImageWidth(image, &iw);
    GdipGetImageHeight(image, &ih);
    if (iw && ih && !GdipCreateFromHDC(dc, &graphics)) {
        GdipSetInterpolationMode(graphics, 7);          /* high-quality bicubic */
        if (tile && style == 0) {
            for (y = 0; y < g_sh; y += ih)
                for (x = 0; x < g_sw; x += iw)
                    GdipDrawImageRectI(graphics, image, x, y, iw, ih);
        } else {
            if (style == 2) {                           /* stretch */
                w = g_sw; h = g_sh;
            } else if (style == 0) {                    /* centre, at its own size */
                w = iw; h = ih;
            } else {                                    /* 6 fit: all of it; 10, 22: fill the screen */
                BOOL wider = (double)iw / ih > (double)g_sw / g_sh;
                BOOL by_width = style == 6 ? wider : !wider;

                w = by_width ? g_sw : (int)((double)iw * g_sh / ih);
                h = by_width ? (int)((double)ih * g_sw / iw) : g_sh;
            }
            GdipDrawImageRectI(graphics, image, (g_sw - w) / 2, (g_sh - h) / 2, w, h);
        }
        GdipDeleteGraphics(graphics);
        ok = TRUE;
    }
    GdipDisposeImage(image);
    return ok;
}

/* ======================================================================
 * Desktop: wallpaper and icons
 * ====================================================================== */

struct icon_item {
    WCHAR path[MAX_PATH];
    WCHAR name[128];
    HICON icon;
    RECT rc;
};

static void hide_menu(void);
static void layout_tasks(void);
static void tray_tips(void);

static struct icon_item *g_icons;
static int g_nicons, g_icon_sel = -1, g_icon_pressed = -1;
static HBITMAP g_wallpaper;
static DWORD g_icons_stamp;

static void make_wallpaper(void)
{
    HDC screen = GetDC(NULL), dc = CreateCompatibleDC(screen);
    TRIVERTEX v[4];
    GRADIENT_TRIANGLE tri[2] = { { 0, 1, 2 }, { 1, 2, 3 } };
    COLORREF mid = RGB((GetRValue(C_WALL_A) + GetRValue(C_WALL_B)) / 2,
                       (GetGValue(C_WALL_A) + GetGValue(C_WALL_B)) / 2,
                       (GetBValue(C_WALL_A) + GetBValue(C_WALL_B)) / 2);
    COLORREF corner[4] = { C_WALL_A, mid, mid, C_WALL_B };
    POINT top[4], left[4], right[4];
    const POINT *faces[3] = { top, left, right };
    BYTE alpha[3] = { 30, 18, 9 };
    HGDIOBJ old;
    RECT rc;
    int i, r;

    if (g_wallpaper)
        DeleteObject(g_wallpaper);
    g_wallpaper = CreateCompatibleBitmap(screen, g_sw, g_sh);
    old = SelectObject(dc, g_wallpaper);

    /* Windows' own background settings: a picture, or a plain colour. */
    if (g_prefs.wallpaper[0] || g_prefs.has_background) {
        SetRect(&rc, 0, 0, g_sw, g_sh);
        fill(dc, &rc, g_prefs.has_background ? g_prefs.background : C_WALL_A);
        if (!g_prefs.wallpaper[0] || draw_picture(dc, g_prefs.wallpaper, g_prefs.style, g_prefs.tile)) {
            SelectObject(dc, old);
            DeleteDC(dc);
            ReleaseDC(NULL, screen);
            return;
        }
        /* a picture that cannot be read: WinCraft's own background instead */
    }

    for (i = 0; i < 4; i++) {
        v[i].x = (i & 1) ? g_sw : 0;
        v[i].y = (i & 2) ? g_sh : 0;
        v[i].Red = GetRValue(corner[i]) << 8;
        v[i].Green = GetGValue(corner[i]) << 8;
        v[i].Blue = GetBValue(corner[i]) << 8;
        v[i].Alpha = 0;
    }
    GradientFill(dc, v, 4, tri, 2, GRADIENT_FILL_TRIANGLE);

    /* A large, faint logo on the right-hand side. */
    r = g_sh * 30 / 100;
    logo_faces(g_sw * 70 / 100, (AREA_TOP + AREA_BOTTOM) / 2, r, top, left, right);
    SetRect(&rc, 0, 0, g_sw, g_sh);
    for (i = 0; i < 3; i++) {
        HRGN rgn = CreatePolygonRgn(faces[i], 4, WINDING);

        SelectClipRgn(dc, rgn);
        blend(dc, &rc, RGB(255, 255, 255), alpha[i]);
        SelectClipRgn(dc, NULL);
        DeleteObject(rgn);
    }

    SetRect(&rc, 0, AREA_BOTTOM - 34, g_sw - 16, AREA_BOTTOM - 12);
    text(dc, L"WinCraft OS " WC_VERSION, &rc, DT_RIGHT | DT_SINGLELINE | DT_VCENTER,
         g_font_small, RGB(170, 196, 224));

    SelectObject(dc, old);
    DeleteDC(dc);
    ReleaseDC(NULL, screen);
}

static int __cdecl icon_cmp(const void *a, const void *b)
{
    return lstrcmpiW(((const struct icon_item *)a)->name, ((const struct icon_item *)b)->name);
}

/* A cheap signature of the Desktop folders, to notice changes. */
static DWORD scan_desktop(struct icon_item **out, int *count)
{
    static const int folders[] = { CSIDL_DESKTOPDIRECTORY, CSIDL_COMMON_DESKTOPDIRECTORY };
    struct icon_item *items = NULL;
    DWORD stamp = 0;
    int n = 0, cap = 0;
    unsigned f;

    for (f = 0; f < ARRAYSIZE(folders); f++) {
        WCHAR dir[MAX_PATH], pattern[MAX_PATH];
        WIN32_FIND_DATAW fd;
        HANDLE h;

        if (FAILED(SHGetFolderPathW(NULL, folders[f], NULL, SHGFP_TYPE_CURRENT, dir)))
            continue;
        PathCombineW(pattern, dir, L"*");
        h = FindFirstFileW(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        do {
            const WCHAR *p;

            if (fd.cFileName[0] == L'.' || !lstrcmpiW(fd.cFileName, L"desktop.ini") ||
                (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) ||
                (g_hide_installer && !lstrcmpiW(fd.cFileName, INSTALLER_LINK)))
                continue;
            for (p = fd.cFileName; *p; p++)
                stamp = stamp * 31 + *p;
            stamp += fd.ftLastWriteTime.dwLowDateTime;
            if (!out)
                continue;
            if (n == cap) {
                cap = cap ? cap * 2 : 32;
                items = HeapReAlloc(GetProcessHeap(), 0,
                                    items ? items : HeapAlloc(GetProcessHeap(), 0, 1),
                                    cap * sizeof(*items));
            }
            ZeroMemory(&items[n], sizeof(items[n]));
            PathCombineW(items[n].path, dir, fd.cFileName);
            lstrcpynW(items[n].name, fd.cFileName, ARRAYSIZE(items[n].name));
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                WCHAR *ext = PathFindExtensionW(items[n].name);

                if (!lstrcmpiW(ext, L".lnk") || !lstrcmpiW(ext, L".url"))
                    *ext = 0;
            }
            n++;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (out) {
        *out = items;
        *count = n;
    }
    return stamp;
}

static void load_icons(void)
{
    int i, x = 12, y = AREA_TOP + 12;

    for (i = 0; i < g_nicons; i++)
        if (g_icons[i].icon)
            DestroyIcon(g_icons[i].icon);
    if (g_icons)
        HeapFree(GetProcessHeap(), 0, g_icons);
    g_icons = NULL;
    g_nicons = 0;
    g_icon_sel = -1;

    g_icons_stamp = scan_desktop(&g_icons, &g_nicons);
    if (g_nicons)
        qsort(g_icons, g_nicons, sizeof(*g_icons), icon_cmp);
    for (i = 0; i < g_nicons; i++) {
        g_icons[i].icon = file_icon(g_icons[i].path, 48);
        SetRect(&g_icons[i].rc, x, y, x + ICON_CELL_W, y + ICON_CELL_H);
        y += ICON_CELL_H + 6;
        if (y + ICON_CELL_H > AREA_BOTTOM) {
            y = AREA_TOP + 12;
            x += ICON_CELL_W + 6;
        }
    }
    if (g_desktop)
        InvalidateRect(g_desktop, NULL, FALSE);
}

static int icon_at(int x, int y)
{
    POINT pt = { x, y };
    int i;

    for (i = 0; i < g_nicons; i++)
        if (PtInRect(&g_icons[i].rc, pt))
            return i;
    return -1;
}

static void paint_desktop(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    HDC mem = CreateCompatibleDC(dc), wall = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, g_sw, g_sh);
    HGDIOBJ old = SelectObject(mem, bmp), old_wall = SelectObject(wall, g_wallpaper);
    int i;

    BitBlt(mem, 0, 0, g_sw, g_sh, wall, 0, 0, SRCCOPY);
    for (i = 0; i < g_nicons; i++) {
        struct icon_item *it = &g_icons[i];
        RECT rc = it->rc, shadow;
        int ix = rc.left + (ICON_CELL_W - 48) / 2;

        if (i == g_icon_sel) {
            HBRUSH br = CreateSolidBrush(C_ACCENT);

            blend(mem, &rc, C_ACCENT, 70);
            FrameRect(mem, &rc, br);
            DeleteObject(br);
        }
        if (it->icon)
            DrawIconEx(mem, ix, rc.top + 8, it->icon, 48, 48, 0, NULL, DI_NORMAL);
        rc.top += 60;
        rc.left += 4;
        rc.right -= 4;
        shadow = rc;
        OffsetRect(&shadow, 1, 1);
        text(mem, it->name, &shadow, DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS | DT_EDITCONTROL,
             g_font, RGB(0, 0, 0));
        text(mem, it->name, &rc, DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS | DT_EDITCONTROL,
             g_font, RGB(255, 255, 255));
    }
    BitBlt(dc, 0, 0, g_sw, g_sh, mem, 0, 0, SRCCOPY);
    SelectObject(wall, old_wall);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(wall);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

static void desktop_menu(HWND hwnd, int x, int y)
{
    HMENU menu = CreatePopupMenu();
    WCHAR dir[MAX_PATH];

    AppendMenuW(menu, MF_STRING, IDM_REFRESH, L"Refresh");
    AppendMenuW(menu, MF_STRING, IDM_OPEN_DESKTOP, L"Open Desktop folder");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_RUN, L"Run...");
    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"Control Panel");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_ABOUT, L"About WinCraft OS");
    switch (popup_menu(menu, TPM_RIGHTBUTTON, x, y)) {
    case IDM_REFRESH:
        load_icons();
        break;
    case IDM_OPEN_DESKTOP:
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, SHGFP_TYPE_CURRENT, dir)))
            system_tool(L"explorer.exe", dir);
        break;
    case IDM_RUN:
        run_dialog();
        break;
    case IDM_SETTINGS:
        system_tool(L"control.exe", NULL);
        break;
    case IDM_ABOUT:
        about();
        break;
    }
    DestroyMenu(menu);
}

static LRESULT CALLBACK desktop_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        SetTimer(hwnd, TIMER_ICONS, 2000, NULL);
        return 0;
    case WM_WINDOWPOSCHANGING: {
        /* The desktop is always the bottom-most window. */
        WINDOWPOS *pos = (WINDOWPOS *)lp;

        pos->hwndInsertAfter = HWND_BOTTOM;
        pos->flags &= ~SWP_NOZORDER;
        return 0;
    }
    case WM_DISPLAYCHANGE:
        /* New screen resolution: refit the desktop and the taskbar. */
        g_sw = LOWORD(lp);
        g_sh = HIWORD(lp);
        make_wallpaper();
        SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, g_sw, g_sh, SWP_NOACTIVATE);
        place_bar(g_prefs.autohide && !IsWindowVisible(g_menu));
        hide_menu();
        load_icons();
        layout_tasks();
        tray_tips();
        InvalidateRect(g_bar, NULL, FALSE);
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;       /* the desktop never becomes the active window */
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint_desktop(hwnd);
        return 0;
    case WM_TIMER:
        if (wp == TIMER_ICONS && scan_desktop(NULL, NULL) != g_icons_stamp)
            load_icons();
        return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN: {
        int hit = icon_at(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));

        if (hit != g_icon_sel) {
            g_icon_sel = hit;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        g_icon_pressed = msg == WM_LBUTTONDOWN ? hit : -1;
        return 0;
    }
    case WM_LBUTTONUP: {
        /* One click opens, when chosen (WinCraft\Shell: SingleClickOpen). */
        int hit = icon_at(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));

        if (g_prefs.single_click && hit >= 0 && hit == g_icon_pressed)
            open_path(g_icons[hit].path, NULL);
        g_icon_pressed = -1;
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        int hit = icon_at(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));

        if (hit >= 0 && !g_prefs.single_click)      /* the first click opened it already */
            open_path(g_icons[hit].path, NULL);
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (g_bar)
            PostMessageW(g_bar, WM_PREFS_CHANGED, 0, 0);
        return 0;
    case WM_RBUTTONUP: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };

        ClientToScreen(hwnd, &pt);
        desktop_menu(hwnd, pt.x, pt.y);
        return 0;
    }
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_CLOSE:
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ======================================================================
 * Start menu
 * ====================================================================== */

struct app {
    WCHAR path[MAX_PATH];
    WCHAR name[96];
    WCHAR group[96];        /* the Programs folder it is in; "" at the top */
    HICON icon;
    BOOL icon_tried;
};

/* A Programs folder holding several programs, as installers that add more
   than one shortcut make: one row, which opens to show them. A folder with a
   single program shows just the program. */
struct group {
    WCHAR name[96];
    int count;
    BOOL open;
};

static struct app *g_apps;
static int g_napps, g_apps_cap;
static struct group g_groups[64];
static int g_ngroups;
static HICON g_folder_icon;
/* the rows shown: an index into g_apps, or -1 - an index into g_groups */
static int *g_view, g_nview;
static int g_menu_sel, g_menu_scroll, g_menu_btn = -1, g_menu_h;
static WCHAR g_filter[64];
static const WCHAR *g_menu_buttons[] = { L"Run...", L"Restart", L"Shut down" };

static int __cdecl app_cmp(const void *a, const void *b)
{
    return lstrcmpiW(((const struct app *)a)->name, ((const struct app *)b)->name);
}

static void scan_programs(const WCHAR *dir, const WCHAR *group, int depth)
{
    WCHAR pattern[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;

    PathCombineW(pattern, dir, L"*");
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        WCHAR path[MAX_PATH], *ext;
        struct app *a;
        int i;

        if (fd.cFileName[0] == L'.')
            continue;
        PathCombineW(path, dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            /* what lies deeper belongs to the first-level folder */
            if (depth < 4 && lstrcmpiW(fd.cFileName, L"StartUp"))
                scan_programs(path, depth ? group : fd.cFileName, depth + 1);
            continue;
        }
        ext = PathFindExtensionW(fd.cFileName);
        if (lstrcmpiW(ext, L".lnk") && lstrcmpiW(ext, L".url") && lstrcmpiW(ext, L".exe"))
            continue;
        if (!StrCmpNIW(fd.cFileName, L"Uninstall", 9) ||
            (g_hide_installer && !lstrcmpiW(fd.cFileName, INSTALLER_LINK)))
            continue;
        if (g_napps == g_apps_cap) {
            g_apps_cap = g_apps_cap ? g_apps_cap * 2 : 64;
            g_apps = HeapReAlloc(GetProcessHeap(), 0,
                                 g_apps ? g_apps : HeapAlloc(GetProcessHeap(), 0, 1),
                                 g_apps_cap * sizeof(*g_apps));
        }
        a = &g_apps[g_napps];
        ZeroMemory(a, sizeof(*a));
        lstrcpynW(a->path, path, MAX_PATH);
        lstrcpynW(a->name, fd.cFileName, ARRAYSIZE(a->name));
        *PathFindExtensionW(a->name) = 0;
        lstrcpynW(a->group, group, ARRAYSIZE(a->group));
        for (i = 0; i < g_napps; i++)       /* per-user entry hides the common one */
            if (!lstrcmpiW(g_apps[i].name, a->name) && !lstrcmpiW(g_apps[i].group, a->group))
                break;
        if (i == g_napps)
            g_napps++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static int group_index(const WCHAR *name)
{
    int i;

    for (i = 0; i < g_ngroups; i++)
        if (!lstrcmpiW(g_groups[i].name, name))
            return i;
    return -1;
}

struct row {
    const WCHAR *name;
    int code;               /* as in g_view */
};

static int __cdecl row_cmp(const void *a, const void *b)
{
    return lstrcmpiW(((const struct row *)a)->name, ((const struct row *)b)->name);
}

/* Programs and folders by name, an open folder's programs under it; while
   searching, every matching program, wherever it is. */
static void build_view(void)
{
    struct row *top;
    int i, j, n = 0;

    g_nview = 0;
    if (g_filter[0]) {
        for (i = 0; i < g_napps; i++)
            if (StrStrIW(g_apps[i].name, g_filter))
                g_view[g_nview++] = i;
        return;
    }
    top = HeapAlloc(GetProcessHeap(), 0, (g_napps + g_ngroups + 1) * sizeof(*top));
    for (i = 0; i < g_napps; i++)
        if (!g_apps[i].group[0]) {
            top[n].name = g_apps[i].name;
            top[n++].code = i;
        }
    for (i = 0; i < g_ngroups; i++)
        if (g_groups[i].count > 1) {
            top[n].name = g_groups[i].name;
            top[n++].code = -1 - i;
        }
    qsort(top, n, sizeof(*top), row_cmp);
    for (j = 0; j < n; j++) {
        g_view[g_nview++] = top[j].code;
        if (top[j].code < 0 && g_groups[-1 - top[j].code].open)
            for (i = 0; i < g_napps; i++)           /* g_apps is sorted by name */
                if (!lstrcmpiW(g_apps[i].group, g_groups[-1 - top[j].code].name))
                    g_view[g_nview++] = i;
    }
    HeapFree(GetProcessHeap(), 0, top);
}

static void apply_filter(void)
{
    build_view();
    g_menu_sel = g_nview ? 0 : -1;
    g_menu_scroll = 0;
}

static void load_programs(void)
{
    static const int folders[] = { CSIDL_PROGRAMS, CSIDL_COMMON_PROGRAMS };
    WCHAR dir[MAX_PATH];
    unsigned f;
    int i;

    for (i = 0; i < g_napps; i++)
        if (g_apps[i].icon)
            DestroyIcon(g_apps[i].icon);
    g_napps = 0;
    for (f = 0; f < ARRAYSIZE(folders); f++)
        if (SUCCEEDED(SHGetFolderPathW(NULL, folders[f], NULL, SHGFP_TYPE_CURRENT, dir)))
            scan_programs(dir, L"", 0);
    if (g_napps)
        qsort(g_apps, g_napps, sizeof(*g_apps), app_cmp);
    g_ngroups = 0;
    for (i = 0; i < g_napps; i++) {
        int g;

        if (!g_apps[i].group[0])
            continue;
        g = group_index(g_apps[i].group);
        if (g < 0 && g_ngroups < (int)ARRAYSIZE(g_groups)) {
            g = g_ngroups++;
            lstrcpynW(g_groups[g].name, g_apps[i].group, ARRAYSIZE(g_groups[g].name));
            g_groups[g].count = 0;
            g_groups[g].open = FALSE;
        }
        if (g >= 0)
            g_groups[g].count++;
    }
    for (i = 0; i < g_napps; i++) {         /* a folder of one is no folder */
        int g = g_apps[i].group[0] ? group_index(g_apps[i].group) : -1;

        if (g < 0 || g_groups[g].count < 2)
            g_apps[i].group[0] = 0;
    }
    if (!g_folder_icon) {
        GetSystemDirectoryW(dir, MAX_PATH);
        PathAppendW(dir, L"shell32.dll");
        g_folder_icon = extract_icon(dir, -4, 32);
    }
    if (g_view)
        HeapFree(GetProcessHeap(), 0, g_view);
    g_view = HeapAlloc(GetProcessHeap(), 0, (g_napps + g_ngroups + 1) * sizeof(int));
    g_filter[0] = 0;
    apply_filter();
}

static int menu_list_h(void)
{
    return g_menu_h - MENU_HEADER - MENU_FOOTER;
}

static void menu_button_rect(int i, RECT *rc)
{
    int w = (MENU_W - 16 * 2 - 8 * 2) / 3;

    SetRect(rc, 16 + i * (w + 8), g_menu_h - MENU_FOOTER + 12,
            16 + i * (w + 8) + w, g_menu_h - 12);
}

static void menu_ensure_visible(void)
{
    int top = g_menu_sel * MENU_ROW, max_scroll = g_nview * MENU_ROW - menu_list_h();

    if (g_menu_sel < 0)
        return;
    if (top < g_menu_scroll)
        g_menu_scroll = top;
    if (top + MENU_ROW > g_menu_scroll + menu_list_h())
        g_menu_scroll = top + MENU_ROW - menu_list_h();
    if (g_menu_scroll > max_scroll)
        g_menu_scroll = max_scroll;
    if (g_menu_scroll < 0)
        g_menu_scroll = 0;
}

static void paint_menu(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, MENU_W, g_menu_h);
    HGDIOBJ old = SelectObject(mem, bmp);
    RECT rc, list;
    HRGN clip;
    WCHAR user[64] = L"";
    DWORD len = ARRAYSIZE(user);
    int i, first, y;

    SetRect(&rc, 0, 0, MENU_W, g_menu_h);
    fill(mem, &rc, C_MENU);

    /* header: logo, user, search text */
    draw_logo(mem, 18, 16, 32, C_MENU);
    GetUserNameW(user, &len);
    SetRect(&rc, 62, 12, MENU_W - 16, 34);
    text(mem, g_filter[0] ? g_filter : L"WinCraft", &rc, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS,
         g_font_large, C_TEXT);
    SetRect(&rc, 62, 34, MENU_W - 16, 54);
    text(mem, g_filter[0] ? L"Searching programs - Esc to clear" : L"Type to search programs",
         &rc, DT_SINGLELINE | DT_VCENTER, g_font_small, C_DIM);
    fill_xywh(mem, 0, MENU_HEADER - 1, MENU_W, 1, C_BAR_EDGE);

    /* program list */
    SetRect(&list, 0, MENU_HEADER, MENU_W, g_menu_h - MENU_FOOTER);
    clip = CreateRectRgnIndirect(&list);
    SelectClipRgn(mem, clip);
    first = g_menu_scroll / MENU_ROW;
    for (i = first; i < g_nview; i++) {
        const WCHAR *name;
        HICON icon;
        int indent = 0;

        y = MENU_HEADER + i * MENU_ROW - g_menu_scroll;
        if (y >= list.bottom)
            break;
        if (i == g_menu_sel) {
            fill_xywh(mem, 6, y + 2, MENU_W - 12, MENU_ROW - 4, C_ACTIVE);
            fill_xywh(mem, 6, y + 10, 3, MENU_ROW - 20, C_ACCENT);
        }
        if (g_view[i] < 0) {
            struct group *g = &g_groups[-1 - g_view[i]];

            name = g->name;
            icon = g_folder_icon;
            draw_chevron(mem, MENU_W - 34, y + MENU_ROW / 2, g->open, C_DIM);
        } else {
            struct app *a = &g_apps[g_view[i]];

            if (!a->icon_tried) {
                a->icon = file_icon(a->path, 32);
                a->icon_tried = TRUE;
            }
            name = a->name;
            icon = a->icon;
            if (a->group[0] && !g_filter[0])
                indent = 28;
        }
        if (icon)
            DrawIconEx(mem, 20 + indent, y + (MENU_ROW - 32) / 2, icon, 32, 32, 0, NULL, DI_NORMAL);
        SetRect(&rc, 66 + indent, y, MENU_W - (g_view[i] < 0 ? 48 : 20), y + MENU_ROW);
        text(mem, name, &rc, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS, g_font, C_TEXT);
    }
    if (!g_nview) {
        SetRect(&rc, 0, MENU_HEADER + 24, MENU_W, MENU_HEADER + 60);
        text(mem, g_napps ? L"No matching programs" : L"No programs installed", &rc,
             DT_CENTER | DT_SINGLELINE, g_font, C_DIM);
    }
    if (g_nview * MENU_ROW > menu_list_h()) {     /* scroll position */
        int total = g_nview * MENU_ROW, h = menu_list_h();
        int thumb = max(24, h * h / total);
        int pos = (h - thumb) * g_menu_scroll / (total - h);

        fill_xywh(mem, MENU_W - 5, MENU_HEADER + pos, 3, thumb, C_BAR_EDGE);
    }
    SelectClipRgn(mem, NULL);
    DeleteObject(clip);

    /* footer */
    fill_xywh(mem, 0, g_menu_h - MENU_FOOTER, MENU_W, MENU_FOOTER, C_BAR);
    fill_xywh(mem, 0, g_menu_h - MENU_FOOTER, MENU_W, 1, C_BAR_EDGE);
    for (i = 0; i < (int)ARRAYSIZE(g_menu_buttons); i++) {
        menu_button_rect(i, &rc);
        fill(mem, &rc, i == g_menu_btn ? C_ACTIVE : C_MENU_ALT);
        text(mem, g_menu_buttons[i], &rc, DT_CENTER | DT_SINGLELINE | DT_VCENTER, g_font,
             i == 2 && i == g_menu_btn ? C_ACCENT : C_TEXT);
    }
    /* frame */
    {
        HBRUSH br = CreateSolidBrush(C_BAR_EDGE);

        SetRect(&rc, 0, 0, MENU_W, g_menu_h);
        FrameRect(mem, &rc, br);
        DeleteObject(br);
    }

    BitBlt(dc, 0, 0, MENU_W, g_menu_h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

static void hide_menu(void)
{
    if (IsWindowVisible(g_menu)) {
        ShowWindow(g_menu, SW_HIDE);
        InvalidateRect(g_bar, NULL, FALSE);
        autohide_later();
    }
}

static void show_menu(void)
{
    place_bar(FALSE);               /* a hidden taskbar comes back with the menu */
    load_programs();
    g_menu_btn = -1;
    g_menu_h = min(620, g_sh - BAR_H - 16);
    SetWindowPos(g_menu, HWND_TOPMOST, 0, g_bar_top ? BAR_INNER_Y : BAR_INNER_Y - g_menu_h, MENU_W, g_menu_h,
                 SWP_SHOWWINDOW);
    SetForegroundWindow(g_menu);
    SetFocus(g_menu);
    InvalidateRect(g_menu, NULL, FALSE);
    InvalidateRect(g_bar, NULL, FALSE);
}

/* Open (1), close (0) or toggle (-1) the folder on a row; it stays selected. */
static void menu_open_group(int row, int open)
{
    struct group *g = &g_groups[-1 - g_view[row]];

    if (open < 0)
        open = !g->open;
    if (g->open == open)
        return;
    g->open = open;
    build_view();
    if (open) {                     /* bring its programs into view */
        g_menu_sel = min(g_nview - 1, row + g->count);
        menu_ensure_visible();
    }
    g_menu_sel = row;
    menu_ensure_visible();
    InvalidateRect(g_menu, NULL, FALSE);
}

static void menu_launch(int view_index)
{
    WCHAR path[MAX_PATH];

    if (view_index < 0 || view_index >= g_nview)
        return;
    if (g_view[view_index] < 0) {
        menu_open_group(view_index, -1);
        return;
    }
    lstrcpynW(path, g_apps[g_view[view_index]].path, MAX_PATH);
    hide_menu();
    open_path(path, NULL);
}

static void menu_button(int i)
{
    hide_menu();
    switch (i) {
    case 0: run_dialog(); break;
    case 1: power(TRUE); break;
    case 2: power(FALSE); break;
    }
}

static LRESULT CALLBACK menu_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint_menu(hwnd);
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE)
            hide_menu();
        return 0;
    case WM_MOUSEMOVE: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int sel = g_menu_sel, btn = -1, i;
        RECT rc;

        if (pt.y >= MENU_HEADER && pt.y < g_menu_h - MENU_FOOTER) {
            i = (pt.y - MENU_HEADER + g_menu_scroll) / MENU_ROW;
            if (i < g_nview)
                sel = i;
        }
        for (i = 0; i < (int)ARRAYSIZE(g_menu_buttons); i++) {
            menu_button_rect(i, &rc);
            if (PtInRect(&rc, pt))
                btn = i;
        }
        if (sel != g_menu_sel || btn != g_menu_btn) {
            g_menu_sel = sel;
            g_menu_btn = btn;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT rc;
        int i;

        for (i = 0; i < (int)ARRAYSIZE(g_menu_buttons); i++) {
            menu_button_rect(i, &rc);
            if (PtInRect(&rc, pt)) {
                menu_button(i);
                return 0;
            }
        }
        if (pt.y >= MENU_HEADER && pt.y < g_menu_h - MENU_FOOTER)
            menu_launch((pt.y - MENU_HEADER + g_menu_scroll) / MENU_ROW);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int max_scroll = g_nview * MENU_ROW - menu_list_h();

        g_menu_scroll -= GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * MENU_ROW * 2;
        if (g_menu_scroll > max_scroll)
            g_menu_scroll = max_scroll;
        if (g_menu_scroll < 0)
            g_menu_scroll = 0;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_KEYDOWN: {
        int page = menu_list_h() / MENU_ROW;

        switch (wp) {
        case VK_ESCAPE:
            if (g_filter[0]) {
                g_filter[0] = 0;
                apply_filter();
            } else {
                hide_menu();
            }
            break;
        case VK_RETURN: menu_launch(g_menu_sel); return 0;
        case VK_RIGHT:
            if (g_menu_sel >= 0 && g_view[g_menu_sel] < 0)
                menu_open_group(g_menu_sel, 1);
            return 0;
        case VK_LEFT:                   /* close the folder, from inside it too */
            if (g_filter[0] || g_menu_sel < 0)
                return 0;
            if (g_view[g_menu_sel] >= 0 && g_apps[g_view[g_menu_sel]].group[0]) {
                while (g_menu_sel > 0 && g_view[g_menu_sel] >= 0)
                    g_menu_sel--;
            }
            if (g_view[g_menu_sel] < 0)
                menu_open_group(g_menu_sel, 0);
            return 0;
        case VK_DOWN:   if (g_menu_sel < g_nview - 1) g_menu_sel++; break;
        case VK_UP:     if (g_menu_sel > 0) g_menu_sel--; break;
        case VK_NEXT:   g_menu_sel = min(g_nview - 1, g_menu_sel + page); break;
        case VK_PRIOR:  g_menu_sel = g_nview ? max(0, g_menu_sel - page) : -1; break;
        case VK_HOME:   g_menu_sel = g_nview ? 0 : -1; break;
        case VK_END:    g_menu_sel = g_nview - 1; break;
        default:        return 0;
        }
        menu_ensure_visible();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_CHAR: {
        int len = lstrlenW(g_filter);

        if (wp == VK_BACK) {
            if (len)
                g_filter[len - 1] = 0;
        } else if (wp >= 32 && len < (int)ARRAYSIZE(g_filter) - 1) {
            g_filter[len] = (WCHAR)wp;
            g_filter[len + 1] = 0;
        } else {
            return 0;
        }
        apply_filter();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_CLOSE:
        hide_menu();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ======================================================================
 * Taskbar
 * ====================================================================== */

struct task {
    HWND hwnd;
    WCHAR title[128];
    HICON icon;         /* owned by the icon cache */
    RECT rc;
};

struct exe_icon {
    WCHAR path[MAX_PATH];
    HICON icon;
};

static struct task g_tasks[64];
static int g_ntasks, g_bar_hot = -1;   /* -1 none, -2 start, -3 peek, >= 0 task, <= -10 tray icon */
static HWND g_active;
static struct exe_icon g_exe_icons[64];
static int g_nexe_icons;
static BOOL g_tracking, g_desktop_shown;

static HICON window_icon(HWND hwnd)
{
    WCHAR path[MAX_PATH];
    DWORD pid = 0, len = MAX_PATH;
    HANDLE proc;
    int i;

    GetWindowThreadProcessId(hwnd, &pid);
    proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc)
        return NULL;
    if (!QueryFullProcessImageNameW(proc, 0, path, &len))
        path[0] = 0;
    CloseHandle(proc);
    if (!path[0])
        return NULL;
    for (i = 0; i < g_nexe_icons; i++)
        if (!lstrcmpiW(g_exe_icons[i].path, path))
            return g_exe_icons[i].icon;
    if (g_nexe_icons == ARRAYSIZE(g_exe_icons))
        return NULL;
    lstrcpynW(g_exe_icons[g_nexe_icons].path, path, MAX_PATH);
    g_exe_icons[g_nexe_icons].icon = file_icon(path, 16);
    return g_exe_icons[g_nexe_icons++].icon;
}

static BOOL is_task_window(HWND hwnd)
{
    LONG_PTR ex;

    if (hwnd == g_desktop || hwnd == g_bar || hwnd == g_menu || !IsWindowVisible(hwnd))
        return FALSE;
    ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_APPWINDOW)
        return TRUE;
    if ((ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) || GetWindow(hwnd, GW_OWNER))
        return FALSE;
    return GetWindowTextLengthW(hwnd) > 0;
}

struct task_scan {
    struct task tasks[64];
    int count;
};

static BOOL CALLBACK enum_task(HWND hwnd, LPARAM lp)
{
    struct task_scan *scan = (struct task_scan *)lp;

    if (scan->count < (int)ARRAYSIZE(scan->tasks) && is_task_window(hwnd)) {
        struct task *t = &scan->tasks[scan->count++];

        ZeroMemory(t, sizeof(*t));
        t->hwnd = hwnd;
        InternalGetWindowText(hwnd, t->title, ARRAYSIZE(t->title));
    }
    return TRUE;
}

/* ---- notification area: the icons programs show with Shell_NotifyIcon.
 * Wine's shell32 sends them to the first "Shell_TrayWnd" window, which is
 * this taskbar, as on Windows (Wine's own floating tray window stays empty,
 * and is turned off: Software\Wine\Explorer, ShowSystray). The message is
 * in shell32's own format, struct notify_data in dlls/shell32/systray.c,
 * the same for 32- and 64-bit programs. ---- */
#define TRAY_MAX        32
#define TRAY_SLOT       24              /* the width of one icon's place */
#define TRAY_HIT(i)     (-10 - (i))     /* what bar_hit returns for icon i */
#define IS_TRAY_HIT(h)  ((h) <= -10)
#define TRAY_INDEX(h)   (-10 - (h))
#define BALLOON_TOOL    1

struct wine_tray_bitmap { UINT width, height, planes, bpp; };
struct wine_tray_data {
    LONG hwnd;
    UINT id, flags, callback;
    struct wine_tray_bitmap icon;
    WCHAR tip[128];
    DWORD state, state_mask;
    WCHAR info[256];
    UINT timeout_or_version;
    WCHAR info_title[64];
    DWORD info_flags;
    GUID guid;
    struct wine_tray_bitmap balloon_icon;
    BYTE bits[];        /* the icon's mask, then its colours, as GetBitmapBits gives them */
};

struct tray_icon {
    HWND owner;
    UINT id, callback, version;
    HICON icon;
    BOOL hidden;
    WCHAR tip[128];
    RECT rc;            /* its place in the taskbar; empty when not shown */
};

static struct tray_icon g_tray[TRAY_MAX];
static int g_ntray, g_tray_left;        /* where the icons start, left of the clock */
static HWND g_tray_tip, g_balloon;      /* tooltips: the icons' texts, a balloon */
static int g_balloon_of = -1;           /* the icon whose balloon is showing */

/* The icon from the bitmaps' bits, sized the way shell32 sized them. */
static HICON tray_bits_icon(const struct wine_tray_bitmap *bm, const BYTE *bits, size_t avail)
{
    ICONINFO ii = { TRUE, 0, 0, NULL, NULL };
    BOOL mono = bm->bpp == 1;           /* a mask twice as tall, and no colours */
    size_t mask, color;
    HICON icon;

    if (!bm->width || !bm->height || bm->width > 256 || bm->height > 256 || bm->planes != 1 || bm->bpp > 32)
        return NULL;
    mask = ((size_t)bm->width * bm->height * (mono ? 2 : 1) + 15) / 16 * 2;
    color = mono ? 0 : ((size_t)bm->width * bm->height * bm->bpp + 15) / 16 * 2;
    if (mask + color > avail)
        return NULL;
    ii.hbmMask = CreateBitmap(bm->width, bm->height * (mono ? 2 : 1), 1, 1, bits);
    if (!mono)
        ii.hbmColor = CreateBitmap(bm->width, bm->height, 1, bm->bpp, bits + mask);
    icon = CreateIconIndirect(&ii);
    DeleteObject(ii.hbmMask);
    if (ii.hbmColor)
        DeleteObject(ii.hbmColor);
    return icon;
}

/* Tell the program about its icon, the way Windows does for its version of
 * the interface; FALSE if the program is gone, and the icon with it. */
static void tray_remove(int i);
static BOOL tray_notify(int i, UINT msg, POINT pt)
{
    struct tray_icon *t = &g_tray[i];
    WPARAM wp = t->id;
    LPARAM lp = msg;

    if (!t->callback)
        return TRUE;
    if (t->version >= NOTIFYICON_VERSION_4) {
        wp = MAKEWPARAM(pt.x, pt.y);
        lp = MAKELPARAM(msg, t->id);
    }
    if (!SendNotifyMessageW(t->owner, t->callback, wp, lp) && GetLastError() == ERROR_INVALID_WINDOW_HANDLE) {
        tray_remove(i);
        return FALSE;
    }
    return TRUE;
}

static void balloon_tool(TTTOOLINFOW *ti, UINT flags)  /* the balloon's one tool */
{
    ZeroMemory(ti, sizeof(*ti));
    ti->cbSize = TTTOOLINFOW_V2_SIZE;
    ti->uFlags = flags;
    ti->hwnd = g_bar;
    ti->uId = BALLOON_TOOL;
}

static POINT tray_anchor(int i)        /* the middle of the icon's edge facing the desktop */
{
    POINT pt = { (g_tray[i].rc.left + g_tray[i].rc.right) / 2, BAR_INNER_Y };

    return pt;
}

static void balloon_close(UINT why)    /* why: NIN_BALLOONTIMEOUT and the like, or 0 */
{
    TTTOOLINFOW ti;
    int i = g_balloon_of;

    if (i < 0)
        return;
    g_balloon_of = -1;
    KillTimer(g_bar, TIMER_BALLOON);
    balloon_tool(&ti, 0);
    SendMessageW(g_balloon, TTM_TRACKACTIVATE, FALSE, (LPARAM)&ti);
    if (why)
        tray_notify(i, why, tray_anchor(i));
    autohide_later();
}

static void balloon_show(int i, const WCHAR *title, const WCHAR *text, DWORD flags, UINT timeout)
{
    TTTOOLINFOW ti;
    DWORD kind = flags & NIIF_ICON_MASK;
    POINT pt;

    balloon_close(g_balloon_of == i ? 0 : NIN_BALLOONHIDE);
    if (!g_balloon || IsRectEmpty(&g_tray[i].rc))
        return;
    place_bar(FALSE);                   /* a taskbar that hides itself comes back for it */
    balloon_tool(&ti, TTF_TRACK | TTF_ABSOLUTE);
    ti.lpszText = (WCHAR *)text;
    SendMessageW(g_balloon, TTM_SETTITLEW, kind == NIIF_INFO ? TTI_INFO : kind == NIIF_WARNING ? TTI_WARNING :
                 kind == NIIF_ERROR ? TTI_ERROR : TTI_NONE, (LPARAM)title);
    SendMessageW(g_balloon, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
    pt = tray_anchor(i);
    SendMessageW(g_balloon, TTM_TRACKPOSITION, 0, MAKELPARAM(pt.x, pt.y));
    SendMessageW(g_balloon, TTM_TRACKACTIVATE, TRUE, (LPARAM)&ti);
    g_balloon_of = i;
    SetTimer(g_bar, TIMER_BALLOON, timeout, NULL);
    tray_notify(i, NIN_BALLOONSHOW, pt);
}

/* A click on the balloon is the program's to handle. */
static WNDPROC g_balloon_proc;
static LRESULT CALLBACK balloon_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_LBUTTONUP && g_balloon_of >= 0) {
        int i = g_balloon_of;

        if (IsWindow(g_tray[i].owner))
            SetForegroundWindow(g_tray[i].owner);   /* it may open a window: let it come to the front */
        balloon_close(NIN_BALLOONUSERCLICK);
        return 0;
    }
    return CallWindowProcW(g_balloon_proc, hwnd, msg, wp, lp);
}

static void tray_remove(int i)
{
    if (g_balloon_of == i)
        balloon_close(NIN_BALLOONHIDE);
    else if (g_balloon_of > i)
        g_balloon_of--;
    if (g_tray[i].icon)
        DestroyIcon(g_tray[i].icon);
    memmove(&g_tray[i], &g_tray[i + 1], (g_ntray - i - 1) * sizeof(g_tray[0]));
    g_ntray--;
}

/* The icons' places, right-aligned against the clock. */
static void tray_place(void)
{
    int i, n = 0, x;

    for (i = 0; i < g_ntray; i++)
        if (!g_tray[i].hidden && g_tray[i].icon)
            n++;
    x = g_tray_left = g_sw - PEEK_W - CLOCK_W - n * TRAY_SLOT;
    for (i = 0; i < g_ntray; i++) {
        if (g_tray[i].hidden || !g_tray[i].icon) {
            SetRectEmpty(&g_tray[i].rc);
            continue;
        }
        SetRect(&g_tray[i].rc, x, 0, x + TRAY_SLOT, BAR_H);
        x += TRAY_SLOT;
    }
}

/* One tooltip per shown icon, with its text. */
static void tray_tips(void)
{
    TTTOOLINFOW ti;
    int i;

    if (!g_tray_tip)
        return;
    ZeroMemory(&ti, sizeof(ti));
    ti.cbSize = TTTOOLINFOW_V2_SIZE;
    ti.hwnd = g_bar;
    for (i = 0; i < TRAY_MAX; i++) {
        ti.uId = i;
        SendMessageW(g_tray_tip, TTM_DELTOOLW, 0, (LPARAM)&ti);
    }
    for (i = 0; i < g_ntray; i++) {
        if (IsRectEmpty(&g_tray[i].rc) || !g_tray[i].tip[0])
            continue;
        ti.uFlags = TTF_SUBCLASS;
        ti.uId = i;
        ti.rect = g_tray[i].rc;
        ti.lpszText = g_tray[i].tip;
        SendMessageW(g_tray_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }
}

static void tray_changed(void)
{
    layout_tasks();                     /* which places the icons first */
    tray_tips();
    InvalidateRect(g_bar, NULL, FALSE);
}

/* Shell_NotifyIcon's message: NIM_ADD, NIM_MODIFY, NIM_DELETE and the others. */
static BOOL tray_message(const COPYDATASTRUCT *cds)
{
    const struct wine_tray_data *d = cds->lpData;
    struct tray_icon *t;
    HWND owner;
    int i;

    if (cds->dwData > NIM_SETVERSION || !d || cds->cbData < sizeof(*d))
        return FALSE;               /* not shell32's message */
    owner = (HWND)(LONG_PTR)d->hwnd;
    for (i = 0; i < g_ntray; i++)
        if (g_tray[i].owner == owner && g_tray[i].id == d->id)
            break;
    if (cds->dwData == NIM_ADD) {
        if (i < g_ntray || g_ntray == TRAY_MAX)
            return FALSE;
        ZeroMemory(&g_tray[i], sizeof(g_tray[i]));
        g_tray[i].owner = owner;
        g_tray[i].id = d->id;
        g_ntray++;
    } else if (i == g_ntray) {
        return FALSE;
    }
    t = &g_tray[i];
    switch (cds->dwData) {
    case NIM_DELETE:
        tray_remove(i);
        tray_changed();
        return TRUE;
    case NIM_SETVERSION:
        t->version = d->timeout_or_version;
        return TRUE;
    case NIM_SETFOCUS:
        return TRUE;
    }
    /* NIM_ADD, NIM_MODIFY */
    if (d->flags & NIF_MESSAGE)
        t->callback = d->callback;
    if (d->flags & NIF_ICON) {
        HICON icon = tray_bits_icon(&d->icon, d->bits, cds->cbData - sizeof(*d));

        if (t->icon)
            DestroyIcon(t->icon);
        t->icon = icon;
    }
    if (d->flags & NIF_TIP)
        lstrcpynW(t->tip, d->tip, ARRAYSIZE(t->tip));
    if ((d->flags & NIF_STATE) && (d->state_mask & NIS_HIDDEN))
        t->hidden = (d->state & NIS_HIDDEN) != 0;
    tray_changed();
    if (d->flags & NIF_INFO) {
        WCHAR title[64], text[256];

        lstrcpynW(title, d->info_title, ARRAYSIZE(title));
        lstrcpynW(text, d->info, ARRAYSIZE(text));
        if (text[0])
            balloon_show(i, title, text, d->info_flags,
                         d->timeout_or_version < 10000 ? 10000 : d->timeout_or_version > 30000 ? 30000 :
                         d->timeout_or_version);
        else if (g_balloon_of == i)
            balloon_close(NIN_BALLOONHIDE);
    }
    return TRUE;
}

/* The pointer on an icon: the program gets the mouse message, and for its
 * newer interface the "selected" and "context menu" notifications. */
static void tray_mouse(int i, UINT msg)
{
    POINT pt;

    GetCursorPos(&pt);
    /* The program shows its menu or window next. The click was the taskbar's,
     * so under Windows' foreground lock, which Wine keeps (without
     * AllowSetForegroundWindow), only the taskbar may bring it forward. */
    if (msg != WM_MOUSEMOVE && IsWindow(g_tray[i].owner))
        SetForegroundWindow(g_tray[i].owner);
    if (!tray_notify(i, msg, pt))
        return;
    if (g_tray[i].version > 0 && msg == WM_LBUTTONUP)
        tray_notify(i, NIN_SELECT, pt);
    else if (g_tray[i].version > 0 && msg == WM_RBUTTONUP)
        tray_notify(i, WM_CONTEXTMENU, pt);
}

/* The tooltips, Wine's floating tray window turned off, and the programs
 * already running asked to add their icons again, as when Windows' taskbar
 * starts. */
static void tray_start(HINSTANCE inst)
{
    TTTOOLINFOW ti;
    HKEY key;
    DWORD off = 0;

    g_tray_tip = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, TOOLTIPS_CLASSW, NULL,
                                 WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, 0, 0, 0, 0, g_bar, NULL, inst, NULL);
    g_balloon = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, TOOLTIPS_CLASSW, NULL,
                                WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP | TTS_BALLOON, 0, 0, 0, 0, g_bar, NULL,
                                inst, NULL);
    if (g_balloon) {
        balloon_tool(&ti, TTF_TRACK | TTF_ABSOLUTE);
        ti.lpszText = (WCHAR *)L"";
        SendMessageW(g_balloon, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        SendMessageW(g_balloon, TTM_SETMAXTIPWIDTH, 0, 320);
        g_balloon_proc = (WNDPROC)SetWindowLongPtrW(g_balloon, GWLP_WNDPROC, (LONG_PTR)balloon_proc);
    }
    /* read by Wine's desktop program when it starts: from the next session on */
    if (!RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Wine\\Explorer", 0, NULL, 0,
                         KEY_QUERY_VALUE | KEY_SET_VALUE, NULL, &key, NULL)) {
        if (RegQueryValueExW(key, L"ShowSystray", NULL, NULL, NULL, NULL))
            RegSetValueExW(key, L"ShowSystray", 0, REG_DWORD, (const BYTE *)&off, sizeof(off));
        RegCloseKey(key);
    }
    PostMessageW(HWND_BROADCAST, RegisterWindowMessageW(L"TaskbarCreated"), 0, 0);
}

/* Icons of programs that ended without removing them. */
static void tray_prune(void)
{
    int i, gone = 0;

    for (i = g_ntray - 1; i >= 0; i--)
        if (!IsWindow(g_tray[i].owner)) {
            tray_remove(i);
            gone = 1;
        }
    if (gone)
        tray_changed();
}

static void layout_tasks(void)
{
    int left = START_W + 6, avail, w, i;

    tray_place();
    avail = g_tray_left - 8 - left;
    if (!g_ntasks)
        return;
    w = avail / g_ntasks;
    if (w > TASK_MAX_W)
        w = TASK_MAX_W;
    if (w < TASK_MIN_W)
        w = TASK_MIN_W;
    for (i = 0; i < g_ntasks; i++)
        SetRect(&g_tasks[i].rc, left + i * w, 0, left + (i + 1) * w - 2, BAR_H);
}

/* Rebuild the window list; keep buttons in order of first appearance. */
static void refresh_tasks(void)
{
    static struct task_scan scan;
    struct task next[64];
    HWND active = GetForegroundWindow();
    BOOL changed = active != g_active;
    int i, j, n = 0;

    scan.count = 0;
    EnumWindows(enum_task, (LPARAM)&scan);

    for (i = 0; i < g_ntasks; i++)              /* survivors, old order */
        for (j = 0; j < scan.count; j++)
            if (scan.tasks[j].hwnd == g_tasks[i].hwnd) {
                next[n] = g_tasks[i];
                if (lstrcmpW(next[n].title, scan.tasks[j].title)) {
                    lstrcpyW(next[n].title, scan.tasks[j].title);
                    changed = TRUE;
                }
                n++;
                scan.tasks[j].hwnd = NULL;
                break;
            }
    for (j = scan.count - 1; j >= 0; j--)       /* newcomers */
        if (scan.tasks[j].hwnd && n < (int)ARRAYSIZE(next)) {
            next[n] = scan.tasks[j];
            next[n].icon = window_icon(next[n].hwnd);
            n++;
            changed = TRUE;
        }
    if (n != g_ntasks)
        changed = TRUE;
    if (!changed)
        return;
    memcpy(g_tasks, next, n * sizeof(*next));
    g_ntasks = n;
    g_active = active;
    layout_tasks();
    InvalidateRect(g_bar, NULL, FALSE);
}

static void paint_bar(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, g_sw, BAR_H);
    HGDIOBJ old = SelectObject(mem, bmp);
    BOOL menu_open = IsWindowVisible(g_menu);
    COLORREF start_bg = menu_open ? C_ACTIVE : g_bar_hot == -2 ? C_HOT : C_BAR;
    WCHAR buf[64];
    SYSTEMTIME now;
    RECT rc;
    int i;
    /* the edge line faces the desktop, the active window's mark the screen's edge */
    int edge = g_bar_top ? BAR_H - 1 : 0, body = g_bar_top ? 0 : 1, mark = g_bar_top ? 0 : BAR_H - 3;

    SetRect(&rc, 0, 0, g_sw, BAR_H);
    fill(mem, &rc, C_BAR);
    fill_xywh(mem, 0, edge, g_sw, 1, C_BAR_EDGE);

    /* Start button */
    fill_xywh(mem, 0, body, START_W, BAR_H - 1, start_bg);
    draw_logo(mem, (START_W - 26) / 2, (BAR_H - 26) / 2 + body, 26, start_bg);

    /* running windows */
    for (i = 0; i < g_ntasks; i++) {
        struct task *t = &g_tasks[i];
        BOOL active = t->hwnd == g_active && !IsIconic(t->hwnd);
        RECT tr = t->rc;
        int x = tr.left + 10;

        tr.top = body;
        tr.bottom = body + BAR_H - 1;
        if (active)
            fill(mem, &tr, C_ACTIVE);
        else if (i == g_bar_hot)
            fill(mem, &tr, C_HOT);
        fill_xywh(mem, tr.left + (active ? 0 : 8), mark,
                  tr.right - tr.left - (active ? 0 : 16), 3, active ? C_ACCENT : C_BAR_EDGE);
        if (t->icon) {
            DrawIconEx(mem, x, (BAR_H - 16) / 2, t->icon, 16, 16, 0, NULL, DI_NORMAL);
            x += 24;
        }
        SetRect(&rc, x, 0, tr.right - 8, BAR_H);
        if (rc.right - rc.left > 20)
            text(mem, t->title, &rc, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS, g_font,
                 IsIconic(t->hwnd) ? C_DIM : C_TEXT);
    }

    /* notification icons */
    for (i = 0; i < g_ntray; i++) {
        RECT ir = g_tray[i].rc;

        if (IsRectEmpty(&ir))
            continue;
        if (g_bar_hot == TRAY_HIT(i)) {
            ir.top = body;
            ir.bottom = body + BAR_H - 1;
            fill(mem, &ir, C_HOT);
        }
        DrawIconEx(mem, ir.left + (TRAY_SLOT - 16) / 2, (BAR_H - 16) / 2, g_tray[i].icon, 16, 16, 0, NULL,
                   DI_NORMAL);
    }

    /* clock */
    GetLocalTime(&now);
    GetTimeFormatW(LOCALE_USER_DEFAULT, g_prefs.seconds ? 0 : TIME_NOSECONDS, &now, NULL, buf,
                   ARRAYSIZE(buf));
    SetRect(&rc, g_sw - CLOCK_W - PEEK_W, 5, g_sw - PEEK_W - 8, BAR_H / 2 + 2);
    text(mem, buf, &rc, DT_RIGHT | DT_SINGLELINE | DT_VCENTER, g_font, C_TEXT);
    GetDateFormatW(LOCALE_USER_DEFAULT, DATE_SHORTDATE, &now, NULL, buf, ARRAYSIZE(buf));
    SetRect(&rc, g_sw - CLOCK_W - PEEK_W, BAR_H / 2, g_sw - PEEK_W - 8, BAR_H - 5);
    text(mem, buf, &rc, DT_RIGHT | DT_SINGLELINE | DT_VCENTER, g_font_small, C_DIM);

    /* show-desktop strip */
    fill_xywh(mem, g_sw - PEEK_W, body, 1, BAR_H - 1, C_BAR_EDGE);
    if (g_bar_hot == -3)
        fill_xywh(mem, g_sw - PEEK_W + 1, body, PEEK_W - 1, BAR_H - 1, C_HOT);

    BitBlt(dc, 0, 0, g_sw, BAR_H, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

static int bar_hit(int x, int y)
{
    POINT pt = { x, y };
    int i;

    if (x < START_W)
        return -2;
    if (x >= g_sw - PEEK_W)
        return -3;
    for (i = 0; i < g_ntray; i++)
        if (PtInRect(&g_tray[i].rc, pt))
            return TRAY_HIT(i);
    for (i = 0; i < g_ntasks; i++)
        if (PtInRect(&g_tasks[i].rc, pt))
            return i;
    return -1;
}

static void activate_task(HWND hwnd)
{
    if (hwnd == GetForegroundWindow() && !IsIconic(hwnd)) {
        PostMessageW(hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
        return;
    }
    if (IsIconic(hwnd))
        ShowWindowAsync(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
}

static void show_desktop(void)
{
    int i;

    g_desktop_shown = !g_desktop_shown;
    for (i = 0; i < g_ntasks; i++) {
        HWND hwnd = g_tasks[i].hwnd;

        if (g_desktop_shown && !IsIconic(hwnd))
            ShowWindowAsync(hwnd, SW_SHOWMINNOACTIVE);
        else if (!g_desktop_shown && IsIconic(hwnd))
            ShowWindowAsync(hwnd, SW_RESTORE);
    }
}

static void task_menu(HWND bar, HWND hwnd, int x)
{
    HMENU menu = CreatePopupMenu();
    BOOL iconic = IsIconic(hwnd), zoomed = IsZoomed(hwnd);

    AppendMenuW(menu, (iconic || zoomed) ? MF_STRING : MF_GRAYED, IDM_RESTORE, L"Restore");
    AppendMenuW(menu, iconic ? MF_GRAYED : MF_STRING, IDM_MINIMIZE, L"Minimize");
    AppendMenuW(menu, zoomed ? MF_GRAYED : MF_STRING, IDM_MAXIMIZE, L"Maximize");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_CLOSE, L"Close");
    switch (popup_menu(menu, BAR_POPUP_ALIGN, x, BAR_INNER_Y)) {
    case IDM_RESTORE:  ShowWindowAsync(hwnd, SW_RESTORE); SetForegroundWindow(hwnd); break;
    case IDM_MINIMIZE: PostMessageW(hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0); break;
    case IDM_MAXIMIZE: PostMessageW(hwnd, WM_SYSCOMMAND, SC_MAXIMIZE, 0); SetForegroundWindow(hwnd); break;
    case IDM_CLOSE:    PostMessageW(hwnd, WM_SYSCOMMAND, SC_CLOSE, 0); break;
    }
    DestroyMenu(menu);
}

static void start_menu_context(HWND bar)
{
    HMENU menu = CreatePopupMenu();

    AppendMenuW(menu, MF_STRING, IDM_RUN, L"Run...");
    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"Control Panel");
    AppendMenuW(menu, MF_STRING, IDM_ABOUT, L"About WinCraft OS");
    switch (popup_menu(menu, BAR_POPUP_ALIGN, 0, BAR_INNER_Y)) {
    case IDM_RUN:      run_dialog(); break;
    case IDM_SETTINGS: system_tool(L"control.exe", NULL); break;
    case IDM_ABOUT:    about(); break;
    }
    DestroyMenu(menu);
}

/* ---- auto-hide (WinCraft\Shell: TaskbarAutoHide): a 2-pixel strip stays
 * at the screen's edge, and the pointer reaching it brings the taskbar back */
#define AUTOHIDE_STRIP  2
static BOOL g_bar_hidden;

static void place_bar(BOOL hidden)
{
    g_bar_hidden = hidden;
    if (g_bar)
        SetWindowPos(g_bar, HWND_TOPMOST, 0,
                     !hidden ? BAR_Y : g_bar_top ? AUTOHIDE_STRIP - BAR_H : g_sh - AUTOHIDE_STRIP,
                     g_sw, BAR_H, SWP_NOACTIVATE);
}

static void autohide_later(void)
{
    if (g_prefs.autohide && g_bar)
        SetTimer(g_bar, TIMER_AUTOHIDE, 800, NULL);
}

/* Read the preferences again and apply what changed. */
static void apply_prefs(void)
{
    struct prefs old = g_prefs;

    read_prefs(&g_prefs);
    if (!memcmp(&old, &g_prefs, sizeof(old)))
        return;
    set_palette();
    g_bar_top = g_prefs.bar_top;
    if (old.bar_top != g_prefs.bar_top) {
        /* the bar moves to the other edge: the menu, the icons and the
         * wallpaper's logo move with it */
        hide_menu();
        place_bar(FALSE);
        load_icons();
    }
    if (lstrcmpW(old.wallpaper, g_prefs.wallpaper) || old.style != g_prefs.style || old.tile != g_prefs.tile ||
        old.has_background != g_prefs.has_background || old.background != g_prefs.background ||
        old.bar_top != g_prefs.bar_top) {
        make_wallpaper();
        InvalidateRect(g_desktop, NULL, FALSE);
    }
    SetTimer(g_bar, TIMER_CLOCK, g_prefs.seconds ? 1000 : 5000, NULL);
    if (old.autohide != g_prefs.autohide || old.bar_top != g_prefs.bar_top) {
        KillTimer(g_bar, TIMER_AUTOHIDE);
        if (g_prefs.autohide)
            autohide_later();
        else
            place_bar(FALSE);
    }
    if (old.autohide != g_prefs.autohide || old.focus_follows_mouse != g_prefs.focus_follows_mouse ||
        old.focus_delay != g_prefs.focus_delay || old.bar_top != g_prefs.bar_top)
        write_wm_prefs();
    InvalidateRect(g_bar, NULL, FALSE);
    InvalidateRect(g_menu, NULL, FALSE);
}

/* ---- Alt+Tab: the windows, most recently used first; Tab moves on, Shift+Tab
 * back, releasing Alt switches to the chosen one, Esc cancels ---- */
#define SWITCH_W        480
#define SWITCH_ROW      40
#define SWITCH_MAX      14
enum { SWITCH_OPEN, SWITCH_OPEN_BACK, SWITCH_NEXT, SWITCH_PREV, SWITCH_GO, SWITCH_CANCEL };
static HWND g_switcher;
static HWND g_switch_list[SWITCH_MAX];
static int g_switch_n, g_switch_sel;

static BOOL CALLBACK collect_switch(HWND hwnd, LPARAM lp)
{
    if (g_switch_n < SWITCH_MAX && is_task_window(hwnd))
        g_switch_list[g_switch_n++] = hwnd;
    return TRUE;
}

static void switcher_show(int step)
{
    HWND front = GetForegroundWindow(), list[SWITCH_MAX];
    int h, i, n = 0, pass;

    g_switch_n = 0;
    EnumWindows(collect_switch, 0);     /* top first: the most recently used */
    if (!g_switch_n)
        return;
    /* as on Windows: the window in front first, minimized ones last */
    for (pass = 0; pass < 3; pass++)
        for (i = 0; i < g_switch_n; i++) {
            HWND w = g_switch_list[i];

            if (pass == 0 ? w == front : w != front && !IsIconic(w) == (pass == 1))
                list[n++] = w;
        }
    memcpy(g_switch_list, list, n * sizeof(HWND));
    g_switch_sel = g_switch_n == 1 ? 0 : step > 0 ? 1 : g_switch_n - 1;
    h = 16 + g_switch_n * SWITCH_ROW;
    SetWindowPos(g_switcher, HWND_TOPMOST, (g_sw - SWITCH_W) / 2, (g_sh - h) / 2, SWITCH_W, h,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);
    InvalidateRect(g_switcher, NULL, FALSE);
}

static void switcher_step(int step)
{
    if (!g_switch_n)
        return;
    g_switch_sel = (g_switch_sel + step + g_switch_n) % g_switch_n;
    InvalidateRect(g_switcher, NULL, FALSE);
}

static void switcher_close(BOOL go)
{
    HWND target = g_switch_n ? g_switch_list[g_switch_sel] : NULL;

    ShowWindow(g_switcher, SW_HIDE);
    g_switch_n = 0;
    if (go && target && IsWindow(target)) {
        /* The keys went to the program in front, so only it may hand the
         * front over (Wine keeps Windows' foreground lock); sharing its input
         * for the moment of the switch makes the request its own. */
        HWND front = GetForegroundWindow();
        DWORD me = GetCurrentThreadId(), owner = front ? GetWindowThreadProcessId(front, NULL) : 0;
        BOOL attached = owner && owner != me && AttachThreadInput(me, owner, TRUE);

        if (IsIconic(target))
            ShowWindowAsync(target, SW_RESTORE);
        SetForegroundWindow(target);
        if (attached)
            AttachThreadInput(me, owner, FALSE);
    }
}

static void paint_switcher(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc, row;
    WCHAR title[256];
    HICON icon;
    int i;

    GetClientRect(hwnd, &rc);
    fill(dc, &rc, C_MENU);
    for (i = 0; i < g_switch_n; i++) {
        SetRect(&row, 8, 8 + i * SWITCH_ROW, rc.right - 8, 8 + (i + 1) * SWITCH_ROW);
        if (i == g_switch_sel) {
            fill(dc, &row, C_ACTIVE);
            fill_xywh(dc, row.left, row.top + 6, 3, SWITCH_ROW - 12, C_ACCENT);
        }
        if ((icon = window_icon(g_switch_list[i])))
            DrawIconEx(dc, row.left + 14, row.top + (SWITCH_ROW - 24) / 2, icon, 24, 24, 0, NULL, DI_NORMAL);
        GetWindowTextW(g_switch_list[i], title, ARRAYSIZE(title));
        row.left += 50;
        row.right -= 10;
        text(dc, title, &row, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS,
             i == g_switch_sel ? g_font_bold : g_font, C_TEXT);
    }
    SetRect(&rc, 0, 0, rc.right, rc.bottom);
    FrameRect(dc, &rc, (HBRUSH)GetStockObject(GRAY_BRUSH));
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK switcher_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint_switcher(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK bar_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg >= WM_MOUSEMOVE && msg <= WM_MBUTTONDBLCLK) {
        int hit = bar_hit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));

        if (IS_TRAY_HIT(hit)) {
            tray_mouse(TRAY_INDEX(hit), msg);
            if (msg != WM_MOUSEMOVE)    /* which also lights the icon up, below */
                return 0;
        }
    }
    switch (msg) {
    case WM_COPYDATA:
        return tray_message((const COPYDATASTRUCT *)lp);
    case WM_CREATE:
        SetTimer(hwnd, TIMER_TASKS, 1000, NULL);    /* safety net; events do the work */
        SetTimer(hwnd, TIMER_CLOCK, g_prefs.seconds ? 1000 : 5000, NULL);
        RegisterHotKey(hwnd, HK_RUN, MOD_WIN, 'R');
        RegisterHotKey(hwnd, HK_FILES, MOD_WIN, 'E');
        RegisterHotKey(hwnd, HK_DESKTOP, MOD_WIN, 'D');
        RegisterHotKey(hwnd, HK_START, MOD_CONTROL, VK_ESCAPE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint_bar(hwnd);
        return 0;
    case WM_WINDOWS_CHANGED:
        /* Window events come in bursts; coalesce them. */
        SetTimer(hwnd, TIMER_EVENT, 30, NULL);
        return 0;
    case WM_TIMER:
        if (wp == TIMER_EVENT) {
            KillTimer(hwnd, TIMER_EVENT);
            refresh_tasks();
        } else if (wp == TIMER_TASKS) {
            refresh_tasks();
            tray_prune();
        } else if (wp == TIMER_BALLOON) {
            balloon_close(NIN_BALLOONTIMEOUT);
        } else if (wp == TIMER_AUTOHIDE) {
            POINT pt;

            KillTimer(hwnd, TIMER_AUTOHIDE);
            GetCursorPos(&pt);
            if (g_prefs.autohide && !IsWindowVisible(g_menu) && g_balloon_of < 0 &&
                (g_bar_top ? pt.y >= BAR_INNER_Y : pt.y < BAR_INNER_Y))
                place_bar(TRUE);
        } else if (wp == TIMER_PREFS) {
            KillTimer(hwnd, TIMER_PREFS);
            apply_prefs();
        } else {
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_MOUSEMOVE: {
        int hot = bar_hit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));

        if (g_bar_hidden)
            place_bar(FALSE);
        KillTimer(hwnd, TIMER_AUTOHIDE);

        if (!g_tracking) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };

            g_tracking = TrackMouseEvent(&tme);
        }
        if (hot != g_bar_hot) {
            g_bar_hot = hot;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_tracking = FALSE;
        g_bar_hot = -1;
        InvalidateRect(hwnd, NULL, FALSE);
        autohide_later();
        return 0;
    case WM_PREFS_CHANGED:
    case WM_SETTINGCHANGE:
        /* changes come in bursts (one value at a time): apply them once */
        SetTimer(hwnd, TIMER_PREFS, 150, NULL);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        int hit = bar_hit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));

        if (hit == -2)
            PostMessageW(hwnd, WM_TOGGLE_START, 0, 0);
        else if (hit == -3)
            show_desktop();
        else if (hit >= 0)
            activate_task(g_tasks[hit].hwnd);
        return 0;
    }
    case WM_MBUTTONUP: {
        int hit = bar_hit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));

        if (hit >= 0)
            PostMessageW(g_tasks[hit].hwnd, WM_SYSCOMMAND, SC_CLOSE, 0);
        return 0;
    }
    case WM_RBUTTONUP: {
        int hit = bar_hit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));

        if (hit >= 0)
            task_menu(hwnd, g_tasks[hit].hwnd, GET_X_LPARAM(lp));
        else if (hit == -2)
            start_menu_context(hwnd);
        return 0;
    }
    case WM_TOGGLE_START:
        if (IsWindowVisible(g_menu))
            hide_menu();
        else
            show_menu();
        return 0;
    case WM_SWITCHER:
        switch (wp) {
        case SWITCH_OPEN:       switcher_show(1); break;
        case SWITCH_OPEN_BACK:  switcher_show(-1); break;
        case SWITCH_NEXT:       switcher_step(1); break;
        case SWITCH_PREV:       switcher_step(-1); break;
        case SWITCH_GO:         switcher_close(TRUE); break;
        case SWITCH_CANCEL:     switcher_close(FALSE); break;
        }
        return 0;
    case WM_SHELL_CMD:
    case WM_HOTKEY:
        switch (wp) {
        case HK_RUN:     run_dialog(); break;
        case HK_FILES:   system_tool(L"explorer.exe", NULL); break;
        case HK_DESKTOP: show_desktop(); break;
        case HK_START:   PostMessageW(hwnd, WM_TOGGLE_START, 0, 0); break;
        }
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_CLOSE:
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* Windows appearing, disappearing, renamed or activated, in any process. */
static void CALLBACK win_event(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG object,
                               LONG child, DWORD thread, DWORD time)
{
    if (object == OBJID_WINDOW && child == CHILDID_SELF && g_bar)
        PostMessageW(g_bar, WM_WINDOWS_CHANGED, 0, 0);
}

/* A Ctrl press and release, so that the program that had Alt down does not
 * take the lone Alt release that ends Alt+Tab for "open the menu bar". */
static void tap_ctrl(void)
{
    INPUT in[2];

    ZeroMemory(in, sizeof(in));
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[1].ki.wVk = VK_CONTROL;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

/* A lone press of the Windows key opens the Start menu; Alt+Tab switches
 * windows. The hook only decides and leaves the work to the taskbar, so as
 * not to hold up the keyboard. */
static LRESULT CALLBACK kbd_hook(int code, WPARAM wp, LPARAM lp)
{
    static BOOL win_down, combo, alt_down, shift_down, switching;

    if (code == HC_ACTION) {
        const KBDLLHOOKSTRUCT *k = (const KBDLLHOOKSTRUCT *)lp;
        BOOL is_win = k->vkCode == VK_LWIN || k->vkCode == VK_RWIN;
        BOOL down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;

        if (!(k->flags & LLKHF_INJECTED)) {
            if (k->vkCode == VK_LMENU || k->vkCode == VK_RMENU || k->vkCode == VK_MENU) {
                alt_down = down;
                if (!down && switching) {
                    switching = FALSE;
                    PostMessageW(g_bar, WM_SWITCHER, SWITCH_GO, 0);
                }
            } else if (k->vkCode == VK_LSHIFT || k->vkCode == VK_RSHIFT || k->vkCode == VK_SHIFT) {
                shift_down = down;
            } else if (k->vkCode == VK_TAB && alt_down) {
                if (down) {
                    if (!switching) {
                        switching = TRUE;
                        tap_ctrl();
                        PostMessageW(g_bar, WM_SWITCHER, shift_down ? SWITCH_OPEN_BACK : SWITCH_OPEN, 0);
                    } else {
                        PostMessageW(g_bar, WM_SWITCHER, shift_down ? SWITCH_PREV : SWITCH_NEXT, 0);
                    }
                }
                return 1;                   /* the program does not see this Tab */
            } else if (k->vkCode == VK_ESCAPE && switching) {
                if (down) {
                    switching = FALSE;
                    PostMessageW(g_bar, WM_SWITCHER, SWITCH_CANCEL, 0);
                }
                return 1;
            }
        }

        if (down) {
            if (is_win && !win_down) {
                win_down = TRUE;
                combo = FALSE;
            } else if (!is_win && win_down) {
                combo = TRUE;
            }
        } else if (is_win) {
            if (win_down && !combo)
                PostMessageW(g_bar, WM_TOGGLE_START, 0, 0);
            win_down = FALSE;
        }
    }
    return CallNextHookEx(g_kbd_hook, code, wp, lp);
}

/* ======================================================================
 * "Download a Program": the way to get the first programs onto a system
 * that ships without a web browser.
 * ====================================================================== */

#define FIREFOX_URL L"https://download.mozilla.org/?product=firefox-latest-ssl&os=win64&lang=en-US"
enum { IDC_URL = 200, IDC_GO, IDC_FIREFOX, IDC_STATUS };
#define WM_DOWNLOAD_DONE (WM_APP + 10)

static HWND g_dl_wnd;
static WCHAR g_dl_url[2048], g_dl_file[MAX_PATH];
static volatile LONG g_dl_cancel;

/* Minimal IBindStatusCallback: only progress reporting and cancellation. */
static HRESULT WINAPI dl_QueryInterface(IBindStatusCallback *iface, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IBindStatusCallback)) {
        *out = iface;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI dl_AddRef(IBindStatusCallback *iface) { return 2; }
static ULONG WINAPI dl_Release(IBindStatusCallback *iface) { return 1; }
static HRESULT WINAPI dl_OnStartBinding(IBindStatusCallback *iface, DWORD r, IBinding *b) { return S_OK; }
static HRESULT WINAPI dl_GetPriority(IBindStatusCallback *iface, LONG *p) { return E_NOTIMPL; }
static HRESULT WINAPI dl_OnLowResource(IBindStatusCallback *iface, DWORD r) { return S_OK; }
static HRESULT WINAPI dl_OnProgress(IBindStatusCallback *iface, ULONG done, ULONG total,
                                    ULONG code, LPCWSTR text)
{
    WCHAR buf[128];

    if (g_dl_cancel)
        return E_ABORT;
    if (total)
        wsprintfW(buf, L"Downloading... %lu.%lu of %lu.%lu MB", done >> 20,
                  (done & 0xFFFFF) * 10 >> 20, total >> 20, (total & 0xFFFFF) * 10 >> 20);
    else
        wsprintfW(buf, L"Downloading... %lu.%lu MB", done >> 20, (done & 0xFFFFF) * 10 >> 20);
    SetDlgItemTextW(g_dl_wnd, IDC_STATUS, buf);
    return S_OK;
}
static HRESULT WINAPI dl_OnStopBinding(IBindStatusCallback *iface, HRESULT hr, LPCWSTR e) { return S_OK; }
static HRESULT WINAPI dl_GetBindInfo(IBindStatusCallback *iface, DWORD *f, BINDINFO *i) { return S_OK; }
static HRESULT WINAPI dl_OnDataAvailable(IBindStatusCallback *iface, DWORD f, DWORD s,
                                         FORMATETC *fe, STGMEDIUM *m) { return S_OK; }
static HRESULT WINAPI dl_OnObjectAvailable(IBindStatusCallback *iface, REFIID r, IUnknown *u) { return S_OK; }

static IBindStatusCallbackVtbl dl_vtbl = {
    dl_QueryInterface, dl_AddRef, dl_Release, dl_OnStartBinding, dl_GetPriority,
    dl_OnLowResource, dl_OnProgress, dl_OnStopBinding, dl_GetBindInfo,
    dl_OnDataAvailable, dl_OnObjectAvailable,
};
static IBindStatusCallback dl_callback = { &dl_vtbl };

/* Pick a file name in Downloads for a URL. */
static void download_target(const WCHAR *url, WCHAR *out)
{
    WCHAR dir[MAX_PATH], name[128] = L"";
    const WCHAR *p, *end = url + lstrlenW(url), *slash;

    for (p = url; *p; p++)
        if (*p == L'?' || *p == L'#') {
            end = p;
            break;
        }
    for (slash = end; slash > url && slash[-1] != L'/'; slash--)
        ;
    if (end - slash > 0 && end - slash < (int)ARRAYSIZE(name))
        lstrcpynW(name, slash, (int)(end - slash) + 1);
    if (!lstrcmpiW(url, FIREFOX_URL))
        lstrcpyW(name, L"Firefox Setup.exe");
    else if (lstrcmpiW(PathFindExtensionW(name), L".exe") &&
             lstrcmpiW(PathFindExtensionW(name), L".msi") &&
             lstrcmpiW(PathFindExtensionW(name), L".zip"))
        lstrcpyW(name, L"setup.exe");
    for (p = name; *p; p++)
        if (wcschr(L"\\/:*?\"<>|%", *p))
            name[p - name] = L'_';

    if (FAILED(SHGetFolderPathW(NULL, CSIDL_PROFILE, NULL, SHGFP_TYPE_CURRENT, dir)))
        GetTempPathW(MAX_PATH, dir);
    PathAppendW(dir, L"Downloads");
    SHCreateDirectoryExW(NULL, dir, NULL);
    PathCombineW(out, dir, name);
}

static DWORD WINAPI download_thread(void *arg)
{
    HRESULT hr;

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    DeleteFileW(g_dl_file);
    hr = URLDownloadToFileW(NULL, g_dl_url, g_dl_file, 0, &dl_callback);
    CoUninitialize();
    PostMessageW(g_dl_wnd, WM_DOWNLOAD_DONE, (WPARAM)hr, 0);
    return 0;
}

static LRESULT CALLBACK download_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    static HANDLE thread;

    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_FIREFOX:
            SetDlgItemTextW(hwnd, IDC_URL, FIREFOX_URL);
            return 0;
        case IDC_GO:
        case IDOK:
            if (thread)
                return 0;
            GetDlgItemTextW(hwnd, IDC_URL, g_dl_url, ARRAYSIZE(g_dl_url));
            StrTrimW(g_dl_url, L" \t\r\n");
            if (StrCmpNIW(g_dl_url, L"http://", 7) && StrCmpNIW(g_dl_url, L"https://", 8)) {
                SetDlgItemTextW(hwnd, IDC_STATUS, L"Enter an address starting with https://");
                return 0;
            }
            download_target(g_dl_url, g_dl_file);
            g_dl_cancel = 0;
            EnableWindow(GetDlgItem(hwnd, IDC_GO), FALSE);
            EnableWindow(GetDlgItem(hwnd, IDC_FIREFOX), FALSE);
            SetDlgItemTextW(hwnd, IDC_STATUS, L"Connecting...");
            thread = CreateThread(NULL, 0, download_thread, NULL, 0, NULL);
            return 0;
        case IDCANCEL:
            SendMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        break;
    case WM_DOWNLOAD_DONE:
        if (thread) {
            CloseHandle(thread);
            thread = NULL;
        }
        EnableWindow(GetDlgItem(hwnd, IDC_GO), TRUE);
        EnableWindow(GetDlgItem(hwnd, IDC_FIREFOX), TRUE);
        if (SUCCEEDED((HRESULT)wp)) {
            SetDlgItemTextW(hwnd, IDC_STATUS, L"Done. Saved in Downloads; starting it now.");
            open_path(g_dl_file, NULL);
        } else {
            DeleteFileW(g_dl_file);
            SetDlgItemTextW(hwnd, IDC_STATUS, g_dl_cancel ? L"Cancelled."
                            : L"The download failed. Check the address and the network connection.");
        }
        return 0;
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    case WM_CLOSE:
        if (thread) {           /* stop the transfer first; closes on WM_DOWNLOAD_DONE */
            g_dl_cancel = 1;
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int download_tool(HINSTANCE inst)
{
    static const WCHAR *intro =
        L"WinCraft runs Windows programs. Paste the web address of a program or its "
        L"installer (.exe or .msi) and it will be downloaded to your Downloads folder "
        L"and started.";
    WNDCLASSEXW wc;
    HFONT font;
    HWND hwnd, ctl;
    RECT rc = { 0, 0, 520, 214 };
    MSG msg;
    int i;

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.hInstance = inst;
    wc.lpfnWndProc = download_proc;
    wc.lpszClassName = L"WinCraftDownload";
    wc.hCursor = LoadCursorW(NULL, (const WCHAR *)IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassExW(&wc);

    AdjustWindowRect(&rc, WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    hwnd = g_dl_wnd = CreateWindowExW(0, wc.lpszClassName, L"Download a Program",
                                      WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                      (GetSystemMetrics(SM_CXSCREEN) - (rc.right - rc.left)) / 2,
                                      (GetSystemMetrics(SM_CYSCREEN) - (rc.bottom - rc.top)) / 3,
                                      rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, inst, NULL);
    if (!hwnd)
        return 1;
    font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    {
        const struct { const WCHAR *cls, *text; DWORD style; int x, y, w, h, id; } items[] = {
            { L"STATIC", intro, 0, 16, 14, 488, 52, 0 },
            { L"STATIC", L"Address:", 0, 16, 78, 60, 20, 0 },
            { L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 78, 75, 426, 24, IDC_URL },
            { L"STATIC", L"", 0, 16, 112, 488, 36, IDC_STATUS },
            { L"BUTTON", L"Get Firefox (web browser)", WS_TABSTOP, 16, 170, 190, 30, IDC_FIREFOX },
            { L"BUTTON", L"Download and run", WS_TABSTOP | BS_DEFPUSHBUTTON, 276, 170, 140, 30, IDC_GO },
            { L"BUTTON", L"Close", WS_TABSTOP, 424, 170, 80, 30, IDCANCEL },
        };

        for (i = 0; i < (int)ARRAYSIZE(items); i++) {
            ctl = CreateWindowExW(0, items[i].cls, items[i].text, WS_CHILD | WS_VISIBLE | items[i].style,
                                  items[i].x, items[i].y, items[i].w, items[i].h, hwnd,
                                  (HMENU)(INT_PTR)items[i].id, inst, NULL);
            SendMessageW(ctl, WM_SETFONT, (WPARAM)font, TRUE);
        }
    }
    ShowWindow(hwnd, SW_SHOW);
    SetFocus(GetDlgItem(hwnd, IDC_URL));
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (IsDialogMessageW(hwnd, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

/* ======================================================================
 * "Install WinCraft OS": the front end of /usr/lib/wincraft/priv.d/install.
 * It only chooses a disk and shows progress; the work is done by the system.
 * ====================================================================== */

enum { IDC_DISKS = 300, IDC_INSTALL, IDC_INSTALL_STATUS, IDC_SPACE, IDC_SPACE_NOTE, IDC_DATA_NAME };
#define UNIX_ROOT L"\\\\?\\unix"

struct disk {
    WCHAR name[32];
    WCHAR label[160];
    unsigned gb;            /* size in GB of 10^9 bytes, as shown */
};

static struct disk g_disks[16];
static int g_ndisks;
static BOOL g_installing;

/* Read a small text file from the host side (sysfs, /run). */
static BOOL read_native_file(const WCHAR *unix_path, char *buf, DWORD size)
{
    WCHAR path[MAX_PATH], *p;
    DWORD got = 0;
    HANDLE h;

    wsprintfW(path, UNIX_ROOT L"%s", unix_path);
    for (p = path; *p; p++)
        if (*p == L'/')
            *p = L'\\';
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return FALSE;
    ReadFile(h, buf, size - 1, &got, NULL);
    CloseHandle(h);
    buf[got] = 0;
    while (got && (buf[got - 1] == '\n' || buf[got - 1] == ' '))
        buf[--got] = 0;
    return TRUE;
}

static void list_disks(void)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(UNIX_ROOT L"\\sys\\block\\*", &fd);

    g_ndisks = 0;
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        WCHAR path[128];
        char text[128], model[128] = "";
        unsigned long long sectors;
        struct disk *d = &g_disks[g_ndisks];

        if (fd.cFileName[0] == L'.' || !StrCmpNW(fd.cFileName, L"loop", 4) ||
            !StrCmpNW(fd.cFileName, L"ram", 3) || !StrCmpNW(fd.cFileName, L"zram", 4) ||
            !StrCmpNW(fd.cFileName, L"sr", 2) || !StrCmpNW(fd.cFileName, L"fd", 2) ||
            !StrCmpNW(fd.cFileName, L"dm-", 3))
            continue;
        wsprintfW(path, L"/sys/block/%s/size", fd.cFileName);
        if (!read_native_file(path, text, sizeof(text)))
            continue;
        sectors = _strtoui64(text, NULL, 10);
        if (sectors < 8388608)          /* under 4 GB: not a candidate */
            continue;
        wsprintfW(path, L"/sys/block/%s/device/model", fd.cFileName);
        read_native_file(path, model, sizeof(model));
        lstrcpynW(d->name, fd.cFileName, ARRAYSIZE(d->name));
        d->gb = (unsigned)(sectors * 512 / 1000000000ULL);
        wsprintfW(d->label, L"%s   -   %S   -   %u GB", fd.cFileName, model[0] ? model : "disk", d->gb);
        g_ndisks++;
    } while (g_ndisks < (int)ARRAYSIZE(g_disks) && FindNextFileW(h, &fd));
    FindClose(h);
}

static LRESULT CALLBACK install_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_DISKS && HIWORD(wp) == LBN_SELCHANGE) {
            int sel = (int)SendDlgItemMessageW(hwnd, IDC_DISKS, LB_GETCURSEL, 0, 0);
            WCHAR text[96];

            if (sel >= 0 && sel < g_ndisks) {      /* the whole disk, unless told otherwise */
                SetDlgItemInt(hwnd, IDC_SPACE, g_disks[sel].gb, FALSE);
                wsprintfW(text, L"GB of %u GB. The rest of the disk stays empty.", g_disks[sel].gb);
                SetDlgItemTextW(hwnd, IDC_SPACE_NOTE, text);
            }
            return 0;
        }
        if (LOWORD(wp) == IDC_INSTALL && !g_installing) {
            int sel = (int)SendDlgItemMessageW(hwnd, IDC_DISKS, LB_GETCURSEL, 0, 0);
            WCHAR text[640], data_name[24], space_text[96];
            unsigned gb, mib;
            BOOL ok;
            int i;

            if (sel < 0 || sel >= g_ndisks) {
                SetDlgItemTextW(hwnd, IDC_INSTALL_STATUS, L"Choose the disk to install to.");
                return 0;
            }
            gb = GetDlgItemInt(hwnd, IDC_SPACE, &ok, FALSE);
            if (!ok || gb < 5 || gb > g_disks[sel].gb) {
                wsprintfW(text, L"The space for WinCraft must be between 5 and %u GB.", g_disks[sel].gb);
                SetDlgItemTextW(hwnd, IDC_INSTALL_STATUS, text);
                return 0;
            }
            GetDlgItemTextW(hwnd, IDC_DATA_NAME, data_name, ARRAYSIZE(data_name));
            for (i = 0; data_name[i]; i++)
                if (!iswalnum(data_name[i]) || data_name[i] > 127)
                    if (data_name[i] != L'-' && data_name[i] != L'_')
                        break;
            if (!data_name[0] || data_name[i] || i > 16) {
                SetDlgItemTextW(hwnd, IDC_INSTALL_STATUS, L"The partition name may use letters, digits, - "
                                L"and _, 16 at most.");
                return 0;
            }
            /* all of the disk, or that many GB of 10^9 bytes */
            mib = gb >= g_disks[sel].gb ? 0 : (unsigned)((unsigned long long)gb * 1000000000ULL / 1048576);
            if (mib)
                wsprintfW(space_text, L"WinCraft takes %u GB of it; the rest stays empty.", gb);
            else
                lstrcpyW(space_text, L"WinCraft takes all of it.");
            wsprintfW(text, L"Install WinCraft OS on this disk?\n\n%s\n\n%s Its data partition is "
                      L"named %s.\n\nEVERYTHING on this disk will be erased: all partitions, any "
                      L"operating system and all files. This cannot be undone.",
                      g_disks[sel].label, space_text, data_name);
            if (MessageBoxW(hwnd, text, L"Erase disk and install",
                            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
                return 0;
            wsprintfW(text, L"/usr/bin/wcctl priv install %s %u %s", g_disks[sel].name, mib, data_name);
            if (!run_native(text, FALSE)) {
                SetDlgItemTextW(hwnd, IDC_INSTALL_STATUS, L"The system service did not respond.");
                return 0;
            }
            g_installing = TRUE;
            for (i = IDC_DISKS; i <= IDC_DATA_NAME; i++)
                if (i != IDC_INSTALL_STATUS && i != IDC_SPACE_NOTE)
                    EnableWindow(GetDlgItem(hwnd, i), FALSE);
            SetDlgItemTextW(hwnd, IDC_INSTALL_STATUS, L"Starting...");
            SetTimer(hwnd, 1, 700, NULL);
        } else if (LOWORD(wp) == IDCANCEL) {
            SendMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        return 0;
    case WM_TIMER: {
        char buf[2048], *line;
        WCHAR text[300];
        int i;

        if (!read_native_file(L"/run/wincraft/install.status", buf, sizeof(buf)) || !buf[0])
            return 0;
        line = strrchr(buf, '\n');
        line = line ? line + 1 : buf;
        if (!strcmp(line, "done")) {
            KillTimer(hwnd, 1);
            g_installing = FALSE;
            SetDlgItemTextW(hwnd, IDC_INSTALL_STATUS, L"WinCraft OS is installed.");
            if (MessageBoxW(hwnd, L"WinCraft OS is installed. Restart now to start it from the "
                            L"disk?\n\nAn installation CD is ejected as the computer restarts. A USB "
                            L"stick can stay in: WinCraft starts from the disk it is installed on. "
                            L"If the computer starts the stick again, remove it or choose the disk "
                            L"in the computer's boot menu.",
                            L"Installation complete", MB_YESNO | MB_ICONINFORMATION) == IDYES)
                run_native(L"/usr/lib/wincraft/end-session reboot", FALSE);
            DestroyWindow(hwnd);
        } else if (!strncmp(line, "error:", 6)) {
            KillTimer(hwnd, 1);
            g_installing = FALSE;
            wsprintfW(text, L"Installation failed: %S", line + 7);
            SetDlgItemTextW(hwnd, IDC_INSTALL_STATUS, text);
            for (i = IDC_DISKS; i <= IDC_DATA_NAME; i++)
                EnableWindow(GetDlgItem(hwnd, i), TRUE);
        } else {
            wsprintfW(text, L"%S...", line);
            SetDlgItemTextW(hwnd, IDC_INSTALL_STATUS, text);
        }
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    case WM_CLOSE:
        if (g_installing)
            return 0;           /* never abandon a disk half written */
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int install_tool(HINSTANCE inst)
{
    static const WCHAR *intro =
        L"This copies WinCraft OS to a disk so the computer starts it directly, and your "
        L"programs and files are kept between sessions.\n\nThe disk you choose is erased "
        L"completely.";
    WNDCLASSEXW wc;
    RECT rc = { 0, 0, 520, 368 };
    char probe[8];
    HFONT font;
    HWND hwnd, ctl;
    MSG msg;
    int i;

    if (!read_native_file(L"/run/wincraft/live", probe, sizeof(probe))) {
        MessageBoxW(NULL, L"WinCraft OS is already installed on this computer.\n\n"
                    L"To install it on another disk, start from the installation medium.",
                    L"Install WinCraft OS", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
        return 0;
    }
    list_disks();

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.hInstance = inst;
    wc.lpfnWndProc = install_proc;
    wc.lpszClassName = L"WinCraftInstall";
    wc.hCursor = LoadCursorW(NULL, (const WCHAR *)IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassExW(&wc);

    AdjustWindowRect(&rc, WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    hwnd = CreateWindowExW(0, wc.lpszClassName, L"Install WinCraft OS",
                           WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                           (GetSystemMetrics(SM_CXSCREEN) - (rc.right - rc.left)) / 2,
                           (GetSystemMetrics(SM_CYSCREEN) - (rc.bottom - rc.top)) / 3,
                           rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, inst, NULL);
    if (!hwnd)
        return 1;
    font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    {
        const struct { const WCHAR *cls, *text; DWORD style; int x, y, w, h, id; } items[] = {
            { L"STATIC", intro, 0, 16, 14, 488, 70, 0 },
            { L"STATIC", L"Install to:", 0, 16, 90, 200, 18, 0 },
            { L"LISTBOX", L"", WS_BORDER | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY, 16, 110, 488, 96, IDC_DISKS },
            { L"STATIC", L"Space for WinCraft:", 0, 16, 222, 132, 18, 0 },
            { L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_NUMBER, 150, 218, 56, 22, IDC_SPACE },
            { L"STATIC", L"GB", 0, 214, 222, 290, 18, IDC_SPACE_NOTE },
            { L"STATIC", L"Data partition name:", 0, 16, 254, 132, 18, 0 },
            { L"EDIT", L"WINCRAFT_DATA", WS_BORDER | WS_TABSTOP, 150, 250, 160, 22, IDC_DATA_NAME },
            { L"STATIC", g_ndisks ? L"" : L"No disk of at least 4 GB was found.", 0, 16, 284, 488, 34, IDC_INSTALL_STATUS },
            { L"BUTTON", L"Erase disk and install", WS_TABSTOP, 250, 324, 166, 30, IDC_INSTALL },
            { L"BUTTON", L"Close", WS_TABSTOP, 424, 324, 80, 30, IDCANCEL },
        };

        for (i = 0; i < (int)ARRAYSIZE(items); i++) {
            ctl = CreateWindowExW(0, items[i].cls, items[i].text, WS_CHILD | WS_VISIBLE | items[i].style,
                                  items[i].x, items[i].y, items[i].w, items[i].h, hwnd,
                                  (HMENU)(INT_PTR)items[i].id, inst, NULL);
            SendMessageW(ctl, WM_SETFONT, (WPARAM)font, TRUE);
        }
    }
    for (i = 0; i < g_ndisks; i++)
        SendDlgItemMessageW(hwnd, IDC_DISKS, LB_ADDSTRING, 0, (LPARAM)g_disks[i].label);
    SendDlgItemMessageW(hwnd, IDC_DATA_NAME, EM_LIMITTEXT, 16, 0);
    if (g_ndisks == 1) {            /* the only choice; the confirmation still asks */
        SendDlgItemMessageW(hwnd, IDC_DISKS, LB_SETCURSEL, 0, 0);
        SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDC_DISKS, LBN_SELCHANGE), 0);
    }
    EnableWindow(GetDlgItem(hwnd, IDC_INSTALL), g_ndisks > 0);
    ShowWindow(hwnd, SW_SHOW);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (IsDialogMessageW(hwnd, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

/* ======================================================================
 * "WinCraft Features": the features added to this computer, which fill a gap
 * of WinCraft's (README, "Features"). Each is a folder of root's under
 * /var/lib/wincraft/features; this window lists them and has
 * /usr/lib/wincraft/priv.d/feature enable, disable or remove one.
 * ====================================================================== */

enum { IDC_FEAT_LIST = 500, IDC_FEAT_INFO, IDC_FEAT_TOGGLE, IDC_FEAT_REMOVE, IDC_FEAT_STATUS };

#define FEATURES_UNIX   L"/var/lib/wincraft/features"

struct feature {
    WCHAR id[48];
    WCHAR name[96];
    WCHAR info[1600];
    BOOL enabled;
};

static struct feature g_feats[64];
static int g_nfeats;
static BOOL g_feat_busy;

/* The value of "key=" in a feature.conf, as text for the window. */
static void feat_value(const char *conf, const char *key, WCHAR *out, int size)
{
    size_t klen = strlen(key);
    const char *p = conf;

    out[0] = 0;
    while (p && *p) {
        if (!strncmp(p, key, klen) && p[klen] == '=') {
            char value[1024];
            size_t n = strcspn(p + klen + 1, "\r\n");

            if (n >= sizeof(value))
                n = sizeof(value) - 1;
            memcpy(value, p + klen + 1, n);
            value[n] = 0;
            MultiByteToWideChar(CP_UTF8, 0, value, -1, out, size);
            return;
        }
        p = strchr(p, '\n');
        if (p)
            p++;
    }
}

static void load_features(void)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(UNIX_ROOT L"\\var\\lib\\wincraft\\features\\*", &fd);

    g_nfeats = 0;
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        struct feature *f = &g_feats[g_nfeats];
        WCHAR path[MAX_PATH], desc[600], created[64], by[96], version[32], task[300];
        char conf[4096] = "";

        if (fd.cFileName[0] == L'.' || !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        lstrcpynW(f->id, fd.cFileName, ARRAYSIZE(f->id));
        wsprintfW(path, FEATURES_UNIX L"/%s/feature.conf", f->id);
        read_native_file(path, conf, sizeof(conf));
        feat_value(conf, "name", f->name, ARRAYSIZE(f->name));
        if (!f->name[0])
            lstrcpynW(f->name, f->id, ARRAYSIZE(f->name));
        feat_value(conf, "description", desc, ARRAYSIZE(desc));
        feat_value(conf, "created", created, ARRAYSIZE(created));
        feat_value(conf, "by", by, ARRAYSIZE(by));
        feat_value(conf, "version", version, ARRAYSIZE(version));
        feat_value(conf, "task", task, ARRAYSIZE(task));
        wsprintfW(path, UNIX_ROOT FEATURES_UNIX L"/%s/enabled", f->id);
        for (WCHAR *q = path; *q; q++)
            if (*q == L'/')
                *q = L'\\';
        f->enabled = GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
        /* the description, then where it comes from */
        lstrcpynW(f->info, desc[0] ? desc : L"(no description)", 700);
        lstrcatW(f->info, L"\r\n");
        if (created[0])
            wsprintfW(f->info + lstrlenW(f->info), L"\r\nAdded: %s", created);
        if (by[0])
            wsprintfW(f->info + lstrlenW(f->info), L"\r\nBy: %s", by);
        if (task[0])                /* the request a WinCraft Agent feature answers */
            wsprintfW(f->info + lstrlenW(f->info), L"\r\nAsked for: %s", task);
        if (version[0])
            wsprintfW(f->info + lstrlenW(f->info), L"\r\nMade on WinCraft OS %s", version);
        wsprintfW(f->info + lstrlenW(f->info), L"\r\nFolder: %s/%s", FEATURES_UNIX, f->id);
        g_nfeats++;
    } while (g_nfeats < (int)ARRAYSIZE(g_feats) && FindNextFileW(h, &fd));
    FindClose(h);
}

static void feat_select(HWND hwnd, int i)
{
    BOOL some = i >= 0 && i < g_nfeats;

    SetDlgItemTextW(hwnd, IDC_FEAT_INFO, some ? g_feats[i].info :
                    g_nfeats ? L"" : L"No features have been added to this computer.");
    SetDlgItemTextW(hwnd, IDC_FEAT_TOGGLE, some && g_feats[i].enabled ? L"Disable" : L"Enable");
    EnableWindow(GetDlgItem(hwnd, IDC_FEAT_TOGGLE), some && !g_feat_busy);
    EnableWindow(GetDlgItem(hwnd, IDC_FEAT_REMOVE), some && !g_feat_busy);
}

static void feat_fill(HWND hwnd, int sel)
{
    int i;

    load_features();
    SendDlgItemMessageW(hwnd, IDC_FEAT_LIST, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < g_nfeats; i++) {
        WCHAR line[160];

        wsprintfW(line, L"%s%s", g_feats[i].name, g_feats[i].enabled ? L"" : L"   (disabled)");
        SendDlgItemMessageW(hwnd, IDC_FEAT_LIST, LB_ADDSTRING, 0, (LPARAM)line);
    }
    if (sel >= g_nfeats)
        sel = g_nfeats - 1;
    SendDlgItemMessageW(hwnd, IDC_FEAT_LIST, LB_SETCURSEL, sel, 0);
    feat_select(hwnd, sel);
}

/* Ask the system to enable, disable or remove a feature; the timer follows. */
static void feat_run(HWND hwnd, const WCHAR *action, const struct feature *f)
{
    WCHAR cmd[160];

    wsprintfW(cmd, L"/usr/bin/wcctl priv feature %s %s", action, f->id);
    if (!run_native(cmd, FALSE)) {
        SetDlgItemTextW(hwnd, IDC_FEAT_STATUS, L"The system service did not respond.");
        return;
    }
    g_feat_busy = TRUE;
    feat_select(hwnd, (int)SendDlgItemMessageW(hwnd, IDC_FEAT_LIST, LB_GETCURSEL, 0, 0));
    SetDlgItemTextW(hwnd, IDC_FEAT_STATUS, L"Working...");
    SetTimer(hwnd, 1, 400, NULL);
}

static LRESULT CALLBACK features_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND: {
        int sel = (int)SendDlgItemMessageW(hwnd, IDC_FEAT_LIST, LB_GETCURSEL, 0, 0);
        const struct feature *f = sel >= 0 && sel < g_nfeats ? &g_feats[sel] : NULL;

        if (LOWORD(wp) == IDC_FEAT_LIST && HIWORD(wp) == LBN_SELCHANGE) {
            feat_select(hwnd, sel);
        } else if (LOWORD(wp) == IDC_FEAT_TOGGLE && f && !g_feat_busy) {
            feat_run(hwnd, f->enabled ? L"disable" : L"enable", f);
        } else if (LOWORD(wp) == IDC_FEAT_REMOVE && f && !g_feat_busy) {
            WCHAR text[400];

            wsprintfW(text, L"Remove the feature \"%s\"?\n\nIts files are deleted from this computer; this "
                      L"cannot be undone. Windows registry settings it made stay as they are.", f->name);
            if (MessageBoxW(hwnd, text, L"Remove feature", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES)
                feat_run(hwnd, L"remove", f);
        } else if (LOWORD(wp) == IDCANCEL) {
            SendMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        return 0;
    }
    case WM_TIMER: {
        char buf[1024], *line;
        WCHAR text[300];

        if (!read_native_file(L"/run/wincraft/feature.status", buf, sizeof(buf)) || !buf[0])
            return 0;
        line = strrchr(buf, '\n');
        line = line ? line + 1 : buf;
        if (!strcmp(line, "done")) {
            KillTimer(hwnd, 1);
            g_feat_busy = FALSE;
            feat_fill(hwnd, (int)SendDlgItemMessageW(hwnd, IDC_FEAT_LIST, LB_GETCURSEL, 0, 0));
            SetDlgItemTextW(hwnd, IDC_FEAT_STATUS, L"Done. The change takes full effect when the computer "
                            L"restarts.");
        } else if (!strncmp(line, "error:", 6)) {
            KillTimer(hwnd, 1);
            g_feat_busy = FALSE;
            wsprintfW(text, L"That did not work: %S", line + 7);
            SetDlgItemTextW(hwnd, IDC_FEAT_STATUS, text);
            feat_fill(hwnd, (int)SendDlgItemMessageW(hwnd, IDC_FEAT_LIST, LB_GETCURSEL, 0, 0));
        }
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == GetDlgItem(hwnd, IDC_FEAT_INFO)) {     /* read-only, but white */
            SetBkColor((HDC)wp, GetSysColor(COLOR_WINDOW));
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        }
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    case WM_CLOSE:
        if (g_feat_busy)
            return 0;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int features_tool(HINSTANCE inst)
{
    static const WCHAR *intro =
        L"Features add to WinCraft OS what it does not do yet, such as a setting it lacks. They are kept on "
        L"this computer and survive updates. A feature that causes trouble can be disabled or removed here; "
        L"the boot menu's \"without your features\" entry starts without any of them.";
    WNDCLASSEXW wc;
    RECT rc = { 0, 0, 560, 400 };
    HFONT font;
    HWND hwnd, ctl;
    MSG msg;
    int i;

    if (CreateMutexW(NULL, TRUE, L"WinCraftFeatures") && GetLastError() == ERROR_ALREADY_EXISTS)
        return 0;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.hInstance = inst;
    wc.lpfnWndProc = features_proc;
    wc.lpszClassName = L"WinCraftFeatures";
    wc.hCursor = LoadCursorW(NULL, (const WCHAR *)IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassExW(&wc);
    AdjustWindowRect(&rc, WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    hwnd = CreateWindowExW(0, wc.lpszClassName, L"WinCraft Features", WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                           (GetSystemMetrics(SM_CXSCREEN) - (rc.right - rc.left)) / 2,
                           (GetSystemMetrics(SM_CYSCREEN) - (rc.bottom - rc.top)) / 3,
                           rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, inst, NULL);
    if (!hwnd)
        return 1;
    font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    {
        const struct { const WCHAR *cls, *text; DWORD style, exstyle; int x, y, w, h, id; } items[] = {
            { L"STATIC", intro, 0, 0, 16, 14, 528, 56, 0 },
            { L"LISTBOX", L"", WS_BORDER | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY, 0, 16, 78, 200, 250, IDC_FEAT_LIST },
            { L"EDIT", L"", ES_MULTILINE | ES_READONLY | WS_VSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE,
              226, 78, 318, 250, IDC_FEAT_INFO },
            { L"STATIC", L"", 0, 0, 16, 336, 528, 18, IDC_FEAT_STATUS },
            { L"BUTTON", L"Enable", WS_TABSTOP, 0, 226, 362, 100, 28, IDC_FEAT_TOGGLE },
            { L"BUTTON", L"Remove", WS_TABSTOP, 0, 334, 362, 100, 28, IDC_FEAT_REMOVE },
            { L"BUTTON", L"Close", WS_TABSTOP, 0, 444, 362, 100, 28, IDCANCEL },
        };

        for (i = 0; i < (int)ARRAYSIZE(items); i++) {
            ctl = CreateWindowExW(items[i].exstyle, items[i].cls, items[i].text, WS_CHILD | WS_VISIBLE | items[i].style,
                                  items[i].x, items[i].y, items[i].w, items[i].h, hwnd,
                                  (HMENU)(INT_PTR)items[i].id, inst, NULL);
            SendMessageW(ctl, WM_SETFONT, (WPARAM)font, TRUE);
        }
    }
    feat_fill(hwnd, 0);
    ShowWindow(hwnd, SW_SHOW);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (IsDialogMessageW(hwnd, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

/* ======================================================================
 * First-time setup: default shortcuts
 * ====================================================================== */

static HRESULT make_link(const WCHAR *folder, const WCHAR *name, const WCHAR *target,
                         const WCHAR *args, const WCHAR *desc, const WCHAR *icon, int icon_index)
{
    WCHAR path[MAX_PATH], file[128];
    IShellLinkW *link;
    IPersistFile *pf;
    HRESULT hr;

    hr = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW,
                          (void **)&link);
    if (FAILED(hr))
        return hr;
    IShellLinkW_SetPath(link, target);
    if (args)
        IShellLinkW_SetArguments(link, args);
    if (desc)
        IShellLinkW_SetDescription(link, desc);
    IShellLinkW_SetIconLocation(link, icon ? icon : target, icon ? icon_index : 0);
    hr = IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&pf);
    if (SUCCEEDED(hr)) {
        wsprintfW(file, L"%s.lnk", name);
        PathCombineW(path, folder, file);
        hr = IPersistFile_Save(pf, path, TRUE);
        IPersistFile_Release(pf);
    }
    IShellLinkW_Release(link);
    return hr;
}

/* Built-in tools live in different directories; find where one really is. */
static BOOL find_tool(const WCHAR *exe, WCHAR *out)
{
    static const WCHAR *dirs[] = {
        L"%SystemRoot%\\system32", L"%SystemRoot%",
        L"%ProgramFiles%\\Windows NT\\Accessories", L"%ProgramFiles%\\Internet Explorer",
    };
    unsigned i;

    for (i = 0; i < ARRAYSIZE(dirs); i++) {
        ExpandEnvironmentStringsW(dirs[i], out, MAX_PATH);
        PathAppendW(out, exe);
        if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES)
            return TRUE;
    }
    return FALSE;
}

static int setup(void)
{
    /* icon: 0 = the program's own, otherwise a stock shell32 resource id */
    static const struct { const WCHAR *name, *exe, *args, *desc; int icon; } tools[] = {
        { L"File Explorer",          L"explorer.exe", NULL,          L"Browse files and drives", 4 },
        { L"Notepad",                L"notepad.exe",  NULL,          L"Edit text files", 0 },
        { L"WordPad",                L"wordpad.exe",  NULL,          L"Write formatted documents", 0 },
        { L"Command Prompt",         L"cmd.exe",      NULL,          L"Windows command line", 0 },
        { L"Task Manager",           L"taskmgr.exe",  NULL,          L"See and end running programs", 0 },
        { L"Registry Editor",        L"regedit.exe",  NULL,          L"Edit the Windows registry", 0 },
        { L"Control Panel",          L"control.exe",  NULL,          L"System settings", 22 },
        { L"Add or Remove Programs", L"control.exe",  L"appwiz.cpl", L"Install and uninstall programs", 22 },
        { L"Compatibility Settings", L"winecfg.exe",  NULL,          L"Windows version, libraries, drives, audio", 0 },
        { L"Minesweeper",            L"winemine.exe", NULL,          L"A classic", 0 },
    };
    WCHAR programs[MAX_PATH], desktop[MAX_PATH], shell32[MAX_PATH], target[MAX_PATH];
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    unsigned i;
    int failed = 0;

    if (FAILED(hr))
        return 1;
    SHGetFolderPathW(NULL, CSIDL_COMMON_PROGRAMS, NULL, SHGFP_TYPE_CURRENT, programs);
    SHGetFolderPathW(NULL, CSIDL_COMMON_DESKTOPDIRECTORY, NULL, SHGFP_TYPE_CURRENT, desktop);
    SHCreateDirectoryExW(NULL, programs, NULL);
    SHCreateDirectoryExW(NULL, desktop, NULL);
    GetSystemDirectoryW(shell32, MAX_PATH);
    PathAppendW(shell32, L"shell32.dll");

    for (i = 0; i < ARRAYSIZE(tools); i++) {
        if (!find_tool(tools[i].exe, target) ||
            FAILED(make_link(programs, tools[i].name, target, tools[i].args, tools[i].desc,
                             tools[i].icon ? shell32 : NULL, -tools[i].icon)))
            failed++;
    }
    if (!find_tool(L"explorer.exe", target) ||
        FAILED(make_link(desktop, L"File Explorer", target, NULL, L"Browse files and drives",
                         shell32, -4)))
        failed++;
    /* the shell itself provides "Download a Program" */
    GetModuleFileNameW(NULL, target, MAX_PATH);
    if (FAILED(make_link(programs, L"Download a Program", target, L"/download",
                         L"Get Windows programs from the web", NULL, 0)) ||
        FAILED(make_link(desktop, L"Download a Program", target, L"/download",
                         L"Get Windows programs from the web", NULL, 0)) ||
        FAILED(make_link(programs, L"WinCraft Features", target, L"/features",
                         L"Features added to WinCraft OS on this computer", NULL, 0)) ||
        FAILED(make_link(programs, L"Install WinCraft OS", target, L"/install",
                         L"Install WinCraft OS on this computer", shell32, -9)) ||
        FAILED(make_link(desktop, L"Install WinCraft OS", target, L"/install",
                         L"Install WinCraft OS on this computer", shell32, -9)))
        failed++;
    /* WinCraft Assistant lives next to the shell. Its desktop icon sets it up;
       it adds its own Start menu entry once set up. */
    PathRemoveFileSpecW(target);
    PathAppendW(target, L"wcagent.exe");
    if (GetFileAttributesW(target) != INVALID_FILE_ATTRIBUTES &&
        FAILED(make_link(desktop, L"WinCraft Assistant", target, NULL,
                         L"Ask about WinCraft OS and this computer", NULL, 0)))
        failed++;
    CoUninitialize();
    return failed;
}

/* ======================================================================
 * Entry point
 * ====================================================================== */

static void apply_system_metrics(void)
{
    MINIMIZEDMETRICS mm;

    /* Minimised windows go to the taskbar, not to icons on the desktop. */
    mm.cbSize = sizeof(mm);
    if (SystemParametersInfoW(SPI_GETMINIMIZEDMETRICS, sizeof(mm), &mm, 0)) {
        mm.iArrange |= ARW_HIDE;
        SystemParametersInfoW(SPI_SETMINIMIZEDMETRICS, sizeof(mm), &mm, 0);
    }
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, WCHAR *cmdline, int show)
{
    WNDCLASSEXW wc;
    HANDLE mutex;
    MSG msg;

    if (StrStrIW(cmdline, L"/setup"))
        return setup();
    if (StrStrIW(cmdline, L"/download")) {
        InitCommonControls();
        return download_tool(inst);
    }
    if (StrStrIW(cmdline, L"/features")) {
        InitCommonControls();
        return features_tool(inst);
    }
    if (StrStrIW(cmdline, L"/install")) {
        InitCommonControls();
        return install_tool(inst);
    }

    mutex = CreateMutexW(NULL, TRUE, L"WinCraftShell");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
        return 0;

    g_inst = inst;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    InitCommonControls();
    g_sw = GetSystemMetrics(SM_CXSCREEN);
    g_sh = GetSystemMetrics(SM_CYSCREEN);
    {
        WCHAR buf[16];
        int h;

        if (GetEnvironmentVariableW(L"WINCRAFT_TASKBAR", buf, ARRAYSIZE(buf)) &&
            (h = StrToIntW(buf)) >= 32 && h <= 96)
            g_bar_h = h;
    }
    {
        char probe[8];

        g_hide_installer = !read_native_file(L"/run/wincraft/live", probe, sizeof(probe));
    }
    g_font = make_font(10, FW_NORMAL);
    g_font_bold = make_font(10, FW_SEMIBOLD);
    g_font_small = make_font(8, FW_NORMAL);
    g_font_large = make_font(13, FW_SEMIBOLD);

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (const WCHAR *)IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));

    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = desktop_proc;
    wc.lpszClassName = L"WinCraftDesktop";
    RegisterClassExW(&wc);
    wc.lpfnWndProc = bar_proc;
    wc.lpszClassName = L"Shell_TrayWnd";   /* as on Windows: programs' notification icons come here */
    RegisterClassExW(&wc);
    wc.style = CS_DROPSHADOW;
    wc.lpfnWndProc = menu_proc;
    wc.lpszClassName = L"WinCraftStartMenu";
    RegisterClassExW(&wc);
    wc.lpfnWndProc = switcher_proc;
    wc.lpszClassName = L"WinCraftSwitcher";
    RegisterClassExW(&wc);

    read_prefs(&g_prefs);
    g_bar_top = g_prefs.bar_top;
    set_palette();
    apply_system_metrics();
    make_wallpaper();
    load_icons();

    g_desktop = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"WinCraftDesktop", L"Desktop",
                                WS_POPUP | WS_CLIPCHILDREN, 0, 0, g_sw, g_sh,
                                NULL, NULL, inst, NULL);
    g_bar = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                            L"Shell_TrayWnd", L"Taskbar", WS_POPUP,
                            0, BAR_Y, g_sw, BAR_H, NULL, NULL, inst, NULL);
    g_menu = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"WinCraftStartMenu", L"Start",
                             WS_POPUP, 0, 0, MENU_W, 400, NULL, NULL, inst, NULL);
    g_switcher = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"WinCraftSwitcher",
                                 L"Switch windows", WS_POPUP, 0, 0, SWITCH_W, 100, NULL, NULL, inst, NULL);
    if (!g_desktop || !g_bar || !g_menu)
        return 1;

    SetWindowPos(g_desktop, HWND_BOTTOM, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ShowWindow(g_bar, SW_SHOWNOACTIVATE);
    tray_start(inst);
    autohide_later();
    write_wm_prefs();
    CloseHandle(CreateThread(NULL, 0, watch_prefs, NULL, 0, NULL));
    refresh_tasks();
    g_kbd_hook = SetWindowsHookExW(WH_KEYBOARD_LL, kbd_hook, inst, 0);
    SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_MINIMIZEEND, NULL, win_event, 0, 0,
                    WINEVENT_OUTOFCONTEXT);
    SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_NAMECHANGE, NULL, win_event, 0, 0,
                    WINEVENT_OUTOFCONTEXT);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_kbd_hook)
        UnhookWindowsHookEx(g_kbd_hook);
    CloseHandle(mutex);
    return 0;
}
