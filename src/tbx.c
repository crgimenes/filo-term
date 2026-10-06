#include "tbx.h"

#include <string.h>

#include "filo_fmt.h"
#include "hl.h"
#include "keys.h"
#include "term.h"
#include "utf8.h"

/* The buffer the builtins edit; the host says which when it registers them. */
static tbuf *(*buf_of)(filo_ctx *ctx) = NULL;
static bool (*saved_of)(filo_ctx *ctx) = NULL;

static int num_arg(filo_ctx *ctx, const filo_value *v, const char *what, size_t *out) {
    if (v->kind != FILO_NUMBER || v->u.num < 0) {
        return filo_fail2(ctx, what, " expects a non-negative number");
    }
    *out = (size_t)v->u.num;
    return FILO_OK;
}

static int b_tb_lines(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    (void)a;
    if (n != 0) {
        return filo_fail(ctx, "tb-lines takes no argument");
    }
    *out = filo_num((double)buf_of(ctx)->nlines);
    return FILO_OK;
}

/* The rune at at, before end, and its length: U+FFFD and one byte for
   what does not decode. */
static size_t next_rune(const tbuf *t, size_t at, size_t end, uint32_t *cp) {
    utf8_dec d;
    utf8_dec_init(&d);
    *cp = 0xFFFD;
    size_t j = 0;
    while (at + j < end && j < 4) {
        int resync = 0;
        utf8_result r = utf8_dec_feed(&d, t->text[at + j], cp, &resync);
        j++;
        if (r == UTF8_RUNE) {
            return j;
        }
        if (r == UTF8_ERROR) {
            break;
        }
    }
    *cp = 0xFFFD;
    return 1;
}

/* How many columns cp takes at column col. */
static size_t rune_cols(uint32_t cp, size_t col) {
    if (cp == '\t') {
        return TB_TAB - (col % TB_TAB);
    }
    return utf8_width(cp) > 1 ? 2 : 1;
}

/* (tb-line i left cols) is line i as shown from column left, cols wide,
   tabs as spaces: a tuple of the text and the rune indexes where the
   selection starts and ends inside it (-1 -1 when none of it is selected). */
static int b_tb_line(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    size_t i = 0;
    size_t left = 0;
    size_t cols = 0;
    if (n != 3 || num_arg(ctx, &a[0], "tb-line", &i) != FILO_OK ||
        num_arg(ctx, &a[1], "tb-line", &left) != FILO_OK ||
        num_arg(ctx, &a[2], "tb-line", &cols) != FILO_OK) {
        return n != 3 ? filo_fail(ctx, "tb-line expects a line, a first column and a width")
                      : FILO_ERR;
    }
    tbuf *t = buf_of(ctx);
    const uint8_t *p = NULL;
    size_t len = 0;
    tb_line(t, i, &p, &len);
    size_t sa = 0;
    size_t sb = 0;
    bool sel = tb_selection(t, &sa, &sb);
    /* walk the line by rune, keeping what falls in the window */
    static uint8_t shown[(TERM_COLS_MAX * 4) + 8];
    size_t k = 0;
    size_t col = 0;
    size_t runes = 0;
    double sel_start = -1;
    double sel_end = -1;
    size_t at = i < t->nlines ? t->lines[i] : t->len;
    size_t end = at + len;
    while (at < end && col < left + cols) {
        uint32_t cp = 0;
        size_t rl = next_rune(t, at, end, &cp);
        size_t w = rune_cols(cp, col);
        if (col >= left) {
            if (sel && at >= sa && at < sb && sel_start < 0) {
                sel_start = (double)runes;
            }
            if (sel && at >= sb && sel_end < 0 && sel_start >= 0) {
                sel_end = (double)runes;
            }
            if (cp == '\t') {
                size_t s = 0;
                while (s < w && k + 1 < sizeof(shown)) {
                    shown[k++] = ' ';
                    runes++;
                    s++;
                }
            } else if (k + rl < sizeof(shown)) {
                if (cp == 0xFFFD && rl == 1) {
                    k += utf8_encode(shown + k, 0xFFFD);
                } else {
                    memcpy(shown + k, t->text + at, rl);
                    k += rl;
                }
                runes++;
            }
        }
        col += w;
        at += rl;
    }
    if (sel && sel_start >= 0 && sel_end < 0) {
        sel_end = (double)runes;
    }
    if (sel && sel_start < 0 && at <= sb && end >= sa && sa < end && sb > at && len > 0 &&
        left > 0) {
        /* the selection covers what is shown but started before the window */
        sel_start = 0;
        sel_end = (double)runes;
    }
    uint8_t *mem = filo_alloc(ctx, k > 0 ? k : 1);
    if (mem == NULL) {
        return filo_fail(ctx, "tb-line: out of memory");
    }
    memcpy(mem, shown, k);
    filo_value parts[3];
    parts[0] = filo_string(mem, (uint32_t)k);
    parts[1] = filo_num(sel_start);
    parts[2] = filo_num(sel_end);
    return filo_tuple(ctx, parts, 3, out);
}

