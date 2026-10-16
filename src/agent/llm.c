/*
 * WinCraft Assistant - talking to the model.
 *
 * Two request formats cover the providers people use:
 *   - Anthropic's Messages API (POST <base>/v1/messages)
 *   - the OpenAI-compatible Chat Completions API (POST <base>/chat/completions),
 *     spoken by OpenAI, OpenRouter, Mistral, Groq and local servers such as
 *     Ollama, llama.cpp and LM Studio.
 * HTTPS goes through WinHTTP, which Wine backs with GnuTLS and the system's
 * certificate store. Requests are not streamed; the chat window shows that
 * the model is working until the whole answer is in.
 *
 * SPDX-License-Identifier: MIT
 */
#include "agent.h"
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TOKENS      4096
#define MAX_RESPONSE    (8 << 20)

static CRITICAL_SECTION g_lock;
static INIT_ONCE g_lock_once = INIT_ONCE_STATIC_INIT;
static HINTERNET g_active;              /* the request in flight, to cancel it */
static volatile LONG g_cancel;

static BOOL CALLBACK init_lock(INIT_ONCE *once, void *param, void **ctx)
{
    InitializeCriticalSection(&g_lock);
    return TRUE;
}

static void lock(void)
{
    InitOnceExecuteOnce(&g_lock_once, init_lock, NULL, NULL);
    EnterCriticalSection(&g_lock);
}

void llm_cancel(void)
{
    lock();
    InterlockedExchange(&g_cancel, 1);
    if (g_active)
        WinHttpCloseHandle(g_active);       /* makes the blocking call return */
    g_active = NULL;
    LeaveCriticalSection(&g_lock);
}

void llm_reset_cancel(void)
{
    InterlockedExchange(&g_cancel, 0);
}

BOOL llm_cancelled(void)
{
    return g_cancel != 0;
}

/* ======================================================================
 * HTTP
 * ====================================================================== */

typedef struct {
    int status;
    char *body;
    size_t len;
    char *error;
} http_result;

static void set_error(char **err, const char *fmt, ...)
{
    sb_t b;
    va_list ap;
    char tmp[512];

    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    sb_init(&b);
    sb_add(&b, tmp);
    free(*err);
    *err = sb_take(&b);
}

static const char *winhttp_reason(DWORD e)
{
    switch (e) {
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:   return "the provider's address could not be found (check the address and the network)";
    case ERROR_WINHTTP_CANNOT_CONNECT:      return "could not connect to the provider";
    case ERROR_WINHTTP_CONNECTION_ERROR:    return "the connection to the provider was lost";
    case ERROR_WINHTTP_TIMEOUT:             return "the provider took too long to answer";
    case ERROR_WINHTTP_SECURE_FAILURE:      return "the secure connection failed (certificate problem, or the clock is wrong)";
    case ERROR_WINHTTP_UNRECOGNIZED_SCHEME:
    case ERROR_WINHTTP_INVALID_URL:         return "the address is not a valid web address";
    case ERROR_WINHTTP_OPERATION_CANCELLED: return "stopped";
    default:                                return NULL;
    }
}

/* method "GET" or "POST"; path is appended to the base address. */
static void http_request(const WCHAR *base, const WCHAR *path, const WCHAR *method,
                         const WCHAR *headers, const char *body, http_result *res)
{
    URL_COMPONENTS uc;
    WCHAR host[256], basepath[1024], url[2048];
    HINTERNET ses = NULL, con = NULL, req = NULL;
    DWORD status = 0, size = sizeof(status), avail, got, err;
    sb_t out;
    size_t blen = body ? strlen(body) : 0;

    ZeroMemory(res, sizeof(*res));
    sb_init(&out);
    _snwprintf(url, ARRAYSIZE(url), L"%s", base);
    url[ARRAYSIZE(url) - 1] = 0;
    ZeroMemory(&uc, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host;
    uc.dwHostNameLength = ARRAYSIZE(host);
    uc.lpszUrlPath = basepath;
    uc.dwUrlPathLength = ARRAYSIZE(basepath);
    if (!WinHttpCrackUrl(url, 0, 0, &uc) ||
        (uc.nScheme != INTERNET_SCHEME_HTTP && uc.nScheme != INTERNET_SCHEME_HTTPS)) {
        set_error(&res->error, "The address is not a valid web address (it should start with https://).");
        return;
    }
    /* "https://host/v1/" + "/models" -> "/v1/models" */
    {
        size_t n = wcslen(basepath);

        while (n && basepath[n - 1] == L'/')
            basepath[--n] = 0;
        _snwprintf(url, ARRAYSIZE(url), L"%s%s", basepath, path);
        url[ARRAYSIZE(url) - 1] = 0;
    }

    ses = WinHttpOpen(L"WinCraftAssistant/" WC_VERSION, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses)
        goto fail;
    /* resolve, connect, send, receive (ms): a model may think for minutes */
    WinHttpSetTimeouts(ses, 15000, 15000, 60000, 300000);
    con = WinHttpConnect(ses, host, uc.nPort, 0);
    if (!con)
        goto fail;
    req = WinHttpOpenRequest(con, method, url, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                             uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!req)
        goto fail;
    lock();
    if (g_cancel) {
        LeaveCriticalSection(&g_lock);
        WinHttpCloseHandle(req);
        req = NULL;
        SetLastError(ERROR_WINHTTP_OPERATION_CANCELLED);
        goto fail;
    }
    g_active = req;
    LeaveCriticalSection(&g_lock);

    if (!WinHttpSendRequest(req, headers, (DWORD)-1L, (void *)body, (DWORD)blen, (DWORD)blen, 0) ||
        !WinHttpReceiveResponse(req, NULL))
        goto fail;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    for (;;) {
        char chunk[16384];

        avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail))
            goto fail;
        if (!avail)
            break;
        if (avail > sizeof(chunk))
            avail = sizeof(chunk);
        if (!WinHttpReadData(req, chunk, avail, &got))
            goto fail;
        if (!got)
            break;
        sb_addn(&out, chunk, got);
        if (out.len > MAX_RESPONSE) {
            set_error(&res->error, "The provider's answer was too large.");
            goto done;
        }
    }
    res->status = (int)status;
    res->len = out.len;
    res->body = sb_take(&out);
    goto done;

