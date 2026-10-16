/*
 * WinCraft Assistant - what the assistant can see and do.
 *
 * The model never gets a command line. It gets a fixed set of tools, each
 * with a tier:
 *   TIER_READ      looks at the system; runs without asking, shown in the chat
 *   TIER_DIAGNOSE  pauses a program to look inside it; asked once per chat
 *   TIER_CHANGE    changes something; asked every time, with the details
 * Nothing here needs root, and nothing reaches wcinit's privileged helpers.
 *
 * SPDX-License-Identifier: MIT
 */
#include "agent.h"
#include <shlobj.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <winhttp.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RESULT_MAX      8000        /* characters of tool output sent to the model */

extern WCHAR g_provider_host[256];  /* set by the chat window */
extern int g_provider_port;

/* ======================================================================
 * Files
 * ====================================================================== */

char *read_file_utf8(const WCHAR *path, size_t max, size_t *len)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, 0, NULL);
    sb_t b;
    char buf[8192];
    DWORD got;

    if (len)
        *len = 0;
    if (h == INVALID_HANDLE_VALUE)
        return NULL;
    sb_init(&b);
    while (b.len < max && ReadFile(h, buf, sizeof(buf), &got, NULL) && got)
        sb_addn(&b, buf, got);
    CloseHandle(h);
    if (len)
        *len = b.len;
    return sb_take(&b);
}

static void unix_path(const char *p, WCHAR *out, int size)
{
    WCHAR *w = w_from_utf8(p), *c;

    _snwprintf(out, size, UNIX_ROOT L"%s", w);
    out[size - 1] = 0;
    free(w);
    for (c = out + wcslen(UNIX_ROOT); *c; c++)
        if (*c == L'/')
            *c = L'\\';
}

char *read_unix_file(const char *path, size_t max)
{
    WCHAR w[MAX_PATH];

    unix_path(path, w, MAX_PATH);
    return read_file_utf8(w, max, NULL);
}

/* The last `max` bytes of a native file, starting at a line. */
static char *read_unix_tail(const char *path, DWORD max)
{
    WCHAR w[MAX_PATH];
    HANDLE h;
    LARGE_INTEGER size, pos;
    char *buf, *start;
    DWORD got = 0;

    unix_path(path, w, MAX_PATH);
    h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                    OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return NULL;
    if (!GetFileSizeEx(h, &size))
        size.QuadPart = 0;
    pos.QuadPart = size.QuadPart > max ? size.QuadPart - max : 0;
    SetFilePointerEx(h, pos, NULL, FILE_BEGIN);
    buf = malloc(max + 1);
    if (!buf)
        abort();
    if (!ReadFile(h, buf, max, &got, NULL))
        got = 0;
    buf[got] = 0;
    CloseHandle(h);
    start = buf;
    if (pos.QuadPart) {
        char *nl = strchr(buf, '\n');

        if (nl)
            start = nl + 1;
    }
    memmove(buf, start, strlen(start) + 1);
    return buf;
}

BOOL app_data_dir(WCHAR *out)
{
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_APPDATA | CSIDL_FLAG_CREATE, NULL, SHGFP_TYPE_CURRENT, out)))
        return FALSE;
    PathAppendW(out, APP_NAME);
    SHCreateDirectoryExW(NULL, out, NULL);
    return TRUE;
}

void activity_log(const char *fmt, ...)
{
    WCHAR path[MAX_PATH];
    SYSTEMTIME st;
    char line[2048];
    int n;
    va_list ap;
    HANDLE h;
    DWORD put;

    if (!app_data_dir(path))
        return;
    PathAppendW(path, L"activity.log");
    GetLocalTime(&st);
    n = snprintf(line, sizeof(line), "%04d-%02d-%02d %02d:%02d:%02d  ", st.wYear, st.wMonth, st.wDay,
                 st.wHour, st.wMinute, st.wSecond);
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof(line) - n - 2, fmt, ap);
    va_end(ap);
    strcat(line, "\n");
    h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    WriteFile(h, line, (DWORD)strlen(line), &put, NULL);
    CloseHandle(h);
}

/* Start a native (Linux) program: run_native(L"/usr/bin/busybox", L"chmod 600 ...").
 * The program gets its plain name as argv[0], which BusyBox needs to pick the
 * applet; from a Windows command line it would get Wine's path instead. */
BOOL run_native(const WCHAR *program, const WCHAR *args, DWORD wait_ms)
{
    static const WCHAR *prefixes[] = { UNIX_ROOT, L"Z:" };
    WCHAR app[MAX_PATH], cmd[1024], *p;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    unsigned i;

    _snwprintf(cmd, ARRAYSIZE(cmd), L"%s %s", PathFindFileNameW(program), args);
    cmd[ARRAYSIZE(cmd) - 1] = 0;
    for (i = 0; i < ARRAYSIZE(prefixes); i++) {
        _snwprintf(app, ARRAYSIZE(app), L"%s%s", prefixes[i], program);
        app[ARRAYSIZE(app) - 1] = 0;
        for (p = app; *p; p++)
            if (*p == L'/')
                *p = L'\\';
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        if (CreateProcessW(app, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            if (wait_ms)
                WaitForSingleObject(pi.hProcess, wait_ms);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            return TRUE;
        }
    }
    activity_log("could not start %S (error %lu)", program, GetLastError());
    return FALSE;
}

/* ======================================================================
 * Small helpers
 * ====================================================================== */

static void cap_result(sb_t *b)
{
    if (b->len > RESULT_MAX) {
        b->len = RESULT_MAX;
        /* do not cut a UTF-8 sequence in half */
        while (b->len && ((unsigned char)b->s[b->len] & 0xc0) == 0x80)
            b->len--;
        b->s[b->len] = 0;
        sb_add(b, "\n[... cut: the rest was too long]");
    }
}

static const char *arg_str(const json *args, const char *key)
{
    const char *s = json_str(args, key);

    return s ? s : "";
}

static long arg_int(const json *args, const char *key, long def)
{
    json *v = json_get(args, key);

    if (v && v->type == J_NUMBER)
        return (long)v->num;
    if (v && v->type == J_STRING && v->str[0])
        return strtol(v->str, NULL, 0);
    return def;
}

static void fmt_bytes(char *out, size_t size, ULONGLONG n)
{
    if (n >= (1ULL << 30))
        snprintf(out, size, "%.1f GB", n / (double)(1ULL << 30));
    else
        snprintf(out, size, "%llu MB", n >> 20);
}

/* "key: value" lines of /proc files */
static ULONGLONG meminfo_kb(const char *info, const char *key)
{
    const char *p = info ? strstr(info, key) : NULL;

    return p ? strtoull(p + strlen(key), NULL, 10) : 0;
}

static char *trim(char *s)
{
    char *e;

    while (*s == ' ' || *s == '\t')
        s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '))
        *--e = 0;
    return s;
}

/* ======================================================================
 * Running programs
 * ====================================================================== */

/* Wine's and WinCraft's own processes, not programs the person started. */
static BOOL system_process(const WCHAR *exe)
{
    static const WCHAR *names[] = {
        L"services.exe", L"winedevice.exe", L"plugplay.exe", L"svchost.exe", L"rpcss.exe",
        L"explorer.exe", L"conhost.exe", L"wcshell.exe", L"wcagent.exe", L"start.exe",
        L"rundll32.exe", L"winedbg.exe", L"wineboot.exe", L"tabtip.exe", L"cmd.exe", L"lsass.exe",
    };
    unsigned i;

    for (i = 0; i < ARRAYSIZE(names); i++)
        if (!lstrcmpiW(exe, names[i]))
            return TRUE;
    return FALSE;
}

struct win_info {
    DWORD pid;
    sb_t *out;
    int count;
};

static BOOL CALLBACK windows_of_pid(HWND hwnd, LPARAM lp)
{
    struct win_info *wi = (struct win_info *)lp;
    WCHAR title[160];
    DWORD pid = 0;

    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER))
        return TRUE;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != wi->pid || !GetWindowTextW(hwnd, title, ARRAYSIZE(title)))
        return TRUE;
    sb_add(wi->out, wi->count++ ? "; " : "  windows: ");
    sb_addc(wi->out, '"');
    sb_addw(wi->out, title);
    sb_addc(wi->out, '"');
    if (IsHungAppWindow(hwnd))
        sb_add(wi->out, " (NOT RESPONDING)");
    return TRUE;
}

static ULONGLONG process_memory(DWORD pid)
{
    PROCESS_MEMORY_COUNTERS pmc;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    ULONGLONG n = 0;

    if (!h)
        return 0;
    if (GetProcessMemoryInfo(h, &pmc, sizeof(pmc)))
        n = pmc.WorkingSetSize;
    CloseHandle(h);
    return n;
}

/* The program's file, as a Windows path (Z: for files outside C:). */
static BOOL process_file(DWORD pid, WCHAR *out, DWORD size)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    BOOL ok;

    if (!h)
        return FALSE;
    ok = QueryFullProcessImageNameW(h, 0, out, &size);
    CloseHandle(h);
    return ok;
}

