/*
 * WinCraft Assistant - the model's Markdown, as far as the chat shows it.
 *
 * Models answer in Markdown. The chat shows a subset: paragraphs, bullet and
 * numbered lists, bold, italics, code (file paths) and web addresses. A
 * heading becomes a bold line and a quote an indented one. A table becomes a
 * list with one item per row, since the chat is narrow. md_render hands the
 * result to an output: the chat window, or plain text for the saved chat.
 *
 * SPDX-License-Identifier: MIT
 */
#include "agent.h"
#include <ctype.h>
#include <string.h>

/* paragraph indents, in twips (1/1440 inch) */
#define LIST_INDENT     120
#define LIST_STEP       320
#define BULLET_HANG     200
#define NUMBER_HANG     260
#define QUOTE_INDENT    300
#define CODE_INDENT     200

#define MAX_CELLS       16

static BOOL word_char(char c)
{
    return isalnum((unsigned char)c) || (unsigned char)c >= 0x80;
}

/* Whether a closing marker follows in s[0, n): `count` times `c`, after a
 * character that is not a space, and for "_" not inside a word. */
static BOOL has_closing(const char *s, size_t n, char c, int count)
{
    size_t i;

    for (i = 1; i + count <= n; i++) {
        if (s[i] != c || (count == 2 && s[i + 1] != c) || (count == 1 && i + 1 < n && s[i + 1] == c))
            continue;
        if (s[i - 1] == ' ')
            continue;
        if (c == '_' && i + count < n && word_char(s[i + count]))
            continue;
        return TRUE;
    }
    return FALSE;
}

static BOOL web_address(const char *s, size_t n)
{
    return (n > 8 && !_strnicmp(s, "https://", 8)) || (n > 7 && !_strnicmp(s, "http://", 7));
}

/* Text within a paragraph: bold, italics, code, links, escapes. */
static void md_inline(md_out *o, const char *s, size_t n, int style)
{
    size_t i = 0, start = 0;

#define FLUSH() do { if (i > start) o->text(o, s + start, i - start, style); } while (0)
    while (i < n) {
        char c = s[i];

        if (c == '\\' && i + 1 < n && strchr("\\`*_{}[]()#+-.!|>", s[i + 1])) {
            FLUSH();
            start = i + 1;                  /* the escaped character is plain text */
            i += 2;
            continue;
        }
        if (c == '`') {
            const char *close = memchr(s + i + 1, '`', n - i - 1);

            if (close) {
                FLUSH();
                o->text(o, s + i + 1, (size_t)(close - (s + i + 1)), style | MD_MONO);
                i = (size_t)(close - s) + 1;
                start = i;
                continue;
            }
        }
        if ((c == '*' || c == '_') && i + 1 < n && s[i + 1] == c) {
            if (style & MD_BOLD) {
                FLUSH();
                style &= ~MD_BOLD;
                i += 2;
                start = i;
                continue;
            }
            if (!(c == '_' && i && word_char(s[i - 1])) && i + 2 < n && s[i + 2] != ' ' &&
                has_closing(s + i + 1, n - i - 1, c, 2)) {
                FLUSH();
                style |= MD_BOLD;
                i += 2;
                start = i;
                continue;
            }
            i += 2;
            continue;
        }
        if (c == '*' || c == '_') {
            if (style & MD_ITALIC) {
                if (i && s[i - 1] != ' ' && !(c == '_' && i + 1 < n && word_char(s[i + 1]))) {
                    FLUSH();
                    style &= ~MD_ITALIC;
                    i++;
                    start = i;
                    continue;
                }
            } else if (!(c == '_' && i && word_char(s[i - 1])) && i + 1 < n && s[i + 1] != ' ' &&
                       has_closing(s + i, n - i, c, 1)) {
                FLUSH();
                style |= MD_ITALIC;
                i++;
                start = i;
                continue;
            }
        }
        if (c == '[') {
            const char *rb = memchr(s + i, ']', n - i), *rp;

            if (rb && (size_t)(rb - s) + 1 < n && rb[1] == '(' &&
                (rp = memchr(rb + 2, ')', n - (size_t)(rb + 2 - s)))) {
                const char *text = s + i + 1, *url = rb + 2;
                size_t tn = (size_t)(rb - text), un = (size_t)(rp - url);

                FLUSH();
                if (web_address(url, un) && (tn != un || strncmp(text, url, un))) {
                    md_inline(o, text, tn, style);
                    o->text(o, " (", 2, style & ~(MD_BOLD | MD_ITALIC));
                    o->text(o, url, un, style & ~(MD_BOLD | MD_ITALIC));
                    o->text(o, ")", 1, style & ~(MD_BOLD | MD_ITALIC));
                } else if (web_address(url, un)) {
                    o->text(o, url, un, style & ~(MD_BOLD | MD_ITALIC));
                } else {
                    md_inline(o, text, tn, style);
                }
                i = (size_t)(rp - s) + 1;
                start = i;
                continue;
            }
        }
        i++;
    }
    FLUSH();
#undef FLUSH
}

