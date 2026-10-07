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
    t->undo_len = 0;
    reindex(t);
}

static void touched(tbuf *t, size_t off) {
    if (off < t->changed) {
        t->changed = off;
    }
}

/* The two edits everything comes down to, undo included. */
static void remove_range(tbuf *t, size_t a, size_t b) {
    touched(t, a);
    memmove(t->text + a, t->text + b, t->len - b);
    t->len -= b - a;
    t->dirty = true;
    reindex(t);
}

static void put(tbuf *t, size_t off, const uint8_t *bytes, size_t n) {
    touched(t, off);
    memmove(t->text + off + n, t->text + off, t->len - off);
    memcpy(t->text + off, bytes, n);
    t->len += n;
    t->dirty = true;
    reindex(t);
}

/* ---- undo ----
   A record is laid down as the bytes it keeps followed by its header, so
   the newest is read from the end. An insertion keeps nothing (taking it
   back deletes off..off+n), a deletion what it deleted, an overwrite what
   was there before. */
enum { U_INS, U_DEL, U_SET };

typedef struct {
    size_t off;
    size_t n;
    size_t cur; /* the cursor before the edit */
    uint8_t kind;
    bool chain; /* undone together with the record below it */
} undo_rec;

static size_t kept(const undo_rec *r) {
    return r->kind == U_INS ? 0 : r->n;
}

/* The record that ends at end. */
static bool record_at(const tbuf *t, size_t end, undo_rec *r) {
    if (end < sizeof(*r)) {
        return false;
    }
    memcpy(r, t->undo + end - sizeof(*r), sizeof(*r));
    return true;
}

/* The newest record, r, made k bytes longer; what it keeps of them is
   written in place already. */
static void grow(tbuf *t, undo_rec *r, size_t k) {
    size_t was = kept(r);
    r->n += k;
    t->undo_len += kept(r) - was;
    memcpy(t->undo + t->undo_len - sizeof(*r), r, sizeof(*r));
}

/* The oldest records dropped so room bytes are free: the newest that fit in
   half of the rest stay, so a full log is not walked on every key. A record
   undone with the one below it does not outlive that one. */
static void make_room(tbuf *t, size_t room) {
    size_t keep = (TB_UNDO - room) / 2;
    size_t from = t->undo_len;
    undo_rec r;
    undo_rec oldest = {0};
    while (record_at(t, from, &r) && t->undo_len - (from - sizeof(r) - kept(&r)) <= keep) {
        from -= sizeof(r) + kept(&r);
        oldest = r;
    }
    if (from < t->undo_len && oldest.chain) {
        from += sizeof(oldest) + kept(&oldest);
    }
    memmove(t->undo, t->undo + from, t->undo_len - from);
    t->undo_len -= from;
}

/* False when the record was not kept. One larger than the whole log
   empties it: the older records undo from a text that will not come back. */
static bool push(tbuf *t, undo_rec r, const uint8_t *bytes) {
    size_t size = sizeof(r) + kept(&r);
    if (size > TB_UNDO) {
        t->undo_len = 0;
        return false;
    }
    if (TB_UNDO - t->undo_len < size) {
        make_room(t, size);
    }
    if (r.chain && t->undo_len == 0) {
        return false; /* what it is undone with is gone */
    }
    if (kept(&r) > 0) {
        memcpy(t->undo + t->undo_len, bytes, kept(&r));
    }
    t->undo_len += kept(&r);
    memcpy(t->undo + t->undo_len, &r, sizeof(r));
    t->undo_len += sizeof(r);
    return true;
}

/* n bytes about to go in at off. A rune typed right after the last
   insertion joins it, up to the end of a line. */
static void log_insert(tbuf *t, size_t off, size_t n, bool chain) {
    undo_rec r;
    if (!chain && n <= 4 && off > 0 && t->text[off - 1] != '\n' && record_at(t, t->undo_len, &r) &&
        r.kind == U_INS && r.off + r.n == off) {
        grow(t, &r, n);
        return;
    }
    (void)push(t, (undo_rec){.off = off, .n = n, .cur = t->cur, .kind = U_INS, .chain = chain},
               t->text);
}

/* a..b about to be deleted. With join, a rune next to the last deletion,
   deleted the same way, joins it: before it for backspace, after it for
   delete. */