static BOOL process_name(DWORD pid, WCHAR *out, int size)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe;
    BOOL found = FALSE;

    if (snap == INVALID_HANDLE_VALUE)
        return FALSE;
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (pe.th32ProcessID == pid) {
            lstrcpynW(out, pe.szExeFile, size);
            found = TRUE;
            break;
        }
    CloseHandle(snap);
    return found;
}

static void list_running(sb_t *b, BOOL detailed)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe;
    WCHAR file[MAX_PATH];
    char mem[32];
    int n = 0;

    if (snap == INVALID_HANDLE_VALUE) {
        sb_add(b, "(could not list programs)\n");
        return;
    }
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        struct win_info wi;

        if (system_process(pe.szExeFile) && !detailed)
            continue;
        fmt_bytes(mem, sizeof(mem), process_memory(pe.th32ProcessID));
        sb_add(b, "- ");
        sb_addw(b, pe.szExeFile);
        sb_addf(b, "  pid %lu, %lu thread%s, %s", (unsigned long)pe.th32ProcessID,
                (unsigned long)pe.cntThreads, pe.cntThreads == 1 ? "" : "s", mem);
        if (system_process(pe.szExeFile))
            sb_add(b, "  [part of Wine or WinCraft]");
        if (process_file(pe.th32ProcessID, file, ARRAYSIZE(file))) {
            sb_add(b, "  file: ");
            sb_addw(b, file);
        }
        wi.pid = pe.th32ProcessID;
        wi.out = b;
        wi.count = 0;
        EnumWindows(windows_of_pid, (LPARAM)&wi);
        sb_addc(b, '\n');
        n++;
    }
    CloseHandle(snap);
    if (!n)
        sb_add(b, "(no programs running besides WinCraft itself)\n");
}

/* ======================================================================
 * Installed programs
 * ====================================================================== */

static void uninstall_entries(sb_t *b, HKEY root, const WCHAR *path, int *n)
{
    HKEY key, sub;
    WCHAR name[256], display[256], version[64];
    DWORD i, len, size, type;

    if (RegOpenKeyExW(root, path, 0, KEY_READ, &key))
        return;
    for (i = 0; *n < 60; i++) {
        len = ARRAYSIZE(name);
        if (RegEnumKeyExW(key, i, name, &len, NULL, NULL, NULL, NULL))
            break;
        if (RegOpenKeyExW(key, name, 0, KEY_READ, &sub))
            continue;
        size = sizeof(display);
        if (!RegQueryValueExW(sub, L"DisplayName", NULL, &type, (BYTE *)display, &size) &&
            type == REG_SZ) {
            sb_add(b, "- ");
            sb_addw(b, display);
            size = sizeof(version);
            if (!RegQueryValueExW(sub, L"DisplayVersion", NULL, &type, (BYTE *)version, &size) &&
                type == REG_SZ && version[0]) {
                sb_add(b, " ");
                sb_addw(b, version);
            }
            sb_addc(b, '\n');
            (*n)++;
        }
        RegCloseKey(sub);
    }
    RegCloseKey(key);
}

static void start_menu_entries(sb_t *b, const WCHAR *dir, const WCHAR *prefix, int depth, int *n)
{
    WCHAR pattern[MAX_PATH], sub[MAX_PATH], label[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;

    PathCombineW(pattern, dir, L"*");
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (fd.cFileName[0] == L'.' || *n >= 80)
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth < 3) {
                PathCombineW(sub, dir, fd.cFileName);
                _snwprintf(label, MAX_PATH, L"%s%s/", prefix, fd.cFileName);
                label[MAX_PATH - 1] = 0;
                start_menu_entries(b, sub, label, depth + 1, n);
            }
        } else if (!lstrcmpiW(PathFindExtensionW(fd.cFileName), L".lnk")) {
            PathRemoveExtensionW(fd.cFileName);
            sb_add(b, "- ");
            sb_addw(b, prefix);
            sb_addw(b, fd.cFileName);
            sb_addc(b, '\n');
            (*n)++;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void list_installed(sb_t *b)
{
    WCHAR dir[MAX_PATH];
    int n = 0;

    sb_add(b, "Programs registered as installed (Add or Remove Programs):\n");
    uninstall_entries(b, HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall", &n);
    uninstall_entries(b, HKEY_LOCAL_MACHINE,
                      L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall", &n);
    uninstall_entries(b, HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall", &n);
    if (!n)
        sb_add(b, "(none)\n");
    n = 0;
    sb_add(b, "Start menu entries:\n");
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_COMMON_PROGRAMS, NULL, SHGFP_TYPE_CURRENT, dir)))
        start_menu_entries(b, dir, L"", 0, &n);
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PROGRAMS, NULL, SHGFP_TYPE_CURRENT, dir)))
        start_menu_entries(b, dir, L"", 0, &n);
}

/* ======================================================================
 * Logs
 * ====================================================================== */

/* Lines that look alarming but are expected on every WinCraft system. */
static BOOL noise(const char *line)
{
    static const char *known[] = {
        "dbus-core: error connecting to system bus", "pci id for fd", "MESA-LOADER",
        "DRI3", "ALSA lib pcm_dsnoop", "unable to open slave", "xkbcomp",
        "Could not resolve keysym", "Errors from xkbcomp are not fatal",
        "regsvr32: Successfully registered", "Warning:", "Ignoring extra symbols",
        /* Xorg's legend line, and modules it tries before the one that works */
        "(WW) warning, (EE) error", "Failed to load module \"fbdev\"", "Failed to load module \"vesa\"",
        "glamor initialization failed",
    };
    unsigned i;

    for (i = 0; i < ARRAYSIZE(known); i++)
        if (strstr(line, known[i]))
            return TRUE;
    /* Wine's own debug channels: fixme and trace are not problems */
    if (strstr(line, ":fixme:") || strstr(line, ":trace:") || !strncmp(line, "fixme:", 6))
        return TRUE;
    return FALSE;
}

static BOOL problem(const char *line)
{
    static const char *marks[] = {
        "err:", "(EE)", "error", "Error", "ERROR", "fail", "Fail", "crash", "Crash",
        "Unhandled", "exception", "segfault", "Segmentation", "killed", "Killed",
        "timeout", "timed out", "not found", "denied",
    };
    unsigned i;

    if (noise(line))
        return FALSE;
    for (i = 0; i < ARRAYSIZE(marks); i++)
        if (strstr(line, marks[i]))
            return TRUE;
    return FALSE;
}

static BOOL valid_log_name(const char *s)
{
    if (!*s || strlen(s) > 40)
        return FALSE;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') ||
              *s == '-' || *s == '_' || *s == '.'))
            return FALSE;
    return TRUE;
}

static void log_names(sb_t *b)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(UNIX_ROOT L"\\run\\log\\*.log", &fd);
    int n = 0;

    if (h == INVALID_HANDLE_VALUE) {
        sb_add(b, "(none)");
        return;
    }
    do {
        PathRemoveExtensionW(fd.cFileName);
        sb_add(b, n++ ? ", " : "");
        sb_addw(b, fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* The last problem lines across all service logs, for the snapshot. */
static void recent_problems(sb_t *b, int max)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(UNIX_ROOT L"\\run\\log\\*.log", &fd);
    char *lines[64];
    int n = 0, i;

    if (h == INVALID_HANDLE_VALUE) {
        sb_add(b, "- none (no system logs found)\n");
        return;
    }
    do {
        char path[MAX_PATH], *text, *line, *save = NULL, *name = utf8_from_w(fd.cFileName);

        snprintf(path, sizeof(path), "/run/log/%s", name);
        text = read_unix_tail(path, 64 * 1024);
        for (line = text ? strtok_r(text, "\n", &save) : NULL; line; line = strtok_r(NULL, "\n", &save)) {
            sb_t one;

            if (!problem(line))
                continue;
            sb_init(&one);
            sb_addf(&one, "[%s] %.300s", name, trim(line));
            if (n == (int)ARRAYSIZE(lines)) {
                free(lines[0]);
                memmove(lines, lines + 1, sizeof(lines[0]) * (n - 1));
                n--;
            }
            lines[n++] = sb_take(&one);
        }
        free(text);
        free(name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    for (i = n > max ? n - max : 0; i < n; i++) {
        sb_addf(b, "- %s\n", lines[i]);
    }
    for (i = 0; i < n; i++)
        free(lines[i]);
    if (!n)
        sb_add(b, "- none\n");
}

/* ======================================================================
 * Crash reports
 * ====================================================================== */

struct crash_list {
    sb_t *out;
    int count;
};

static void find_crashes(const WCHAR *dir, int depth, struct crash_list *cl)
{
    WCHAR pattern[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;

    PathCombineW(pattern, dir, L"*");
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (fd.cFileName[0] == L'.')
            continue;
        PathCombineW(path, dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth < 7)
                find_crashes(path, depth + 1, cl);
        } else {
            const WCHAR *ext = PathFindExtensionW(fd.cFileName);

            if (!lstrcmpiW(ext, L".dmp") || !lstrcmpiW(ext, L".extra")) {
                SYSTEMTIME st;

                cl->count++;
                if (cl->out && cl->count <= 25) {
                    FileTimeToSystemTime(&fd.ftLastWriteTime, &st);
                    sb_add(cl->out, "- ");
                    sb_addw(cl->out, path);
                    sb_addf(cl->out, "  (%lu KB, %04d-%02d-%02d %02d:%02d UTC)\n",
                            (unsigned long)(fd.nFileSizeLow / 1024), st.wYear, st.wMonth, st.wDay,
                            st.wHour, st.wMinute);
                }
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static int crash_reports(sb_t *out)
{
    struct crash_list cl = { out, 0 };
    WCHAR dir[MAX_PATH];

    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, SHGFP_TYPE_CURRENT, dir)))
        find_crashes(dir, 0, &cl);
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, dir)))
        find_crashes(dir, 0, &cl);
    if (GetWindowsDirectoryW(dir, MAX_PATH)) {
        PathAppendW(dir, L"temp");
        find_crashes(dir, 0, &cl);
    }
    return cl.count;
}

