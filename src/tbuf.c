#include "tbuf.h"

#include <string.h>

#include "utf8.h"

static void reindex(tbuf *t) {
    t->nlines = 1;
    t->lines[0] = 0;
    size_t i = 0;
    while (i < t->len && t->nlines < TB_LINES_MAX) {
        if (t->text[i] == '\n') {
            t->lines[t->nlines] = i + 1;
            t->nlines++;
        }
        i++;
    }
}

void tb_init(tbuf *t) {
    t->len = 0;
    t->cur = 0;
    t->goal = 0;
    t->anchored = false;
    t->anchor = 0;
    t->clip_len = 0;
    t->top = 0;
    t->left = 0;
    t->dirty = false;
    t->changed = 0;
    reindex(t);
}

static void touched(tbuf *t, size_t off) {
    if (off < t->changed) {
        t->changed = off;
    }
}

/* Whether data fits the buffer, its bytes and its lines. */
static bool fits(const uint8_t *data, size_t n) {
    if (n > TB_CAP) {
        return false;
    }
    size_t nl = 1;
    size_t i = 0;
    while (i < n) {
        if (data[i] == '\n') {
            nl++;
        }
        i++;
    }
    return nl <= TB_LINES_MAX;
}

bool tb_load(tbuf *t, const uint8_t *data, size_t n) {
    if (!fits(data, n)) {
        return false;
    }
    tb_init(t);
    memcpy(t->text, data, n);
    t->len = n;
    reindex(t);
    return true;
}

static size_t line_end(const tbuf *t, size_t i) {
    return i + 1 < t->nlines ? t->lines[i + 1] - 1 : t->len;
}

