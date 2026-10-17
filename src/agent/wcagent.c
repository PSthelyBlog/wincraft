/*
 * wcagent - WinCraft Assistant.
 *
 * A chat window that answers questions about WinCraft OS and this computer,
 * with a model from a provider the person chooses (their own account, or a
 * model server on their network). The assistant knows WinCraft from a
 * reference pack built into the system image, matching this version, and
 * sees the machine through a fixed set of tools (tools.c). Every change to
 * the computer waits for the person's permission in a dialog the model has
 * no way to answer.
 *
 *   wcagent.exe             open the chat (first time: the setup window)
 *   wcagent.exe /settings   open the setup window
 *
 * Kept in %APPDATA%\WinCraft Assistant: settings.ini, key (the API key, if
 * the person lets it be remembered), activity.log, chats\*.txt.
 *
 * SPDX-License-Identifier: MIT
 */
#include "agent.h"
#include <windowsx.h>
#include <commctrl.h>
#include <richedit.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_STEPS       12          /* model calls per question */

enum {
    IDC_TRANSCRIPT = 100, IDC_INPUT, IDC_SEND, IDC_STOP, IDC_STATUS,
    IDM_NEW = 200, IDM_SEE, IDM_ACTIVITY, IDM_SETTINGS, IDM_EXIT, IDM_ABOUT,
    /* setup window */
    IDC_PRESET = 300, IDC_BASE, IDC_KEY, IDC_REMEMBER, IDC_MODEL, IDC_LOAD, IDC_TEST, IDC_SAVE,
    IDC_SETUP_STATUS, IDC_WEB,
};

enum {
    WM_APP_APPEND = WM_APP + 1,     /* lParam: WCHAR* (freed here) */
    WM_APP_STATUS,                  /* lParam: WCHAR* (freed here) */
    WM_APP_DONE,
    WM_APP_APPROVE,                 /* lParam: struct approval* ; returns BOOL */
    WM_APP_SETUP_RESULT,            /* wParam: what; lParam: struct setup_job* */
};

static HINSTANCE g_inst;
static HWND g_main, g_transcript, g_input, g_send, g_stop, g_status;
static HFONT g_font, g_font_bold;
static config g_cfg;
WCHAR g_provider_host[256];         /* also read by tools.c */
int g_provider_port;
static json *g_messages, *g_tools_json;
static char *g_system, *g_snapshot;
static BOOL g_busy, g_diag_allowed, g_web_allowed;   /* "Allow for this chat" */
static BOOL g_rich;                 /* the chat is a rich edit control, not a plain one */
static long g_tok_in, g_tok_out;
static WCHAR g_chat_file[MAX_PATH];
static WNDPROC g_input_proc;
static HANDLE g_stop_event;         /* set by Stop: the chat thread stops waiting */

/* ======================================================================
 * Settings
 * ====================================================================== */

static const struct preset {
    const WCHAR *name;
    int format;
    const WCHAR *base;
    BOOL needs_key;
    const WCHAR *hint;
} g_presets[] = {
    { L"Anthropic (Claude)", FMT_ANTHROPIC, L"https://api.anthropic.com", TRUE,
      L"Create a key at console.anthropic.com." },
    { L"OpenAI", FMT_OPENAI, L"https://api.openai.com/v1", TRUE,
      L"Create a key at platform.openai.com." },
    { L"OpenRouter", FMT_OPENAI, L"https://openrouter.ai/api/v1", TRUE,
      L"One key for models from many providers: openrouter.ai." },
    { L"Ollama on another computer", FMT_OPENAI, L"http://192.168.1.10:11434/v1", FALSE,
      L"Put that computer's address in place of 192.168.1.10. No key needed." },
    { L"Other OpenAI-compatible service", FMT_OPENAI, L"https://", FALSE,
      L"Any server with an OpenAI-compatible /chat/completions endpoint." },
};

static void settings_path(WCHAR *out, const WCHAR *file)
{
    app_data_dir(out);
    PathAppendW(out, file);
}

static BOOL load_config(config *c)
{
    WCHAR ini[MAX_PATH], keyfile[MAX_PATH], fmt[32];
    char *key;

    ZeroMemory(c, sizeof(*c));
    settings_path(ini, L"settings.ini");
    settings_path(keyfile, L"key");
    GetPrivateProfileStringW(L"Provider", L"Format", L"", fmt, ARRAYSIZE(fmt), ini);
    GetPrivateProfileStringW(L"Provider", L"Preset", L"", c->preset, ARRAYSIZE(c->preset), ini);
    GetPrivateProfileStringW(L"Provider", L"Address", L"", c->base_url, ARRAYSIZE(c->base_url), ini);
    GetPrivateProfileStringW(L"Provider", L"Model", L"", c->model, ARRAYSIZE(c->model), ini);
    c->remember_key = GetPrivateProfileIntW(L"Provider", L"RememberKey", 1, ini);
    c->web_pages = GetPrivateProfileIntW(L"Assistant", L"WebPages", 1, ini);
    c->format = !lstrcmpiW(fmt, L"anthropic") ? FMT_ANTHROPIC : FMT_OPENAI;
    if (c->remember_key && (key = read_file_utf8(keyfile, 4096, NULL))) {
        WCHAR *w = w_from_utf8(key);

        lstrcpynW(c->key, w, ARRAYSIZE(c->key));
        StrTrimW(c->key, L" \r\n\t");
        free(w);
        free(key);
    }
    return fmt[0] && c->base_url[0] && c->model[0];
}

static BOOL preset_needs_key(const WCHAR *name)
{
    unsigned i;

    for (i = 0; i < ARRAYSIZE(g_presets); i++)
        if (!lstrcmpW(name, g_presets[i].name))
            return g_presets[i].needs_key;
    return FALSE;
}

static void save_config(const config *c)
{
    WCHAR ini[MAX_PATH], keyfile[MAX_PATH], cmd[MAX_PATH + 64], unixname[MAX_PATH];
    HANDLE h;
    DWORD put;
    char *key;

    settings_path(ini, L"settings.ini");
    settings_path(keyfile, L"key");
    WritePrivateProfileStringW(L"Provider", L"Format", c->format == FMT_ANTHROPIC ? L"anthropic" : L"openai", ini);
    WritePrivateProfileStringW(L"Provider", L"Preset", c->preset, ini);
    WritePrivateProfileStringW(L"Provider", L"Address", c->base_url, ini);
    WritePrivateProfileStringW(L"Provider", L"Model", c->model, ini);
    WritePrivateProfileStringW(L"Provider", L"RememberKey", c->remember_key ? L"1" : L"0", ini);
    WritePrivateProfileStringW(L"Assistant", L"WebPages", c->web_pages ? L"1" : L"0", ini);
    DeleteFileW(keyfile);
    if (!c->remember_key || !c->key[0])
        return;
    h = CreateFileW(keyfile, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    key = utf8_from_w(c->key);
    WriteFile(h, key, (DWORD)strlen(key), &put, NULL);
    SecureZeroMemory(key, strlen(key));
    free(key);
    CloseHandle(h);
    /* Only the desktop user may read it: Wine creates files readable by all. */
    {
        typedef char *(CDECL *unix_name_fn)(const WCHAR *);
        unix_name_fn get = (unix_name_fn)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
                                                        "wine_get_unix_file_name");
        char *u = get ? get(keyfile) : NULL;

        if (u) {
            MultiByteToWideChar(CP_UTF8, 0, u, -1, unixname, MAX_PATH);
            HeapFree(GetProcessHeap(), 0, u);
            _snwprintf(cmd, ARRAYSIZE(cmd), L"chmod 600 \"%s\"", unixname);
            cmd[ARRAYSIZE(cmd) - 1] = 0;
            if (!run_native(L"/usr/bin/busybox", cmd, 5000))
                activity_log("could not restrict access to the key file");
        } else {
            activity_log("could not find the key file's native path");
        }
    }
}

