#include <string.h>

#include "md.h"

/* The style invariant the pager depends on: every style change is a single
   self-contained SGR that starts with 0, so the last one seen fully
   describes the state and a wrapped row can restore it from one offset. */

static void md_puts(const md *r, const char *s) {
    r->emit(r->user, (const uint8_t *)s, strlen(s));
}

static void md_indent(const md *r, size_t n) {
    for (size_t i = 0; i < n; i++) {
        md_puts(r, " ");
    }
}

typedef enum {
    MD_B_PARA = 0,
    MD_B_HEADING,
    MD_B_QUOTE,
    MD_B_CODE,
} md_block;

static void style(const md *r) {
    md_puts(r, "\x1b[0");
    if (r->block == MD_B_HEADING) {
        md_puts(r, ";1");
        if (r->heading <= 2) {
            md_puts(r, ";36");
        }
        if (r->heading == 1) {
            md_puts(r, ";4");
        }
    }
    if (r->block == MD_B_QUOTE) {
        md_puts(r, ";2;3");
    }
    if (r->bold) {
        md_puts(r, ";1");
    }
    if (r->italic) {
        md_puts(r, ";3");
    }
    if (r->code) {
        md_puts(r, ";33");
    }
    if (r->link) {
        md_puts(r, ";4;36");
    }
    md_puts(r, "m");
}

/* ---- inline ---- */

static bool flanks(const uint8_t *s, size_t n, size_t open, size_t close, size_t mark) {
    /* an intraword _ is part of an identifier (snake_case), not emphasis */
    if (open > 0 && s[open - 1] != ' ') {
        return false;
    }
    size_t after = close + mark;
    if (after < n && s[after] != ' ' && s[after] != '.' && s[after] != ',') {
        return false;
    }
    return true;
}

/* Finds the closing run of `mark` (1 or 2 bytes of ch) after `from`;
   returns n when there is none. */
static size_t find_close(const uint8_t *s, size_t n, size_t from, uint8_t ch, size_t mark) {
    size_t i = from;
    while (i + mark <= n) {
        if (s[i] == ch && (mark == 1 || s[i + 1] == ch)) {
            return i;
        }
        i++;
    }
    return n;
}

static void put_link_open(const md *r, const char *url) {
    md_puts(r, "\x1b]8;;");
    md_puts(r, url);
    md_puts(r, "\x1b\\");
}

static void put_link_close(const md *r) {
    md_puts(r, "\x1b]8;;\x1b\\");
}

static bool str_append(char *dst, size_t cap, size_t *len, const char *s, size_t n) {
    if (n > cap - 1 - *len) {
        return false;
    }
    memcpy(dst + *len, s, n);
    *len += n;
    dst[*len] = '\0';
    return true;
}

/* Builds the absolute URL a target resolves to, or returns false when it
   cannot become a terminal hyperlink: a bare relative path has no base to
   resolve against, so it stays plain text. */
static bool link_url(const md *r, const uint8_t *url, size_t n, char *dst, size_t cap) {
    size_t len = 0;
    dst[0] = '\0';
    if (n == 0 || n > cap - 1) {
        return false;
    }
    if (n > 4 && memcmp(url, "{{<", 3) == 0) {
        size_t q = 0;
        while (q < n && url[q] != '"') {
            q++;
        }
        size_t e = q + 1;
        while (e < n && url[e] != '"') {
            e++;
        }
        if (e >= n) {
            return false;
        }
        char page[MD_URL_MAX];
        if (r->slug == NULL || !r->slug(r->user, url + q + 1, e - q - 1, page, sizeof(page))) {
            return false;
        }
        size_t pl = strlen(page) - 3; /* the page URL drops the .md */
        if (!str_append(dst, cap, &len, r->base, strlen(r->base))) {
            return false;
        }
        if (!str_append(dst, cap, &len, page, pl)) {
            return false;
        }
        return str_append(dst, cap, &len, "/", 1);
    }
    if (url[0] == '/' && !str_append(dst, cap, &len, r->base, strlen(r->base))) {
        return false;
    }
    bool ok = false;
    if (url[0] == '/') {
        ok = r->base[0] != '\0'; /* no base, no site to point into */
    }
    if (n > 7 && memcmp(url, "http://", 7) == 0) {
        ok = true;
    }
    if (n > 8 && memcmp(url, "https://", 8) == 0) {
        ok = true;
    }
    if (n > 7 && memcmp(url, "mailto:", 7) == 0) {
        ok = true;
    }
    if (!ok) {
        return false;
    }
    return str_append(dst, cap, &len, (const char *)url, n);
}