/* ======================================================================
 * Network
 * ====================================================================== */

static void network_summary(sb_t *b, BOOL detailed)
{
    ULONG size = 32768;
    IP_ADAPTER_ADDRESSES *aa = malloc(size), *a;
    char *route, *resolv, *line, *save = NULL;
    int up = 0;

    if (aa && GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, NULL, aa,
                                   &size) == NO_ERROR) {
        for (a = aa; a; a = a->Next) {
            IP_ADAPTER_UNICAST_ADDRESS *u;

            if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || a->IfType == IF_TYPE_TUNNEL)
                continue;
            /* a down Ethernet port matters (cable?); down tunnels such as sit0 do not */
            if (a->OperStatus != IfOperStatusUp && a->IfType != IF_TYPE_ETHERNET_CSMACD)
                continue;
            sb_add(b, up++ ? "; " : "");
            sb_addw(b, a->FriendlyName);
            sb_add(b, a->OperStatus == IfOperStatusUp ? " up" : " down");
            for (u = a->FirstUnicastAddress; u; u = u->Next) {
                char ip[64] = "";
                DWORD iplen = sizeof(ip);

                if (!WSAAddressToStringA(u->Address.lpSockaddr, u->Address.iSockaddrLength, NULL, ip, &iplen))
                    sb_addf(b, " %s", ip);
            }
        }
    }
    free(aa);
    if (!up)
        sb_add(b, "no network adapter found");

    route = read_unix_file("/proc/net/route", 16384);
    for (line = route ? strtok_r(route, "\n", &save) : NULL; line; line = strtok_r(NULL, "\n", &save)) {
        char ifname[32];
        unsigned dest, gw;

        if (sscanf(line, "%31s %x %x", ifname, &dest, &gw) == 3 && dest == 0) {
            sb_addf(b, "; gateway %u.%u.%u.%u via %s", gw & 0xff, (gw >> 8) & 0xff, (gw >> 16) & 0xff,
                    gw >> 24, ifname);
            break;
        }
    }
    free(route);
    if (!line)
        sb_add(b, "; no default gateway (not connected?)");
    resolv = read_unix_file("/etc/resolv.conf", 4096);
    save = NULL;
    for (line = resolv ? strtok_r(resolv, "\n", &save) : NULL; line; line = strtok_r(NULL, "\n", &save))
        if (!strncmp(line, "nameserver", 10))
            sb_addf(b, "; DNS %s", trim(line + 10));
    free(resolv);
    sb_addc(b, '\n');
    (void)detailed;
}

static void connection_test(sb_t *b)
{
    WSADATA wsa;
    ADDRINFOW hints, *res = NULL;
    SOCKET s;
    char host[256];
    WCHAR port[16];

    if (!g_provider_host[0]) {
        sb_add(b, "No provider configured, so no connection test was made.\n");
        return;
    }
    WideCharToMultiByte(CP_UTF8, 0, g_provider_host, -1, host, sizeof(host), NULL, NULL);
    WSAStartup(MAKEWORD(2, 2), &wsa);
    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    _snwprintf(port, ARRAYSIZE(port), L"%d", g_provider_port ? g_provider_port : 443);
    port[ARRAYSIZE(port) - 1] = 0;
    if (GetAddrInfoW(g_provider_host, port, &hints, &res) || !res) {
        sb_addf(b, "Name lookup of %s FAILED: DNS does not work or the name is wrong.\n", host);
        WSACleanup();
        return;
    }
    sb_addf(b, "Name lookup of %s works.\n", host);
    s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s != INVALID_SOCKET) {
        DWORD ms = 5000;

        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&ms, sizeof(ms));
        if (!connect(s, res->ai_addr, (int)res->ai_addrlen))
            sb_addf(b, "A connection to %s port %S works.\n", host, port);
        else
            sb_addf(b, "A connection to %s port %S FAILED.\n", host, port);
        closesocket(s);
    }
    FreeAddrInfoW(res);
    WSACleanup();
}

/* ======================================================================
 * The snapshot sent at the start of each chat
 * ====================================================================== */

static const char *os_name(char *buf, size_t size)
{
    char *rel = read_unix_file("/etc/os-release", 4096), *p;

    snprintf(buf, size, "WinCraft OS");
    if (rel && (p = strstr(rel, "PRETTY_NAME=\""))) {
        char *e;

        p += 13;
        e = strchr(p, '"');
        if (e)
            snprintf(buf, size, "%.*s", (int)(e - p), p);
    }
    free(rel);
    return buf;
}

static void boot_options(sb_t *b)
{
    char *cmd = read_unix_file("/proc/cmdline", 4096), *tok, *save = NULL;
    int n = 0;

    for (tok = cmd ? strtok_r(cmd, " \n", &save) : NULL; tok; tok = strtok_r(NULL, " \n", &save)) {
        /* the parts that only matter to the console */
        if (!strncmp(tok, "console=", 8) || !strncmp(tok, "loglevel=", 9) || !strcmp(tok, "quiet") ||
            !strncmp(tok, "vt.", 3) || !strncmp(tok, "net.ifnames", 11) || !strncmp(tok, "BOOT_IMAGE", 10) ||
            !strncmp(tok, "initrd=", 7))
            continue;
        sb_add(b, n++ ? " " : "");
        sb_add(b, tok);
    }
    free(cmd);
    if (!n)
        sb_add(b, "(none)");
}