static HRESULT make_link(const WCHAR *folder, const WCHAR *name, const WCHAR *target, const WCHAR *desc)
{
    WCHAR path[MAX_PATH], file[128];
    IShellLinkW *link;
    IPersistFile *pf;
    HRESULT hr;

    hr = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link);
    if (FAILED(hr))
        return hr;
    IShellLinkW_SetPath(link, target);
    IShellLinkW_SetDescription(link, desc);
    IShellLinkW_SetIconLocation(link, target, 0);
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

/* "Installing" the assistant: once set up, it gets a Start menu entry. */
static void add_start_menu_entry(void)
{
    WCHAR programs[MAX_PATH], self[MAX_PATH];

    if (FAILED(SHGetFolderPathW(NULL, CSIDL_PROGRAMS | CSIDL_FLAG_CREATE, NULL, SHGFP_TYPE_CURRENT, programs)))
        return;
    GetModuleFileNameW(NULL, self, MAX_PATH);
    make_link(programs, APP_NAME, self, L"Ask about WinCraft OS and this computer");
}

static BOOL live_session(void)
{
    return GetFileAttributesW(UNIX_ROOT L"\\run\\wincraft\\live") != INVALID_FILE_ATTRIBUTES;
}

/* ======================================================================
 * The setup window
 * ====================================================================== */

struct setup_job {
    config cfg;
    int what;           /* IDC_LOAD or IDC_TEST */
    char *models;       /* NUL-separated */
    char *text;         /* test reply or error */
    BOOL ok;
};

static HWND g_setup;
static BOOL g_setup_saved, g_setup_busy;

static void setup_status(const WCHAR *s)
{
    SetDlgItemTextW(g_setup, IDC_SETUP_STATUS, s);
}

static void setup_read(config *c)
{
    int sel = (int)SendDlgItemMessageW(g_setup, IDC_PRESET, CB_GETCURSEL, 0, 0);

    ZeroMemory(c, sizeof(*c));
    if (sel < 0 || sel >= (int)ARRAYSIZE(g_presets))
        sel = 0;
    c->format = g_presets[sel].format;
    lstrcpynW(c->preset, g_presets[sel].name, ARRAYSIZE(c->preset));
    GetDlgItemTextW(g_setup, IDC_BASE, c->base_url, ARRAYSIZE(c->base_url));
    GetDlgItemTextW(g_setup, IDC_KEY, c->key, ARRAYSIZE(c->key));
    GetDlgItemTextW(g_setup, IDC_MODEL, c->model, ARRAYSIZE(c->model));
    StrTrimW(c->base_url, L" \t");
    StrTrimW(c->key, L" \t\r\n");
    StrTrimW(c->model, L" \t");
    c->remember_key = IsDlgButtonChecked(g_setup, IDC_REMEMBER) == BST_CHECKED;
    c->web_pages = IsDlgButtonChecked(g_setup, IDC_WEB) == BST_CHECKED;
}

static DWORD WINAPI setup_thread(void *arg)
{
    struct setup_job *job = arg;

    llm_reset_cancel();
    if (job->what == IDC_LOAD) {
        job->models = llm_models(&job->cfg, &job->text);
        job->ok = job->models != NULL;
    } else {
        json *msgs = json_new(J_ARRAY);
        llm_reply r;

        llm_add_user(&job->cfg, msgs, "This is a connection test. Reply with the single word OK.");
        job->ok = llm_chat(&job->cfg, "You are being tested by a setup program.", NULL, msgs, &r);
        job->text = job->ok ? utf8_dup(r.text ? r.text : "(an empty answer)") : utf8_dup(r.error);
        llm_reply_free(&r);
        json_free(msgs);
    }
    PostMessageW(g_setup, WM_APP_SETUP_RESULT, 0, (LPARAM)job);
    return 0;
}

static void setup_preset_changed(BOOL fill_address)
{
    int sel = (int)SendDlgItemMessageW(g_setup, IDC_PRESET, CB_GETCURSEL, 0, 0);
    WCHAR hint[256];

    if (sel < 0 || sel >= (int)ARRAYSIZE(g_presets))
        return;
    if (fill_address) {
        SetDlgItemTextW(g_setup, IDC_BASE, g_presets[sel].base);
        SendDlgItemMessageW(g_setup, IDC_MODEL, CB_RESETCONTENT, 0, 0);
        SetDlgItemTextW(g_setup, IDC_MODEL, L"");
    }
    _snwprintf(hint, ARRAYSIZE(hint), L"%s%s", g_presets[sel].hint,
               g_presets[sel].needs_key ? L" Then press Load models." : L"");
    hint[ARRAYSIZE(hint) - 1] = 0;
    setup_status(hint);
}

static void setup_start(int what)
{
    struct setup_job *job;
    HANDLE t;

    if (g_setup_busy)
        return;
    job = calloc(1, sizeof(*job));
    setup_read(&job->cfg);
    job->what = what;
    if (StrCmpNIW(job->cfg.base_url, L"http://", 7) && StrCmpNIW(job->cfg.base_url, L"https://", 8)) {
        setup_status(L"The address must start with https:// (or http:// for a server on your network).");
        free(job);
        return;
    }
    if (preset_needs_key(job->cfg.preset) && !job->cfg.key[0]) {
        setup_status(L"Enter your API key first.");
        free(job);
        return;
    }
    if (what == IDC_TEST && !job->cfg.model[0]) {
        setup_status(L"Choose or type a model first.");
        free(job);
        return;
    }
    g_setup_busy = TRUE;
    setup_status(what == IDC_LOAD ? L"Asking the provider for its models..." : L"Sending a short test message...");
    t = CreateThread(NULL, 0, setup_thread, job, 0, NULL);
    if (t)
        CloseHandle(t);
}

static void setup_result(struct setup_job *job)
{
    WCHAR *w;

    g_setup_busy = FALSE;
    if (job->what == IDC_LOAD && job->ok) {
        HWND combo = GetDlgItem(g_setup, IDC_MODEL);
        WCHAR current[160];
        const char *m;
        int pick = -1, n = 0;

        GetWindowTextW(combo, current, ARRAYSIZE(current));
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        for (m = job->models; *m; m += strlen(m) + 1) {
            w = w_from_utf8(m);
            SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)w);
            /* keep the person's choice; otherwise suggest a balanced Claude model */
            if (current[0] ? !lstrcmpW(w, current) : (pick < 0 && strstr(m, "sonnet") != NULL))
                pick = n;
            free(w);
            n++;
        }
        SendMessageW(combo, CB_SETCURSEL, pick >= 0 ? pick : 0, 0);
        setup_status(L"Models loaded. Pick one, then Test or Save.");
    } else if (job->what == IDC_TEST && job->ok) {
        WCHAR msg[512];

        w = w_from_utf8(job->text);
        _snwprintf(msg, ARRAYSIZE(msg), L"It works. The model answered: %.200s", w);
        msg[ARRAYSIZE(msg) - 1] = 0;
        setup_status(msg);
        free(w);
    } else {
        w = w_from_utf8(job->text ? job->text : "Something went wrong.");
        setup_status(w);
        free(w);
    }
    free(job->models);
    free(job->text);
    SecureZeroMemory(job->cfg.key, sizeof(job->cfg.key));
    free(job);
}