static void render_inline(md *r, const uint8_t *s, size_t n);

/* Splits "[text](url)" starting at the bracket. Parsing is separate from
   emitting so the caller can flush the text before it first. */
typedef struct {
    size_t text;
    size_t text_end;
    size_t url;
    size_t url_end;
} md_link;

static bool link_parse(const uint8_t *s, size_t n, size_t at, md_link *out) {
    size_t close = at + 1;
    while (close < n && s[close] != ']') {
        close++;
    }
    if (close + 1 >= n || s[close + 1] != '(') {
        return false;
    }
    size_t end = close + 2;
    while (end < n && s[end] != ')') {
        end++;
    }
    if (end >= n) {
        return false;
    }
    out->text = at + 1;
    out->text_end = close;
    out->url = close + 2;
    out->url_end = end;
    return true;
}

static void render_link(md *r, const uint8_t *s, const md_link *l, bool image) {
    char url[MD_URL_MAX];
    bool href = link_url(r, s + l->url, l->url_end - l->url, url, sizeof(url));
    if (href) {
        put_link_open(r, url);
        r->link = true;
        style(r);
    }
    if (image) {
        md_puts(r, "[img] ");
    }
    render_inline(r, s + l->text, l->text_end - l->text);
    if (href) {
        r->link = false;
        style(r);
        put_link_close(r);
    }
}

/* Reads name="value" out of a shortcode's argument span. */
static bool attr_of(const uint8_t *s, size_t n, const char *name, size_t *off, size_t *len) {
    char pat[16];
    size_t pl = strlen(name);
    if (pl + 2 >= sizeof(pat)) {
        return false;
    }
    memcpy(pat, name, pl);
    pat[pl] = '=';
    pat[pl + 1] = '"';
    for (size_t i = 0; i + pl + 2 <= n; i++) {
        if (memcmp(s + i, pat, pl + 2) != 0) {
            continue;
        }
        size_t v = i + pl + 2;
        size_t e = v;
        while (e < n && s[e] != '"') {
            e++;
        }
        if (e >= n) {
            return false;
        }
        *off = v;
        *len = e - v;
        return true;
    }
    return false;
}

/* Renders a Hugo shortcode. The ones the site actually uses become links;
   anything else is dropped, because raw template syntax on screen is worse
   than nothing. */
static void render_shortcode(md *r, const uint8_t *s, size_t n) {
    size_t i = 0;
    while (i < n && s[i] == ' ') {
        i++;
    }
    size_t name = i;
    while (i < n && s[i] != ' ') {
        i++;
    }
    size_t name_len = i - name;
    while (i < n && s[i] == ' ') {
        i++;
    }
    char url[MD_URL_MAX];
    size_t off = 0;
    size_t len = 0;
    if (name_len == 7 && memcmp(s + name, "youtube", 7) == 0) {
        size_t id = i;
        while (i < n && s[i] != ' ') {
            i++;
        }
        size_t ulen = 0;
        url[0] = '\0';
        if (!str_append(url, sizeof(url), &ulen, "https://www.youtube.com/watch?v=", 32) ||
            !str_append(url, sizeof(url), &ulen, (const char *)s + id, i - id)) {
            return;
        }
        put_link_open(r, url);
        r->link = true;
        style(r);
        md_puts(r, "\xe2\x96\xb6 video");
        r->link = false;
        style(r);
        put_link_close(r);
        return;
    }
    if (name_len == 4 && memcmp(s + name, "anim", 4) == 0) {
        bool href = false;
        if (attr_of(s, n, "href", &off, &len)) {
            href = link_url(r, s + off, len, url, sizeof(url));
        }
        if (href) {
            put_link_open(r, url);
            r->link = true;
            style(r);
        }
        md_puts(r, "[anim]");
        size_t aoff = 0;
        size_t alen = 0;
        if (attr_of(s, n, "alt", &aoff, &alen)) {
            md_puts(r, " ");
            r->emit(r->user, s + aoff, alen);
        }
        if (href) {
            r->link = false;
            style(r);
            put_link_close(r);
        }
    }
}