char *system_snapshot(void)
{
    sb_t b;
    char name[128], *meminfo, *cpuinfo, *uptime, *kbd, tmp[64], tmp2[64], *p;
    ULARGE_INTEGER free_c, total_c;
    BOOL live = GetFileAttributesW(UNIX_ROOT L"\\run\\wincraft\\live") != INVALID_FILE_ATTRIBUTES;
    double up = 0;
    int cpus = 0;
    const char *(*wine_version)(void);
    TIME_ZONE_INFORMATION tz;

    sb_init(&b);
    sb_addf(&b, "System: %s, %s.\n", os_name(name, sizeof(name)),
            live ? "LIVE session (nothing is saved: all changes are lost at restart)"
                 : "installed (changes are kept on the data partition)");
    uptime = read_unix_file("/proc/uptime", 256);
    if (uptime)
        up = atof(uptime);
    free(uptime);
    sb_addf(&b, "Up for %d h %02d min. Boot options: ", (int)(up / 3600), ((int)up / 60) % 60);
    boot_options(&b);
    sb_addc(&b, '\n');

    meminfo = read_unix_file("/proc/meminfo", 16384);
    fmt_bytes(tmp, sizeof(tmp), meminfo_kb(meminfo, "MemTotal:") * 1024);
    fmt_bytes(tmp2, sizeof(tmp2), meminfo_kb(meminfo, "MemAvailable:") * 1024);
    sb_addf(&b, "Memory: %s total, %s available.\n", tmp, tmp2);
    free(meminfo);
    cpuinfo = read_unix_file("/proc/cpuinfo", 256 * 1024);
    for (p = cpuinfo; p && (p = strstr(p, "processor\t")); p++)
        cpus++;
    p = cpuinfo ? strstr(cpuinfo, "model name") : NULL;
    if (p && (p = strchr(p, ':'))) {
        char *e = strchr(p, '\n');

        sb_addf(&b, "CPU: %d x %.*s\n", cpus, (int)(e ? e - p - 2 : 60), p + 2);
    }
    free(cpuinfo);
    if (GetDiskFreeSpaceExW(L"C:\\", &free_c, &total_c, NULL)) {
        fmt_bytes(tmp, sizeof(tmp), free_c.QuadPart);
        fmt_bytes(tmp2, sizeof(tmp2), total_c.QuadPart);
        sb_addf(&b, "Space for programs and files: %s free of %s%s.\n", tmp, tmp2,
                live ? " (in memory)" : "");
    }
    kbd = read_unix_file("/run/wincraft/kbd", 64);
    GetTimeZoneInformation(&tz);
    sb_addf(&b, "Screen %dx%d; keyboard layout %s; time zone ", GetSystemMetrics(SM_CXSCREEN),
            GetSystemMetrics(SM_CYSCREEN), kbd && *kbd ? trim(kbd) : "us (default)");
    sb_addw(&b, tz.StandardName[0] ? tz.StandardName : L"UTC");
    sb_add(&b, ".\n");
    free(kbd);

    sb_add(&b, "Network: ");
    network_summary(&b, FALSE);

    wine_version = (void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version");
    sb_addf(&b, "Wine %s (64- and 32-bit Windows programs).\n", wine_version ? wine_version() : "?");

    sb_add(&b, "\nInstalled programs:\n");
    list_installed(&b);
    sb_add(&b, "\nRunning programs:\n");
    list_running(&b, FALSE);
    sb_add(&b, "\nRecent problems in the system logs:\n");
    recent_problems(&b, 8);
    sb_addf(&b, "\nCrash reports on disk: %d\n", crash_reports(NULL));
    return sb_take(&b);
}

char *knowledge_text(void)
{
    char *k = read_unix_file("/usr/share/wincraft/assistant/knowledge.md", 512 * 1024);

    return k ? k : utf8_dup("You are WinCraft Assistant, built into WinCraft OS. "
                            "Your reference documentation is missing from this system.");
}

/* ======================================================================
 * Tool: look inside a program with Wine's debugger
 * ====================================================================== */

static char *debugger_output(DWORD pid)
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE in_r, in_w, out_r, out_w;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    WCHAR cmd[MAX_PATH + 16];
    char script[128], buf[8192];
    DWORD put, got, start = GetTickCount();
    sb_t out;

    if (!CreatePipe(&in_r, &in_w, &sa, 0))
        return NULL;
    if (!CreatePipe(&out_r, &out_w, &sa, 0)) {
        CloseHandle(in_r);
        CloseHandle(in_w);
        return NULL;
    }
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    GetSystemDirectoryW(cmd, MAX_PATH);
    PathAppendW(cmd, L"winedbg.exe");
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = out_w;
    if (!CreateProcessW(cmd, NULL, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(in_r);
        CloseHandle(in_w);
        CloseHandle(out_r);
        CloseHandle(out_w);
        return NULL;
    }
    CloseHandle(in_r);
    CloseHandle(out_w);
    snprintf(script, sizeof(script), "attach 0x%lx\nbt all\ndetach\nquit\n", (unsigned long)pid);
    WriteFile(in_w, script, (DWORD)strlen(script), &put, NULL);
    CloseHandle(in_w);
    sb_init(&out);
    while (ReadFile(out_r, buf, sizeof(buf), &got, NULL) && got) {
        sb_addn(&out, buf, got);
        if (out.len > (4 << 20) || GetTickCount() - start > 60000)
            break;
    }
    if (WaitForSingleObject(pi.hProcess, 5000) == WAIT_TIMEOUT)
        TerminateProcess(pi.hProcess, 1);
    CloseHandle(out_r);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return sb_take(&out);
}

/* Name a frame in one of Wine's own DLLs from its export table, e.g.
 * win32u+0x13cc4 -> win32u!NtUserGetMessage+0x14. The DLLs in system32 are
 * the same files the program uses, so the offsets match (64-bit programs only;
 * a 32-bit one runs the copies in syswow64). */
static void frame_name(const char *mod, const char *off, char *out, size_t size)
{
    static struct { char name[96]; BYTE *base; } cache[24];
    static int ncache;
    WCHAR wmod[80], path[MAX_PATH];
    BYTE *base = NULL;
    IMAGE_NT_HEADERS *nt;
    IMAGE_DATA_DIRECTORY *dir;
    IMAGE_EXPORT_DIRECTORY *ex;
    DWORD *funcs, *names, rva = strtoul(off, NULL, 16), best = 0, i;
    WORD *ords;
    const char *best_name = NULL;
    int c;

    snprintf(out, size, "%s+%s", mod, off);
    for (c = 0; c < ncache; c++)
        if (!strcmp(cache[c].name, mod))
            base = cache[c].base;
    if (c == ncache && !base) {
        HMODULE h;

        MultiByteToWideChar(CP_UTF8, 0, mod, -1, wmod, ARRAYSIZE(wmod) - 4);
        if (!wcschr(wmod, L'.'))
            wcscat(wmod, L".dll");
        GetSystemDirectoryW(path, MAX_PATH);
        PathAppendW(path, wmod);
        h = GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES ? NULL : GetModuleHandleW(wmod);
        if (!h && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
            h = LoadLibraryExW(path, NULL, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        base = (BYTE *)((ULONG_PTR)h & ~(ULONG_PTR)3);
        if (ncache < (int)ARRAYSIZE(cache)) {
            snprintf(cache[ncache].name, sizeof(cache[ncache].name), "%s", mod);
            cache[ncache++].base = base;
        }
    }
    if (!base || ((IMAGE_DOS_HEADER *)base)->e_magic != IMAGE_DOS_SIGNATURE)
        return;
    nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return;
    dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir->VirtualAddress || !dir->Size)
        return;
    ex = (IMAGE_EXPORT_DIRECTORY *)(base + dir->VirtualAddress);
    funcs = (DWORD *)(base + ex->AddressOfFunctions);
    names = (DWORD *)(base + ex->AddressOfNames);
    ords = (WORD *)(base + ex->AddressOfNameOrdinals);
    for (i = 0; i < ex->NumberOfNames; i++) {
        DWORD f;

        if (ords[i] >= ex->NumberOfFunctions)
            continue;
        f = funcs[ords[i]];
        /* forwarders point into the export directory, not at code */
        if (f >= dir->VirtualAddress && f < dir->VirtualAddress + dir->Size)
            continue;
        if (f <= rva && f >= best) {
            best = f;
            best_name = (const char *)(base + names[i]);
        }
    }
    if (best_name && rva - best < 0x10000)
        snprintf(out, size, "%s!%s+0x%lx", mod, best_name, (unsigned long)(rva - best));
}

/* Threads of `pid`, each with its top frames; identical stacks are merged. */
static void summarize_backtrace(sb_t *b, char *text, DWORD pid)
{
    char *line, *save = NULL;
    struct { char *frames; char tids[96]; int n; } groups[48];
    int ngroups = 0, i, frames = 0, skipped = 0;
    BOOL in = FALSE, names = FALSE, wow64 = TRUE;
    sb_t cur;
    char tid[16] = "";
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);

    if (proc && IsWow64Process(proc, &wow64))
        names = !wow64;
    if (proc)
        CloseHandle(proc);

    sb_init(&cur);
    for (line = strtok_r(text, "\n", &save); ; line = strtok_r(NULL, "\n", &save)) {
        BOOL header = line && strstr(line, "Backtracing for thread ");

        if (!line || header) {
            /* the thread the debugger injects to stop the program */
            if (in && cur.len && (strstr(cur.s, "DbgBreakPoint") || strstr(cur.s, "DbgUiRemoteBreakin"))) {
                skipped++;
            } else if (in && cur.len) {
                for (i = 0; i < ngroups && strcmp(groups[i].frames, cur.s); i++)
                    ;
                if (i == ngroups && ngroups < (int)ARRAYSIZE(groups)) {
                    groups[ngroups].frames = utf8_dup(cur.s);
                    groups[ngroups].tids[0] = 0;
                    groups[ngroups].n = 0;
                    ngroups++;
                }
                if (i < ngroups) {
                    if (strlen(groups[i].tids) < sizeof(groups[i].tids) - 8)
                        snprintf(groups[i].tids + strlen(groups[i].tids), 8, "%s%s",
                                 groups[i].n ? " " : "", tid);
                    groups[i].n++;
                }
            }
            sb_free(&cur);
            frames = 0;
            if (!line)
                break;
            /* "Backtracing for thread 0150 in process 014c (...)" */
            in = FALSE;
            {
                char *t = strstr(line, "thread "), *p = strstr(line, "process ");

                if (t && p) {
                    snprintf(tid, sizeof(tid), "%lx", strtoul(t + 7, NULL, 16));
                    in = strtoul(p + 8, NULL, 16) == pid;
                }
            }
            continue;
        }
        if (!in || frames >= 8)
            continue;
        /* " 3 0x006ffffa425b61 in mozglue (+0x55b61) (0x...)" -> "mozglue+0x55b61" */
        {
            char *p = strstr(line, " in ");
            char mod[96], off[32] = "";

            if (!p || !strstr(line, "0x"))
                continue;
            if (sscanf(p + 4, "%95s (+%31[0-9a-fx])", mod, off) >= 1) {
                char name[192];

                sb_add(&cur, frames ? " < " : "");
                if (off[0] && names)
                    frame_name(mod, off, name, sizeof(name));
                else
                    snprintf(name, sizeof(name), off[0] ? "%s+%s" : "%s", mod, off);
                sb_add(&cur, name);
                frames++;
            }
        }
    }
    if (!ngroups) {
        sb_add(b, "The debugger could not read this program's threads.\n");
        return;
    }
    sb_addf(b, "Threads of process %lu, innermost frame first. Frames in Wine's own DLLs are named "
               "module!function+offset; the program's own modules have no names, only module+offset%s.\n",
            (unsigned long)pid, names ? "" : " (a 32-bit program: no names at all)");
    if (skipped)
        sb_add(b, "(The debugger's own thread is left out.)\n");
    for (i = 0; i < ngroups; i++) {
        sb_addf(b, "- %d thread(s) [%s]: %s\n", groups[i].n, groups[i].tids, groups[i].frames);
        free(groups[i].frames);
    }
}

/* ======================================================================
 * Tools
 * ====================================================================== */

const tool_def g_tools[] = {
    { "get_system_snapshot", TIER_READ,
      "Collect a fresh overview of this computer: WinCraft version, live or installed, memory, disk, "
      "network, Wine version, installed and running programs, recent problems in the logs.",
      "{\"type\":\"object\",\"properties\":{}}" },
    { "list_programs", TIER_READ,
      "List the running Windows programs with their process id (pid), memory use, file, window titles "
      "and whether a window is not responding. Set include_system to also list Wine's own processes.",
      "{\"type\":\"object\",\"properties\":{\"include_system\":{\"type\":\"boolean\"}}}" },
    { "list_installed_programs", TIER_READ,
      "List installed Windows programs (Add or Remove Programs) and the Start menu entries.",
      "{\"type\":\"object\",\"properties\":{}}" },
    { "read_log", TIER_READ,
      "Read the end of a WinCraft service log in /run/log. 'desktop' holds the output of Wine and of "
      "every Windows program; others include Xorg.0, net, time, syslog. Leave name empty to list them.",
      "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},"
      "\"lines\":{\"type\":\"integer\",\"description\":\"how many lines, at most 200\"},"
      "\"problems_only\":{\"type\":\"boolean\",\"description\":\"only lines that look like errors\"}}}" },
    { "read_registry", TIER_READ,
      "Read a key of the Windows registry: its values and subkeys. Example key: "
      "HKEY_CURRENT_USER\\Software\\Wine\\DllOverrides",
      "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\"}},\"required\":[\"key\"]}" },
    { "check_network", TIER_READ,
      "Check the network: adapters and addresses, gateway, DNS servers, and whether the AI "
      "provider's server can be reached.",
      "{\"type\":\"object\",\"properties\":{}}" },
    { "list_crash_reports", TIER_READ,
      "List crash reports (.dmp and .extra files) that programs left in the user's folders.",
      "{\"type\":\"object\",\"properties\":{}}" },
    { "disk_usage", TIER_READ,
      "Show free space and how WinCraft's storage is mounted (system image, data partition).",
      "{\"type\":\"object\",\"properties\":{}}" },
    { "program_backtrace", TIER_DIAGNOSE,
      "Look inside a running program with Wine's debugger: where each of its threads is waiting. "
      "Use it for a program that is not responding. Pauses the program for a second or two. "
      "Needs the person's permission.",
      "{\"type\":\"object\",\"properties\":{\"pid\":{\"type\":\"integer\"}},\"required\":[\"pid\"]}" },
    { "end_program", TIER_CHANGE,
      "Stop a running program at once, like End Task (unsaved work is lost). Needs the person's "
      "permission.",
      "{\"type\":\"object\",\"properties\":{\"pid\":{\"type\":\"integer\"}},\"required\":[\"pid\"]}" },
    { "set_dll_override", TIER_CHANGE,
      "Change how Wine loads a DLL, for all programs or for one program (Wine's DllOverrides). mode: "
      "native, builtin, native,builtin, builtin,native, disabled, or default to remove the override. "
      "Needs the person's permission.",
      "{\"type\":\"object\",\"properties\":{\"dll\":{\"type\":\"string\"},\"mode\":{\"type\":\"string\"},"
      "\"program\":{\"type\":\"string\",\"description\":\"e.g. game.exe; empty for all programs\"}},"
      "\"required\":[\"dll\",\"mode\"]}" },
    { "set_registry_value", TIER_CHANGE,
      "Set a value under HKEY_CURRENT_USER (only there). type is string or dword. Needs the person's "
      "permission.",
      "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},"
      "\"type\":{\"type\":\"string\",\"enum\":[\"string\",\"dword\"]},\"data\":{\"type\":\"string\"}},"
      "\"required\":[\"key\",\"name\",\"type\",\"data\"]}" },
    { "download_and_run", TIER_CHANGE,
      "Download a Windows program or installer from an https address into the Downloads folder and "
      "start it, like Download a Program. Needs the person's permission.",
      "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\"}},\"required\":[\"url\"]}" },
};
const int g_ntools = ARRAYSIZE(g_tools);

const tool_def *tool_find(const char *name)
{
    int i;

    for (i = 0; i < g_ntools; i++)
        if (!strcmp(g_tools[i].name, name))
            return &g_tools[i];
    return NULL;
}

json *tools_json(int format)
{
    json *arr = json_new(J_ARRAY);
    int i;

    for (i = 0; i < g_ntools; i++) {
        json *t = json_push(arr, json_new(J_OBJECT));
        json *schema = json_parse(g_tools[i].schema, strlen(g_tools[i].schema));

        if (format == FMT_ANTHROPIC) {
            json_set_str(t, "name", g_tools[i].name);
            json_set_str(t, "description", g_tools[i].description);
            json_set(t, "input_schema", schema);
        } else {
            json *fn;

            json_set_str(t, "type", "function");
            fn = json_set(t, "function", json_new(J_OBJECT));
            json_set_str(fn, "name", g_tools[i].name);
            json_set_str(fn, "description", g_tools[i].description);
            json_set(fn, "parameters", schema);
        }
    }
    return arr;
}

static HKEY parse_root(const char *key, const char **rest)
{
    static const struct { const char *name; HKEY root; } roots[] = {
        { "HKEY_CURRENT_USER\\", HKEY_CURRENT_USER }, { "HKCU\\", HKEY_CURRENT_USER },
        { "HKEY_LOCAL_MACHINE\\", HKEY_LOCAL_MACHINE }, { "HKLM\\", HKEY_LOCAL_MACHINE },
        { "HKEY_CLASSES_ROOT\\", HKEY_CLASSES_ROOT }, { "HKCR\\", HKEY_CLASSES_ROOT },
        { "HKEY_USERS\\", HKEY_USERS }, { "HKU\\", HKEY_USERS },
    };
    unsigned i;

    for (i = 0; i < ARRAYSIZE(roots); i++)
        if (!_strnicmp(key, roots[i].name, strlen(roots[i].name))) {
            *rest = key + strlen(roots[i].name);
            return roots[i].root;
        }
    return NULL;
}

static void describe_value(sb_t *b, DWORD type, const BYTE *data, DWORD size)
{
    switch (type) {
    case REG_SZ:
    case REG_EXPAND_SZ: {
        WCHAR *w = malloc(size + sizeof(WCHAR));

        memcpy(w, data, size);
        w[size / sizeof(WCHAR)] = 0;
        sb_add(b, "\"");
        sb_addw(b, w);
        sb_add(b, "\"");
        free(w);
        break;
    }
    case REG_DWORD:
        sb_addf(b, "dword %lu (0x%lx)", *(const DWORD *)data, *(const DWORD *)data);
        break;
    default:
        sb_addf(b, "(type %lu, %lu bytes)", (unsigned long)type, (unsigned long)size);
    }
}

static void read_registry(sb_t *b, const char *path)
{
    const char *rest;
    HKEY root = parse_root(path, &rest), key;
    WCHAR *wrest, name[256];
    BYTE data[2048];
    DWORD i, len, size, type;

    if (!root) {
        sb_add(b, "The key must start with HKEY_CURRENT_USER\\, HKEY_LOCAL_MACHINE\\, "
                  "HKEY_CLASSES_ROOT\\ or HKEY_USERS\\.");
        return;
    }
    wrest = w_from_utf8(rest);
    if (RegOpenKeyExW(root, wrest, 0, KEY_READ, &key)) {
        sb_addf(b, "The key %s does not exist.", path);
        free(wrest);
        return;
    }
    free(wrest);
    sb_addf(b, "Values of %s:\n", path);
    for (i = 0; i < 80; i++) {
        len = ARRAYSIZE(name);
        size = sizeof(data) - 2;
        if (RegEnumValueW(key, i, name, &len, NULL, &type, data, &size))
            break;
        sb_add(b, "- ");
        sb_addw(b, name[0] ? name : L"(default)");
        sb_add(b, " = ");
        describe_value(b, type, data, size);
        sb_addc(b, '\n');
    }
    if (!i)
        sb_add(b, "(no values)\n");
    sb_add(b, "Subkeys:\n");
    for (i = 0; i < 80; i++) {
        len = ARRAYSIZE(name);
        if (RegEnumKeyExW(key, i, name, &len, NULL, NULL, NULL, NULL))
            break;
        sb_add(b, "- ");
        sb_addw(b, name);
        sb_addc(b, '\n');
    }
    if (!i)
        sb_add(b, "(none)\n");
    RegCloseKey(key);
}

static void read_log(sb_t *b, const json *args, char **label)
{
    const char *name = arg_str(args, "name");
    long lines = arg_int(args, "lines", 60);
    BOOL problems = json_get(args, "problems_only") && json_get(args, "problems_only")->type == J_TRUE;
    char path[96], *text, *line, *save = NULL, **keep;
    int n = 0, i;

    if (!valid_log_name(name)) {
        sb_add(b, "Available logs: ");
        log_names(b);
        sb_addc(b, '\n');
        *label = utf8_dup("Listed the system logs");
        return;
    }
    if (lines < 1)
        lines = 1;
    if (lines > 200)
        lines = 200;
    snprintf(path, sizeof(path), "/run/log/%s%s", name, strstr(name, ".log") ? "" : ".log");
    text = read_unix_tail(path, 256 * 1024);
    if (!text) {
        sb_addf(b, "There is no log called %s. Available logs: ", name);
        log_names(b);
        sb_addc(b, '\n');
        *label = utf8_dup("Listed the system logs");
        return;
    }
    keep = calloc(lines, sizeof(*keep));
    for (line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (problems ? !problem(line) : (strstr(line, ":trace:") != NULL))
            continue;
        if (n == lines) {
            memmove(keep, keep + 1, sizeof(*keep) * (lines - 1));
            n--;
        }
        keep[n++] = line;
    }
    sb_addf(b, "Last %d %slines of %s:\n", n, problems ? "problem " : "", path);
    for (i = 0; i < n; i++)
        sb_addf(b, "%.400s\n", keep[i]);
    free(keep);
    free(text);
    *label = malloc(128);
    snprintf(*label, 128, "Read the %s log", name);
}

static void disk_usage(sb_t *b)
{
    ULARGE_INTEGER fr, total;
    char t1[32], t2[32], *mounts, *line, *save = NULL;

    if (GetDiskFreeSpaceExW(L"C:\\", &fr, &total, NULL)) {
        fmt_bytes(t1, sizeof(t1), fr.QuadPart);
        fmt_bytes(t2, sizeof(t2), total.QuadPart);
        sb_addf(b, "C: (programs and files) %s free of %s.\n", t1, t2);
    }
    sb_add(b, "WinCraft's storage:\n");
    mounts = read_unix_file("/proc/mounts", 64 * 1024);
    for (line = mounts ? strtok_r(mounts, "\n", &save) : NULL; line; line = strtok_r(NULL, "\n", &save)) {
        char dev[128], dir[128], type[32];

        if (sscanf(line, "%127s %127s %31s", dev, dir, type) != 3)
            continue;
        if (!strcmp(dir, "/live/medium"))
            sb_addf(b, "- system partition or boot medium: %s (%s), read-only\n", dev, type);
        else if (!strcmp(dir, "/live/system"))
            sb_addf(b, "- system image: %s (%s), read-only\n", dev, type);
        else if (!strcmp(dir, "/live/data"))
            sb_addf(b, "- data partition: %s (%s): everything installed or changed is kept here\n", dev, type);
        else if (!strcmp(dir, "/"))
            sb_addf(b, "- the running system: %s, the system image with the changes on top\n", type);
    }
    free(mounts);
    if (GetFileAttributesW(UNIX_ROOT L"\\run\\wincraft\\live") != INVALID_FILE_ATTRIBUTES)
        sb_add(b, "This is a live session: changes are kept in memory and lost at restart.\n");
}

/* ---- changes ---- */

static BOOL valid_dll_name(const char *s)
{
    size_t n = strlen(s);

    if (!n || n > 64)
        return FALSE;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') ||
              *s == '_' || *s == '-' || *s == '.'))
            return FALSE;
    return TRUE;
}

