/*
 * WinCraft Assistant - shared declarations.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WCAGENT_H
#define WCAGENT_H

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define _WIN32_WINNT 0x0A00
#define COBJMACROS
#include <winsock2.h>     /* before windows.h, which would bring in winsock.h */
#include <windows.h>
#include <stdarg.h>
#include <stddef.h>

#define WC_VERSION      L"0.6.2"
#define APP_NAME        L"WinCraft Assistant"

/* Wine's prefix for native (Linux) paths, e.g. UNIX_ROOT L"\\proc\\meminfo". */
#define UNIX_ROOT       L"\\\\?\\unix"

/* ---- growable UTF-8 string ---- */
typedef struct {
    char *s;
    size_t len, cap;
} sb_t;

void sb_init(sb_t *b);
void sb_free(sb_t *b);
void sb_add(sb_t *b, const char *s);
void sb_addn(sb_t *b, const char *s, size_t n);
void sb_addc(sb_t *b, char c);
void sb_addf(sb_t *b, const char *fmt, ...);
void sb_addw(sb_t *b, const WCHAR *w);          /* UTF-16 in, UTF-8 out */
char *sb_take(sb_t *b);                         /* caller frees */

char *utf8_dup(const char *s);
char *utf8_from_w(const WCHAR *w);
WCHAR *w_from_utf8(const char *s);              /* caller frees */
WCHAR *w_from_utf8_crlf(const char *s);         /* "\n" -> "\r\n" for EDIT controls */

/* ---- JSON ---- */
typedef enum { J_NULL, J_FALSE, J_TRUE, J_NUMBER, J_STRING, J_ARRAY, J_OBJECT } jtype;

typedef struct json json;
struct json {
    jtype type;
    double num;
    char *str;          /* J_STRING */
    char *key;          /* set when this value is an object member */
    json *child;        /* first element / member */
    json *next;
};

json *json_parse(const char *text, size_t len);
void json_free(json *v);
char *json_dump(const json *v);                 /* compact; caller frees */
void json_dump_to(sb_t *b, const json *v);
void json_quote(sb_t *b, const char *s);        /* "..." with escapes */
json *json_get(const json *obj, const char *key);
const char *json_str(const json *obj, const char *key);    /* NULL unless a string */
double json_num(const json *obj, const char *key, double def);
int json_len(const json *arr);
json *json_at(const json *arr, int i);
json *json_copy(const json *v);

json *json_new(jtype t);
json *json_new_str(const char *s);
json *json_new_num(double n);
json *json_set(json *obj, const char *key, json *val);     /* returns val */
json *json_set_str(json *obj, const char *key, const char *s);
json *json_push(json *arr, json *val);                     /* returns val */

/* ---- settings ---- */
enum { FMT_ANTHROPIC, FMT_OPENAI };

typedef struct {
    int format;
    WCHAR preset[64];
    WCHAR base_url[512];
    WCHAR model[160];
    WCHAR key[512];
    BOOL remember_key;
    BOOL web_pages;             /* the assistant may ask to read web pages */
} config;

/* ---- talking to the model ---- */
typedef struct {
    char *id, *name;
    json *input;        /* parsed arguments (object) */
} tool_call;

typedef struct {
    char *text;                 /* the model's words, may be NULL */
    tool_call calls[16];
    int ncalls;
    long in_tokens, out_tokens;
    int http_status;
    char *error;                /* set on failure, human readable */
    json *turn;                 /* the model's turn, to add to the conversation */
} llm_reply;

/* Does not change `messages`: on success the caller adds r->turn to them. */
int llm_chat(const config *cfg, const char *system, const json *tools, const json *messages,
             llm_reply *r);
void llm_reply_free(llm_reply *r);
/* After the tools ran: add their results to the conversation. */
void llm_add_results(const config *cfg, json *messages, const llm_reply *r, char **results);
void llm_add_user(const config *cfg, json *messages, const char *text);
/* Model names the provider offers, NUL-separated, double NUL at the end. */
char *llm_models(const config *cfg, char **error);
void llm_cancel(void);
void llm_reset_cancel(void);
BOOL llm_cancelled(void);
/* Host name and port of the configured provider. */
void llm_host(const config *cfg, WCHAR *out, int size, int *port);

/* ---- tools and what the assistant knows ---- */
enum { TIER_READ, TIER_DIAGNOSE, TIER_WEB, TIER_CHANGE, TIER_AGENT };

typedef struct {
    const char *name;
    int tier;
    const char *description;
    const char *schema;         /* JSON Schema of the arguments */
} tool_def;

/* Asks the person; returns TRUE to go ahead. */
typedef BOOL (*approve_fn)(const tool_def *t, const char *title, const char *detail, void *ctx);

extern const tool_def g_tools[];
extern const int g_ntools;
extern BOOL g_agent_mode;          /* this window is the WinCraft Agent's */

/* The agent's sandbox: a new one, and the end of it (tools.c). */
void agent_begin(void);
void agent_end(void);

json *tools_json(int format, BOOL web_pages);
const tool_def *tool_find(const char *name);
/* Runs a tool; returns the result text for the model (caller frees). *label
 * receives a short line for the transcript, e.g. "Looked at running programs". */
char *tool_run(const tool_def *t, const json *args, approve_fn approve, void *ctx, char **label);
char *system_snapshot(void);
char *knowledge_text(void);

/* ---- the model's Markdown, as the chat shows it (mdtext.c) ---- */
enum { MD_BOLD = 1, MD_ITALIC = 2, MD_MONO = 4, MD_GREY = 8 };

typedef struct md_out md_out;
struct md_out {
    /* a paragraph: indents in twips for the chat window, spaces for plain text */
    void (*begin)(md_out *o, int indent, int hang, int spaces);
    void (*text)(md_out *o, const char *s, size_t n, int style);
    void (*end)(md_out *o);
};
void md_render(const char *md, md_out *o, int style);
char *md_plain(const char *md);    /* caller frees */

/* ---- files ---- */
char *read_file_utf8(const WCHAR *path, size_t max, size_t *len);
char *read_unix_file(const char *unix_path, size_t max);    /* NULL if missing */
BOOL app_data_dir(WCHAR *out);         /* %APPDATA%\WinCraft Assistant, created */
void activity_log(const char *fmt, ...);
BOOL run_native(const WCHAR *program, const WCHAR *args, DWORD wait_ms);

#endif