/* The highlighter's state at the start of a line. A paint asks for its
   lines in order, so the one after the last asked for is kept; and every
   HL_EVERY lines a mark keeps it too, so a paint anywhere walks at most
   that many lines to its first one. An edit takes back the marks past it:
   a line's state is what the text above it made. */
enum {
    HL_EVERY = 256,
    HL_MARKS = (TB_LINES_MAX / HL_EVERY) + 1,
};

static struct {
    const tbuf *t;
    hl_lang lang;
    hl_state mark[HL_MARKS]; /* mark[k]: the state at line k * HL_EVERY */
    size_t marks;            /* how many hold; mark[0] always, once begun */
    size_t next;             /* the line after the last one asked for */
    hl_state next_st;        /* the state there, when next is not 0 */
} hlc;

static hl_state state_at(tbuf *t, hl_lang lang, size_t i) {
    if (hlc.t != t || hlc.lang != lang || hlc.marks == 0) {
        hlc.t = t;
        hlc.lang = lang;
        hl_begin(&hlc.mark[0], lang);
        hlc.marks = 1;
        hlc.next = 0;
    }
    if (t->changed != SIZE_MAX) {
        while (hlc.marks > 1 && ((hlc.marks - 1) * HL_EVERY >= t->nlines ||
                                 t->lines[(hlc.marks - 1) * HL_EVERY] > t->changed)) {
            hlc.marks--;
        }
        if (hlc.next > 0 && (hlc.next >= t->nlines || t->lines[hlc.next] > t->changed)) {
            hlc.next = 0;
        }
        t->changed = SIZE_MAX;
    }
    if (hlc.next > 0 && hlc.next == i) {
        return hlc.next_st;
    }
    size_t k = i / HL_EVERY;
    if (k >= hlc.marks) {
        k = hlc.marks - 1;
    }
    hl_state st = hlc.mark[k];
    for (size_t line = k * HL_EVERY; line < i && line < t->nlines; line++) {
        if (line % HL_EVERY == 0 && line / HL_EVERY == hlc.marks && hlc.marks < HL_MARKS) {
            hlc.mark[hlc.marks++] = st;
        }
        const uint8_t *p = NULL;
        size_t n = 0;
        tb_line(t, line, &p, &n);
        hl_spans(&st, p, n, NULL, NULL);
    }
    if (i % HL_EVERY == 0 && i / HL_EVERY == hlc.marks && hlc.marks < HL_MARKS) {
        hlc.mark[hlc.marks++] = st;
    }
    return st;
}

/* The classes of the bytes shown, [from, from + n) of the line. */
typedef struct {
    uint8_t *cls;
    size_t from;
    size_t n;
} shown_classes;

/* cppcheck-suppress constParameterCallback ; hl_span's shape */
static void classify(void *user, hl_class cls, size_t from, size_t to) {
    const shown_classes *s = user;
    size_t a = from > s->from ? from - s->from : 0;
    size_t b = to > s->from ? to - s->from : 0;
    for (size_t k = a; k < b && k < s->n; k++) {
        s->cls[k] = (uint8_t)cls;
    }
}

/* (tb-spans i left cols lang) is line i as shown from column left, cols
   wide, tabs as spaces, as tb-line shows it, in stretches: a list of
   tuples of the text, its class (0 plain, 1 comment, 2 string, 3 number,
   4 keyword) and whether it is selected. lang is a language as a code
   fence names it ("c", "go", "filo", "red"...); one it does not know is
   all plain. */