/* A program's file name, which becomes a registry key name: anything Windows
 * allows in a file name (notepad++.exe, My Game (x86).exe), but no path. */
static BOOL valid_program_name(const char *s)
{
    size_t n = strlen(s);

    if (!n || n > 128)
        return FALSE;
    for (; *s; s++)
        if ((unsigned char)*s < 0x20 || strchr("\\/:*?\"<>|", *s))
            return FALSE;
    return TRUE;
}

static void set_dll_override(sb_t *b, const json *args, approve_fn approve, void *ctx,
                             const tool_def *t, char **label)
{
    static const struct { const char *mode, *words; } modes[] = {
        { "native", "use the DLL the program brings (Windows' own), never Wine's" },
        { "builtin", "use Wine's own version only" },
        { "native,builtin", "prefer the program's DLL, fall back to Wine's" },
        { "builtin,native", "prefer Wine's version, fall back to the program's" },
        { "disabled", "do not load it at all" },
        { "default", "remove the override (Wine decides)" },
    };
    const char *dll = arg_str(args, "dll"), *mode = arg_str(args, "mode"), *prog = arg_str(args, "program");
    char dllname[80], title[160], detail[640];
    const char *words = NULL;
    WCHAR path[512], *wdll, *wmode;
    unsigned i;
    HKEY key;
    LONG rc;

    snprintf(dllname, sizeof(dllname), "%s", dll);
    if (strlen(dllname) > 4 && !_stricmp(dllname + strlen(dllname) - 4, ".dll"))
        dllname[strlen(dllname) - 4] = 0;
    for (i = 0; i < ARRAYSIZE(modes); i++)
        if (!strcmp(mode, modes[i].mode))
            words = modes[i].words;
    if (!valid_dll_name(dllname) || !words || (*prog && !valid_program_name(prog))) {
        sb_add(b, "Invalid request: dll must be a DLL name, mode one of native, builtin, "
                  "native,builtin, builtin,native, disabled, default; program an .exe name.");
        *label = utf8_dup("A library change was refused (invalid request)");
        return;
    }
    snprintf(title, sizeof(title), "Change how Wine loads %s.dll?", dllname);
    snprintf(detail, sizeof(detail),
             "For %s: %s.\n\nThis is Wine's own setting. It can be changed back in Compatibility "
             "Settings, on the Libraries tab.", *prog ? prog : "all programs", words);
    if (!approve(t, title, detail, ctx)) {
        sb_add(b, "The person did not allow this change. Nothing was changed.");
        *label = utf8_dup("A library change was not allowed");
        return;
    }
    if (*prog) {
        WCHAR *wprog = w_from_utf8(prog);   /* %S would read it in the ANSI code page */

        _snwprintf(path, ARRAYSIZE(path), L"Software\\Wine\\AppDefaults\\%s\\DllOverrides", wprog);
        free(wprog);
    } else
        _snwprintf(path, ARRAYSIZE(path), L"Software\\Wine\\DllOverrides");
    path[ARRAYSIZE(path) - 1] = 0;
    wdll = w_from_utf8(dllname);
    wmode = w_from_utf8(mode);
    rc = RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL);
    if (!rc) {
        if (!strcmp(mode, "default"))
            rc = RegDeleteValueW(key, wdll);
        else
            rc = RegSetValueExW(key, wdll, 0, REG_SZ, (const BYTE *)wmode,
                                (DWORD)((wcslen(wmode) + 1) * sizeof(WCHAR)));
        RegCloseKey(key);
    }
    free(wdll);
    free(wmode);
    if (rc && !(rc == ERROR_FILE_NOT_FOUND && !strcmp(mode, "default"))) {
        sb_addf(b, "The change failed (error %ld).", rc);
        *label = utf8_dup("A library change failed");
        return;
    }
    sb_addf(b, "Done: %s.dll is now set to '%s' for %s. Programs pick this up when they next start.",
            dllname, mode, *prog ? prog : "all programs");
    *label = malloc(160);
    snprintf(*label, 160, "Changed how Wine loads %s.dll (allowed)", dllname);
    activity_log("changed DllOverrides: %s=%s for %s", dllname, mode, *prog ? prog : "all programs");
}