static LRESULT CALLBACK setup_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_PRESET:
            if (HIWORD(wp) == CBN_SELCHANGE)
                setup_preset_changed(TRUE);
            return 0;
        case IDC_LOAD:
        case IDC_TEST:
            setup_start(LOWORD(wp));
            return 0;
        case IDC_SAVE: {
            config c;

            setup_read(&c);
            if (StrCmpNIW(c.base_url, L"http://", 7) && StrCmpNIW(c.base_url, L"https://", 8)) {
                setup_status(L"The address must start with https:// (or http:// for a server on your network).");
                return 0;
            }
            if (preset_needs_key(c.preset) && !c.key[0]) {
                setup_status(L"Enter your API key.");
                return 0;
            }
            if (!c.model[0]) {
                setup_status(L"Choose or type a model.");
                return 0;
            }
            save_config(&c);
            g_cfg = c;
            g_setup_saved = TRUE;
            DestroyWindow(hwnd);
            return 0;
        }
        case IDCANCEL:
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_APP_SETUP_RESULT:
        setup_result((struct setup_job *)lp);
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g_setup = NULL;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* Returns TRUE when the person saved working settings. */
static BOOL setup_window(HWND owner, BOOL first)
{
    static const WCHAR *intro =
        L"The assistant answers questions about WinCraft OS and this computer, using an AI "
        L"model from a provider you choose: your own account, or a model server on your network.";
    static const WCHAR *privacy =
        L"What you type, and what the assistant looks up about this computer, is sent to that "
        L"provider. Nothing on the computer is changed unless you allow it.";
    WNDCLASSEXW wc;
    RECT rc = { 0, 0, 560, 430 };
    HWND ctl;
    MSG msg;
    int i, sel = 0;
    config cur;

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.hInstance = g_inst;
    wc.lpfnWndProc = setup_proc;
    wc.lpszClassName = L"WinCraftAssistantSetup";
    wc.hCursor = LoadCursorW(NULL, (const WCHAR *)IDC_ARROW);
    wc.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(1));
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassExW(&wc);
    AdjustWindowRect(&rc, WS_CAPTION | WS_SYSMENU, FALSE);
    g_setup = CreateWindowExW(0, wc.lpszClassName, first ? L"Set up WinCraft Assistant" : L"WinCraft Assistant settings",
                              WS_CAPTION | WS_SYSMENU,
                              (GetSystemMetrics(SM_CXSCREEN) - (rc.right - rc.left)) / 2,
                              (GetSystemMetrics(SM_CYSCREEN) - (rc.bottom - rc.top)) / 3,
                              rc.right - rc.left, rc.bottom - rc.top, owner, NULL, g_inst, NULL);
    if (!g_setup)
        return FALSE;
    {
        const struct { const WCHAR *cls, *text; DWORD style; int x, y, w, h, id; } items[] = {
            { L"STATIC", intro, 0, 16, 14, 528, 36, 0 },
            { L"STATIC", L"Provider:", 0, 16, 64, 90, 20, 0 },
            { L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 110, 60, 434, 200, IDC_PRESET },
            { L"STATIC", L"Address:", 0, 16, 100, 90, 20, 0 },
            { L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 110, 96, 434, 24, IDC_BASE },
            { L"STATIC", L"API key:", 0, 16, 136, 90, 20, 0 },
            { L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD, 110, 132, 434, 24, IDC_KEY },
            { L"BUTTON", L"Remember the key on this computer", WS_TABSTOP | BS_AUTOCHECKBOX, 110, 160, 434, 22, IDC_REMEMBER },
            { L"STATIC", L"Model:", 0, 16, 196, 90, 20, 0 },
            { L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL, 110, 192, 314, 240, IDC_MODEL },
            { L"BUTTON", L"Load models", WS_TABSTOP, 432, 191, 112, 26, IDC_LOAD },
            { L"BUTTON", L"Let the assistant read web pages, after asking you", WS_TABSTOP | BS_AUTOCHECKBOX,
              110, 226, 434, 22, IDC_WEB },
            { L"STATIC", L"", 0, 16, 258, 528, 40, IDC_SETUP_STATUS },
            { L"STATIC", privacy, 0, 16, 306, 528, 36, 0 },
            { L"STATIC", L"", 0, 16, 346, 528, 20, 1 },
            { L"BUTTON", L"Test", WS_TABSTOP, 252, 384, 90, 30, IDC_TEST },
            { L"BUTTON", L"Save", WS_TABSTOP | BS_DEFPUSHBUTTON, 350, 384, 90, 30, IDC_SAVE },
            { L"BUTTON", L"Cancel", WS_TABSTOP, 448, 384, 96, 30, IDCANCEL },
        };

        for (i = 0; i < (int)ARRAYSIZE(items); i++) {
            ctl = CreateWindowExW(0, items[i].cls, items[i].text, WS_CHILD | WS_VISIBLE | items[i].style,
                                  items[i].x, items[i].y, items[i].w, items[i].h, g_setup,
                                  (HMENU)(INT_PTR)items[i].id, g_inst, NULL);
            SendMessageW(ctl, WM_SETFONT, (WPARAM)g_font, TRUE);
        }
    }
    if (live_session())
        SetDlgItemTextW(g_setup, 1, L"This is a live session: these settings are lost when the computer restarts.");
    for (i = 0; i < (int)ARRAYSIZE(g_presets); i++)
        SendDlgItemMessageW(g_setup, IDC_PRESET, CB_ADDSTRING, 0, (LPARAM)g_presets[i].name);
    load_config(&cur);
    for (i = 0; i < (int)ARRAYSIZE(g_presets); i++)
        if (!lstrcmpW(cur.preset, g_presets[i].name))
            sel = i;
    SendDlgItemMessageW(g_setup, IDC_PRESET, CB_SETCURSEL, sel, 0);
    if (cur.base_url[0]) {
        SetDlgItemTextW(g_setup, IDC_BASE, cur.base_url);
        SetDlgItemTextW(g_setup, IDC_MODEL, cur.model);
        SetDlgItemTextW(g_setup, IDC_KEY, cur.key[0] ? cur.key : g_cfg.key);
        setup_preset_changed(FALSE);
    } else {
        setup_preset_changed(TRUE);
    }
    CheckDlgButton(g_setup, IDC_REMEMBER, cur.base_url[0] && !cur.remember_key ? BST_UNCHECKED : BST_CHECKED);
    CheckDlgButton(g_setup, IDC_WEB, cur.web_pages ? BST_CHECKED : BST_UNCHECKED);
    i = cur.base_url[0] ? IDC_KEY : IDC_PRESET;
    SecureZeroMemory(&cur, sizeof(cur));

    g_setup_saved = FALSE;
    if (owner)
        EnableWindow(owner, FALSE);
    ShowWindow(g_setup, SW_SHOW);
    SetFocus(GetDlgItem(g_setup, i));
    while (g_setup && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (IsDialogMessageW(g_setup, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (owner) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }
    return g_setup_saved;
}

/* ======================================================================
 * Text windows: what the assistant can see, the activity log
 * ====================================================================== */

static LRESULT CALLBACK text_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_SIZE) {
        MoveWindow(GetDlgItem(hwnd, 1), 0, 0, LOWORD(lp), HIWORD(lp), TRUE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void show_text(const WCHAR *title, const char *text, BOOL at_end)
{
    static BOOL registered;
    WCHAR *w = w_from_utf8_crlf(text);
    HWND hwnd, edit;

    if (!registered) {
        WNDCLASSEXW wc;

        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.hInstance = g_inst;
        wc.lpfnWndProc = text_proc;
        wc.lpszClassName = L"WinCraftAssistantText";
        wc.hCursor = LoadCursorW(NULL, (const WCHAR *)IDC_ARROW);
        wc.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(1));
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        RegisterClassExW(&wc);
        registered = TRUE;
    }
    hwnd = CreateWindowExW(0, L"WinCraftAssistantText", title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                           700, 520, NULL, NULL, g_inst, NULL);
    edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
                           ES_AUTOVSCROLL, 0, 0, 100, 100, hwnd, (HMENU)1, g_inst, NULL);
    SendMessageW(edit, WM_SETFONT, (WPARAM)g_font, TRUE);
    SendMessageW(edit, EM_SETLIMITTEXT, 8 << 20, 0);
    SetWindowTextW(edit, w);
    if (at_end) {
        int len = GetWindowTextLengthW(edit);

        SendMessageW(edit, EM_SETSEL, len, len);
        SendMessageW(edit, EM_SCROLLCARET, 0, 0);
    }
    free(w);
    ShowWindow(hwnd, SW_SHOW);
    {
        RECT rc;

        GetClientRect(hwnd, &rc);
        MoveWindow(edit, 0, 0, rc.right, rc.bottom, TRUE);
    }
}

