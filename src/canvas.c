#include <string.h>

#include "canvas.h"
#include "utf8.h"

static const cv_cell blank_cell = {
    .cp = ' ',
    .fg = CV_COLOR_DEFAULT,
    .bg = CV_COLOR_DEFAULT,
    .attr = 0,
};

/* ---- runes ---- */

typedef struct {
    const uint8_t *s;
    size_t len;
    size_t i;
    utf8_dec dec;
} rune_iter;

static void rune_begin(rune_iter *it, cv_text s) {
    it->s = s.ptr;
    it->len = s.ptr != NULL ? s.len : 0;
    it->i = 0;
    utf8_dec_init(&it->dec);
}

static bool rune_next(rune_iter *it, uint32_t *cp) {
    while (it->i < it->len) {
        uint8_t b = it->s[it->i];
        it->i++;
        int resync = 0;
        utf8_result r = utf8_dec_feed(&it->dec, b, cp, &resync);
        if (r == UTF8_RUNE) {
            return true;
        }
        if (r == UTF8_ERROR && resync != 0) {
            r = utf8_dec_feed(&it->dec, b, cp, &resync);
            if (r == UTF8_RUNE) {
                return true;
            }
        }
    }
    return false;
}

cv_text cv_cstr(const char *s) {
    cv_text t = {(const uint8_t *)s, s != NULL ? strlen(s) : 0};
    return t;
}

size_t cv_text_width(cv_text s) {
    rune_iter it;
    rune_begin(&it, s);
    size_t w = 0;
    uint32_t cp = 0;
    while (rune_next(&it, &cp)) {
        int cw = utf8_width(cp);
        if (cw > 0) {
            w += (size_t)cw;
        }
    }
    return w;
}

/* ---- the grid ---- */

void cv_pen_reset(canvas *c) {
    c->pen = blank_cell;
}

void cv_pen(canvas *c, int16_t fg, int16_t bg, uint8_t attr) {
    c->pen.fg = fg;
    c->pen.bg = bg;
    c->pen.attr = attr;
}

void cv_clear(canvas *c) {
    uint16_t y = 0;
    while (y < c->rows) {
        uint16_t x = 0;
        while (x < c->cols) {
            c->cells[y][x] = blank_cell;
            x++;
        }
        y++;
    }
}

void cv_reset(canvas *c, uint16_t rows, uint16_t cols) {
    c->rows = rows < TERM_ROWS_MAX ? rows : TERM_ROWS_MAX;
    c->cols = cols < TERM_COLS_MAX ? cols : TERM_COLS_MAX;
    cv_pen_reset(c);
    cv_clear(c);
}

static bool inside(const canvas *c, int32_t row, int32_t col) {
    if (row < 0 || col < 0) {
        return false;
    }
    if (row >= (int32_t)c->rows || col >= (int32_t)c->cols) {
        return false;
    }
    return true;
}

/* Writes one rune, keeping double-width pairs consistent: landing on either
   half of an existing wide rune blanks the other half, which would otherwise
   stay on screen as a stray. Returns the columns consumed. */
static int32_t put_cell(canvas *c, int32_t row, int32_t col, uint32_t cp) {
    int w = utf8_width(cp);
    if (w <= 0) {
        return 0; /* combining marks have no cell of their own; dropping one
                     beats shifting the whole row */
    }
    if (!inside(c, row, col)) {
        return w;
    }
    size_t y = (size_t)row;
    size_t x = (size_t)col;
    if (c->cells[y][x].cp == 0 && x > 0) {
        c->cells[y][x - 1] = blank_cell;
    }
    if (x + 1 < c->cols && c->cells[y][x + 1].cp == 0) {
        c->cells[y][x + 1] = blank_cell;
    }
    /* The pen carries the colours this cell keeps and only cp is replaced.
       cppcheck reads the write as clobbering the union, but cp belongs to
       the anonymous struct, not to raw's side of it. */
    cv_cell cell = c->pen;
    /* cppcheck-suppress redundantInitialization ; only cp is replaced */
    cell.cp = cp;
    if (w == 2 && x + 1 >= c->cols) {
        cell.cp = ' '; /* a wide rune must not be split by the right edge */
        c->cells[y][x] = cell;
        return w;
    }
    c->cells[y][x] = cell;
    if (w == 2) {
        cv_cell tail = c->pen;
        /* cppcheck-suppress redundantInitialization ; only cp is replaced */
        tail.cp = 0;
        c->cells[y][x + 1] = tail;
    }
    return w;
}