size_t tb_line_of(const tbuf *t, size_t off) {
    size_t lo = 0;
    size_t hi = t->nlines;
    while (hi - lo > 1) {
        size_t mid = lo + ((hi - lo) / 2);
        if (t->lines[mid] <= off) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}

void tb_line(const tbuf *t, size_t i, const uint8_t **p, size_t *n) {
    if (i >= t->nlines) {
        *p = t->text;
        *n = 0;
        return;
    }
    *p = t->text + t->lines[i];
    *n = line_end(t, i) - t->lines[i];
}

/* One rune at off: its code point and byte length (1 for a bad byte). */
static size_t rune_at(const tbuf *t, size_t off, uint32_t *cp) {
    utf8_dec d;
    utf8_dec_init(&d);
    size_t k = 0;
    while (off + k < t->len && k < 4) {
        int resync = 0;
        utf8_result r = utf8_dec_feed(&d, t->text[off + k], cp, &resync);
        k++;
        if (r == UTF8_RUNE) {
            return k;
        }
        if (r == UTF8_ERROR) {
            break;
        }
    }
    *cp = 0xFFFD;
    return 1;
}

static size_t col_advance(size_t col, uint32_t cp) {
    if (cp == '\t') {
        return col + (TB_TAB - (col % TB_TAB));
    }
    int w = utf8_width(cp);
    return col + (size_t)(w > 0 ? w : 1);
}

size_t tb_col_of(const tbuf *t, size_t off) {
    size_t i = tb_line_of(t, off);
    size_t at = t->lines[i];
    size_t col = 0;
    while (at < off) {
        uint32_t cp = 0;
        size_t k = rune_at(t, at, &cp);
        col = col_advance(col, cp);
        at += k;
    }
    return col;
}

size_t tb_off_at(const tbuf *t, size_t i, size_t col) {
    if (i >= t->nlines) {
        return t->len;
    }
    size_t at = t->lines[i];
    size_t end = line_end(t, i);
    size_t c = 0;
    while (at < end) {
        uint32_t cp = 0;
        size_t k = rune_at(t, at, &cp);
        size_t next = col_advance(c, cp);
        if (next > col) {
            break;
        }
        c = next;
        at += k;
    }
    return at;
}

static void set_cursor(tbuf *t, size_t off, bool select, bool keep_goal) {
    if (select && !t->anchored) {
        t->anchored = true;
        t->anchor = t->cur;
    }
    if (!select) {
        t->anchored = false;
    }
    t->cur = off;
    if (!keep_goal) {
        t->goal = tb_col_of(t, off);
    }
}

bool tb_selection(const tbuf *t, size_t *a, size_t *b) {
    if (!t->anchored || t->anchor == t->cur) {
        return false;
    }
    *a = t->anchor < t->cur ? t->anchor : t->cur;
    *b = t->anchor < t->cur ? t->cur : t->anchor;
    return true;
}

static void erase(tbuf *t, size_t a, size_t b) {
    touched(t, a);
    memmove(t->text + a, t->text + b, t->len - b);
    t->len -= b - a;
    t->cur = a;
    t->anchored = false;
    t->dirty = true;
    reindex(t);
}

bool tb_insert(tbuf *t, const uint8_t *bytes, size_t n) {
    size_t a = 0;
    size_t b = 0;
    if (tb_selection(t, &a, &b)) {
        erase(t, a, b);
    }
    if (n > TB_CAP - t->len) {
        return false;
    }
    size_t nl = 0;
    size_t i = 0;
    while (i < n) {
        if (bytes[i] == '\n') {
            nl++;
        }
        i++;
    }
    if (t->nlines + nl > TB_LINES_MAX) {
        return false;
    }
    touched(t, t->cur);
    memmove(t->text + t->cur + n, t->text + t->cur, t->len - t->cur);
    memcpy(t->text + t->cur, bytes, n);
    t->len += n;
    t->cur += n;
    t->anchored = false;
    t->dirty = true;
    reindex(t);
    t->goal = tb_col_of(t, t->cur);
    return true;
}

void tb_backspace(tbuf *t) {
    size_t a = 0;
    size_t b = 0;
    if (tb_selection(t, &a, &b)) {
        erase(t, a, b);
    } else if (t->cur > 0) {
        size_t k = utf8_last_rune_len(t->text, t->cur);
        erase(t, t->cur - (k > 0 ? k : 1), t->cur);
    }
    t->goal = tb_col_of(t, t->cur);
}

void tb_delete(tbuf *t) {
    size_t a = 0;
    size_t b = 0;
    if (tb_selection(t, &a, &b)) {
        erase(t, a, b);
    } else if (t->cur < t->len) {
        uint32_t cp = 0;
        size_t k = rune_at(t, t->cur, &cp);
        erase(t, t->cur, t->cur + k);
    }
    t->goal = tb_col_of(t, t->cur);
}

void tb_delete_line(tbuf *t) {
    size_t i = tb_line_of(t, t->cur);
    size_t a = t->lines[i];
    size_t b = i + 1 < t->nlines ? t->lines[i + 1] : t->len;
    if (b == a && i > 0 && a == t->len) {
        a--; /* the empty last line: take the newline before it */
    }
    erase(t, a, b);
    if (t->cur > t->len) {
        t->cur = t->len;
    }
    t->cur = tb_off_at(t, tb_line_of(t, t->cur), t->goal);
}

void tb_left(tbuf *t, bool select) {
    if (t->cur == 0) {
        set_cursor(t, 0, select, false);
        return;
    }
    size_t k = utf8_last_rune_len(t->text, t->cur);
    set_cursor(t, t->cur - (k > 0 ? k : 1), select, false);
}

void tb_right(tbuf *t, bool select) {
    if (t->cur >= t->len) {
        set_cursor(t, t->len, select, false);
        return;
    }
    uint32_t cp = 0;
    size_t k = rune_at(t, t->cur, &cp);
    set_cursor(t, t->cur + k, select, false);
}

void tb_up(tbuf *t, size_t n, bool select) {
    size_t i = tb_line_of(t, t->cur);
    size_t to = i > n ? i - n : 0;
    set_cursor(t, tb_off_at(t, to, t->goal), select, true);
}

void tb_down(tbuf *t, size_t n, bool select) {
    size_t i = tb_line_of(t, t->cur);
    size_t to = i + n < t->nlines ? i + n : t->nlines - 1;
    set_cursor(t, tb_off_at(t, to, t->goal), select, true);
}

void tb_home(tbuf *t, bool select) {
    set_cursor(t, t->lines[tb_line_of(t, t->cur)], select, false);
}

void tb_end(tbuf *t, bool select) {
    set_cursor(t, line_end(t, tb_line_of(t, t->cur)), select, false);
}

void tb_doc_home(tbuf *t, bool select) {
    set_cursor(t, 0, select, false);
}

void tb_doc_end(tbuf *t, bool select) {
    set_cursor(t, t->len, select, false);
}

void tb_goto_line(tbuf *t, size_t i) {
    if (i >= t->nlines) {
        i = t->nlines - 1;
    }
    set_cursor(t, t->lines[i], false, false);
}

void tb_copy(tbuf *t) {
    size_t a = 0;
    size_t b = 0;
    if (!tb_selection(t, &a, &b)) {
        return;
    }
    size_t n = b - a;
    if (n > TB_CLIP) {
        n = TB_CLIP; /* what fits; the selection stays for a smaller cut */
    }
    memcpy(t->clip, t->text + a, n);
    t->clip_len = n;
}

void tb_cut(tbuf *t) {
    size_t a = 0;
    size_t b = 0;
    if (!tb_selection(t, &a, &b) || b - a > TB_CLIP) {
        return;
    }
    tb_copy(t);
    erase(t, a, b);
    t->goal = tb_col_of(t, t->cur);
}

bool tb_paste(tbuf *t) {
    if (t->clip_len == 0) {
        return false;
    }
    return tb_insert(t, t->clip, t->clip_len);
}

void tb_view(tbuf *t, size_t rows, size_t cols) {
    if (rows == 0 || cols == 0) {
        return;
    }
    size_t line = tb_line_of(t, t->cur);
    size_t col = tb_col_of(t, t->cur);
    if (line < t->top) {
        t->top = line;
    } else if (line >= t->top + rows) {
        t->top = line - rows + 1;
    }
    if (col < t->left) {
        t->left = col;
    } else if (col >= t->left + cols) {
        t->left = col - cols + 1;
    }
}

bool tb_find(tbuf *t, const uint8_t *needle, size_t n) {
    if (n == 0 || n > t->len) {
        return false;
    }
    size_t from = t->cur;
    size_t a = 0;
    size_t b = 0;
    if (tb_selection(t, &a, &b) && a == t->cur) {
        from = a + 1; /* past the match on show */
    }
    size_t tries = 0;
    size_t i = from;
    while (tries <= t->len) {
        if (i + n > t->len) {
            i = 0;
        }
        if (memcmp(t->text + i, needle, n) == 0) {
            t->anchored = true;
            t->anchor = i + n;
            t->cur = i;
            t->goal = tb_col_of(t, i);
            return true;
        }
        i++;
        tries++;
    }
    return false;
}

bool tb_set_byte(tbuf *t, size_t off, uint8_t v) {
    if (off > t->len || (off == t->len && t->len >= TB_CAP)) {
        return false;
    }
    bool was_nl = false;
    if (off < t->len) {
        was_nl = t->text[off] == '\n';
    }
    if (v == '\n' && !was_nl && t->nlines >= TB_LINES_MAX) {
        return false;
    }
    if (off == t->len) {
        t->len++;
    }
    touched(t, off);
    t->text[off] = v;
    t->anchored = false;
    t->dirty = true;
    if (was_nl || v == '\n') {
        reindex(t);
    }
    return true;
}

bool tb_replace(tbuf *t, const uint8_t *data, size_t n, size_t cur) {
    if (!fits(data, n)) {
        return false;
    }
    touched(t, 0);
    memmove(t->text, data, n);
    t->len = n;
    t->anchored = false;
    t->dirty = true;
    reindex(t);
    tb_seek(t, cur, true);
    return true;
}

void tb_seek(tbuf *t, size_t off, bool at_rune) {
    if (off > t->len) {
        off = t->len;
    }
    while (at_rune && off > 0 && off < t->len && (t->text[off] & 0xC0U) == 0x80U) {
        off--;
    }
    set_cursor(t, off, false, false);
}
