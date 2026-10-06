#include <string.h>

#include "pager.h"

#include "keys.h"
#include "utf8.h"

void pager_reset(pager *p, const char *name) {
    p->text_len = 0;
    p->esc = 0;
    p->truncated = false;
    p->nlines = 0;
    p->nrows = 0;
    p->top = 0;
    p->patlen = 0;
    p->typing = false;
    p->not_found = false;
    size_t n = strlen(name);
    if (n >= sizeof(p->name)) {
        n = sizeof(p->name) - 1;
    }
    memcpy(p->name, name, n);
    p->name[n] = '\0';
}

static void load_byte(pager *p, uint8_t b) {
    if (p->text_len >= PAGER_TEXT_CAP) {
        p->truncated = true;
        return;
    }
    p->text[p->text_len] = b;
    p->text_len++;
}

/* Escape states: 1 just after ESC, 2 inside a CSI, 3 inside an OSC string. */
enum {
    PG_ESC_NONE = 0,
    PG_ESC_ESC,
    PG_ESC_CSI,
    PG_ESC_OSC,
};

void pager_load(pager *p, const uint8_t *data, size_t n) {
    size_t i = 0;
    while (i < n) {
        uint8_t b = data[i];
        i++;
        if (p->esc != PG_ESC_NONE) {
            load_byte(p, b);
            if (p->esc == PG_ESC_ESC) {
                p->esc = PG_ESC_NONE;
                if (b == '[') {
                    p->esc = PG_ESC_CSI;
                }
                if (b == ']') {
                    p->esc = PG_ESC_OSC;
                }
                continue;
            }
            if (p->esc == PG_ESC_CSI && b >= 0x40 && b <= 0x7E) {
                p->esc = PG_ESC_NONE;
            }
            /* an OSC ends at BEL or at the ST that follows its own ESC */
            if (p->esc == PG_ESC_OSC && (b == 0x07 || b == '\\')) {
                p->esc = PG_ESC_NONE;
            }
            continue;
        }
        if (b == 0x1B) {
            p->esc = PG_ESC_ESC;
            load_byte(p, b);
            continue;
        }
        if (b == '\t') {
            size_t k = 0;
            while (k < PAGER_TAB_WIDTH) {
                load_byte(p, ' ');
                k++;
            }
            continue;
        }
        if (b == '\n' || (b >= 0x20 && b != 0x7F)) {
            load_byte(p, b);
        }
        /* \r, DEL and other control bytes are dropped */
    }
}

/* ---- wrapping ---- */

static void emit_row(pager *p, size_t off, size_t len, size_t sgr, size_t link, size_t indent) {
    if (p->nrows >= PAGER_ROWS_MAX) {
        p->truncated = true;
        return;
    }
    p->rows[p->nrows].off = (uint32_t)off;
    p->rows[p->nrows].len = (uint16_t)len;
    p->rows[p->nrows].sgr = (uint32_t)sgr;
    p->rows[p->nrows].link = (uint32_t)link;
    p->rows[p->nrows].indent = (uint16_t)indent;
    p->nrows++;
}

/* Length of the escape sequence at off, or 0 when there is none. */
static size_t esc_len(const pager *p, size_t off) {
    if (off >= p->text_len || p->text[off] != 0x1B) {
        return 0;
    }
    size_t i = off + 1;
    if (i < p->text_len && p->text[i] == '[') {
        i++;
        while (i < p->text_len && (p->text[i] < 0x40 || p->text[i] > 0x7E)) {
            i++;
        }
        return i < p->text_len ? i + 1 - off : p->text_len - off;
    }
    if (i < p->text_len && p->text[i] == ']') {
        i++;
        while (i < p->text_len && p->text[i] != 0x07 && p->text[i] != '\\') {
            i++;
        }
        return i < p->text_len ? i + 1 - off : p->text_len - off;
    }
    return i < p->text_len ? 2 : 1;
}

static size_t skip_escapes(const pager *p, size_t i, size_t end) {
    while (i < end && esc_len(p, i) > 0) {
        i += esc_len(p, i);
    }
    return i;
}