/* Locates the argument span of a shortcode opened at `at` ("{{<" or "{{%").
   Returns false when it is not closed on this line. */
static bool shortcode_span(const uint8_t *s, size_t n, size_t at, size_t *arg, size_t *arg_len,
                           size_t *end) {
    /* "{{<" closes with ">}}" and "{{%" with "%}}" */
    uint8_t close = s[at + 2] == '<' ? '>' : '%';
    for (size_t i = at + 3; i + 3 <= n; i++) {
        if (s[i] == close && s[i + 1] == '}' && s[i + 2] == '}') {
            *arg = at + 3;
            *arg_len = i - *arg;
            *end = i + 3;
            return true;
        }
    }
    return false;
}

static void render_inline(md *r, const uint8_t *s, size_t n) {
    size_t i = 0;
    size_t plain = 0;
    while (i < n) {
        uint8_t b = s[i];
        if (b == '\\' && i + 1 < n) {
            r->emit(r->user, s + plain, i - plain);
            i++;
            plain = i;
            i++;
            continue;
        }
        if (b == '`') {
            size_t close = find_close(s, n, i + 1, '`', 1);
            if (close < n) {
                r->emit(r->user, s + plain, i - plain);
                r->code = true;
                style(r);
                r->emit(r->user, s + i + 1, close - i - 1);
                r->code = false;
                style(r);
                i = close + 1;
                plain = i;
                continue;
            }
        }
        if ((b == '*' || b == '_') && i + 1 < n) {
            size_t mark = s[i + 1] == b ? 2 : 1;
            size_t close = find_close(s, n, i + mark, b, mark);
            bool ok = false;
            if (close < n && close > i + mark && s[i + mark] != ' ') {
                ok = true;
            }
            if (ok && b == '_' && !flanks(s, n, i, close, mark)) {
                ok = false; /* snake_case is an identifier, not emphasis */
            }
            if (ok) {
                r->emit(r->user, s + plain, i - plain);
                bool *flag = mark == 2 ? &r->bold : &r->italic;
                *flag = true;
                style(r);
                render_inline(r, s + i + mark, close - i - mark);
                *flag = false;
                style(r);
                i = close + mark;
                plain = i;
                continue;
            }
        }
        if (b == '{' && i + 2 < n && s[i + 1] == '{' && (s[i + 2] == '<' || s[i + 2] == '%')) {
            size_t arg = 0;
            size_t arg_len = 0;
            size_t end = 0;
            if (shortcode_span(s, n, i, &arg, &arg_len, &end)) {
                r->emit(r->user, s + plain, i - plain);
                render_shortcode(r, s + arg, arg_len);
                i = end;
                plain = i;
                continue;
            }
        }
        md_link l;
        bool image = false;
        if (b == '!' && i + 1 < n && s[i + 1] == '[') {
            image = true;
        }
        if ((b == '[' || image) && link_parse(s, n, image ? i + 1 : i, &l)) {
            r->emit(r->user, s + plain, i - plain);
            render_link(r, s, &l, image);
            i = l.url_end + 1;
            plain = i;
            continue;
        }
        i++;
    }
    r->emit(r->user, s + plain, n - plain);
}