int32_t cv_put(canvas *c, int32_t row, int32_t col, cv_text s) {
    rune_iter it;
    rune_begin(&it, s);
    int32_t advanced = 0;
    uint32_t cp = 0;
    while (rune_next(&it, &cp)) {
        advanced += put_cell(c, row, col + advanced, cp);
    }
    return advanced;
}

/* Draws a run of cells with the string's runes in order, repeating it as
   often as needed. An empty string leaves the run untouched. */
static void tile(canvas *c, int32_t row, int32_t col, int32_t len, int32_t drow, int32_t dcol,
                 cv_text s) {
    if (s.ptr == NULL || s.len == 0 || len <= 0) {
        return;
    }
    rune_iter it;
    rune_begin(&it, s);
    int32_t i = 0;
    while (i < len) {
        uint32_t cp = 0;
        if (!rune_next(&it, &cp)) {
            rune_begin(&it, s);
            if (!rune_next(&it, &cp)) {
                return;
            }
        }
        put_cell(c, row + (drow * i), col + (dcol * i), cp);
        i++;
    }
}

void cv_fill(canvas *c, int32_t row, int32_t col, int32_t rows, int32_t cols, cv_text s) {
    int32_t y = 0;
    while (y < rows) {
        tile(c, row + y, col, cols, 0, 1, s);
        y++;
    }
}

void cv_scroll(canvas *c, int32_t row, int32_t col, int32_t rows, int32_t cols, int32_t by) {
    if (by == 0 || rows <= 0 || cols <= 0) {
        return;
    }
    int32_t dist = by < 0 ? -by : by;
    if (dist >= rows) {
        cv_fill(c, row, col, rows, cols, cv_cstr(" "));
        return;
    }
    int32_t moved = rows - dist;
    int32_t y = 0;
    while (y < moved) {
        /* up copies from the top down and down from the bottom up, so no
           row is read after something has been written over it */
        int32_t to = by < 0 ? row + y : row + rows - 1 - y;
        int32_t from = to - by;
        int32_t x = 0;
        while (x < cols) {
            if (to >= 0 && to < c->rows && from >= 0 && from < c->rows && col + x >= 0 &&
                col + x < c->cols) {
                c->cells[to][col + x] = c->cells[from][col + x];
            }
            x++;
        }
        y++;
    }
    cv_fill(c, by < 0 ? row + moved : row, col, dist, cols, cv_cstr(" "));
}

void cv_box(canvas *c, int32_t row, int32_t col, int32_t rows, int32_t cols,
            const cv_text border[8]) {
    if (rows <= 0 || cols <= 0) {
        return;
    }
    int32_t last_row = row + rows - 1;
    int32_t last_col = col + cols - 1;
    tile(c, row, col + 1, cols - 2, 0, 1, border[1]);
    if (rows > 1) {
        tile(c, last_row, col + 1, cols - 2, 0, 1, border[5]);
    }
    tile(c, row + 1, col, rows - 2, 1, 0, border[7]);
    if (cols > 1) {
        tile(c, row + 1, last_col, rows - 2, 1, 0, border[3]);
    }
    /* corners last: on a one-row or one-column box they win over the edges */
    tile(c, row, col, 1, 0, 1, border[0]);
    tile(c, row, last_col, 1, 0, 1, border[2]);
    tile(c, last_row, last_col, 1, 0, 1, border[4]);
    tile(c, last_row, col, 1, 0, 1, border[6]);
}

void cv_blit(canvas *dst, int32_t row, int32_t col, const canvas *src) {
    int32_t y = 0;
    while (y < (int32_t)src->rows) {
        int32_t x = 0;
        while (x < (int32_t)src->cols) {
            const cv_cell *cell = &src->cells[y][x];
            if (cell->cp != 0 && inside(dst, row + y, col + x)) {
                dst->cells[row + y][col + x] = *cell;
            }
            x++;
        }
        y++;
    }
}

void cv_copy(canvas *dst, const canvas *src) {
    dst->rows = src->rows;
    dst->cols = src->cols;
    dst->pen = src->pen;
    uint16_t y = 0;
    while (y < src->rows) {
        memcpy(dst->cells[y], src->cells[y], (size_t)src->cols * sizeof(cv_cell));
        y++;
    }
}

/* ---- emitting ---- */