static void show_what_it_sees(void)
{
    sb_t b;
    char *knowledge = knowledge_text(), *snap = g_snapshot ? NULL : system_snapshot();
    char host[256];

    WideCharToMultiByte(CP_UTF8, 0, g_provider_host[0] ? g_provider_host : L"(no provider set up)", -1, host,
                        sizeof(host), NULL, NULL);
    sb_init(&b);
    sb_addf(&b, "At the start of each chat, the assistant sends %s:\n\n"
                "1. WinCraft's public documentation, which comes with the system (%u KB): the README, "
                "the changelog, notes on the fixes WinCraft carries for Wine, and troubleshooting guides.\n"
                "2. This overview of the computer%s:\n\n", host, (unsigned)(strlen(knowledge) / 1024),
            g_snapshot ? ", as sent for the current chat" : ", as it would be sent now");
    sb_add(&b, g_snapshot ? g_snapshot : snap);
    sb_add(&b, "\nDuring the chat it looks things up with tools, listed in the chat as it uses them, and "
               "everything you type is sent too. It never reads your documents. Changes to the computer, "
               "looking inside a running program and reading pages on the internet only happen after you allow "
               "them. Every tool use is recorded in the activity log.\n");
    show_text(L"What the assistant can see", b.s, FALSE);
    sb_free(&b);
    free(knowledge);
    free(snap);
}

static void show_activity(void)
{
    WCHAR path[MAX_PATH];
    char *text;

    settings_path(path, L"activity.log");
    text = read_file_utf8(path, 4 << 20, NULL);
    show_text(L"WinCraft Assistant activity", text && *text ? text : "Nothing recorded yet.\n", TRUE);
    free(text);
}

/* ======================================================================
 * The chat
 * ====================================================================== */