/* One line as a paragraph of its own. */
static void md_line(md_out *o, int indent, int hang, int spaces, const char *s, size_t n, int style)
{
    o->begin(o, indent, hang, spaces);
    md_inline(o, s, n, style);
    o->end(o);
}

/* Cells of a table row "| a | b |", as pointers into the text. */
static int table_cells(const char *s, size_t n, const char **cell, size_t *len)
{
    size_t i = 0, start;
    int k = 0;

    while (i < n && s[i] == ' ')
        i++;
    if (i < n && s[i] == '|')
        i++;
    while (i < n && k < MAX_CELLS) {
        start = i;
        while (i < n && !(s[i] == '|' && (i == 0 || s[i - 1] != '\\')))
            i++;
        cell[k] = s + start;
        len[k] = i - start;
        while (len[k] && cell[k][0] == ' ')
            cell[k]++, len[k]--;
        while (len[k] && cell[k][len[k] - 1] == ' ')
            len[k]--;
        if (i < n || len[k])                /* not the empty piece after a closing '|' */
            k++;
        if (i < n)
            i++;
    }
    return k;
}

static BOOL separator_row(const char **cell, const size_t *len, int k)
{
    int j;
    size_t i;

    for (j = 0; j < k; j++)
        for (i = 0; i < len[j]; i++)
            if (!strchr(":- ", cell[j][i]))
                return FALSE;
    return k > 0;
}

static size_t line_length(const char *p, const char **next)
{
    const char *e = strchr(p, '\n');
    size_t n = e ? (size_t)(e - p) : strlen(p);

    *next = e ? e + 1 : p + n;
    while (n && (p[n - 1] == '\r' || p[n - 1] == ' ' || p[n - 1] == '\t'))
        n--;
    return n;
}

static BOOL table_line(const char *p, size_t n)
{
    while (n && *p == ' ')
        p++, n--;
    return n > 1 && *p == '|' && memchr(p + 1, '|', n - 1);
}

/* A table becomes a list: per row, the first cell in bold, then the others
 * as "heading: value". Returns where the text after the table starts. */
static const char *md_table(md_out *o, const char *p)
{
    const char *head[MAX_CELLS], *cell[MAX_CELLS], *next, *after;
    size_t head_len[MAX_CELLS], len[MAX_CELLS], n;
    int nhead = 0, k, j, shown;

    n = line_length(p, &next);
    nhead = table_cells(p, n, head, head_len);
    n = line_length(next, &after);
    if (table_line(next, n) && (k = table_cells(next, n, cell, len)) && separator_row(cell, len, k))
        p = after;                          /* a heading row */
    else
        nhead = 0;
    while (*p) {
        n = line_length(p, &next);
        if (!table_line(p, n))
            break;
        k = table_cells(p, n, cell, len);
        if (!separator_row(cell, len, k)) {
            o->begin(o, LIST_INDENT, BULLET_HANG, 0);
            o->text(o, "\xe2\x80\xa2 ", 4, 0);
            shown = 0;
            for (j = 0; j < k; j++) {
                if (!len[j])
                    continue;
                if (!shown++) {
                    md_inline(o, cell[j], len[j], MD_BOLD);
                    continue;
                }
                if (shown == 2)
                    o->text(o, " \xe2\x80\x94 ", 5, 0);     /* " - " after the first cell, then "; " */
                else
                    o->text(o, "; ", 2, 0);
                if (j < nhead && head_len[j]) {
                    md_inline(o, head[j], head_len[j], 0);
                    o->text(o, ": ", 2, 0);
                }
                md_inline(o, cell[j], len[j], 0);
            }
            o->end(o);
        }
        p = next;
    }
    return p;
}