/* The column a wrapped line's next rows start at: under its first text, so
   past the leading blanks and, in a list item, past the marker ("• ",
   "12. "). Escapes take no columns. */
static size_t hang_of(const pager *p, size_t i, size_t end) {
    size_t lead = 0;
    for (i = skip_escapes(p, i, end); i < end && p->text[i] == ' ';
         i = skip_escapes(p, i + 1, end)) {
        lead++;
    }
    size_t mark = 0;
    if (i + 3 <= end && memcmp(p->text + i, "\xe2\x80\xa2", 3) == 0) {
        mark = 1;
        i += 3;
    } else {
        size_t d = 0;
        while (d < 4 && i + d < end && p->text[i + d] >= '0' && p->text[i + d] <= '9') {
            d++;
        }
        if (d > 0 && i + d < end && (p->text[i + d] == '.' || p->text[i + d] == ')')) {
            mark = d + 1;
            i += d + 1;
        }
    }
    i = skip_escapes(p, i, end);
    if (mark > 0 && i < end && p->text[i] == ' ') {
        return lead + mark + 1;
    }
    return lead;
}

/* Wraps one logical line [off, end) into display rows of at most cols
   columns, preferring to break after a space; the rows after the first
   hang at hang_of, unless that takes half the width. */
static size_t wrap_line(pager *p, size_t off, size_t cols) {
    size_t row_start = off;
    size_t w = 0;
    size_t break_at = 0; /* byte offset just after the last space */
    size_t break_w = 0;
    size_t sgr = p->sgr;   /* style in effect at row_start, offset + 1 */
    size_t link = p->link; /* open hyperlink at row_start, offset + 1 */
    utf8_dec d;
    utf8_dec_init(&d);
    size_t i = off;
    size_t rune_start = off;
    /* load_byte never fills past the cap; carrying that bound here keeps the
       scan provably inside the buffer even as escapes advance i in jumps */
    size_t end = p->text_len < PAGER_TEXT_CAP ? p->text_len : PAGER_TEXT_CAP;
    size_t hang = hang_of(p, off, end);
    if (hang * 2 > cols) {
        hang = 0;
    }
    size_t indent = 0; /* the first row starts at the margin */
    while (i < end && p->text[i] != '\n') {
        size_t el = esc_len(p, i);
        if (el > 0) {
            /* escapes take no columns, but they do change the state a
               wrapped row has to restore */
            uint8_t kind = i + 1 < p->text_len ? p->text[i + 1] : 0;
            if (kind == '[') {
                p->sgr = i + 1;
            }
            if (kind == ']') {
                /* ESC ] 8 ; ; URL ST opens; the same with no URL closes */
                bool empty = true;
                if (el > 6 && i + 5 < p->text_len) {
                    uint8_t first = p->text[i + 5];
                    if (first != 0x1B && first != 0x07) {
                        empty = false;
                    }
                }
                p->link = 0;
                /* esc_len returns more than 6 for an OSC 8 carrying a URL, so
                   this branch is live; test_md_pager_wraps_styles asserts it.
                   cppcheck's value flow does not reach through esc_len. */
                /* cppcheck-suppress knownConditionTrueFalse */
                if (!empty) {
                    p->link = i + 1;
                }
            }
            i += el;
            if (i > end) {
                i = end; /* an unterminated escape ends with the text */
            }
            continue;
        }
        uint32_t cp = 0;
        int resync = 0;
        if (d.need == 0) {
            rune_start = i; /* this byte starts a new rune */
        }
        utf8_result r = utf8_dec_feed(&d, p->text[i], &cp, &resync);
        i++;
        if (r == UTF8_MORE) {
            continue;
        }
        if (r == UTF8_ERROR) {
            cp = UTF8_REPLACEMENT;
            if (resync) {
                i--; /* the bad sequence ends here; re-feed this byte */
            }
        }
        size_t cw = (size_t)utf8_width(cp);
        if (w + cw > cols && w > 0) {
            if (break_at > row_start) {
                emit_row(p, row_start, break_at - row_start, sgr, link, indent);
                row_start = break_at;
                w = w - break_w + hang;
            } else {
                emit_row(p, row_start, rune_start - row_start, sgr, link, indent);
                row_start = rune_start;
                w = hang;
            }
            indent = hang;
            sgr = p->sgr;
            link = p->link;
            break_at = 0;
            break_w = 0;
        }
        w += cw;
        if (cp == ' ') {
            break_at = i;
            break_w = w;
        }
    }
    emit_row(p, row_start, i - row_start, sgr, link, indent);
    if (i < p->text_len) {
        i++; /* skip the \n */
    }
    return i;
}