static void emit_cell(term *t, const cv_cell *cell, cv_cell *pen) {
    if (cell->fg != pen->fg || cell->bg != pen->bg || cell->attr != pen->attr) {
        term_puts(t, "\x1b[0");
        if ((cell->attr & CV_A_BOLD) != 0) {
            term_puts(t, ";1");
        }
        if ((cell->attr & CV_A_DIM) != 0) {
            term_puts(t, ";2");
        }
        if ((cell->attr & CV_A_REV) != 0) {
            term_puts(t, ";7");
        }
        if (cell->fg != CV_COLOR_DEFAULT) {
            term_puts(t, ";38;5;");
            term_put_u32(t, (uint32_t)cell->fg);
        }
        if (cell->bg != CV_COLOR_DEFAULT) {
            term_puts(t, ";48;5;");
            term_put_u32(t, (uint32_t)cell->bg);
        }
        term_puts(t, "m");
        *pen = *cell;
    }
    uint8_t buf[UTF8_MAX_BYTES];
    size_t n = utf8_encode(buf, cell->cp);
    term_write(t, buf, n);
}

static bool same_cell(const cv_cell *a, const cv_cell *b) {
    return a->raw == b->raw;
}

/* True when every cell in [from, to) on this row already carries the pen,
   so rewriting them adds no escape of its own. */
static bool plain_run(const canvas *c, uint16_t row, uint16_t from, uint16_t to,
                      const cv_cell *pen) {
    uint16_t x = from;
    while (x < to) {
        const cv_cell *cell = &c->cells[row][x];
        if (cell->cp == 0 || cell->fg != pen->fg || cell->bg != pen->bg ||
            cell->attr != pen->attr) {
            return false;
        }
        x++;
    }
    return true;
}

void cv_flush(term *t, canvas *shown, const canvas *next) {
    if (shown->rows != next->rows || shown->cols != next->cols) {
        cv_reset(shown, next->rows, next->cols);
        term_puts(t, "\x1b[2J");
    }
    term_puts(t, "\x1b[0m");
    cv_cell pen = blank_cell;
    bool placed = false;
    uint16_t cur_row = 0;
    uint16_t cur_col = 0;
    uint16_t y = 0;
    while (y < next->rows) {
        uint16_t x = 0;
        while (x < next->cols) {
            const cv_cell *cell = &next->cells[y][x];
            if (cell->cp == 0) {
                x++; /* the right half of a wide rune goes out with its left */
                continue;
            }
            int w = utf8_width(cell->cp);
            uint16_t span = w == 2 ? 2 : 1;
            bool same = same_cell(cell, &shown->cells[y][x]);
            if (same && span == 2 && x + 1 < next->cols) {
                same = same_cell(&next->cells[y][x + 1], &shown->cells[y][x + 1]);
            }
            if (same) {
                x = (uint16_t)(x + span);
                continue;
            }
            bool bridged = false;
            if (placed && cur_row == y && x > cur_col && x - cur_col <= CV_GAP_MAX) {
                bridged = plain_run(next, y, cur_col, x, &pen);
            }
            if (bridged) {
                while (cur_col < x) {
                    emit_cell(t, &next->cells[y][cur_col], &pen);
                    cur_col++;
                }
            } else if (!placed || cur_row != y || cur_col != x) {
                term_put_at(t, (uint16_t)(x + 1), (uint16_t)(y + 1));
                placed = true;
            }
            emit_cell(t, cell, &pen);
            cur_row = y;
            cur_col = (uint16_t)(x + span);
            x = (uint16_t)(x + span);
        }
        y++;
    }
    term_puts(t, "\x1b[0m");
    cv_copy(shown, next);
}

/* ---- capture ---- */

void cv_capture_begin(cv_capture *cap, canvas *c) {
    cv_capture_begin_at(cap, c, 0, 0);
    cap->fragment = false;
}

void cv_capture_begin_at(cv_capture *cap, canvas *c, int32_t row, int32_t col) {
    cap->c = c;
    cap->row = 0;
    cap->col = 0;
    cap->row0 = row;
    cap->col0 = col;
    cap->fragment = true;
    cap->pen = blank_cell;
}