static void log_erase(tbuf *t, size_t a, size_t b, bool join) {
    size_t k = b - a;
    undo_rec r;
    if (join && k <= 4 && record_at(t, t->undo_len, &r) && r.kind == U_DEL &&
        TB_UNDO - t->undo_len >= k) {
        uint8_t *start = t->undo + t->undo_len - sizeof(r) - r.n;
        if (b == r.off && t->cur == b && r.cur == r.off + r.n) {
            memmove(start + k, start, r.n);
            memcpy(start, t->text + a, k);
            r.off = a;
            grow(t, &r, k);
            return;
        }
        if (a == r.off && t->cur == a && r.cur == r.off) {
            memcpy(start + r.n, t->text + a, k);
            grow(t, &r, k);
            return;
        }
    }
    (void)push(t, (undo_rec){.off = a, .n = k, .cur = t->cur, .kind = U_DEL}, t->text + a);
}

/* The byte at off about to be overwritten. Kept once: a byte the newest
   record already restores or removes needs nothing more, and the next one
   over joins it. */
static void log_set(tbuf *t, size_t off) {
    undo_rec r;
    if (record_at(t, t->undo_len, &r) && r.kind != U_DEL) {
        if (off >= r.off && off < r.off + r.n) {
            return;
        }
        if (r.kind == U_SET && off == r.off + r.n && t->undo_len < TB_UNDO) {
            t->undo[t->undo_len - sizeof(r)] = t->text[off];
            grow(t, &r, 1);
            return;
        }
    }
    (void)push(t, (undo_rec){.off = off, .n = 1, .cur = t->cur, .kind = U_SET}, t->text + off);
}

bool tb_undo(tbuf *t) {
    undo_rec r;
    if (!record_at(t, t->undo_len, &r)) {
        return false;
    }
    for (;;) {
        t->undo_len -= sizeof(r) + kept(&r);
        const uint8_t *bytes = t->undo + t->undo_len;
        switch (r.kind) {
        case U_INS:
            remove_range(t, r.off, r.off + r.n);
            break;
        case U_DEL:
            put(t, r.off, bytes, r.n);
            break;
        default:
            touched(t, r.off);
            memcpy(t->text + r.off, bytes, r.n);
            t->dirty = true;
            reindex(t);
            break;
        }
        if (!r.chain || !record_at(t, t->undo_len, &r)) {
            break;
        }
    }
    tb_seek(t, r.cur, false);
    return true;
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

static void erase(tbuf *t, size_t a, size_t b, bool join) {
    log_erase(t, a, b, join);
    remove_range(t, a, b);
    t->cur = a;
    t->anchored = false;
}

bool tb_insert(tbuf *t, const uint8_t *bytes, size_t n) {
    size_t a = 0;
    size_t b = 0;
    bool chain = false;
    if (tb_selection(t, &a, &b)) {
        erase(t, a, b, false);
        chain = true;
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
    log_insert(t, t->cur, n, chain);
    put(t, t->cur, bytes, n);
    t->cur += n;
    t->anchored = false;
    t->goal = tb_col_of(t, t->cur);
    return true;
}

void tb_backspace(tbuf *t) {
    size_t a = 0;
    size_t b = 0;
    if (tb_selection(t, &a, &b)) {
        erase(t, a, b, false);
    } else if (t->cur > 0) {
        size_t k = utf8_last_rune_len(t->text, t->cur);
        erase(t, t->cur - (k > 0 ? k : 1), t->cur, true);
    }
    t->goal = tb_col_of(t, t->cur);
}

void tb_delete(tbuf *t) {
    size_t a = 0;
    size_t b = 0;
    if (tb_selection(t, &a, &b)) {
        erase(t, a, b, false);
    } else if (t->cur < t->len) {
        uint32_t cp = 0;
        size_t k = rune_at(t, t->cur, &cp);
        erase(t, t->cur, t->cur + k, true);
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
    erase(t, a, b, false);
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
    erase(t, a, b, false);
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
        log_insert(t, off, 1, false);
        t->len++;
    } else {
        log_set(t, off);
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
    log_erase(t, 0, t->len, false);
    log_insert(t, 0, n, true);
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