/* Keys that make programs start on their own or change the session: a
 * manipulated conversation could ask for them, so they are never set here. */
static BOOL protected_key(const char *path)
{
    static const char *parts[] = {
        "\\currentversion\\run", "\\policies", "\\winlogon", "\\environment", "\\shell folders",
        "\\currentversion\\explorer\\user shell folders", "\\classes\\", "\\appinit",
        "\\image file execution options",
    };
    char lower[512];
    size_t i;

    snprintf(lower, sizeof(lower), "\\%s", path);
    for (i = 0; lower[i]; i++)
        lower[i] = (char)tolower((unsigned char)lower[i]);
    for (i = 0; i < ARRAYSIZE(parts); i++)
        if (strstr(lower, parts[i]))
            return TRUE;
    return FALSE;
}

static void set_registry_value(sb_t *b, const json *args, approve_fn approve, void *ctx,
                               const tool_def *t, char **label)
{
    const char *path = arg_str(args, "key"), *name = arg_str(args, "name"), *type = arg_str(args, "type"),
               *data = arg_str(args, "data"), *rest;
    HKEY root = parse_root(path, &rest), key;
    char detail[1024];
    sb_t cur;
    WCHAR *wrest, *wname, *wdata;
    BYTE old[1024];
    DWORD oldsize = sizeof(old) - 2, oldtype, dw;
    LONG rc;
    BOOL dword = !strcmp(type, "dword");

    if (root != HKEY_CURRENT_USER || (!dword && strcmp(type, "string"))) {
        sb_add(b, "Refused: only values under HKEY_CURRENT_USER, of type string or dword, can be set.");
        *label = utf8_dup("A registry change was refused (outside HKEY_CURRENT_USER)");
        return;
    }
    if (protected_key(rest)) {
        sb_add(b, "Refused: this key starts programs automatically or changes how the system behaves. "
                  "If the person really wants it, they can set it themselves in Registry Editor.");
        *label = utf8_dup("A registry change was refused (protected key)");
        return;
    }
    wrest = w_from_utf8(rest);
    wname = w_from_utf8(name);
    wdata = w_from_utf8(data);
    sb_init(&cur);
    if (!RegOpenKeyExW(HKEY_CURRENT_USER, wrest, 0, KEY_READ, &key)) {
        if (!RegQueryValueExW(key, wname, NULL, &oldtype, old, &oldsize))
            describe_value(&cur, oldtype, old, oldsize);
        RegCloseKey(key);
    }
    snprintf(detail, sizeof(detail), "Key: %s\nValue: %s\nNew data: %s%s\nNow: %s", path,
             *name ? name : "(default)", dword ? "dword " : "", data, cur.len ? cur.s : "not set");
    sb_free(&cur);
    if (!approve(t, "Change a registry setting?", detail, ctx)) {
        sb_add(b, "The person did not allow this change. Nothing was changed.");
        *label = utf8_dup("A registry change was not allowed");
        goto out;
    }
    rc = RegCreateKeyExW(HKEY_CURRENT_USER, wrest, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL);
    if (!rc) {
        if (dword) {
            dw = strtoul(data, NULL, 0);
            rc = RegSetValueExW(key, wname, 0, REG_DWORD, (const BYTE *)&dw, sizeof(dw));
        } else {
            rc = RegSetValueExW(key, wname, 0, REG_SZ, (const BYTE *)wdata,
                                (DWORD)((wcslen(wdata) + 1) * sizeof(WCHAR)));
        }
        RegCloseKey(key);
    }
    if (rc) {
        sb_addf(b, "The change failed (error %ld).", rc);
        *label = utf8_dup("A registry change failed");
    } else {
        sb_add(b, "Done: the value was set.");
        *label = utf8_dup("Changed a registry setting (allowed)");
        activity_log("set registry value %s : %s = %s", path, name, data);
    }
out:
    free(wrest);
    free(wname);
    free(wdata);
}