static int b_tb_spans(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    size_t i = 0;
    size_t left = 0;
    size_t cols = 0;
    if (n != 4 || num_arg(ctx, &a[0], "tb-spans", &i) != FILO_OK ||
        num_arg(ctx, &a[1], "tb-spans", &left) != FILO_OK ||
        num_arg(ctx, &a[2], "tb-spans", &cols) != FILO_OK || a[3].kind != FILO_STRING) {
        return n != 4 || a[3].kind != FILO_STRING
                   ? filo_fail(ctx,
                               "tb-spans expects a line, a first column, a width and a language")
                   : FILO_ERR;
    }
    char name[24];
    size_t nl = a[3].u.str.len < sizeof(name) - 1 ? a[3].u.str.len : 0;
    memcpy(name, a[3].u.str.ptr, nl);
    name[nl] = '\0';
    tbuf *t = buf_of(ctx);
    if (i >= t->nlines) {
        filo_value none = {0};
        return filo_list(ctx, &none, 0, out);
    }
    const uint8_t *p = NULL;
    size_t len = 0;
    tb_line(t, i, &p, &len);
    size_t start = t->lines[i];
    size_t end = start + len;
    /* the bytes the window shows */
    size_t col = 0;
    size_t at = start;
    size_t ws = end;
    while (at < end && col < left + cols) {
        uint32_t cp = 0;
        size_t rl = next_rune(t, at, end, &cp);
        if (col >= left && ws == end) {
            ws = at;
        }
        col += rune_cols(cp, col);
        at += rl;
    }
    size_t we = at;
    static uint8_t cls[(TERM_COLS_MAX * 4) + 8];
    shown_classes sc = {cls, ws - start, we > ws ? we - ws : 0};
    if (sc.n > sizeof(cls)) {
        sc.n = sizeof(cls);
    }
    memset(cls, HL_PLAIN, sc.n);
    hl_lang lang = hl_lang_of(name);
    hl_state st = state_at(t, lang, i);
    hl_spans(&st, p, len, classify, &sc);
    hlc.next = i + 1;
    hlc.next_st = st;
    /* the stretches: the runes shown, cut where the class or the selection
       changes */
    size_t sa = 0;
    size_t sb = 0;
    bool sel = tb_selection(t, &sa, &sb);
    static uint8_t text[(TERM_COLS_MAX * 4) + 8];
    static size_t cut[TERM_COLS_MAX + 1];
    static uint8_t cut_cls[TERM_COLS_MAX + 1];
    static bool cut_sel[TERM_COLS_MAX + 1];
    size_t k = 0;
    size_t ncut = 0;
    col = 0;
    at = start;
    while (at < end && col < left + cols) {
        uint32_t cp = 0;
        size_t rl = next_rune(t, at, end, &cp);
        size_t w = rune_cols(cp, col);
        if (col >= left && at >= ws && at - ws < sc.n) {
            uint8_t c = cls[at - ws];
            bool s = false;
            if (sel && at >= sa && at < sb) {
                s = true;
            }
            if (ncut == 0 || c != cut_cls[ncut - 1] || s != cut_sel[ncut - 1]) {
                if (ncut == TERM_COLS_MAX + 1) {
                    break;
                }
                cut[ncut] = k;
                cut_cls[ncut] = c;
                cut_sel[ncut] = s;
                ncut++;
            }
            if (cp == '\t') {
                for (size_t sp = 0; sp < w && k + 1 < sizeof(text); sp++) {
                    text[k++] = ' ';
                }
            } else if (cp == 0xFFFD && rl == 1 && k + 4 < sizeof(text)) {
                k += utf8_encode(text + k, 0xFFFD);
            } else if (k + rl < sizeof(text)) {
                memcpy(text + k, t->text + at, rl);
                k += rl;
            }
        }
        col += w;
        at += rl;
    }
    uint8_t *mem = filo_alloc(ctx, k > 0 ? k : 1);
    filo_value *items = filo_alloc(ctx, sizeof(filo_value) * (ncut > 0 ? ncut : 1));
    if (mem == NULL || items == NULL) {
        return filo_fail(ctx, "tb-spans: out of memory");
    }
    memcpy(mem, text, k);
    for (size_t c = 0; c < ncut; c++) {
        size_t to = c + 1 < ncut ? cut[c + 1] : k;
        filo_value parts[3];
        parts[0] = filo_string(mem + cut[c], (uint32_t)(to - cut[c]));
        parts[1] = filo_num(cut_cls[c]);
        parts[2] = filo_bool(cut_sel[c]);
        if (filo_tuple(ctx, parts, 3, &items[c]) != FILO_OK) {
            return FILO_ERR;
        }
    }
    return filo_list(ctx, items, (uint32_t)ncut, out);
}

static int b_tb_cursor(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    (void)a;
    if (n != 0) {
        return filo_fail(ctx, "tb-cursor takes no argument");
    }
    const tbuf *t = buf_of(ctx);
    filo_value parts[2];
    parts[0] = filo_num((double)tb_line_of(t, t->cur));
    parts[1] = filo_num((double)tb_col_of(t, t->cur));
    return filo_tuple(ctx, parts, 2, out);
}