fail:
    err = GetLastError();
    if (g_cancel)
        set_error(&res->error, "Stopped.");
    else if (winhttp_reason(err))
        set_error(&res->error, "Could not reach the provider: %s.", winhttp_reason(err));
    else
        set_error(&res->error, "Could not reach the provider (error %lu).", (unsigned long)err);
done:
    sb_free(&out);
    lock();
    if (g_active == req) {
        g_active = NULL;
        if (req)
            WinHttpCloseHandle(req);
    }
    LeaveCriticalSection(&g_lock);
    if (con)
        WinHttpCloseHandle(con);
    if (ses)
        WinHttpCloseHandle(ses);
}

void llm_host(const config *cfg, WCHAR *out, int size, int *port)
{
    URL_COMPONENTS uc;

    ZeroMemory(&uc, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = out;
    uc.dwHostNameLength = size;
    out[0] = 0;
    *port = 443;
    if (!WinHttpCrackUrl(cfg->base_url, 0, 0, &uc))
        lstrcpynW(out, cfg->base_url, size);
    else
        *port = uc.nPort;
}

static void auth_headers(const config *cfg, WCHAR *out, int size)
{
    if (cfg->format == FMT_ANTHROPIC)
        _snwprintf(out, size, L"Content-Type: application/json\r\n"
                              L"anthropic-version: 2023-06-01\r\n"
                              L"x-api-key: %s\r\n", cfg->key);
    else if (cfg->key[0])
        _snwprintf(out, size, L"Content-Type: application/json\r\n"
                              L"Authorization: Bearer %s\r\n", cfg->key);
    else    /* a local server needs no key */
        _snwprintf(out, size, L"Content-Type: application/json\r\n");
    out[size - 1] = 0;
}

/* The provider's own explanation of a failed request, when there is one. */
static void http_error(const http_result *h, char **err)
{
    json *j = h->body ? json_parse(h->body, h->len) : NULL;
    const char *msg = NULL;
    json *e = json_get(j, "error");

    if (e && e->type == J_OBJECT)
        msg = json_str(e, "message");
    else if (e && e->type == J_STRING)
        msg = e->str;
    if (!msg)
        msg = json_str(j, "message");
    switch (h->status) {
    case 401:
    case 403:
        set_error(err, "The provider refused the API key (%d)%s%s", h->status, msg ? ": " : ".", msg ? msg : "");
        break;
    case 404:
        set_error(err, "The provider does not know this address or model (404)%s%s", msg ? ": " : ".", msg ? msg : "");
        break;
    case 429:
        set_error(err, "The provider is limiting requests or the account is out of credit (429)%s%s",
                  msg ? ": " : ".", msg ? msg : "");
        break;
    default:
        set_error(err, "The provider answered with an error (%d)%s%s", h->status, msg ? ": " : ".", msg ? msg : "");
    }
    json_free(j);
}

/* ======================================================================
 * Conversations
 * ====================================================================== */

void llm_add_user(const config *cfg, json *messages, const char *text)
{
    json *m = json_push(messages, json_new(J_OBJECT));

    json_set_str(m, "role", "user");
    json_set_str(m, "content", text);
}

void llm_add_results(const config *cfg, json *messages, const llm_reply *r, char **results)
{
    int i;

    if (cfg->format == FMT_ANTHROPIC) {
        /* all results go back together, in one user turn */
        json *m = json_push(messages, json_new(J_OBJECT));
        json *content;

        json_set_str(m, "role", "user");
        content = json_set(m, "content", json_new(J_ARRAY));
        for (i = 0; i < r->ncalls; i++) {
            json *b = json_push(content, json_new(J_OBJECT));

            json_set_str(b, "type", "tool_result");
            json_set_str(b, "tool_use_id", r->calls[i].id);
            json_set_str(b, "content", results[i]);
        }
    } else {
        for (i = 0; i < r->ncalls; i++) {
            json *m = json_push(messages, json_new(J_OBJECT));

            json_set_str(m, "role", "tool");
            json_set_str(m, "tool_call_id", r->calls[i].id);
            json_set_str(m, "content", results[i]);
        }
    }
}

void llm_reply_free(llm_reply *r)
{
    int i;

    free(r->text);
    free(r->error);
    json_free(r->turn);
    for (i = 0; i < r->ncalls; i++) {
        free(r->calls[i].id);
        free(r->calls[i].name);
        json_free(r->calls[i].input);
    }
    ZeroMemory(r, sizeof(*r));
}

static void add_call(llm_reply *r, const char *id, const char *name, json *input)
{
    tool_call *c;

    if (r->ncalls >= (int)ARRAYSIZE(r->calls)) {
        json_free(input);
        return;
    }
    c = &r->calls[r->ncalls++];
    c->id = utf8_dup(id ? id : "call");
    c->name = utf8_dup(name ? name : "");
    c->input = input && input->type == J_OBJECT ? input : (json_free(input), json_new(J_OBJECT));
}

static int chat_anthropic(const config *cfg, const char *system, const json *tools, const json *messages,
                          llm_reply *r)
{
    json *req = json_new(J_OBJECT), *sys, *blk, *resp = NULL, *content, *c;
    WCHAR headers[1024];
    http_result h;
    sb_t text;
    char *body, *model = utf8_from_w(cfg->model);

    json_set_str(req, "model", model);
    json_set(req, "max_tokens", json_new_num(MAX_TOKENS));
    /* the long, unchanging part of the prompt is marked for the provider's cache */
    sys = json_set(req, "system", json_new(J_ARRAY));
    blk = json_push(sys, json_new(J_OBJECT));
    json_set_str(blk, "type", "text");
    json_set_str(blk, "text", system);
    json_set_str(json_set(blk, "cache_control", json_new(J_OBJECT)), "type", "ephemeral");
    if (tools)
        json_set(req, "tools", json_copy(tools));
    json_set(req, "messages", json_copy(messages));
    body = json_dump(req);
    json_free(req);
    free(model);

    auth_headers(cfg, headers, ARRAYSIZE(headers));
    http_request(cfg->base_url, L"/v1/messages", L"POST", headers, body, &h);
    free(body);
    r->http_status = h.status;
    if (h.error) {
        r->error = h.error;
        return 0;
    }
    if (h.status != 200) {
        http_error(&h, &r->error);
        free(h.body);
        return 0;
    }
    resp = json_parse(h.body, h.len);
    free(h.body);
    content = json_get(resp, "content");
    if (!content || content->type != J_ARRAY) {
        set_error(&r->error, "The provider's answer could not be read.");
        json_free(resp);
        return 0;
    }
    sb_init(&text);
    for (c = content->child; c; c = c->next) {
        const char *type = json_str(c, "type");

        if (type && !strcmp(type, "text")) {
            if (text.len)
                sb_add(&text, "\n\n");
            sb_add(&text, json_str(c, "text"));
        } else if (type && !strcmp(type, "tool_use")) {
            add_call(r, json_str(c, "id"), json_str(c, "name"), json_copy(json_get(c, "input")));
        }
    }
    r->text = text.len ? sb_take(&text) : (sb_free(&text), NULL);
    r->in_tokens = (long)json_num(json_get(resp, "usage"), "input_tokens", 0) +
                   (long)json_num(json_get(resp, "usage"), "cache_read_input_tokens", 0) +
                   (long)json_num(json_get(resp, "usage"), "cache_creation_input_tokens", 0);
    r->out_tokens = (long)json_num(json_get(resp, "usage"), "output_tokens", 0);

    /* the assistant's turn goes into the conversation exactly as it came */
    r->turn = json_new(J_OBJECT);
    json_set_str(r->turn, "role", "assistant");
    json_set(r->turn, "content", json_copy(content));
    json_free(resp);
    return 1;
}

static int chat_openai(const config *cfg, const char *system, const json *tools, const json *messages,
                       llm_reply *r)
{
    json *req = json_new(J_OBJECT), *msgs, *m, *resp = NULL, *msg, *calls, *c;
    WCHAR headers[1024];
    http_result h;
    char *body, *model = utf8_from_w(cfg->model);
    const char *content;

    json_set_str(req, "model", model);
    msgs = json_set(req, "messages", json_new(J_ARRAY));
    m = json_push(msgs, json_new(J_OBJECT));
    json_set_str(m, "role", "system");
    json_set_str(m, "content", system);
    for (c = messages->child; c; c = c->next)
        json_push(msgs, json_copy(c));
    if (tools)
        json_set(req, "tools", json_copy(tools));
    body = json_dump(req);
    json_free(req);
    free(model);

    auth_headers(cfg, headers, ARRAYSIZE(headers));
    http_request(cfg->base_url, L"/chat/completions", L"POST", headers, body, &h);
    free(body);
    r->http_status = h.status;
    if (h.error) {
        r->error = h.error;
        return 0;
    }
    if (h.status != 200) {
        http_error(&h, &r->error);
        free(h.body);
        return 0;
    }
    resp = json_parse(h.body, h.len);
    free(h.body);
    msg = json_get(json_at(json_get(resp, "choices"), 0), "message");
    if (!msg || msg->type != J_OBJECT) {
        set_error(&r->error, "The provider's answer could not be read.");
        json_free(resp);
        return 0;
    }
    content = json_str(msg, "content");
    if (content && *content)
        r->text = utf8_dup(content);
    calls = json_get(msg, "tool_calls");
    for (c = calls ? calls->child : NULL; c; c = c->next) {
        json *fn = json_get(c, "function");
        const char *args = json_str(fn, "arguments");
        json *input = args ? json_parse(args, strlen(args)) : NULL;

        add_call(r, json_str(c, "id"), json_str(fn, "name"), input);
    }
    r->in_tokens = (long)json_num(json_get(resp, "usage"), "prompt_tokens", 0);
    r->out_tokens = (long)json_num(json_get(resp, "usage"), "completion_tokens", 0);

    /* keep the assistant's turn: role, content and tool calls */
    {
        json *keep = r->turn = json_new(J_OBJECT);

        json_set_str(keep, "role", "assistant");
        if (content)
            json_set_str(keep, "content", content);
        else
            json_set(keep, "content", json_new(J_NULL));
        if (calls && calls->type == J_ARRAY && calls->child)
            json_set(keep, "tool_calls", json_copy(calls));
    }
    json_free(resp);
    return 1;
}

int llm_chat(const config *cfg, const char *system, const json *tools, const json *messages, llm_reply *r)
{
    ZeroMemory(r, sizeof(*r));
    if (cfg->format == FMT_ANTHROPIC)
        return chat_anthropic(cfg, system, tools, messages, r);
    return chat_openai(cfg, system, tools, messages, r);
}

/* ======================================================================
 * Models on offer
 * ====================================================================== */

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Chat models only: OpenAI's list also has embedding, speech and image models. */
static BOOL chat_model(const char *id)
{
    static const char *skip[] = { "embed", "tts", "whisper", "dall-e", "moderation", "image",
                                  "transcribe", "realtime", "audio", "davinci", "babbage" };
    unsigned i;

    for (i = 0; i < ARRAYSIZE(skip); i++)
        if (strstr(id, skip[i]))
            return FALSE;
    return TRUE;
}

char *llm_models(const config *cfg, char **error)
{
    WCHAR headers[1024];
    http_result h;
    json *j, *data, *c;
    char *ids[512];
    int n = 0, i;
    sb_t out;

    *error = NULL;
    auth_headers(cfg, headers, ARRAYSIZE(headers));
    http_request(cfg->base_url, cfg->format == FMT_ANTHROPIC ? L"/v1/models?limit=100" : L"/models",
                 L"GET", headers, NULL, &h);
    if (h.error) {
        *error = h.error;
        return NULL;
    }
    if (h.status != 200) {
        http_error(&h, error);
        free(h.body);
        return NULL;
    }
    j = json_parse(h.body, h.len);
    free(h.body);
    data = json_get(j, "data");
    if (!data)
        data = json_get(j, "models");       /* some local servers */
    for (c = data ? data->child : NULL; c && n < (int)ARRAYSIZE(ids); c = c->next) {
        const char *id = json_str(c, "id");

        if (!id)
            id = json_str(c, "name");
        if (id && chat_model(id))
            ids[n++] = (char *)id;
    }
    if (cfg->format != FMT_ANTHROPIC)       /* Anthropic lists the newest first */
        qsort(ids, n, sizeof(ids[0]), cmp_str);
    sb_init(&out);
    for (i = 0; i < n; i++) {
        sb_add(&out, ids[i]);
        sb_addc(&out, 0);
    }
    sb_addc(&out, 0);
    json_free(j);
    if (!n) {
        sb_free(&out);
        *error = utf8_dup("The provider lists no models. Type the model's name instead.");
        return NULL;
    }
    return sb_take(&out);
}
