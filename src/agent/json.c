/*
 * WinCraft Assistant - strings and JSON.
 *
 * A small JSON reader and writer: enough for the model APIs, which send and
 * receive UTF-8 objects. Values form a tree of siblings; object members carry
 * their key.
 *
 * SPDX-License-Identifier: MIT
 */
#include "agent.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ======================================================================
 * Strings
 * ====================================================================== */

void sb_init(sb_t *b)
{
    b->s = NULL;
    b->len = b->cap = 0;
}

void sb_free(sb_t *b)
{
    free(b->s);
    sb_init(b);
}

static void sb_grow(sb_t *b, size_t extra)
{
    size_t need = b->len + extra + 1;

    if (need <= b->cap)
        return;
    b->cap = b->cap ? b->cap : 256;
    while (b->cap < need)
        b->cap *= 2;
    b->s = realloc(b->s, b->cap);
    if (!b->s)
        abort();
}

void sb_addn(sb_t *b, const char *s, size_t n)
{
    sb_grow(b, n);
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = 0;
}

void sb_add(sb_t *b, const char *s)
{
    if (s)
        sb_addn(b, s, strlen(s));
}

void sb_addc(sb_t *b, char c)
{
    sb_addn(b, &c, 1);
}

void sb_addf(sb_t *b, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    sb_grow(b, n);
    va_start(ap, fmt);
    vsnprintf(b->s + b->len, n + 1, fmt, ap);
    va_end(ap);
    b->len += n;
}

void sb_addw(sb_t *b, const WCHAR *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);

    if (n <= 1)
        return;
    sb_grow(b, n);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, b->s + b->len, n, NULL, NULL);
    b->len += n - 1;
}

char *sb_take(sb_t *b)
{
    char *s = b->s ? b->s : utf8_dup("");

    sb_init(b);
    return s;
}

char *utf8_dup(const char *s)
{
    size_t n = s ? strlen(s) : 0;
    char *d = malloc(n + 1);

    if (!d)
        abort();
    if (n)
        memcpy(d, s, n);
    d[n] = 0;
    return d;
}

char *utf8_from_w(const WCHAR *w)
{
    sb_t b;

    sb_init(&b);
    sb_addw(&b, w);
    return sb_take(&b);
}

WCHAR *w_from_utf8(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s ? s : "", -1, NULL, 0);
    WCHAR *w = malloc((n > 0 ? n : 1) * sizeof(WCHAR));

    if (!w)
        abort();
    w[0] = 0;
    MultiByteToWideChar(CP_UTF8, 0, s ? s : "", -1, w, n);
    return w;
}

WCHAR *w_from_utf8_crlf(const char *s)
{
    sb_t b;
    WCHAR *w;

    sb_init(&b);
    for (; s && *s; s++) {
        if (*s == '\n' && (b.len == 0 || b.s[b.len - 1] != '\r'))
            sb_addc(&b, '\r');
        sb_addc(&b, *s);
    }
    w = w_from_utf8(b.s);
    sb_free(&b);
    return w;
}

/* ======================================================================
 * JSON values
 * ====================================================================== */

json *json_new(jtype t)
{
    json *v = calloc(1, sizeof(*v));

    if (!v)
        abort();
    v->type = t;
    return v;
}

json *json_new_str(const char *s)
{
    json *v = json_new(J_STRING);

    v->str = utf8_dup(s);
    return v;
}

json *json_new_num(double n)
{
    json *v = json_new(J_NUMBER);

    v->num = n;
    return v;
}

void json_free(json *v)
{
    while (v) {
        json *next = v->next;

        json_free(v->child);
        free(v->str);
        free(v->key);
        free(v);
        v = next;
    }
}

static void append_child(json *parent, json *val)
{
    json **p = &parent->child;

    while (*p)
        p = &(*p)->next;
    *p = val;
}

json *json_set(json *obj, const char *key, json *val)
{
    json **p = &obj->child;

    /* replace an existing member of the same name */
    while (*p) {
        if ((*p)->key && !strcmp((*p)->key, key)) {
            json *old = *p;

            val->next = old->next;
            old->next = NULL;
            json_free(old);
            *p = val;
            val->key = utf8_dup(key);
            return val;
        }
        p = &(*p)->next;
    }
    val->key = utf8_dup(key);
    *p = val;
    return val;
}

json *json_set_str(json *obj, const char *key, const char *s)
{
    return json_set(obj, key, json_new_str(s));
}

json *json_push(json *arr, json *val)
{
    append_child(arr, val);
    return val;
}

json *json_get(const json *obj, const char *key)
{
    json *c;

    if (!obj || obj->type != J_OBJECT)
        return NULL;
    for (c = obj->child; c; c = c->next)
        if (c->key && !strcmp(c->key, key))
            return c;
    return NULL;
}

const char *json_str(const json *obj, const char *key)
{
    json *v = json_get(obj, key);

    return v && v->type == J_STRING ? v->str : NULL;
}

double json_num(const json *obj, const char *key, double def)
{
    json *v = json_get(obj, key);

    return v && v->type == J_NUMBER ? v->num : def;
}