/* ---- blocks ---- */

static size_t indent_of(const uint8_t *s, size_t n, size_t max) {
    size_t i = 0;
    while (i < n && i < max && s[i] == ' ') {
        i++;
    }
    return i;
}

/* A fence is three or more of the same marker; the rest is the info string. */
static bool fence_at(const uint8_t *s, size_t n, char *ch, size_t *len) {
    if (n < 3 || (s[0] != '`' && s[0] != '~')) {
        return false;
    }
    size_t i = 0;
    while (i < n && s[i] == s[0]) {
        i++;
    }
    if (i < 3) {
        return false;
    }
    *ch = (char)s[0];
    *len = i;
    return true;
}

static bool rule_at(const uint8_t *s, size_t n) {
    if (n < 3) {
        return false;
    }
    uint8_t c = s[0];
    if (c != '-' && c != '*' && c != '_') {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (s[i] != c) {
            return false;
        }
    }
    return true;
}

static void put_rule(const md *r) {
    md_puts(r, "\x1b[0;2m");
    for (size_t i = 0; i < MD_RULE_WIDTH; i++) {
        md_puts(r, "\xe2\x94\x80"); /* ─ */
    }
    md_puts(r, "\x1b[0m");
}

/* Emits the marker of a list item and returns the offset of its text, or 0
   when the line is not a list item. */
static size_t list_marker(md *r, const uint8_t *s, size_t n, size_t ind) {
    size_t i = ind;
    if (i + 1 < n && (s[i] == '-' || s[i] == '*' || s[i] == '+') && s[i + 1] == ' ') {
        md_indent(r, ind);
        md_puts(r, "\x1b[0;36m\xe2\x80\xa2\x1b[0m "); /* • */
        return i + 2;
    }
    size_t digits = i;
    while (digits < n && s[digits] >= '0' && s[digits] <= '9') {
        digits++;
    }
    if (digits > i && digits + 1 < n && s[digits] == '.' && s[digits + 1] == ' ') {
        md_indent(r, ind);
        md_puts(r, "\x1b[0;36m");
        r->emit(r->user, s + i, digits - i);
        md_puts(r, ".\x1b[0m ");
        return digits + 2;
    }
    return 0;
}

/* Decides the block for a fresh line and emits its prefix; returns the
   offset where the inline content starts. */
static size_t open_block(md *r, const uint8_t *s, size_t n) {
    r->block = MD_B_PARA;
    r->heading = 0;
    size_t ind = indent_of(s, n, 8);
    if (n - ind >= 1 && s[ind] == '#') {
        size_t h = ind;
        while (h < n && s[h] == '#' && h - ind < 6) {
            h++;
        }
        if (h < n && s[h] == ' ') {
            r->block = MD_B_HEADING;
            r->heading = (uint8_t)(h - ind);
            style(r);
            return h + 1;
        }
    }
    if (n - ind >= 1 && s[ind] == '>') {
        r->block = MD_B_QUOTE;
        md_puts(r, "\x1b[0;2m\xe2\x94\x82\x1b[0m "); /* │ */
        style(r);
        size_t t = ind + 1;
        if (t < n && s[t] == ' ') {
            t++;
        }
        return t;
    }
    size_t text = list_marker(r, s, n, ind);
    if (text > 0) {
        style(r);
        return text;
    }
    style(r);
    return 0;
}

static void end_line(md *r) {
    md_puts(r, "\x1b[0m");
    r->bold = false;
    r->italic = false;
    r->code = false;
    r->link = false;
    r->block = MD_B_PARA;
    r->heading = 0;
    r->line_open = false;
}