void md_render(const char *md, md_out *o, int style)
{
    const char *p = md, *next, *t;
    BOOL code = FALSE, blank = TRUE;
    size_t n, ind, tl, d;

    while (*p) {
        n = line_length(p, &next);
        for (ind = 0; ind < n && p[ind] == ' '; ind++)
            ;
        t = p + ind;
        tl = n - ind;
        if (tl >= 3 && !strncmp(t, "```", 3)) {             /* code block fence */
            code = !code;
            p = next;
            continue;
        }
        if (code) {
            o->begin(o, CODE_INDENT, 0, 4);
            o->text(o, p, n, style | MD_MONO);
            o->end(o);
            blank = FALSE;
            p = next;
            continue;
        }
        if (!tl) {                                          /* one empty line for any number */
            if (!blank) {
                o->begin(o, 0, 0, 0);
                o->end(o);
            }
            blank = TRUE;
            p = next;
            continue;
        }
        if (tl >= 3 && strspn(t, "-*_ ") == tl && (t[0] == '-' || t[0] == '*' || t[0] == '_')) {
            p = next;                                       /* a horizontal rule: nothing, like a blank line */
            continue;
        }
        blank = FALSE;
        if (table_line(t, tl)) {
            p = md_table(o, p);
            continue;
        }
        if (*t == '#') {                                    /* heading: a bold line */
            for (d = 0; d < tl && t[d] == '#'; d++)
                ;
            if (d <= 6 && d < tl && t[d] == ' ') {
                md_line(o, 0, 0, 0, t + d + 1, tl - d - 1, style | MD_BOLD);
                p = next;
                continue;
            }
        }
        if (*t == '>') {                                    /* quote */
            t++, tl--;
            if (tl && *t == ' ')
                t++, tl--;
            md_line(o, QUOTE_INDENT, 0, 2, t, tl, style | MD_GREY);
            p = next;
            continue;
        }
        if ((*t == '-' || *t == '*' || *t == '+') && tl > 1 && t[1] == ' ') {
            int level = ind >= 2 ? 1 : 0;

            o->begin(o, LIST_INDENT + level * LIST_STEP, BULLET_HANG, level * 2);
            o->text(o, "\xe2\x80\xa2 ", 4, style);
            md_inline(o, t + 2, tl - 2, style);
            o->end(o);
            p = next;
            continue;
        }
        for (d = 0; d < tl && d < 4 && isdigit((unsigned char)t[d]); d++)
            ;
        if (d && d + 1 < tl && (t[d] == '.' || t[d] == ')') && t[d + 1] == ' ') {
            int level = ind >= 2 ? 1 : 0;

            o->begin(o, LIST_INDENT + level * LIST_STEP, NUMBER_HANG, level * 2);
            o->text(o, t, d + 2, style);
            md_inline(o, t + d + 2, tl - d - 2, style);
            o->end(o);
            p = next;
            continue;
        }
        if (ind >= 2)                                       /* more text of a list item */
            md_line(o, LIST_INDENT + BULLET_HANG + (ind >= 4 ? LIST_STEP : 0), 0, ind >= 4 ? 4 : 2, t, tl,
                    style);
        else
            md_line(o, 0, 0, 0, t, tl, style);
        p = next;
    }
}

/* ---- plain text, for the saved chat and a chat window without rich text ---- */

struct plain_out {
    md_out o;
    sb_t b;
};

static void plain_begin(md_out *o, int indent, int hang, int spaces)
{
    struct plain_out *po = (struct plain_out *)o;

    while (spaces-- > 0)
        sb_addc(&po->b, ' ');
}

static void plain_text(md_out *o, const char *s, size_t n, int style)
{
    sb_addn(&((struct plain_out *)o)->b, s, n);
}

static void plain_end(md_out *o)
{
    sb_addc(&((struct plain_out *)o)->b, '\n');
}

char *md_plain(const char *md)
{
    struct plain_out po = { { plain_begin, plain_text, plain_end } };

    sb_init(&po.b);
    sb_add(&po.b, "");
    md_render(md, &po.o, 0);
    return sb_take(&po.b);
}