static void cap_sgr(cv_capture *cap, const uint32_t *ps, size_t n) {
    if (n == 0) {
        cap->pen = blank_cell;
        return;
    }
    size_t i = 0;
    while (i < n) {
        uint32_t p = ps[i];
        if (p == 0) {
            cap->pen = blank_cell;
        } else if (p == 1) {
            cap->pen.attr = (uint8_t)(cap->pen.attr | CV_A_BOLD);
        } else if (p == 2) {
            cap->pen.attr = (uint8_t)(cap->pen.attr | CV_A_DIM);
        } else if (p == 7) {
            cap->pen.attr = (uint8_t)(cap->pen.attr | CV_A_REV);
        } else if (p == 27) {
            cap->pen.attr = (uint8_t)(cap->pen.attr & (uint8_t)(0xFFU ^ CV_A_REV));
        } else if (p >= 30 && p <= 37) {
            cap->pen.fg = (int16_t)(p - 30);
        } else if (p >= 90 && p <= 97) {
            cap->pen.fg = (int16_t)(p - 90 + 8);
        } else if (p >= 40 && p <= 47) {
            cap->pen.bg = (int16_t)(p - 40);
        } else if (p >= 100 && p <= 107) {
            cap->pen.bg = (int16_t)(p - 100 + 8);
        } else if (p == 38 && i + 2 < n && ps[i + 1] == 5) {
            cap->pen.fg = (int16_t)ps[i + 2];
            i += 2;
        } else if (p == 48 && i + 2 < n && ps[i + 1] == 5) {
            cap->pen.bg = (int16_t)ps[i + 2];
            i += 2;
        } else if (p == 39) {
            cap->pen.fg = CV_COLOR_DEFAULT;
        } else if (p == 49) {
            cap->pen.bg = CV_COLOR_DEFAULT;
        }
        i++;
    }
}

static void cap_putc(cv_capture *cap, uint32_t cp) {
    canvas *c = cap->c;
    cv_cell saved = c->pen;
    c->pen = cap->pen;
    int32_t w = put_cell(c, cap->row0 + cap->row, cap->col0 + cap->col, cp);
    c->pen = saved;
    if (w == 0) {
        w = 1; /* a rune with no width of its own still costs the source a step */
    }
    cap->col += w;
}

void cv_capture_feed(cv_capture *cap, const uint8_t *data, size_t n) {
    canvas *c = cap->c;
    utf8_dec dec;
    utf8_dec_init(&dec);
    size_t i = 0;
    while (i < n) {
        uint8_t b = data[i];
        if (b == 0x1B && i + 1 < n && data[i + 1] == '[') {
            i += 2;
            uint32_t ps[8] = {0};
            size_t np = 0;
            bool have_digit = false;
            while (i < n) {
                uint8_t f = data[i];
                if (f >= '0' && f <= '9') {
                    if (np < 8) {
                        ps[np] = (ps[np] * 10) + (uint32_t)(f - '0');
                    }
                    have_digit = true;
                    i++;
                    continue;
                }
                if (f == ';') {
                    if (np < 8) {
                        np++;
                    }
                    have_digit = true;
                    i++;
                    continue;
                }
                break;
            }
            if (have_digit && np < 8) {
                np++;
            }
            if (i >= n) {
                return;
            }
            uint8_t final = data[i];
            i++;
            int32_t count = np > 0 && ps[0] > 0 ? (int32_t)ps[0] : 1;
            if (final == 'm') {
                cap_sgr(cap, ps, np);
            } else if (final == 'H' || final == 'f') {
                cap->row = np > 0 && ps[0] > 0 ? (int32_t)ps[0] - 1 : 0;
                cap->col = np > 1 && ps[1] > 0 ? (int32_t)ps[1] - 1 : 0;
            } else if (final == 'A') {
                cap->row -= count;
            } else if (final == 'B') {
                cap->row += count;
            } else if (final == 'C') {
                cap->col += count;
            } else if (final == 'D') {
                cap->col -= count;
            } else if (final == 'G') {
                cap->col = np > 0 && ps[0] > 0 ? (int32_t)ps[0] - 1 : 0;
            } else if (final == 'J' && !cap->fragment) {
                cv_clear(c);
            } else if (final == 'K' && !cap->fragment) {
                int32_t x = cap->col;
                while (x < (int32_t)c->cols) {
                    if (x >= 0 && cap->row >= 0 && cap->row < (int32_t)c->rows) {
                        c->cells[cap->row][x] = blank_cell;
                    }
                    x++;
                }
            }
            continue;
        }
        i++;
        if (b == '\r') {
            cap->col = 0;
            continue;
        }
        if (b == '\n') {
            cap->row++;
            continue;
        }
        if (b < 0x20) {
            continue;
        }
        uint32_t cp = 0;
        int resync = 0;
        utf8_result r = utf8_dec_feed(&dec, b, &cp, &resync);
        if (r == UTF8_RUNE) {
            cap_putc(cap, cp);
            continue;
        }
        if (r == UTF8_ERROR && resync != 0) {
            r = utf8_dec_feed(&dec, b, &cp, &resync);
            if (r == UTF8_RUNE) {
                cap_putc(cap, cp);
            }
        }
    }
}