int json_len(const json *arr)
{
    int n = 0;
    json *c;

    if (!arr || (arr->type != J_ARRAY && arr->type != J_OBJECT))
        return 0;
    for (c = arr->child; c; c = c->next)
        n++;
    return n;
}

json *json_at(const json *arr, int i)
{
    json *c;

    if (!arr || arr->type != J_ARRAY)
        return NULL;
    for (c = arr->child; c && i > 0; c = c->next)
        i--;
    return c;
}

json *json_copy(const json *v)
{
    json *c, *n;

    if (!v)
        return NULL;
    n = json_new(v->type);
    n->num = v->num;
    if (v->str)
        n->str = utf8_dup(v->str);
    for (c = v->child; c; c = c->next) {
        json *cc = json_copy(c);

        if (c->key)
            cc->key = utf8_dup(c->key);
        append_child(n, cc);
    }
    return n;
}

/* ======================================================================
 * Reading
 * ====================================================================== */

typedef struct {
    const char *p, *end;
    int depth;
} reader;

static void skip_ws(reader *r)
{
    while (r->p < r->end && (*r->p == ' ' || *r->p == '\t' || *r->p == '\n' || *r->p == '\r'))
        r->p++;
}

static int hex4(const char *p, unsigned *out)
{
    unsigned v = 0;
    int i;

    for (i = 0; i < 4; i++) {
        char c = p[i];

        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return 0;
    }
    *out = v;
    return 1;
}

static void put_utf8(sb_t *b, unsigned cp)
{
    char t[4];

    if (cp < 0x80) {
        sb_addc(b, (char)cp);
    } else if (cp < 0x800) {
        t[0] = 0xc0 | (cp >> 6);
        t[1] = 0x80 | (cp & 0x3f);
        sb_addn(b, t, 2);
    } else if (cp < 0x10000) {
        t[0] = 0xe0 | (cp >> 12);
        t[1] = 0x80 | ((cp >> 6) & 0x3f);
        t[2] = 0x80 | (cp & 0x3f);
        sb_addn(b, t, 3);
    } else {
        t[0] = 0xf0 | (cp >> 18);
        t[1] = 0x80 | ((cp >> 12) & 0x3f);
        t[2] = 0x80 | ((cp >> 6) & 0x3f);
        t[3] = 0x80 | (cp & 0x3f);
        sb_addn(b, t, 4);
    }
}

static char *read_string(reader *r)
{
    sb_t b;

    sb_init(&b);
    r->p++;     /* opening quote */
    while (r->p < r->end && *r->p != '"') {
        char c = *r->p++;

        if (c != '\\') {
            sb_addc(&b, c);
            continue;
        }
        if (r->p >= r->end)
            break;
        c = *r->p++;
        switch (c) {
        case 'b': sb_addc(&b, '\b'); break;
        case 'f': sb_addc(&b, '\f'); break;
        case 'n': sb_addc(&b, '\n'); break;
        case 'r': sb_addc(&b, '\r'); break;
        case 't': sb_addc(&b, '\t'); break;
        case 'u': {
            unsigned cp, lo;

            if (r->end - r->p < 4 || !hex4(r->p, &cp))
                goto fail;
            r->p += 4;
            if (cp >= 0xd800 && cp < 0xdc00 && r->end - r->p >= 6 && r->p[0] == '\\' &&
                r->p[1] == 'u' && hex4(r->p + 2, &lo) && lo >= 0xdc00 && lo < 0xe000) {
                cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                r->p += 6;
            }
            put_utf8(&b, cp);
            break;
        }
        default: sb_addc(&b, c); break;
        }
    }
    if (r->p >= r->end)
        goto fail;
    r->p++;     /* closing quote */
    return sb_take(&b);
fail:
    sb_free(&b);
    return NULL;
}

static json *read_value(reader *r);

static json *read_container(reader *r, jtype t)
{
    json *v = json_new(t);
    char close = t == J_ARRAY ? ']' : '}';

    if (++r->depth > 64)
        goto fail;
    r->p++;
    skip_ws(r);
    if (r->p < r->end && *r->p == close) {
        r->p++;
        r->depth--;
        return v;
    }
    for (;;) {
        char *key = NULL;
        json *item;

        skip_ws(r);
        if (t == J_OBJECT) {
            if (r->p >= r->end || *r->p != '"' || !(key = read_string(r)))
                goto fail;
            skip_ws(r);
            if (r->p >= r->end || *r->p != ':') {
                free(key);
                goto fail;
            }
            r->p++;
        }
        if (!(item = read_value(r))) {
            free(key);
            goto fail;
        }
        item->key = key;
        append_child(v, item);
        skip_ws(r);
        if (r->p < r->end && *r->p == ',') {
            r->p++;
            continue;
        }
        if (r->p < r->end && *r->p == close) {
            r->p++;
            r->depth--;
            return v;
        }
        goto fail;
    }
fail:
    json_free(v);
    return NULL;
}