static int b_tb_view(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    size_t rows = 0;
    size_t cols = 0;
    if (n != 2 || num_arg(ctx, &a[0], "tb-view", &rows) != FILO_OK ||
        num_arg(ctx, &a[1], "tb-view", &cols) != FILO_OK) {
        return n != 2 ? filo_fail(ctx, "tb-view expects rows and columns") : FILO_ERR;
    }
    tbuf *t = buf_of(ctx);
    tb_view(t, rows, cols);
    filo_value parts[2];
    parts[0] = filo_num((double)t->top);
    parts[1] = filo_num((double)t->left);
    return filo_tuple(ctx, parts, 2, out);
}

/* (tb-key key page): the editing keys, true when the key was one. Runes
   insert, the arrows move (with Shift they select), Home/End/PgUp/PgDn
   as everywhere, Ctrl-Home/End to the ends of the text, Ctrl-Ins copies,
   Shift-Ins pastes, Shift-Del cuts, Ctrl-Y takes the line. */
static int b_tb_key(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    size_t key = 0;
    size_t page = 1;
    if (n != 2 || num_arg(ctx, &a[0], "tb-key", &key) != FILO_OK ||
        num_arg(ctx, &a[1], "tb-key", &page) != FILO_OK) {
        return n != 2 ? filo_fail(ctx, "tb-key expects a key and a page height") : FILO_ERR;
    }
    tbuf *t = buf_of(ctx);
    uint32_t cp = (uint32_t)key;
    uint32_t base = FT_KEY_BASE(cp);
    bool shift = (cp & (uint32_t)FT_KEY_SHIFT) != 0;
    bool ctrl = (cp & (uint32_t)FT_KEY_CTRL) != 0;
    bool took = true;
    if (cp == '\r' || cp == '\n') {
        (void)tb_insert(t, (const uint8_t *)"\n", 1);
    } else if (cp == '\t') {
        (void)tb_insert(t, (const uint8_t *)"    ", TB_TAB);
    } else if (cp == 0x08 || cp == 0x7f) {
        tb_backspace(t);
    } else if (cp == 0x19) {
        tb_delete_line(t);
    } else if (cp >= 0x20 && !ft_key_is_code(cp)) {
        uint8_t bytes[UTF8_MAX_BYTES];
        size_t k = utf8_encode(bytes, cp);
        (void)tb_insert(t, bytes, k);
    } else if (base == FT_KEY_DEL) {
        if (shift) {
            tb_cut(t);
        } else {
            tb_delete(t);
        }
    } else if (base == FT_KEY_INS) {
        if (ctrl) {
            tb_copy(t);
        } else if (shift) {
            (void)tb_paste(t);
        }
    } else if (base == FT_KEY_LEFT) {
        tb_left(t, shift);
    } else if (base == FT_KEY_RIGHT) {
        tb_right(t, shift);
    } else if (base == FT_KEY_UP) {
        tb_up(t, 1, shift);
    } else if (base == FT_KEY_DOWN) {
        tb_down(t, 1, shift);
    } else if (base == FT_KEY_PGUP) {
        tb_up(t, page > 0 ? page : 1, shift);
    } else if (base == FT_KEY_PGDN) {
        tb_down(t, page > 0 ? page : 1, shift);
    } else if (base == FT_KEY_HOME) {
        if (ctrl) {
            tb_doc_home(t, shift);
        } else {
            tb_home(t, shift);
        }
    } else if (base == FT_KEY_END) {
        if (ctrl) {
            tb_doc_end(t, shift);
        } else {
            tb_end(t, shift);
        }
    } else {
        took = false;
    }
    *out = filo_bool(took);
    return FILO_OK;
}

static int b_tb_dirty(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    (void)a;
    if (n != 0) {
        return filo_fail(ctx, "tb-dirty takes no argument");
    }
    tbuf *t = buf_of(ctx);
    if (t->dirty && saved_of != NULL && saved_of(ctx)) {
        t->dirty = false;
    }
    *out = filo_bool(t->dirty);
    return FILO_OK;
}

static int b_tb_find(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    if (n != 1 || a[0].kind != FILO_STRING) {
        return filo_fail(ctx, "tb-find expects a string");
    }
    *out = filo_bool(tb_find(buf_of(ctx), a[0].u.str.ptr, a[0].u.str.len));
    return FILO_OK;
}

/* (tb-goto line): the cursor to the start of that line (1-based, as people
   count them). */
static int b_tb_goto(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    size_t line = 0;
    if (n != 1 || num_arg(ctx, &a[0], "tb-goto", &line) != FILO_OK) {
        return n != 1 ? filo_fail(ctx, "tb-goto expects a line number") : FILO_ERR;
    }
    tb_goto_line(buf_of(ctx), line > 0 ? line - 1 : 0);
    *out = filo_bool(true);
    return FILO_OK;
}