static void finish_line(md *r, bool newline) {
    const uint8_t *s = r->line;
    size_t n = r->line_len;
    r->line_len = 0;

    if (r->source) {
        hl_line(&r->hl, s, n, r->emit, r->user);
        if (newline) {
            md_puts(r, "\n");
        }
        return;
    }

    /* a fence may be indented, and inside a list item usually is */
    size_t find = indent_of(s, n, 8);
    if (r->in_code) {
        char ch = 0;
        size_t len = 0;
        if (fence_at(s + find, n - find, &ch, &len) && ch == r->fence_ch && len >= r->fence_len) {
            r->in_code = false;
            return; /* the fence marker takes no row of its own */
        }
        hl_line(&r->hl, s, n, r->emit, r->user);
        if (newline) {
            md_puts(r, "\n");
        }
        return;
    }

    char ch = 0;
    size_t len = 0;
    if (!r->line_open && fence_at(s + find, n - find, &ch, &len)) {
        char info[24];
        size_t k = 0;
        size_t i = find + len;
        /* indent_of stops at n and fence_at counts inside its own length, so
           find + len <= n; the analyzer cannot connect the two bounds. */
        /* NOLINTNEXTLINE(clang-analyzer-security.ArrayBound) */
        while (i < n && s[i] != ' ' && k + 1 < sizeof(info)) {
            info[k] = (char)s[i];
            k++;
            i++;
        }
        info[k] = '\0';
        r->in_code = true;
        r->fence_ch = ch;
        r->fence_len = len;
        hl_begin(&r->hl, hl_lang_of(info));
        return;
    }

    if (n == 0 && !r->line_open) {
        md_puts(r, "\n");
        return;
    }
    size_t text = 0;
    if (!r->line_open) {
        if (rule_at(s, n)) {
            put_rule(r);
            if (newline) {
                md_puts(r, "\n");
            }
            return;
        }
        text = open_block(r, s, n);
        r->line_open = true;
    }
    render_inline(r, s + text, n - text);
    if (!newline) {
        return; /* the line is longer than the buffer: it continues */
    }
    end_line(r);
    md_puts(r, "\n");
}

/* ---- stream ---- */

void md_reset(md *r, bool enabled) {
    hl_emit emit = r->emit;
    void *user = r->user;
    const char *base = r->base;
    md_slug_fn slug = r->slug;
    memset(r, 0, sizeof(*r));
    r->enabled = enabled;
    r->emit = emit;
    r->user = user;
    r->base = base != NULL ? base : "";
    r->slug = slug;
}

void md_source(md *r, hl_lang lang) {
    md_reset(r, false);
    if (lang == HL_NONE) {
        return;
    }
    r->source = true;
    hl_begin(&r->hl, lang);
}

void md_feed(md *r, const uint8_t *data, size_t n) {
    if (!r->enabled && !r->source) {
        r->emit(r->user, data, n);
        return;
    }
    for (size_t i = 0; i < n; i++) {
        uint8_t b = data[i];
        /* the source never drives the terminal: only this renderer emits
           escapes, so anything escape-shaped in the file is dropped */
        if (r->esc == 1) {
            r->esc = 0;
            if (b == '[') {
                r->esc = 2;
            }
            if (b == ']' || b == 'P' || b == '^' || b == '_') {
                r->esc = 3;
            }
            continue;
        }
        if (r->esc == 2) {
            if (b >= 0x40 && b <= 0x7E) {
                r->esc = 0;
            }
            continue;
        }
        if (r->esc == 3) {
            /* a control string runs to BEL or to the ST that ends it */
            if (b == 0x07 || b == '\\') {
                r->esc = 0;
            }
            continue;
        }
        if (b == 0x1B) {
            r->esc = 1;
            continue;
        }
        if (b == '\r') {
            continue;
        }
        if (b == '\n') {
            finish_line(r, true);
            continue;
        }
        if (r->line_len >= MD_LINE_MAX) {
            finish_line(r, false); /* flush and keep going on the same line */
        }
        r->line[r->line_len] = b;
        r->line_len++;
    }
}

void md_end(md *r) {
    if (!r->enabled && !r->source) {
        return;
    }
    if (r->line_len > 0 || r->line_open) {
        finish_line(r, true); /* every line closes its own style, so there is
                                 nothing left to reset here */
    }
}