static void end_program(sb_t *b, const json *args, approve_fn approve, void *ctx, const tool_def *t,
                        char **label)
{
    DWORD pid = (DWORD)arg_int(args, "pid", 0);
    WCHAR exe[MAX_PATH];
    char title[300], detail[800], *name;
    struct win_info wi;
    sb_t wins;
    HANDLE h;

    if (!pid || !process_name(pid, exe, MAX_PATH)) {
        sb_addf(b, "There is no running program with pid %lu. Use list_programs.", (unsigned long)pid);
        *label = utf8_dup("Could not find the program to end");
        return;
    }
    name = utf8_from_w(exe);
    if (system_process(exe)) {
        sb_addf(b, "Refused: %s is part of Wine or WinCraft, ending it would break the desktop.", name);
        *label = utf8_dup("Refused to end a part of the system");
        free(name);
        return;
    }
    sb_init(&wins);
    wi.pid = pid;
    wi.out = &wins;
    wi.count = 0;
    EnumWindows(windows_of_pid, (LPARAM)&wi);
    snprintf(title, sizeof(title), "End %s?", name);
    snprintf(detail, sizeof(detail), "%s (process %lu) will be stopped at once, like End Task in Task "
             "Manager. Anything not saved in it is lost.%s%s", name, (unsigned long)pid,
             wins.len ? "\n\n" : "", wins.len ? trim(wins.s) : "");
    sb_free(&wins);
    if (!approve(t, title, detail, ctx)) {
        sb_add(b, "The person did not allow this. The program is still running.");
        *label = malloc(160);
        snprintf(*label, 160, "Ending %s was not allowed", name);
        free(name);
        return;
    }
    h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (h && TerminateProcess(h, 1)) {
        sb_addf(b, "Done: %s was ended.", name);
        *label = malloc(160);
        snprintf(*label, 160, "Ended %s (allowed)", name);
        activity_log("ended program %s (pid %lu)", name, (unsigned long)pid);
    } else {
        sb_addf(b, "Ending %s failed (error %lu).", name, GetLastError());
        *label = utf8_dup("Ending the program failed");
    }
    if (h)
        CloseHandle(h);
    free(name);
}

/* Fetch an https address into a file. The assistant names itself instead of
 * posing as a browser, so download sites send the file rather than a page that
 * forwards a browser to it. WinHTTP follows redirects, but not from https to
 * plain http. */
static BOOL download_file(const WCHAR *url, const WCHAR *path, ULONGLONG *size, char *err, size_t errsize)
{
    URL_COMPONENTS uc;
    WCHAR host[256];
    HINTERNET ses = NULL, con = NULL, req = NULL;
    HANDLE f = INVALID_HANDLE_VALUE;
    DWORD status = 0, len = sizeof(status), got, put;
    BOOL ok = FALSE;
    static char chunk[65536];

    *size = 0;
    ZeroMemory(&uc, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.dwHostNameLength = uc.dwUrlPathLength = (DWORD)-1;   /* point into url: the path keeps its ?query */
    if (!WinHttpCrackUrl(url, 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS ||
        uc.dwHostNameLength >= ARRAYSIZE(host)) {
        snprintf(err, errsize, "the address is not a valid https address");
        return FALSE;
    }
    lstrcpynW(host, uc.lpszHostName, uc.dwHostNameLength + 1);
    ses = WinHttpOpen(L"WinCraftAssistant/" WC_VERSION, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (ses)
        WinHttpSetTimeouts(ses, 15000, 15000, 60000, 60000);
    if (!ses || !(con = WinHttpConnect(ses, host, uc.nPort, 0)) ||
        !(req = WinHttpOpenRequest(con, L"GET", uc.lpszUrlPath, NULL, WINHTTP_NO_REFERER,
                                   WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)) ||
        !WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, NULL)) {
        DWORD e = GetLastError();

        switch (e) {
        case ERROR_WINHTTP_NAME_NOT_RESOLVED: snprintf(err, errsize, "the site's address could not be found"); break;
        case ERROR_WINHTTP_CANNOT_CONNECT:    snprintf(err, errsize, "could not connect to the site"); break;
        case ERROR_WINHTTP_TIMEOUT:           snprintf(err, errsize, "the site took too long to answer"); break;
        case ERROR_WINHTTP_REDIRECT_FAILED:   /* it will not follow https to plain http */
            snprintf(err, errsize, "the site sent it on to an address that is not https, which is not downloaded");
            break;
        case ERROR_WINHTTP_SECURE_FAILURE:
            snprintf(err, errsize, "the secure connection failed (certificate problem, or the clock is wrong)");
            break;
        default: snprintf(err, errsize, "the connection failed (error %lu)", (unsigned long)e); break;
        }
        goto out;
    }
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &len, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        snprintf(err, errsize, "the site answered with HTTP status %lu", (unsigned long)status);
        goto out;
    }
    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        snprintf(err, errsize, "the file could not be created in Downloads");
        goto out;
    }
    for (;;) {
        if (llm_cancelled()) {
            snprintf(err, errsize, "stopped by the person");
            goto out;
        }
        if (!WinHttpReadData(req, chunk, sizeof(chunk), &got)) {
            snprintf(err, errsize, "the connection broke off after %llu bytes", *size);
            goto out;
        }
        if (!got)
            break;
        if (!WriteFile(f, chunk, got, &put, NULL) || put != got) {
            snprintf(err, errsize, "the file could not be written (is the disk full?)");
            goto out;
        }
        *size += got;
    }
    ok = TRUE;
out:
    if (f != INVALID_HANDLE_VALUE) {
        CloseHandle(f);
        if (!ok)
            DeleteFileW(path);
    }
    if (req)
        WinHttpCloseHandle(req);
    if (con)
        WinHttpCloseHandle(con);
    if (ses)
        WinHttpCloseHandle(ses);
    return ok;
}