static void pager_wrap(pager *p, const term *t) {
    size_t cols = t->cols > 2 ? t->cols : 2;
    p->nrows = 0;
    p->sgr = 0;
    p->link = 0;
    size_t i = 0;
    if (p->text_len == 0) {
        emit_row(p, 0, 0, 0, 0, 0);
        return;
    }
    while (i < p->text_len && p->nrows < PAGER_ROWS_MAX) {
        i = wrap_line(p, i, cols);
    }
}

/* ---- painting ---- */

static size_t visible_rows(const term *t) {
    return t->rows > 1 ? (size_t)t->rows - 1 : 1;
}

static size_t max_top(const pager *p, const term *t) {
    size_t vis = visible_rows(t);
    return p->nrows > vis ? p->nrows - vis : 0;
}
/* ---- looking for text ---- */

static uint8_t fold(uint8_t b) {
    if (b >= 'A' && b <= 'Z') {
        return (uint8_t)(b + 32);
    }
    return b;
}

/* Lower case matches either case; a capital, only itself (less -i). */
static bool folds(const pager *p) {
    for (size_t i = 0; i < p->patlen; i++) {
        if (p->pat[i] >= 'A' && p->pat[i] <= 'Z') {
            return false;
        }
    }
    return true;
}

/* A row's text without its escapes, in dst (cap bytes): where each byte of
   it came from goes in at, so the painter can find a match again. */
static size_t row_plain(const pager *p, const pager_row *row, uint8_t *dst, uint32_t *at,
                        size_t cap) {
    size_t n = 0;
    size_t i = row->off;
    size_t end = row->off + row->len;
    while (i < end && n < cap) {
        size_t el = esc_len(p, i);
        if (el > 0) {
            i += el;
            continue;
        }
        dst[n] = p->text[i];
        at[n] = (uint32_t)i;
        n++;
        i++;
    }
    return n;
}

/* Where the pattern is in plain from start, or n when it is not. */
static size_t find_in(const pager *p, const uint8_t *plain, size_t n, size_t start) {
    bool f = folds(p);
    for (size_t i = start; p->patlen > 0 && i + p->patlen <= n; i++) {
        size_t k = 0;
        while (k < p->patlen) {
            uint8_t a = plain[i + k];
            uint8_t b = p->pat[k];
            if (f) {
                a = fold(a);
            }
            if (a != b) {
                break;
            }
            k++;
        }
        if (k == p->patlen) {
            return i;
        }
    }
    return n;
}

static uint8_t plain_buf[4 * TERM_COLS_MAX];
static uint32_t plain_at[4 * TERM_COLS_MAX];

static bool row_has(const pager *p, size_t r) {
    size_t n = row_plain(p, &p->rows[r], plain_buf, plain_at, sizeof(plain_buf));
    return find_in(p, plain_buf, n, 0) < n;
}

/* A row's bytes, the pattern reversed wherever it is. */
static void write_row(const pager *p, term *t, const pager_row *row) {
    size_t n = 0;
    if (p->patlen > 0 && !p->typing) {
        n = row_plain(p, row, plain_buf, plain_at, sizeof(plain_buf));
    }
    size_t from = row->off; /* written up to here */
    size_t k = find_in(p, plain_buf, n, 0);
    while (k < n) {
        size_t a = plain_at[k];
        size_t b = plain_at[k + p->patlen - 1] + 1;
        term_write(t, p->text + from, a - from);
        term_puts(t, "\x1b[7m");
        term_write(t, p->text + a, b - a);
        term_puts(t, "\x1b[27m");
        from = b;
        k = find_in(p, plain_buf, n, k + p->patlen);
    }
    term_write(t, p->text + from, row->off + row->len - from);
}

