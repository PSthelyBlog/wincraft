/*
 * wclistwin - list the windows Windows programs show, with their text.
 *
 * For the WinCraft Agent's sandbox, which has a private screen but no way to
 * look at it: this prints each visible top-level window of the Wine session it
 * runs in (title, class, program, place and size), then the controls inside
 * it with their text, so a test can check what a feature shows.
 *
 *   wclistwin.exe [SECONDS]    wait up to SECONDS for a first window (default 0)
 *
 * Run it in the same Wine session (same user and WINEPREFIX) as the program
 * whose windows it should see; /usr/lib/wincraft/sandbox-display does that.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <stdarg.h>
#include <wchar.h>
#include <stdlib.h>

#define MAX_TOP      64
#define MAX_CHILDREN 60
#define MAX_TEXT     300

static HWND g_top[MAX_TOP];
static int g_ntop;

static void out(const WCHAR *s)
{
    char buf[4 * MAX_TEXT + 512];
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, buf, sizeof(buf) - 1, NULL, NULL);
    DWORD done;

    if (n > 0)
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), buf, (DWORD)(n - 1), &done, NULL);
}

static void outf(const WCHAR *fmt, ...)
{
    WCHAR line[MAX_TEXT + 256];
    va_list ap;

    va_start(ap, fmt);
    _vsnwprintf(line, ARRAYSIZE(line) - 1, fmt, ap);
    va_end(ap);
    line[ARRAYSIZE(line) - 1] = 0;
    out(line);
}

/* A window's text on one line: line breaks shown as " / ", cut at MAX_TEXT. */
static void window_text(HWND hwnd, WCHAR *text, int size)
{
    WCHAR *p;
    int n = (int)SendMessageTimeoutW(hwnd, WM_GETTEXT, size, (LPARAM)text, SMTO_ABORTIFHUNG, 500, NULL)
            ? (int)wcslen(text) : 0;

    text[n < size ? n : size - 1] = 0;
    for (p = text; *p; p++)
        if (*p == '\r' || *p == '\n' || *p == '\t')
            *p = ' ';
}

static BOOL CALLBACK collect_top(HWND hwnd, LPARAM lp)
{
    RECT rc;

    if (g_ntop < MAX_TOP && IsWindowVisible(hwnd) && GetWindowRect(hwnd, &rc)
        && rc.right > rc.left && rc.bottom > rc.top)
        g_top[g_ntop++] = hwnd;
    return TRUE;
}

static BOOL CALLBACK show_child(HWND hwnd, LPARAM lp)
{
    int *count = (int *)lp;
    WCHAR cls[64], text[MAX_TEXT];

    if (!IsWindowVisible(hwnd) || ++*count > MAX_CHILDREN)
        return *count <= MAX_CHILDREN;
    GetClassNameW(hwnd, cls, ARRAYSIZE(cls));
    window_text(hwnd, text, ARRAYSIZE(text));
    if (text[0])
        outf(L"    %ls: \"%ls\"\n", cls, text);
    else
        outf(L"    %ls\n", cls);
    return TRUE;
}

static void program_of(HWND hwnd, WCHAR *name, DWORD size)
{
    DWORD pid = 0;
    HANDLE h;
    WCHAR path[MAX_PATH];
    DWORD len = ARRAYSIZE(path);

    name[0] = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if ((h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid))) {
        if (QueryFullProcessImageNameW(h, 0, path, &len)) {
            const WCHAR *base = wcsrchr(path, '\\');
            lstrcpynW(name, base ? base + 1 : path, size);
        }
        CloseHandle(h);
    }
}

int wmain(int argc, WCHAR **argv)
{
    int wait = argc > 1 ? _wtoi(argv[1]) : 0, i, children;
    DWORD start = GetTickCount();
    WCHAR title[MAX_TEXT], cls[64], prog[MAX_PATH];
    RECT rc;

    for (;;) {
        g_ntop = 0;
        EnumWindows(collect_top, 0);
        if (g_ntop || (int)((GetTickCount() - start) / 1000) >= wait)
            break;
        Sleep(250);
    }
    if (!g_ntop) {
        out(L"No windows on this screen.\n");
        return 1;
    }
    outf(L"Windows on this screen (%d), top first:\n", g_ntop);
    for (i = 0; i < g_ntop; i++) {
        HWND hwnd = g_top[i];

        window_text(hwnd, title, ARRAYSIZE(title));
        GetClassNameW(hwnd, cls, ARRAYSIZE(cls));
        program_of(hwnd, prog, ARRAYSIZE(prog));
        GetWindowRect(hwnd, &rc);
        outf(L"- \"%ls\" (%ls, %ls), %ldx%ld at %ld,%ld%ls\n", title, prog[0] ? prog : L"?", cls,
             rc.right - rc.left, rc.bottom - rc.top, rc.left, rc.top,
             (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) ? L", on top" : L"");
        children = 0;
        EnumChildWindows(hwnd, show_child, (LPARAM)&children);
        if (children > MAX_CHILDREN)
            out(L"    (more controls, not shown)\n");
    }
    return 0;
}