/* (tb-clip): what was last copied or cut, for the terminal's clipboard. */
static int b_tb_clip(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    (void)a;
    if (n != 0) {
        return filo_fail(ctx, "tb-clip takes no argument");
    }
    const tbuf *t = buf_of(ctx);
    uint8_t *mem = filo_alloc(ctx, t->clip_len > 0 ? t->clip_len : 1);
    if (mem == NULL) {
        return filo_fail(ctx, "tb-clip: out of memory");
    }
    memcpy(mem, t->clip, t->clip_len);
    *out = filo_string(mem, (uint32_t)t->clip_len);
    return FILO_OK;
}

static int b_tb_copy(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    (void)a;
    if (n != 0) {
        return filo_fail(ctx, "tb-copy takes no argument");
    }
    tb_copy(buf_of(ctx));
    *out = filo_bool(true);
    return FILO_OK;
}

static int b_tb_cut(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    (void)a;
    if (n != 0) {
        return filo_fail(ctx, "tb-cut takes no argument");
    }
    tb_cut(buf_of(ctx));
    *out = filo_bool(true);
    return FILO_OK;
}

static int b_tb_paste(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    (void)a;
    if (n != 0) {
        return filo_fail(ctx, "tb-paste takes no argument");
    }
    *out = filo_bool(tb_paste(buf_of(ctx)));
    return FILO_OK;
}

/* ---- bytes: what a hex view works with ---- */

static int b_tb_size(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    (void)a;
    if (n != 0) {
        return filo_fail(ctx, "tb-size takes no argument");
    }
    *out = filo_num((double)buf_of(ctx)->len);
    return FILO_OK;
}

/* (tb-offset): the cursor as a byte offset, which is what a hex view shows. */
static int b_tb_offset(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    (void)a;
    if (n != 0) {
        return filo_fail(ctx, "tb-offset takes no argument");
    }
    *out = filo_num((double)buf_of(ctx)->cur);
    return FILO_OK;
}

/* (tb-seek off) puts the cursor on a byte; (tb-seek off #t) on the start of
   the rune that byte is in, for the line view. */
static int b_tb_seek(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    size_t off = 0;
    if (n < 1 || n > 2 || num_arg(ctx, &a[0], "tb-seek", &off) != FILO_OK) {
        return n < 1 || n > 2 ? filo_fail(ctx, "tb-seek expects an offset") : FILO_ERR;
    }
    bool at_rune = false;
    if (n == 2 && a[1].kind == FILO_BOOL) {
        at_rune = a[1].u.b;
    }
    tb_seek(buf_of(ctx), off, at_rune);
    *out = filo_num((double)buf_of(ctx)->cur);
    return FILO_OK;
}

/* (tb-byte-at off): 0 to 255, or -1 past the end. */
static int b_tb_byte_at(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    size_t off = 0;
    if (n != 1 || num_arg(ctx, &a[0], "tb-byte-at", &off) != FILO_OK) {
        return n != 1 ? filo_fail(ctx, "tb-byte-at expects an offset") : FILO_ERR;
    }
    const tbuf *t = buf_of(ctx);
    *out = filo_num(off < t->len ? (double)t->text[off] : -1.0);
    return FILO_OK;
}

/* (tb-set-byte off v): the byte replaced, or added at the end; #f when it
   does not fit. */
static int b_tb_set_byte(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    size_t off = 0;
    size_t v = 0;
    if (n != 2 || num_arg(ctx, &a[0], "tb-set-byte", &off) != FILO_OK ||
        num_arg(ctx, &a[1], "tb-set-byte", &v) != FILO_OK) {
        return n != 2 ? filo_fail(ctx, "tb-set-byte expects an offset and a byte") : FILO_ERR;
    }
    if (v > 255) {
        return filo_fail(ctx, "tb-set-byte: a byte is 0 to 255");
    }
    *out = filo_bool(tb_set_byte(buf_of(ctx), off, (uint8_t)v));
    return FILO_OK;
}

/* Code page 437, the PC's own: what a byte looked like on the screens hex
   editors were born on, the control bytes included. */