static void paint_status(const pager *p, term *t) {
    if (p->typing || p->not_found) {
        term_puts(t, "\x1b[7m ");
        if (p->typing) {
            term_puts(t, "/");
            term_write(t, p->pat, p->patlen);
        } else {
            term_puts(t, "Pattern not found");
        }
        term_puts(t, " \x1b[27m\x1b[K");
        return;
    }
    size_t vis = visible_rows(t);
    size_t bottom = p->top + vis;
    if (bottom > p->nrows) {
        bottom = p->nrows;
    }
    uint32_t pct = p->nrows > 0 ? (uint32_t)((bottom * 100) / p->nrows) : 100;
    term_puts(t, "\x1b[7m ");
    size_t used = 1;
    size_t cols = (size_t)t->cols;
    /* name, truncated to leave room for the fixed tail */
    size_t tail = 24; /* " 100% · q quit " and slack */
    if (cols > tail + 2) {
        const char *name = p->name;
        size_t nw = utf8_swidth(name);
        size_t maxn = cols - tail;
        if (nw > maxn) {
            /* show the end of the path: skip whole runes from the front */
            size_t skip = 0;
            utf8_dec d;
            utf8_dec_init(&d);
            size_t i = 0;
            while (name[i] != '\0' && nw > maxn) {
                uint32_t cp = 0;
                int resync = 0;
                utf8_result r = utf8_dec_feed(&d, (uint8_t)name[i], &cp, &resync);
                i++;
                if (r == UTF8_RUNE) {
                    nw -= (size_t)utf8_width(cp);
                    skip = i;
                }
                if (r == UTF8_ERROR) {
                    nw--;
                    skip = i;
                }
            }
            name += skip;
        }
        term_puts(t, name);
        used += nw;
    }
    term_puts(t, "  ");
    term_put_u32(t, pct);
    term_puts(t, "%");
    used += 3; /* two spaces + '%' */
    used += 1;
    if (pct >= 10) {
        used += 1;
    }
    if (pct >= 100) {
        used += 1;
    }
    if (p->truncated) {
        term_puts(t, " [truncated]");
        used += 12;
    }
    size_t hint_w = 9;
    if (cols > used + hint_w + 1) {
        term_puts(t, " \xc2\xb7 q quit");
        used += hint_w;
    }
    while (used + 1 < cols) {
        term_puts(t, " ");
        used++;
    }
    term_puts(t, "\x1b[27m\x1b[K");
}

static void paint(const pager *p, term *t) {
    size_t vis = visible_rows(t);
    term_puts(t, "\x1b[H");
    size_t i = 0;
    while (i < vis) {
        size_t idx = p->top + i;
        if (idx < p->nrows) {
            const pager_row *row = &p->rows[idx];
            term_puts(t, "\x1b[0m");
            term_put_spaces(t, row->indent);
            if (row->link > 0) {
                term_write(t, p->text + row->link - 1, esc_len(p, row->link - 1));
            }
            if (row->sgr > 0) {
                term_write(t, p->text + row->sgr - 1, esc_len(p, row->sgr - 1));
            }
            write_row(p, t, row);
            /* unconditional: a row that opened a link without closing it
               would otherwise turn the rest of the screen into that link */
            term_puts(t, "\x1b]8;;\x1b\\\x1b[0m");
        }
        term_puts(t, "\x1b[K\r\n");
        i++;
    }
    paint_status(p, t);
}

/* ---- the reader's keys ---- */

void pager_show(pager *p, term *t) {
    pager_wrap(p, t);
    p->top = 0;
    term_puts(t, "\x1b[?1007h\x1b[?25l"); /* wheel-as-arrows, hidden cursor */
    paint(p, t);
}