/* Models write Markdown; the transcript is plain text. */
static void chat_file_append(const char *utf8)
{
    HANDLE h;
    DWORD put;

    if (!g_chat_file[0])
        return;
    h = CreateFileW(g_chat_file, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    WriteFile(h, utf8, (DWORD)strlen(utf8), &put, NULL);
    CloseHandle(h);
}

static void transcript_add(const char *utf8)
{
    WCHAR *w = w_from_utf8_crlf(utf8);
    int len = GetWindowTextLengthW(g_transcript);

    SendMessageW(g_transcript, EM_SETSEL, len, len);
    SendMessageW(g_transcript, EM_REPLACESEL, FALSE, (LPARAM)w);
    SendMessageW(g_transcript, EM_SCROLLCARET, 0, 0);
    free(w);
    chat_file_append(utf8);
}

/* ---- the chat as rich text ----
 *
 * What the model writes is Markdown, shown through md_render (mdtext.c):
 * lists, bold, italics and code look as such, and the control turns web
 * addresses into links (see open_link). Labels are bold and look-up lines
 * grey, so the answers stand out. The saved chat stays plain text. */

enum { CHAT_TEXT, CHAT_QUIET, CHAT_YOU, CHAT_ANSWER };

struct rich_out {
    md_out o;
    int style;
};

static void rich_to_end(void)
{
    GETTEXTLENGTHEX gl = { GTL_NUMCHARS | GTL_PRECISE, 1200 };
    LONG end = (LONG)SendMessageW(g_transcript, EM_GETTEXTLENGTHEX, (WPARAM)&gl, 0);
    CHARRANGE cr = { end, end };

    SendMessageW(g_transcript, EM_EXSETSEL, 0, (LPARAM)&cr);
}

static void rich_insert(const WCHAR *w, int style)
{
    CHARFORMAT2W cf;

    rich_to_end();
    ZeroMemory(&cf, sizeof(cf));
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_BOLD | CFM_ITALIC | CFM_COLOR | CFM_FACE | CFM_SIZE;
    cf.dwEffects = (style & MD_BOLD ? CFE_BOLD : 0) | (style & MD_ITALIC ? CFE_ITALIC : 0) |
                   (style & MD_GREY ? 0 : CFE_AUTOCOLOR);
    cf.crTextColor = GetSysColor(COLOR_GRAYTEXT);
    cf.yHeight = style & MD_MONO ? 190 : 200;              /* twips: 9.5 and 10 points */
    lstrcpyW(cf.szFaceName, style & MD_MONO ? L"Liberation Mono" : L"Liberation Sans");
    SendMessageW(g_transcript, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    SendMessageW(g_transcript, EM_REPLACESEL, FALSE, (LPARAM)w);
}

static void rich_begin(md_out *o, int indent, int hang, int spaces)
{
    PARAFORMAT2 pf;

    rich_to_end();
    ZeroMemory(&pf, sizeof(pf));
    pf.cbSize = sizeof(pf);
    pf.dwMask = PFM_STARTINDENT | PFM_OFFSET;
    pf.dxStartIndent = indent;
    pf.dxOffset = hang;
    SendMessageW(g_transcript, EM_SETPARAFORMAT, 0, (LPARAM)&pf);
}

static void rich_text(md_out *o, const char *s, size_t n, int style)
{
    int len = MultiByteToWideChar(CP_UTF8, 0, s, (int)n, NULL, 0);
    WCHAR *w = malloc((len + 1) * sizeof(WCHAR));

    MultiByteToWideChar(CP_UTF8, 0, s, (int)n, w, len);
    w[len] = 0;
    rich_insert(w, style | ((struct rich_out *)o)->style);
    free(w);
}

static void rich_end(md_out *o)
{
    rich_insert(L"\r", 0);
}

/* Text taken as it is, one paragraph per line. */
static void rich_lines(const char *utf8, int style)
{
    struct rich_out ro = { { rich_begin, rich_text, rich_end }, style };
    const char *p = utf8, *e;

    while (*p) {
        e = strchr(p, '\n');
        rich_begin(&ro.o, 0, 0, 0);
        rich_text(&ro.o, p, e ? (size_t)(e - p) : strlen(p), 0);
        if (!e)
            break;
        rich_end(&ro.o);
        p = e + 1;
    }
}

/* Add to the chat, and to the saved chat as plain text. */
static void chat_add(int kind, const char *utf8)
{
    sb_t file;
    char *plain = NULL;

    sb_init(&file);
    if (kind == CHAT_ANSWER) {
        plain = md_plain(utf8);
        sb_addf(&file, "Assistant:\n%s\n", plain);
    } else if (kind == CHAT_YOU) {
        sb_addf(&file, "You:\n%s\n\n", utf8);
    } else {
        sb_add(&file, utf8);
    }
    if (!g_rich) {
        transcript_add(file.s);
    } else {
        if (kind == CHAT_ANSWER) {
            struct rich_out ro = { { rich_begin, rich_text, rich_end }, 0 };

            rich_lines("Assistant:\n", MD_BOLD);
            md_render(utf8, &ro.o, 0);
            rich_lines("\n", 0);
        } else if (kind == CHAT_YOU) {
            rich_lines("You:\n", MD_BOLD);
            rich_lines(utf8, 0);
            rich_lines("\n\n", 0);
        } else {
            rich_lines(utf8, kind == CHAT_QUIET ? MD_GREY : 0);
        }
        SendMessageW(g_transcript, WM_VSCROLL, SB_BOTTOM, 0);
        chat_file_append(file.s);
    }
    sb_free(&file);
    free(plain);
}

/* The web browser the person installed: Windows browsers list themselves
 * under StartMenuInternet. (Wine's own handler for web addresses,
 * winebrowser, looks for a Linux browser, and WinCraft has none.) */
static BOOL browser_command(WCHAR *cmd, DWORD size)
{
    static const HKEY roots[] = { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE };
    WCHAR name[256], key[600];
    DWORD n, type;
    unsigned i;
    HKEY h;

    for (i = 0; i < ARRAYSIZE(roots); i++) {
        if (RegOpenKeyExW(roots[i], L"Software\\Clients\\StartMenuInternet", 0, KEY_READ, &h))
            continue;
        n = sizeof(name) - sizeof(WCHAR);
        ZeroMemory(name, sizeof(name));
        if (RegQueryValueExW(h, NULL, NULL, &type, (BYTE *)name, &n) || type != REG_SZ || !name[0]) {
            n = ARRAYSIZE(name);
            if (RegEnumKeyExW(h, 0, name, &n, NULL, NULL, NULL, NULL)) {
                RegCloseKey(h);
                continue;
            }
        }
        RegCloseKey(h);
        _snwprintf(key, ARRAYSIZE(key), L"Software\\Clients\\StartMenuInternet\\%s\\shell\\open\\command", name);
        key[ARRAYSIZE(key) - 1] = 0;
        if (RegOpenKeyExW(roots[i], key, 0, KEY_READ, &h))
            continue;
        n = (size - 1) * sizeof(WCHAR);
        ZeroMemory(cmd, size * sizeof(WCHAR));
        if (!RegQueryValueExW(h, NULL, NULL, &type, (BYTE *)cmd, &n) && (type == REG_SZ || type == REG_EXPAND_SZ) &&
            cmd[0]) {
            RegCloseKey(h);
            return TRUE;
        }
        RegCloseKey(h);
    }
    return FALSE;
}

/* A web address clicked in the chat. */
static void open_link(const WCHAR *url)
{
    WCHAR cmd[1024], line[4200], full[2100], *arg;
    const WCHAR *p;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;

    if (!StrCmpNIW(url, L"www.", 4)) {
        _snwprintf(full, ARRAYSIZE(full), L"https://%s", url);
        full[ARRAYSIZE(full) - 1] = 0;
        url = full;
    }
    if (StrCmpNIW(url, L"https://", 8) && StrCmpNIW(url, L"http://", 7))
        return;                                 /* web addresses only */
    for (p = url; *p; p++)
        if (*p <= L' ' || *p == L'"')
            return;                             /* nothing that could leave the quotes below */
    if (!browser_command(cmd, ARRAYSIZE(cmd))) {
        _snwprintf(line, ARRAYSIZE(line), L"There is no web browser on this computer to open\n%s\n\nInstall "
                   L"Firefox with Download a Program (Get Firefox), then click the address again.", url);
        line[ARRAYSIZE(line) - 1] = 0;
        MessageBoxW(g_main, line, APP_NAME, MB_OK | MB_ICONINFORMATION);
        return;
    }
    if ((arg = wcsstr(cmd, L"%1")))             /* a command with a place for the address */
        _snwprintf(line, ARRAYSIZE(line), L"%.*s%s%s", (int)(arg - cmd), cmd, url, arg + 2);
    else
        _snwprintf(line, ARRAYSIZE(line), L"%s \"%s\"", cmd, url);
    line[ARRAYSIZE(line) - 1] = 0;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    if (CreateProcessW(NULL, line, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        activity_log("opened a link from the chat: %S", url);
    } else {
        MessageBoxW(g_main, L"The web browser could not be started.", APP_NAME, MB_OK | MB_ICONWARNING);
    }
}

/* From the worker thread: hand text to the window. */
static void post_chat(int kind, const char *fmt, ...)
{
    sb_t b;
    va_list ap;
    int n;
    char *buf;

    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    buf = malloc(n + 1);
    va_start(ap, fmt);
    vsnprintf(buf, n + 1, fmt, ap);
    va_end(ap);
    sb_init(&b);
    sb_add(&b, buf);
    free(buf);
    PostMessageW(g_main, WM_APP_APPEND, (WPARAM)kind, (LPARAM)sb_take(&b));
}

static void post_status(const WCHAR *s)
{
    PostMessageW(g_main, WM_APP_STATUS, 0, (LPARAM)_wcsdup(s));
}

static void update_status(void)
{
    WCHAR s[512];

    if (g_busy)
        return;
    _snwprintf(s, ARRAYSIZE(s), L"%s at %s.  This chat: %ld tokens sent, %ld received.", g_cfg.model,
               g_provider_host, g_tok_in, g_tok_out);
    s[ARRAYSIZE(s) - 1] = 0;
    SetWindowTextW(g_status, s);
}

struct approval {
    const tool_def *tool;
    const char *title, *detail;
    BOOL ok;
};

static BOOL approve_cb(const tool_def *t, const char *title, const char *detail, void *ctx)
{
    struct approval a = { t, title, detail, FALSE };

    if (llm_cancelled())
        return FALSE;
    if (t->tier == TIER_WEB && !g_cfg.web_pages) {
        activity_log("refused %s: switched off in the settings", t->name);
        return FALSE;
    }
    if ((t->tier == TIER_DIAGNOSE && g_diag_allowed) || (t->tier == TIER_WEB && g_web_allowed)) {
        activity_log("allowed (for this chat) %s: %s", t->name, title);
        return TRUE;
    }
    SendMessageW(g_main, WM_APP_APPROVE, 0, (LPARAM)&a);
    return a.ok;
}

typedef HRESULT (WINAPI *task_dialog_fn)(const TASKDIALOGCONFIG *, int *, int *, BOOL *);

/* Wine's task dialog shows its text in static controls that take "&" as the
 * mark of a keyboard shortcut, so an address would lose its "&"s: double them. */
static WCHAR *no_prefix(const WCHAR *s)
{
    const WCHAR *p;
    WCHAR *out, *q;
    size_t n = 0;

    for (p = s; *p; p++)
        n += *p == L'&' ? 2 : 1;
    out = malloc((n + 1) * sizeof(WCHAR));
    for (p = s, q = out; *p; p++) {
        if (*p == L'&')
            *q++ = L'&';
        *q++ = *p;
    }
    *q = 0;
    return out;
}

static void ask_permission(struct approval *a)
{
    WCHAR *title = w_from_utf8(a->title), *detail = w_from_utf8(a->detail);
    WCHAR *td_title = no_prefix(title), *td_detail = no_prefix(detail);
    TASKDIALOG_BUTTON buttons[3];
    TASKDIALOGCONFIG tdc;
    task_dialog_fn td = (task_dialog_fn)(void *)GetProcAddress(GetModuleHandleW(L"comctl32.dll"), "TaskDialogIndirect");
    int pressed = 0, n = 0;
    BOOL per_chat = a->tool->tier == TIER_DIAGNOSE || a->tool->tier == TIER_WEB;

    buttons[n].nButtonID = 100;
    buttons[n++].pszButtonText = L"&Allow";
    if (per_chat) {
        buttons[n].nButtonID = 101;
        buttons[n++].pszButtonText = L"Allow for this &chat";
    }
    buttons[n].nButtonID = 102;
    buttons[n++].pszButtonText = L"&Don't allow";
    ZeroMemory(&tdc, sizeof(tdc));
    tdc.cbSize = sizeof(tdc);
    tdc.hwndParent = g_main;
    tdc.hInstance = g_inst;
    tdc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    tdc.pszWindowTitle = APP_NAME;
    tdc.pszMainIcon = per_chat ? TD_INFORMATION_ICON : TD_WARNING_ICON;
    tdc.pszMainInstruction = td_title;
    tdc.pszContent = td_detail;
    tdc.cButtons = n;
    tdc.pButtons = buttons;
    tdc.nDefaultButton = 102;
    if (!td || FAILED(td(&tdc, &pressed, NULL, NULL))) {
        WCHAR text[1200];

        _snwprintf(text, ARRAYSIZE(text), L"%s\n\n%s", title, detail);
        text[ARRAYSIZE(text) - 1] = 0;
        pressed = MessageBoxW(g_main, text, APP_NAME, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES ? 100 : 102;
    }
    a->ok = pressed == 100 || pressed == 101;
    if (pressed == 101 && a->tool->tier == TIER_WEB)
        g_web_allowed = TRUE;
    else if (pressed == 101)
        g_diag_allowed = TRUE;
    activity_log("%s %s: %s", a->ok ? (pressed == 101 ? "allowed for this chat" : "allowed") : "refused",
                 a->tool->name, a->title);
    free(td_title);
    free(td_detail);
    free(title);
    free(detail);
}

static void build_system_prompt(void)
{
    sb_t b;
    char *knowledge = knowledge_text();
    SYSTEMTIME st;

    free(g_snapshot);
    g_snapshot = system_snapshot();
    GetSystemTime(&st);
    sb_init(&b);
    sb_add(&b, knowledge);
    sb_addf(&b, "\n\n# This computer right now\n\nCollected %04d-%02d-%02d %02d:%02d UTC, when this chat "
                "started. Call get_system_snapshot for a fresh copy.\n\n", st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute);
    sb_add(&b, g_snapshot);
    if (!g_cfg.web_pages)
        sb_add(&b, "\n\nReading web pages is switched off in this assistant's settings, so read_web_page is not "
                   "available. When a page would help, say so, and that the person can switch it on under "
                   "Chat > Settings.\n");
    free(g_system);
    g_system = sb_take(&b);
    free(knowledge);
}

/* When a turn ends early, close it so the conversation stays valid. */
static void close_turn(const char *why)
{
    json *m = json_push(g_messages, json_new(J_OBJECT));

    json_set_str(m, "role", "assistant");
    json_set_str(m, "content", why);
}

/* One request to the model, on its own thread. Wine's WinHTTP does not
 * interrupt a request that is waiting for an answer when another thread closes
 * it, so Stop abandons the request instead: the job owns copies of everything
 * it needs and frees itself when the answer comes in late. */
struct request {
    LONG refs;
    HANDLE done;
    config cfg;
    char *system;
    json *tools, *messages;
    llm_reply r;
    int ok;
};

static void request_release(struct request *q)
{
    if (InterlockedDecrement(&q->refs))
        return;
    CloseHandle(q->done);
    SecureZeroMemory(q->cfg.key, sizeof(q->cfg.key));
    free(q->system);
    json_free(q->tools);
    json_free(q->messages);
    llm_reply_free(&q->r);
    free(q);
}

static DWORD WINAPI request_thread(void *arg)
{
    struct request *q = arg;

    q->ok = llm_chat(&q->cfg, q->system, q->tools, q->messages, &q->r);
    SetEvent(q->done);
    request_release(q);
    return 0;
}

/* Returns 1 with *r filled, 0 on failure (r->error set), -1 when stopped. */
static int ask_model(llm_reply *r)
{
    struct request *q = calloc(1, sizeof(*q));
    HANDLE t, waits[2];
    int ret;

    ZeroMemory(r, sizeof(*r));
    q->refs = 2;
    q->done = CreateEventW(NULL, TRUE, FALSE, NULL);
    q->cfg = g_cfg;
    q->system = utf8_dup(g_system);
    q->tools = json_copy(g_tools_json);
    q->messages = json_copy(g_messages);
    t = CreateThread(NULL, 0, request_thread, q, 0, NULL);
    if (!t) {
        q->refs = 1;
        request_release(q);
        r->error = utf8_dup("Could not start the request.");
        return 0;
    }
    CloseHandle(t);
    waits[0] = q->done;
    waits[1] = g_stop_event;
    if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0) {
        llm_cancel();       /* closes the connection, if WinHTTP notices */
        request_release(q);
        return -1;
    }
    *r = q->r;              /* hand the answer over */
    ZeroMemory(&q->r, sizeof(q->r));
    ret = q->ok;
    request_release(q);
    return ret;
}

static DWORD WINAPI chat_thread(void *arg)
{
    char *question = arg;
    int step, ok;
    WCHAR host[256];

    llm_reset_cancel();
    ResetEvent(g_stop_event);
    if (!g_system) {
        post_status(L"Looking at this computer...");
        build_system_prompt();
    }
    llm_add_user(&g_cfg, g_messages, question);
    lstrcpynW(host, g_provider_host, ARRAYSIZE(host));
    for (step = 0; step < MAX_STEPS; step++) {
        llm_reply r;
        char *results[16], *label;
        int i;

        post_status(L"Thinking...");
        ok = ask_model(&r);
        if (ok <= 0) {
            if (ok < 0)
                post_chat(CHAT_TEXT, "Stopped.\n\n");
            else
                post_chat(CHAT_TEXT, "%s\n\n", r.error ? r.error : "The request failed.");
            activity_log("request to %S %s: %s", host, ok < 0 ? "stopped" : "failed",
                         ok < 0 ? "by the person" : r.error ? r.error : "?");
            if (step == 0) {
                /* forget the question, so asking again works */
                json *last = g_messages->child;

                if (last && !last->next) {
                    json_free(last);
                    g_messages->child = NULL;
                } else {
                    while (last && last->next && last->next->next)
                        last = last->next;
                    if (last && last->next) {
                        json_free(last->next);
                        last->next = NULL;
                    }
                }
            } else {
                close_turn(ok < 0 ? "(Stopped by the person.)" : "(The previous request failed.)");
            }
            llm_reply_free(&r);
            break;
        }
        json_push(g_messages, r.turn);
        r.turn = NULL;
        g_tok_in += r.in_tokens;
        g_tok_out += r.out_tokens;
        if (r.text)
            post_chat(CHAT_ANSWER, "%s", r.text);
        if (!r.ncalls) {
            llm_reply_free(&r);
            break;
        }
        for (i = 0; i < r.ncalls; i++) {
            const tool_def *t = tool_find(r.calls[i].name);
            char *args = json_dump(r.calls[i].input);

            if (!t) {
                results[i] = utf8_dup("There is no tool of that name.");
            } else if (llm_cancelled()) {
                results[i] = utf8_dup("Stopped by the person before this ran.");
            } else if (t->tier == TIER_WEB && !g_cfg.web_pages) {
                results[i] = utf8_dup("Reading web pages is switched off in this assistant's settings, so nothing "
                                      "was read. The person can switch it on under Chat > Settings.");
                post_chat(CHAT_QUIET, "  \xc2\xb7 Reading web pages is switched off in the settings\n");
            } else {
                WCHAR status[128];

                _snwprintf(status, ARRAYSIZE(status), L"Using %S...", t->name);
                status[ARRAYSIZE(status) - 1] = 0;
                post_status(status);
                results[i] = tool_run(t, r.calls[i].input, approve_cb, NULL, &label);
                post_chat(CHAT_QUIET, "  \xc2\xb7 %s\n", label);
                free(label);
            }
            activity_log("tool %s %.300s -> %u bytes to %S", r.calls[i].name, args,
                         (unsigned)strlen(results[i]), host);
            free(args);
        }
        post_chat(CHAT_TEXT, "\n");
        llm_add_results(&g_cfg, g_messages, &r, results);
        for (i = 0; i < r.ncalls; i++)
            free(results[i]);
        llm_reply_free(&r);
        if (llm_cancelled()) {
            post_chat(CHAT_TEXT, "Stopped.\n\n");
            close_turn("(Stopped by the person.)");
            break;
        }
        if (step == MAX_STEPS - 1) {
            post_chat(CHAT_TEXT, "(The assistant stopped here: it used %d steps for this question.)\n\n", MAX_STEPS);
            close_turn("(Stopped after too many steps.)");
        }
    }
    free(question);
    PostMessageW(g_main, WM_APP_DONE, 0, 0);
    return 0;
}

static void new_chat(void)
{
    WCHAR dir[MAX_PATH];
    SYSTEMTIME st;
    WCHAR host[256];

    json_free(g_messages);
    g_messages = json_new(J_ARRAY);
    free(g_system);
    g_system = NULL;
    free(g_snapshot);
    g_snapshot = NULL;
    g_diag_allowed = g_web_allowed = FALSE;
    g_tok_in = g_tok_out = 0;
    g_chat_file[0] = 0;
    if (app_data_dir(dir)) {
        PathAppendW(dir, L"chats");
        SHCreateDirectoryExW(NULL, dir, NULL);
        GetLocalTime(&st);
        _snwprintf(g_chat_file, MAX_PATH, L"%s\\%04d-%02d-%02d_%02d%02d%02d.txt", dir, st.wYear, st.wMonth,
                   st.wDay, st.wHour, st.wMinute, st.wSecond);
        g_chat_file[MAX_PATH - 1] = 0;
    }
    SetWindowTextW(g_transcript, L"");
    lstrcpynW(host, g_provider_host, ARRAYSIZE(host));
    {
        char intro[1536];

        snprintf(intro, sizeof(intro),
                 "Ask about WinCraft OS or this computer: why a program does not start or stops "
                 "responding, what is installed, how something works here. The assistant looks things "
                 "up on this computer as it goes and lists what it looked at. It changes nothing "
                 "without asking you first.\n\nModel: %.120S at %.120S\n\n", g_cfg.model, host);
        chat_add(CHAT_QUIET, intro);
    }
    update_status();
}

static void send_question(void)
{
    int len = GetWindowTextLengthW(g_input);
    WCHAR *w;
    char *q;
    HANDLE t;

    if (g_busy || !len)
        return;
    w = malloc((len + 1) * sizeof(WCHAR));
    GetWindowTextW(g_input, w, len + 1);
    StrTrimW(w, L" \t\r\n");
    if (!w[0]) {
        free(w);
        return;
    }
    q = utf8_from_w(w);
    free(w);
    SetWindowTextW(g_input, L"");
    chat_add(CHAT_YOU, q);
    g_busy = TRUE;
    EnableWindow(g_send, FALSE);
    EnableWindow(g_stop, TRUE);
    t = CreateThread(NULL, 0, chat_thread, q, 0, NULL);
    if (t)
        CloseHandle(t);
    else {
        free(q);
        g_busy = FALSE;
    }
}

static LRESULT CALLBACK input_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN && wp == VK_RETURN && !(GetKeyState(VK_SHIFT) & 0x8000)) {
        send_question();
        return 0;
    }
    if (msg == WM_CHAR && wp == '\r' && !(GetKeyState(VK_SHIFT) & 0x8000))
        return 0;
    if (msg == WM_KEYDOWN && wp == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
        SendMessageW(hwnd, EM_SETSEL, 0, -1);
        return 0;
    }
    return CallWindowProcW(g_input_proc, hwnd, msg, wp, lp);
}

static void layout(HWND hwnd)
{
    RECT rc;
    int w, h, input_h = 72, status_h = 24, pad = 8, btn_w = 96;

    GetClientRect(hwnd, &rc);
    w = rc.right;
    h = rc.bottom;
    MoveWindow(g_transcript, pad, pad, w - 2 * pad, h - input_h - status_h - 3 * pad, TRUE);
    MoveWindow(g_input, pad, h - input_h - status_h - pad, w - 3 * pad - btn_w, input_h, TRUE);
    MoveWindow(g_send, w - pad - btn_w, h - input_h - status_h - pad, btn_w, 32, TRUE);
    MoveWindow(g_stop, w - pad - btn_w, h - status_h - pad - 34, btn_w, 32, TRUE);
    MoveWindow(g_status, pad, h - status_h, w - 2 * pad, status_h - 4, TRUE);
}

static void reconfigure(void)
{
    if (g_busy)
        return;
    if (setup_window(g_main, FALSE)) {
        llm_host(&g_cfg, g_provider_host, ARRAYSIZE(g_provider_host), &g_provider_port);
        json_free(g_tools_json);
        g_tools_json = tools_json(g_cfg.format, g_cfg.web_pages);
        new_chat();
    }
}

static LRESULT CALLBACK main_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        HMENU bar = CreateMenu(), chat = CreatePopupMenu(), help = CreatePopupMenu();

        AppendMenuW(chat, MF_STRING, IDM_NEW, L"&New chat\tCtrl+N");
        AppendMenuW(chat, MF_STRING, IDM_SEE, L"What the assistant can &see...");
        AppendMenuW(chat, MF_STRING, IDM_ACTIVITY, L"&Activity log...");
        AppendMenuW(chat, MF_SEPARATOR, 0, NULL);
        AppendMenuW(chat, MF_STRING, IDM_SETTINGS, L"Se&ttings...");
        AppendMenuW(chat, MF_SEPARATOR, 0, NULL);
        AppendMenuW(chat, MF_STRING, IDM_EXIT, L"&Close");
        AppendMenuW(help, MF_STRING, IDM_ABOUT, L"&About WinCraft Assistant");
        AppendMenuW(bar, MF_POPUP, (UINT_PTR)chat, L"&Chat");
        AppendMenuW(bar, MF_POPUP, (UINT_PTR)help, L"&Help");
        SetMenu(hwnd, bar);

        if (LoadLibraryW(L"msftedit.dll"))
            g_transcript = CreateWindowExW(WS_EX_CLIENTEDGE, L"RICHEDIT50W", L"", WS_CHILD | WS_VISIBLE |
                                           WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 0, 0, 10, 10,
                                           hwnd, (HMENU)IDC_TRANSCRIPT, g_inst, NULL);
        g_rich = g_transcript != NULL;
        if (!g_rich)
            g_transcript = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                                           ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 0, 0, 10, 10, hwnd,
                                           (HMENU)IDC_TRANSCRIPT, g_inst, NULL);
        g_input = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP |
                                  ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN, 0, 0, 10, 10, hwnd,
                                  (HMENU)IDC_INPUT, g_inst, NULL);
        g_send = CreateWindowExW(0, L"BUTTON", L"Send", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                 0, 0, 10, 10, hwnd, (HMENU)IDC_SEND, g_inst, NULL);
        g_stop = CreateWindowExW(0, L"BUTTON", L"Stop", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_DISABLED,
                                 0, 0, 10, 10, hwnd, (HMENU)IDC_STOP, g_inst, NULL);
        g_status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS,
                                   0, 0, 10, 10, hwnd, (HMENU)IDC_STATUS, g_inst, NULL);
        SendMessageW(g_transcript, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(g_input, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(g_send, WM_SETFONT, (WPARAM)g_font_bold, TRUE);
        SendMessageW(g_stop, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(g_status, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(g_transcript, EM_SETLIMITTEXT, 16 << 20, 0);
        if (g_rich) {
            SendMessageW(g_transcript, EM_EXLIMITTEXT, 0, 16 << 20);
            SendMessageW(g_transcript, EM_SETBKGNDCOLOR, 0, GetSysColor(COLOR_WINDOW));
            SendMessageW(g_transcript, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELONG(4, 4));
            SendMessageW(g_transcript, EM_AUTOURLDETECT, TRUE, 0);
            SendMessageW(g_transcript, EM_SETEVENTMASK, 0, ENM_LINK);
        }
        g_input_proc = (WNDPROC)SetWindowLongPtrW(g_input, GWLP_WNDPROC, (LONG_PTR)input_proc);
        return 0;
    }
    case WM_SIZE:
        layout(hwnd);
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = 420;
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = 320;
        return 0;
    case WM_SETFOCUS:
        SetFocus(g_input);
        return 0;
    case WM_CTLCOLORSTATIC:
        /* the read-only transcript reads better on white */
        if ((HWND)lp == g_transcript) {
            SetBkColor((HDC)wp, GetSysColor(COLOR_WINDOW));
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        }
        break;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_SEND:
            send_question();
            SetFocus(g_input);
            return 0;
        case IDC_STOP:
            SetEvent(g_stop_event);
            llm_cancel();
            SetWindowTextW(g_status, L"Stopping...");
            /* the button is about to be disabled; Wine drops the focus of a disabled window */
            SetFocus(g_input);
            return 0;
        case IDM_NEW:
            if (!g_busy)
                new_chat();
            return 0;
        case IDM_SEE:
            show_what_it_sees();
            return 0;
        case IDM_ACTIVITY:
            show_activity();
            return 0;
        case IDM_SETTINGS:
            reconfigure();
            return 0;
        case IDM_EXIT:
            DestroyWindow(hwnd);
            return 0;
        case IDM_ABOUT:
            MessageBoxW(hwnd, L"WinCraft Assistant " WC_VERSION L"\n\nPart of WinCraft OS. MIT License.\n\n"
                        L"It answers with the model you chose in Settings, and knows WinCraft from the "
                        L"documentation built into this version of the system.",
                        L"About WinCraft Assistant", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        break;
    case WM_APP_APPEND:
        chat_add((int)wp, (const char *)lp);
        free((void *)lp);
        return 0;
    case WM_NOTIFY: {
        ENLINK *link = (ENLINK *)lp;

        if (link->nmhdr.idFrom == IDC_TRANSCRIPT && link->nmhdr.code == EN_LINK && link->msg == WM_LBUTTONUP) {
            CHARRANGE sel;
            WCHAR url[2048];
            TEXTRANGEW tr;

            /* a click, not the end of selecting text */
            SendMessageW(g_transcript, EM_EXGETSEL, 0, (LPARAM)&sel);
            if (sel.cpMin == sel.cpMax && link->chrg.cpMax - link->chrg.cpMin < (LONG)ARRAYSIZE(url)) {
                tr.chrg = link->chrg;
                tr.lpstrText = url;
                SendMessageW(g_transcript, EM_GETTEXTRANGE, 0, (LPARAM)&tr);
                open_link(url);
            }
            return 1;
        }
        break;
    }
    case WM_APP_STATUS:
        SetWindowTextW(g_status, (const WCHAR *)lp);
        free((void *)lp);
        return 0;
    case WM_APP_APPROVE:
        ask_permission((struct approval *)lp);
        return 0;
    case WM_APP_DONE:
        g_busy = FALSE;
        EnableWindow(g_send, TRUE);
        EnableWindow(g_stop, FALSE);
        update_status();
        SetFocus(g_input);
        return 0;
    case WM_CLOSE:
        if (g_busy) {
            SetEvent(g_stop_event);
            llm_cancel();
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HFONT make_font(int pt, int weight)
{
    return CreateFontW(-MulDiv(pt, 96, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS,
                       L"Liberation Sans");
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, WCHAR *cmdline, int show)
{
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    WNDCLASSEXW wc;
    HANDLE mutex;
    ACCEL accel[] = { { FCONTROL | FVIRTKEY, 'N', IDM_NEW } };
    HACCEL haccel;
    MSG msg;
    BOOL have;

    g_inst = inst;
    g_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    mutex = CreateMutexW(NULL, TRUE, L"WinCraftAssistant");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(L"WinCraftAssistant", NULL);

        if (other) {
            ShowWindow(other, SW_RESTORE);
            SetForegroundWindow(other);
        }
        return 0;
    }
    InitCommonControlsEx(&icc);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    g_font = make_font(10, FW_NORMAL);
    g_font_bold = make_font(10, FW_SEMIBOLD);

    have = load_config(&g_cfg);
    if (!have || StrStrIW(cmdline, L"/settings") ||
        (preset_needs_key(g_cfg.preset) && !g_cfg.key[0])) {
        if (!setup_window(NULL, !have))
            return 0;
        if (!have)
            add_start_menu_entry();
    }
    llm_host(&g_cfg, g_provider_host, ARRAYSIZE(g_provider_host), &g_provider_port);
    g_tools_json = tools_json(g_cfg.format, g_cfg.web_pages);
    activity_log("opened; provider %S, model %S", g_provider_host, g_cfg.model);

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.hInstance = inst;
    wc.lpfnWndProc = main_proc;
    wc.lpszClassName = L"WinCraftAssistant";
    wc.hCursor = LoadCursorW(NULL, (const WCHAR *)IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassExW(&wc);
    g_main = CreateWindowExW(0, wc.lpszClassName, APP_NAME, WS_OVERLAPPEDWINDOW,
                             (GetSystemMetrics(SM_CXSCREEN) - 760) / 2, (GetSystemMetrics(SM_CYSCREEN) - 600) / 3,
                             760, 600, NULL, NULL, inst, NULL);
    if (!g_main)
        return 1;
    new_chat();
    ShowWindow(g_main, show);
    UpdateWindow(g_main);
    SetFocus(g_input);
    haccel = CreateAcceleratorTableW(accel, ARRAYSIZE(accel));
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (TranslateAcceleratorW(g_main, haccel, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    SecureZeroMemory(g_cfg.key, sizeof(g_cfg.key));
    CloseHandle(mutex);
    return 0;
}