static const uint16_t cp437[256] = {
    0x0020, 0x263A, 0x263B, 0x2665, 0x2666, 0x2663, 0x2660, 0x2022, 0x25D8, 0x25CB, 0x25D9, 0x2642,
    0x2640, 0x266A, 0x266B, 0x263C, 0x25BA, 0x25C4, 0x2195, 0x203C, 0x00B6, 0x00A7, 0x25AC, 0x21A8,
    0x2191, 0x2193, 0x2192, 0x2190, 0x221F, 0x2194, 0x25B2, 0x25BC, 0x0020, 0x0021, 0x0022, 0x0023,
    0x0024, 0x0025, 0x0026, 0x0027, 0x0028, 0x0029, 0x002A, 0x002B, 0x002C, 0x002D, 0x002E, 0x002F,
    0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037, 0x0038, 0x0039, 0x003A, 0x003B,
    0x003C, 0x003D, 0x003E, 0x003F, 0x0040, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047,
    0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F, 0x0050, 0x0051, 0x0052, 0x0053,
    0x0054, 0x0055, 0x0056, 0x0057, 0x0058, 0x0059, 0x005A, 0x005B, 0x005C, 0x005D, 0x005E, 0x005F,
    0x0060, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067, 0x0068, 0x0069, 0x006A, 0x006B,
    0x006C, 0x006D, 0x006E, 0x006F, 0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077,
    0x0078, 0x0079, 0x007A, 0x007B, 0x007C, 0x007D, 0x007E, 0x2302, 0x00C7, 0x00FC, 0x00E9, 0x00E2,
    0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, 0x00FF, 0x00D6, 0x00DC, 0x00A2,
    0x00A3, 0x00A5, 0x20A7, 0x0192, 0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,
    0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB, 0x2591, 0x2592, 0x2593, 0x2502,
    0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566,
    0x2560, 0x2550, 0x256C, 0x2567, 0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B,
    0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580, 0x03B1, 0x00DF, 0x0393, 0x03C0,
    0x03A3, 0x03C3, 0x00B5, 0x03C4, 0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248, 0x00B0, 0x2219, 0x00B7, 0x221A,
    0x207F, 0x00B2, 0x25A0, 0x00A0,
};

static size_t put_hex(uint8_t *dst, uint32_t v, uint32_t digits) {
    static const char hexd[] = "0123456789ABCDEF";
    for (uint32_t i = 0; i < digits; i++) {
        dst[digits - 1 - i] = (uint8_t)hexd[(v >> (4U * i)) & 15U];
    }
    return digits;
}

/* (tb-hex-row off n cp437): n bytes from off as a hex view shows them, a
   tuple of the hex ("E9 6D 15 ...", an extra space after the eighth) and
   the text beside it, each byte a character of code page 437 when cp437 is
   true and a dot when it is not printable otherwise. Past the end the row
   keeps its width in blanks. */
static int b_tb_hex_row(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    size_t off = 0;
    size_t count = 0;
    if (n != 3 || num_arg(ctx, &a[0], "tb-hex-row", &off) != FILO_OK ||
        num_arg(ctx, &a[1], "tb-hex-row", &count) != FILO_OK || a[2].kind != FILO_BOOL) {
        return n != 3 || a[2].kind != FILO_BOOL
                   ? filo_fail(ctx,
                               "tb-hex-row expects an offset, a count and #t for code page 437")
                   : FILO_ERR;
    }
    if (count > 64) {
        return filo_fail(ctx, "tb-hex-row: at most 64 bytes a row");
    }
    const tbuf *t = buf_of(ctx);
    uint8_t *hex = filo_alloc(ctx, (count * 3U) + 2U);
    uint8_t *text = filo_alloc(ctx, (count * UTF8_MAX_BYTES) + 1U);
    if (hex == NULL || text == NULL) {
        return filo_fail(ctx, "tb-hex-row: out of memory");
    }
    size_t h = 0;
    size_t k = 0;
    for (size_t i = 0; i < count; i++) {
        if (i > 0) {
            hex[h++] = ' ';
        }
        if (i == 8) {
            hex[h++] = ' ';
        }
        if (off + i >= t->len) {
            hex[h++] = ' ';
            hex[h++] = ' ';
            text[k++] = ' ';
            continue;
        }
        uint8_t b = t->text[off + i];
        h += put_hex(hex + h, b, 2);
        if (a[2].u.b) {
            k += utf8_encode(text + k, cp437[b]);
        } else {
            text[k++] = b >= 0x20 && b < 0x7F ? b : (uint8_t)'.';
        }
    }
    filo_value parts[2];
    parts[0] = filo_string(hex, (uint32_t)h);
    parts[1] = filo_string(text, (uint32_t)k);
    return filo_tuple(ctx, parts, 2, out);
}