static json *read_value(reader *r)
{
    skip_ws(r);
    if (r->p >= r->end)
        return NULL;
    switch (*r->p) {
    case '{': return read_container(r, J_OBJECT);
    case '[': return read_container(r, J_ARRAY);
    case '"': {
        char *s = read_string(r);
        json *v;

        if (!s)
            return NULL;
        v = json_new(J_STRING);
        v->str = s;
        return v;
    }
    case 't':
        if (r->end - r->p >= 4 && !strncmp(r->p, "true", 4)) {
            r->p += 4;
            return json_new(J_TRUE);
        }
        return NULL;
    case 'f':
        if (r->end - r->p >= 5 && !strncmp(r->p, "false", 5)) {
            r->p += 5;
            return json_new(J_FALSE);
        }
        return NULL;
    case 'n':
        if (r->end - r->p >= 4 && !strncmp(r->p, "null", 4)) {
            r->p += 4;
            return json_new(J_NULL);
        }
        return NULL;
    default: {
        char tmp[64];
        size_t n = 0;
        char *end;
        json *v;

        while (r->p + n < r->end && n < sizeof(tmp) - 1 &&
               strchr("+-0123456789.eE", r->p[n]))
            n++;
        if (!n)
            return NULL;
        memcpy(tmp, r->p, n);
        tmp[n] = 0;
        v = json_new(J_NUMBER);
        v->num = strtod(tmp, &end);
        if (end == tmp) {
            json_free(v);
            return NULL;
        }
        r->p += end - tmp;
        return v;
    }
    }
}

json *json_parse(const char *text, size_t len)
{
    reader r;
    json *v;

    if (!text)
        return NULL;
    r.p = text;
    r.end = text + len;
    r.depth = 0;
    v = read_value(&r);
    skip_ws(&r);
    if (v && r.p != r.end) {
        json_free(v);
        return NULL;
    }
    return v;
}

/* ======================================================================
 * Writing
 * ====================================================================== */

/* The length of the valid UTF-8 sequence at p (2 to 4), or 0 if it is not
 * one: a stray continuation byte, a cut or overlong sequence, a surrogate,
 * or a code point beyond U+10FFFF. */
static int utf8_len(const unsigned char *p)
{
    int n, i;

    if (p[0] >= 0xc2 && p[0] <= 0xdf)
        n = 2;
    else if (p[0] >= 0xe0 && p[0] <= 0xef)
        n = 3;
    else if (p[0] >= 0xf0 && p[0] <= 0xf4)
        n = 4;
    else
        return 0;
    for (i = 1; i < n; i++)
        if ((p[i] & 0xc0) != 0x80)
            return 0;
    if ((p[0] == 0xe0 && p[1] < 0xa0) ||        /* overlong */
        (p[0] == 0xed && p[1] >= 0xa0) ||       /* surrogate */
        (p[0] == 0xf0 && p[1] < 0x90) ||        /* overlong */
        (p[0] == 0xf4 && p[1] >= 0x90))         /* beyond U+10FFFF */
        return 0;
    return n;
}

/* Text from programs, logs and pages may hold any bytes, and one that is not
 * UTF-8 makes the whole request invalid JSON: each such byte becomes U+FFFD. */
void json_quote(sb_t *b, const char *s)
{
    sb_addc(b, '"');
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        int n;

        switch (c) {
        case '"':  sb_add(b, "\\\""); break;
        case '\\': sb_add(b, "\\\\"); break;
        case '\n': sb_add(b, "\\n"); break;
        case '\r': sb_add(b, "\\r"); break;
        case '\t': sb_add(b, "\\t"); break;
        default:
            if (c < 0x20)
                sb_addf(b, "\\u%04x", c);
            else if (c < 0x80)
                sb_addc(b, (char)c);
            else if ((n = utf8_len((const unsigned char *)s))) {
                sb_addn(b, s, n);
                s += n - 1;
            } else
                sb_add(b, "\\ufffd");
        }
    }
    sb_addc(b, '"');
}

void json_dump_to(sb_t *b, const json *v)
{
    json *c;

    switch (v->type) {
    case J_NULL:  sb_add(b, "null"); break;
    case J_FALSE: sb_add(b, "false"); break;
    case J_TRUE:  sb_add(b, "true"); break;
    case J_NUMBER:
        if (isfinite(v->num) && v->num == floor(v->num) && fabs(v->num) < 1e15)
            sb_addf(b, "%.0f", v->num);
        else if (isfinite(v->num))
            sb_addf(b, "%.17g", v->num);
        else
            sb_add(b, "null");
        break;
    case J_STRING: json_quote(b, v->str); break;
    case J_ARRAY:
    case J_OBJECT:
        sb_addc(b, v->type == J_ARRAY ? '[' : '{');
        for (c = v->child; c; c = c->next) {
            if (v->type == J_OBJECT) {
                json_quote(b, c->key ? c->key : "");
                sb_addc(b, ':');
            }
            json_dump_to(b, c);
            if (c->next)
                sb_addc(b, ',');
        }
        sb_addc(b, v->type == J_ARRAY ? ']' : '}');
        break;
    }
}

char *json_dump(const json *v)
{
    sb_t b;

    sb_init(&b);
    if (v)
        json_dump_to(&b, v);
    return sb_take(&b);
}