void pager_hide(term *t) {
    term_puts(t, "\x1b[?25h\x1b[?1007l");
}

static void scroll_to(pager *p, term *t, size_t top) {
    size_t mt = max_top(p, t);
    if (top > mt) {
        top = mt;
    }
    if (top == p->top) {
        return;
    }
    p->top = top;
    paint(p, t);
}

/* The first row from r on (down, or up) with the pattern, at the top; the
   status says so when there is none. */
static void look(pager *p, term *t, size_t r, bool down) {
    while (r < p->nrows) {
        if (row_has(p, r)) {
            p->top = r > max_top(p, t) ? max_top(p, t) : r;
            paint(p, t);
            return;
        }
        if (!down && r == 0) {
            break;
        }
        r = down ? r + 1 : r - 1;
    }
    p->not_found = true;
    paint(p, t);
}

/* A key while the pattern is typed after /. */
static void typing_key(pager *p, term *t, uint32_t cp) {
    if (cp == FT_KEY_ESC || cp == 0x03) {
        p->typing = false;
        p->patlen = 0;
    } else if (cp == '\r' || cp == '\n') {
        p->typing = false;
        if (p->patlen > 0) {
            look(p, t, p->top + 1 < p->nrows ? p->top + 1 : p->top, true);
            return;
        }
    } else if (cp == 0x7f || cp == 0x08) {
        while (p->patlen > 0 && (p->pat[p->patlen - 1] & 0xC0U) == 0x80U) {
            p->patlen--; /* the rest of a rune */
        }
        if (p->patlen > 0) {
            p->patlen--;
        }
    } else if (cp >= 0x20 && !ft_key_is_code(cp)) {
        uint8_t b[UTF8_MAX_BYTES];
        size_t k = utf8_encode(b, cp);
        if (k <= sizeof(p->pat) - p->patlen) {
            memcpy(p->pat + p->patlen, b, k);
            p->patlen += k;
        }
    }
    paint(p, t);
}

bool pager_key(pager *p, term *t, uint32_t cp) {
    size_t vis = visible_rows(t);
    size_t page = vis > 1 ? vis - 1 : 1;
    if (p->typing) {
        typing_key(p, t, cp);
        return true;
    }
    if (p->not_found) {
        p->not_found = false; /* the message goes; the key still counts, as in less */
        paint(p, t);
    }
    switch (cp) {
    case '/':
        p->typing = true;
        p->patlen = 0;
        paint(p, t);
        break;
    case 'n':
        if (p->patlen > 0) {
            look(p, t, p->top + 1, true);
        }
        break;
    case 'N':
        if (p->patlen > 0 && p->top > 0) {
            look(p, t, p->top - 1, false);
        }
        break;
    case 'q':
    case FT_KEY_ESC:
        return false;
    case 'j':
    case '\r':
    case '\n':
    case FT_KEY_DOWN:
        scroll_to(p, t, p->top + 1);
        break;
    case 'k':
    case FT_KEY_UP:
        scroll_to(p, t, p->top > 0 ? p->top - 1 : 0);
        break;
    case ' ':
    case 'f':
    case FT_KEY_PGDN:
        scroll_to(p, t, p->top + page);
        break;
    case 'b':
    case FT_KEY_PGUP:
        scroll_to(p, t, p->top > page ? p->top - page : 0);
        break;
    case 'd':
        scroll_to(p, t, p->top + (page / 2));
        break;
    case 'u':
        scroll_to(p, t, p->top > (page / 2) ? p->top - (page / 2) : 0);
        break;
    case 'g':
    case FT_KEY_HOME:
        scroll_to(p, t, 0);
        break;
    case 'G':
    case FT_KEY_END:
        scroll_to(p, t, max_top(p, t));
        break;
    default:
        break;
    }
    return true;
}

void pager_resize(pager *p, term *t) {
    pager_wrap(p, t);
    if (p->top > max_top(p, t)) {
        p->top = max_top(p, t);
    }
    paint(p, t);
}