/* Whether a file is a Windows program (.exe) or a Windows Installer package
 * (.msi). If not, *what says what it looks like instead. */
static BOOL runnable_file(const WCHAR *path, char *what, size_t size)
{
    char head[4096], *t, *e;
    DWORD got = 0;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);

    if (f == INVALID_HANDLE_VALUE) {
        snprintf(what, size, "a file that cannot be read");
        return FALSE;
    }
    ReadFile(f, head, sizeof(head) - 1, &got, NULL);
    CloseHandle(f);
    head[got] = 0;
    if (got >= 2 && head[0] == 'M' && head[1] == 'Z')
        return TRUE;
    if (got >= 8 && !memcmp(head, "\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1", 8))
        return TRUE;
    if (got >= 4 && !memcmp(head, "PK\3\4", 4)) {
        snprintf(what, size, "a ZIP archive");
        return FALSE;
    }
    if (StrStrIA(head, "<html") || StrStrIA(head, "<!doctype html")) {
        if ((t = StrStrIA(head, "<title>")) && (e = StrStrIA(t += 7, "</title>")) && e - t < 120) {
            char *p;

            *e = 0;
            for (p = t; *p; p++)
                if ((unsigned char)*p < ' ' || *p == '"')
                    *p = ' ';
            snprintf(what, size, "a web page titled \"%s\"", trim(t));
        } else {
            snprintf(what, size, "a web page");
        }
        return FALSE;
    }
    snprintf(what, size, "a file that is not a Windows program");
    return FALSE;
}

static void download_and_run(sb_t *b, const json *args, approve_fn approve, void *ctx,
                             const tool_def *t, char **label)
{
    const char *url = arg_str(args, "url");
    WCHAR *wurl, file[MAX_PATH], *dl = NULL, path[MAX_PATH], *slash, *q;
    char detail[1200], *upath, err[200], what[200], bytes[32];
    ULONGLONG got;

    if (_strnicmp(url, "https://", 8) || strlen(url) > 1000 || strpbrk(url, " \"\r\n")) {
        sb_add(b, "Refused: only https:// addresses can be downloaded.");
        *label = utf8_dup("A download was refused (not https)");
        return;
    }
    wurl = w_from_utf8(url);
    /* the file name is the address's last part, minus any ?query */
    slash = wcsrchr(wurl + 8, L'/');
    lstrcpynW(file, slash && slash[1] ? slash + 1 : L"download.exe", MAX_PATH);
    if ((q = wcspbrk(file, L"?#")))
        *q = 0;
    for (q = file; *q; q++)
        if (wcschr(L"\\/:*\"<>|", *q))
            *q = L'_';
    if (!file[0])
        lstrcpyW(file, L"download.exe");
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_Downloads, KF_FLAG_CREATE, NULL, &dl))) {
        sb_add(b, "The Downloads folder is not available.");
        *label = utf8_dup("A download failed");
        free(wurl);
        return;
    }
    PathCombineW(path, dl, file);
    CoTaskMemFree(dl);
    upath = utf8_from_w(path);
    snprintf(detail, sizeof(detail), "From: %s\nSaved as: %s\n\nThe program then starts with your "
             "rights. Only allow this if you trust where it comes from.", url, upath);
    if (!approve(t, "Download and run a program?", detail, ctx)) {
        sb_add(b, "The person did not allow the download. Nothing was downloaded.");
        *label = utf8_dup("A download was not allowed");
        goto out;
    }
    if (!download_file(wurl, path, &got, err, sizeof(err))) {
        sb_addf(b, "The download failed: %s. Nothing was run.", err);
        *label = utf8_dup("The download failed");
        goto out;
    }
    fmt_bytes(bytes, sizeof(bytes), got);
    if (got < (1 << 20))
        snprintf(bytes, sizeof(bytes), "%llu KB", (got + 1023) >> 10);
    if (!runnable_file(path, what, sizeof(what))) {
        DeleteFileW(path);
        sb_addf(b, "The address gave %s (%s), not a Windows program or installer, so nothing was run and "
                   "the file was deleted. A site that shows a download page needs the direct address of "
                   "the .exe or .msi file instead.", what, bytes);
        *label = utf8_dup("Downloaded something that is not a program; nothing was run");
        activity_log("download from %s was %s; deleted", url, what);
        goto out;
    }
    if ((INT_PTR)ShellExecuteW(NULL, L"open", path, NULL, NULL, SW_SHOWNORMAL) <= 32) {
        sb_addf(b, "Downloaded to %s (%s), but it could not be started.", upath, bytes);
        *label = utf8_dup("Downloaded a program, but it did not start");
    } else {
        sb_addf(b, "Downloaded to %s (%s) and started. Its window should appear on the desktop.", upath, bytes);
        *label = utf8_dup("Downloaded and started a program (allowed)");
        activity_log("downloaded and ran %s -> %s", url, upath);
    }
out:
    free(upath);
    free(wurl);
}

static void program_backtrace(sb_t *b, const json *args, approve_fn approve, void *ctx,
                              const tool_def *t, char **label)
{
    DWORD pid = (DWORD)arg_int(args, "pid", 0);
    WCHAR exe[MAX_PATH], host[256];
    char title[300], detail[800], *name, *out;

    if (!pid || !process_name(pid, exe, MAX_PATH)) {
        sb_addf(b, "There is no running program with pid %lu. Use list_programs.", (unsigned long)pid);
        *label = utf8_dup("Could not find the program to look into");
        return;
    }
    name = utf8_from_w(exe);
    lstrcpynW(host, g_provider_host[0] ? g_provider_host : L"the AI provider", ARRAYSIZE(host));
    snprintf(title, sizeof(title), "Look inside %s?", name);
    snprintf(detail, sizeof(detail), "The assistant wants to attach Wine's debugger to %s (process %lu) "
             "and note where each of its threads is waiting. The program pauses for a second or two, "
             "then carries on. The list of threads and code locations is sent to %.100S.", name,
             (unsigned long)pid, host);
    if (!approve(t, title, detail, ctx)) {
        sb_add(b, "The person did not allow this.");
        *label = utf8_dup("Looking inside the program was not allowed");
        free(name);
        return;
    }
    out = debugger_output(pid);
    if (!out) {
        sb_add(b, "Wine's debugger could not be started.");
        *label = utf8_dup("The debugger could not be started");
    } else {
        summarize_backtrace(b, out, pid);
        *label = malloc(160);
        snprintf(*label, 160, "Looked inside %s with the debugger (allowed)", name);
        activity_log("debugger backtrace of %s (pid %lu)", name, (unsigned long)pid);
    }
    free(out);
    free(name);
}

char *tool_run(const tool_def *t, const json *args, approve_fn approve, void *ctx, char **label)
{
    sb_t b;
    const char *n = t->name;

    sb_init(&b);
    *label = NULL;
    if (!strcmp(n, "get_system_snapshot")) {
        char *s = system_snapshot();

        sb_add(&b, s);
        free(s);
        *label = utf8_dup("Looked at the system overview");
    } else if (!strcmp(n, "list_programs")) {
        json *sys = json_get(args, "include_system");

        list_running(&b, sys && sys->type == J_TRUE);
        *label = utf8_dup("Looked at the running programs");
    } else if (!strcmp(n, "list_installed_programs")) {
        list_installed(&b);
        *label = utf8_dup("Looked at the installed programs");
    } else if (!strcmp(n, "read_log")) {
        read_log(&b, args, label);
    } else if (!strcmp(n, "read_registry")) {
        read_registry(&b, arg_str(args, "key"));
        *label = utf8_dup("Read a registry key");
    } else if (!strcmp(n, "check_network")) {
        network_summary(&b, TRUE);
        connection_test(&b);
        *label = utf8_dup("Checked the network");
    } else if (!strcmp(n, "list_crash_reports")) {
        int count = crash_reports(&b);

        if (!count)
            sb_add(&b, "No crash reports found.\n");
        else if (count > 25)
            sb_addf(&b, "... %d in total.\n", count);
        *label = utf8_dup("Looked for crash reports");
    } else if (!strcmp(n, "disk_usage")) {
        disk_usage(&b);
        *label = utf8_dup("Looked at the storage");
    } else if (!strcmp(n, "program_backtrace")) {
        program_backtrace(&b, args, approve, ctx, t, label);
    } else if (!strcmp(n, "end_program")) {
        end_program(&b, args, approve, ctx, t, label);
    } else if (!strcmp(n, "set_dll_override")) {
        set_dll_override(&b, args, approve, ctx, t, label);
    } else if (!strcmp(n, "set_registry_value")) {
        set_registry_value(&b, args, approve, ctx, t, label);
    } else if (!strcmp(n, "download_and_run")) {
        download_and_run(&b, args, approve, ctx, t, label);
    }
    if (!*label)
        *label = utf8_dup(n);
    cap_result(&b);
    return sb_take(&b);
}