static int hex_digit(uint8_t c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* (tb-find-bytes "4d 5a"): the next place after the cursor holding those
   bytes, wrapping, found as tb-find finds text: the cursor on the first of
   them and the rest selected, so the next search goes past it. Spaces
   between pairs are for reading and are skipped. */
static int b_tb_find_bytes(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    if (n != 1 || a[0].kind != FILO_STRING) {
        return filo_fail(ctx, "tb-find-bytes expects a string of hex digits");
    }
    uint8_t want[TB_FIND_MAX];
    size_t len = 0;
    int high = -1;
    for (uint32_t i = 0; i < a[0].u.str.len; i++) {
        uint8_t c = a[0].u.str.ptr[i];
        if (c == ' ') {
            continue;
        }
        int d = hex_digit(c);
        if (d < 0 || (high < 0 && len == sizeof(want))) {
            return filo_fail(ctx, "tb-find-bytes expects pairs of hex digits");
        }
        if (high < 0) {
            high = d;
            continue;
        }
        want[len++] = (uint8_t)((high * 16) + d);
        high = -1;
    }
    if (high >= 0 || len == 0) {
        return filo_fail(ctx, "tb-find-bytes expects pairs of hex digits");
    }
    *out = filo_bool(tb_find(buf_of(ctx), want, len)); /* as tb-find: on the first byte */
    return FILO_OK;
}

/* A whole number from 0 to 2^53, for the number formats below. */
static int arg_count(filo_ctx *ctx, const filo_value *v, const char *name, uint64_t *out) {
    if (v->kind != FILO_NUMBER || v->u.num < 0 || v->u.num > 9007199254740992.0 ||
        (double)(uint64_t)v->u.num != v->u.num) {
        return filo_fail2(ctx, name, " expects a whole number from 0");
    }
    *out = (uint64_t)v->u.num;
    return FILO_OK;
}

/* (hex n digits) and (bin n digits): n in base 16 (upper case) or 2, with
   leading zeros to at least digits. */
static int radix_text(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out,
                      const char *name, uint32_t bits) {
    uint64_t v = 0;
    uint64_t digits = 0;
    if (n != 2 || arg_count(ctx, &a[0], name, &v) != FILO_OK ||
        arg_count(ctx, &a[1], name, &digits) != FILO_OK) {
        return n != 2 ? filo_fail2(ctx, name, " expects a number and a width") : FILO_ERR;
    }
    if (digits > 64) {
        return filo_fail2(ctx, name, ": at most 64 digits");
    }
    uint8_t tmp[64];
    size_t k = 0;
    uint32_t mask = (1U << bits) - 1U;
    do {
        tmp[k++] = (uint8_t)"0123456789ABCDEF"[v & mask];
        v >>= bits;
    } while (v > 0 && k < sizeof(tmp));
    while (k < digits) {
        tmp[k++] = '0';
    }
    uint8_t *s = filo_alloc(ctx, k);
    if (s == NULL) {
        return filo_fail2(ctx, name, ": out of memory");
    }
    for (size_t i = 0; i < k; i++) {
        s[i] = tmp[k - 1 - i];
    }
    *out = filo_string(s, (uint32_t)k);
    return FILO_OK;
}

static int b_hex(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    return radix_text(ctx, a, n, out, "hex", 4);
}

static int b_bin(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    return radix_text(ctx, a, n, out, "bin", 1);
}

/* (cp437 b): the character byte b is in code page 437. */
static int b_cp437(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    uint64_t b = 0;
    if (n != 1 || arg_count(ctx, &a[0], "cp437", &b) != FILO_OK) {
        return n != 1 ? filo_fail(ctx, "cp437 expects a byte") : FILO_ERR;
    }
    if (b > 255) {
        return filo_fail(ctx, "cp437: a byte is 0 to 255");
    }
    uint8_t *s = filo_alloc(ctx, UTF8_MAX_BYTES);
    if (s == NULL) {
        return filo_fail(ctx, "cp437: out of memory");
    }
    *out = filo_string(s, (uint32_t)utf8_encode(s, cp437[b]));
    return FILO_OK;
}

/* ---- formatting ---- */

static uint8_t fmt_mem[FT_CFG_TB_FMT_MEM];

/* Whether every string and every paren of the source closes: laid out, one
   that does not could carry the rest of the file into a string. */
static bool closes(const uint8_t *p, size_t n) {
    size_t depth = 0;
    size_t i = 0;
    while (i < n) {
        uint8_t c = p[i];
        i++;
        if (c == ';') {
            while (i < n && p[i] != '\n') {
                i++;
            }
        } else if (c == '"') {
            while (i < n && p[i] != '"') {
                i += p[i] == '\\' ? 2U : 1U;
            }
            if (i >= n) {
                return false;
            }
            i++;
        } else if (c == '(') {
            depth++;
        } else if (c == ')') {
            if (depth == 0) {
                return false;
            }
            depth--;
        }
    }
    return depth == 0;
}

static bool blank(uint8_t c) {
    switch (c) {
    case ' ':
    case '\t':
    case '\r':
    case '\n':
        return true;
    default:
        return false;
    }
}

/* Whether b is a with only blanks moved, and where in b the byte is that
   has as many others before it, blanks aside, as the one at cur in a: the
   cursor on the same character of the code. */
static bool same_code(const uint8_t *a, size_t an, const uint8_t *b, size_t bn, size_t cur,
                      size_t *at) {
    size_t i = 0;
    size_t j = 0;
    *at = bn;
    for (;;) {
        while (i < an && blank(a[i])) {
            i++;
        }
        while (j < bn && blank(b[j])) {
            j++;
        }
        if (i >= cur && *at == bn) {
            *at = j;
        }
        if (i == an) {
            return j == bn;
        }
        if (j == bn) {
            return false;
        }
        if (a[i] != b[j]) {
            return false;
        }
        i++;
        j++;
    }
}

/* (tb-format): the text laid out as filofmt lays out Filo source, as one
   edit, the cursor kept on the same character of the code; "" when it is
   done, else why not. There is no undo, so only blanks may move: a source
   whose strings or parens do not close is refused, and so is any layout
   that would change more than blanks. */
static int b_tb_format(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    (void)a;
    if (n != 0) {
        return filo_fail(ctx, "tb-format takes no argument");
    }
    tbuf *t = buf_of(ctx);
    const char *why = "";
    size_t len = 0;
    size_t at = 0;
    if (!closes(t->text, t->len)) {
        why = "a string or a paren does not close";
    } else {
        const char *text =
            filo_fmt((const char *)t->text, t->len, 2, 80, fmt_mem, sizeof(fmt_mem), &len);
        if (text == NULL) {
            why = "too large to format";
        } else if (!same_code(t->text, t->len, (const uint8_t *)text, len, t->cur, &at)) {
            why = "the layout would change more than blanks";
        } else if ((len != t->len || memcmp(text, t->text, len) != 0) &&
                   !tb_replace(t, (const uint8_t *)text, len, at)) {
            why = "the result does not fit the buffer";
        }
    }
    *out = filo_cstring(why);
    return FILO_OK;
}

void tbx_register(filo_ctx *ctx, tbuf *(*buf_fn)(filo_ctx *ctx), bool (*saved_fn)(filo_ctx *ctx)) {
    buf_of = buf_fn;
    saved_of = saved_fn;
    (void)filo_register_builtin(ctx, "tb-lines", b_tb_lines);
    (void)filo_register_builtin(ctx, "tb-line", b_tb_line);
    (void)filo_register_builtin(ctx, "tb-spans", b_tb_spans);
    (void)filo_register_builtin(ctx, "tb-cursor", b_tb_cursor);
    (void)filo_register_builtin(ctx, "tb-view", b_tb_view);
    (void)filo_register_builtin(ctx, "tb-key", b_tb_key);
    (void)filo_register_builtin(ctx, "tb-dirty", b_tb_dirty);
    (void)filo_register_builtin(ctx, "tb-find", b_tb_find);
    (void)filo_register_builtin(ctx, "tb-goto", b_tb_goto);
    (void)filo_register_builtin(ctx, "tb-clip", b_tb_clip);
    (void)filo_register_builtin(ctx, "tb-copy", b_tb_copy);
    (void)filo_register_builtin(ctx, "tb-cut", b_tb_cut);
    (void)filo_register_builtin(ctx, "tb-paste", b_tb_paste);
    (void)filo_register_builtin(ctx, "tb-size", b_tb_size);
    (void)filo_register_builtin(ctx, "tb-offset", b_tb_offset);
    (void)filo_register_builtin(ctx, "tb-seek", b_tb_seek);
    (void)filo_register_builtin(ctx, "tb-byte-at", b_tb_byte_at);
    (void)filo_register_builtin(ctx, "tb-set-byte", b_tb_set_byte);
    (void)filo_register_builtin(ctx, "tb-hex-row", b_tb_hex_row);
    (void)filo_register_builtin(ctx, "tb-find-bytes", b_tb_find_bytes);
    (void)filo_register_builtin(ctx, "tb-format", b_tb_format);
    (void)filo_register_builtin(ctx, "hex", b_hex);
    (void)filo_register_builtin(ctx, "bin", b_bin);
    (void)filo_register_builtin(ctx, "cp437", b_cp437);
}
