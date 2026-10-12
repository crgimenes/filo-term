#include "field.h"

#include <stdint.h>
#include <string.h>

#include "keys.h"
#include "utf8.h"

static size_t rune_len_at(const uint8_t *buf, size_t at) {
    uint8_t b = buf[at];
    if ((b & 0xE0U) == 0xC0U) {
        return 2;
    }
    if ((b & 0xF0U) == 0xE0U) {
        return 3;
    }
    if ((b & 0xF8U) == 0xF0U) {
        return 4;
    }
    return 1;
}

static size_t runes_in(const uint8_t *buf, size_t len) {
    size_t n = 0;
    size_t at = 0;
    while (at < len) {
        at += rune_len_at(buf, at);
        n++;
    }
    return n;
}

/* How many bytes of text fit in maxw columns. */
static size_t fit_bytes(const uint8_t *buf, size_t len, size_t maxw) {
    size_t at = 0;
    while (at < len) {
        size_t k = rune_len_at(buf, at);
        if (at + k > len || cv_text_width((cv_text){buf, at + k}) > maxw) {
            break;
        }
        at += k;
    }
    return at;
}

/* Draws the field where the screen asked for it and says where the caret
   landed, so the terminal's own cursor can sit there. Content wider than
   the field scrolls, keeping the caret in view. */
void field_draw(field *f, canvas *c, int32_t row, int32_t col, int32_t width, uint32_t most,
                bool secret, int32_t *caret_row, int32_t *caret_col) {
    f->on = true;
    f->max = most;
    f->rows = 1;

    cv_text typed = {f->buf, f->len};
    size_t shown_w = 0;
    size_t caret_w = 0;
    if (secret) {
        size_t stars = f->runes;
        if (stars > (size_t)width - 1) {
            stars = (size_t)width - 1;
        }
        cv_fill(c, row, col, 1, (int32_t)stars, cv_cstr("*"));
        shown_w = stars;
        caret_w = runes_in(f->buf, f->cur);
        if (caret_w > stars) {
            caret_w = stars;
        }
    } else {
        /* the window slides so the caret is always in it: drop leading
           runes until the text up to the caret fits, then show what fits
           after it */
        size_t from = 0;
        while (from < f->cur &&
               cv_text_width((cv_text){f->buf + from, f->cur - from}) > (size_t)width - 1) {
            from += rune_len_at(f->buf, from);
        }
        typed.ptr = f->buf + from;
        typed.len = fit_bytes(f->buf + from, f->len - from, (size_t)width - 1);
        shown_w = cv_text_width(typed);
        (void)cv_put(c, row, col, typed);
        caret_w = cv_text_width((cv_text){f->buf + from, f->cur - from});
    }
    cv_fill(c, row, col + (int32_t)shown_w, 1, width - (int32_t)shown_w, cv_cstr(" "));
    *caret_row = row;
    *caret_col = col + (int32_t)caret_w;
}

field_line field_wrap_line(const uint8_t *buf, size_t len, size_t maxw, size_t from) {
    size_t at = from;
    size_t w = 0;
    size_t space = SIZE_MAX; /* the last space of this line, if any */
    while (at < len) {
        if (buf[at] == '\n') {
            return (field_line){from, at, at + 1};
        }
        size_t k = rune_len_at(buf, at);
        if (at + k > len) {
            k = len - at;
        }
        size_t rw = cv_text_width((cv_text){buf + at, k});
        if (w + rw > maxw && at > from) {
            if (buf[at] == ' ') {
                return (field_line){from, at, at + 1}; /* the space itself is the break */
            }
            if (space != SIZE_MAX) {
                return (field_line){from, space, space + 1};
            }
            return (field_line){from, at, at};
        }
        if (buf[at] == ' ') {
            space = at;
        }
        w += rw;
        at += k;
    }
    return (field_line){from, len, len + 1};
}

void field_draw_area(field *f, canvas *c, int32_t row, int32_t col, int32_t width, int32_t rows,
                     uint32_t most, bool secret, int32_t *caret_row, int32_t *caret_col) {
    if (rows <= 1 || secret) {
        field_draw(f, c, row, col, width, most, secret, caret_row, caret_col);
        return;
    }
    f->on = true;
    f->max = most;
    f->rows = rows;

    /* two walks over the text instead of a table of its lines: the first
       finds the caret's line, the second draws the window around it */
    size_t maxw = (size_t)width - 1; /* the caret needs a cell after a full line */
    size_t on = 0;
    field_line caret = {0, 0, 1};
    size_t i = 0;
    for (size_t at = 0; at <= f->len; i++) {
        field_line l = field_wrap_line(f->buf, f->len, maxw, at);
        if (f->cur >= l.start) { /* the last line starting at or before the caret */
            on = i;
            caret = l;
        }
        at = l.next;
    }
    size_t top = on >= (size_t)rows ? on - (size_t)rows + 1 : 0;
    cv_fill(c, row, col, rows, width, cv_cstr(" "));
    i = 0;
    for (size_t at = 0; at <= f->len && i < top + (size_t)rows; i++) {
        field_line l = field_wrap_line(f->buf, f->len, maxw, at);
        if (i >= top) {
            (void)cv_put(c, row + (int32_t)(i - top), col,
                         (cv_text){f->buf + l.start, l.end - l.start});
        }
        at = l.next;
    }
    size_t caret_w = cv_text_width((cv_text){f->buf + caret.start, f->cur - caret.start});
    if (caret_w > maxw) {
        caret_w = maxw;
    }
    *caret_row = row + (int32_t)(on - top);
    *caret_col = col + (int32_t)caret_w;
}

static void field_insert(field *f, uint32_t cp) {
    if (f->max > 0 && f->runes >= f->max) {
        return;
    }
    uint8_t buf[UTF8_MAX_BYTES];
    size_t n = utf8_encode(buf, cp);
    if (f->len + n > sizeof(f->buf)) {
        return;
    }
    memmove(f->buf + f->cur + n, f->buf + f->cur, f->len - f->cur);
    memcpy(f->buf + f->cur, buf, n);
    f->len += n;
    f->cur += n;
    f->runes++;
}

static void field_erase(field *f, size_t at, size_t n) {
    memmove(f->buf + at, f->buf + at + n, f->len - (at + n));
    f->len -= n;
    f->runes--;
    if (f->cur > at) {
        f->cur = at;
    }
}

void field_clear(field *f) {
    f->len = 0;
    f->cur = 0;
    f->runes = 0;
    f->after_cr = false;
}

/* The field's own keys, besides the runes: the caret moves and erases
   where it is, the way a line is edited anywhere. */
void field_key(field *f, uint32_t cp) {
    bool cr = f->after_cr;
    f->after_cr = false;
    if (cp == '\r' || cp == '\n') {
        if (cp == '\n' && cr) {
            return; /* the LF of a CRLF: the CR already broke the line */
        }
        f->after_cr = cp == '\r';
        field_insert(f, '\n');
        return;
    }
    if (cp == 0x08 || cp == 0x7F) {
        if (f->cur > 0) {
            size_t k = utf8_last_rune_len(f->buf, f->cur);
            field_erase(f, f->cur - (k > 0 ? k : 1), k > 0 ? k : 1);
        }
    } else if (cp == FT_KEY_DEL) {
        if (f->cur < f->len) {
            field_erase(f, f->cur, rune_len_at(f->buf, f->cur));
        }
    } else if (cp == 0x15) {
        field_clear(f);
    } else if (cp == FT_KEY_LEFT) {
        if (f->cur > 0) {
            size_t k = utf8_last_rune_len(f->buf, f->cur);
            f->cur -= k > 0 ? k : 1;
        }
    } else if (cp == FT_KEY_RIGHT) {
        if (f->cur < f->len) {
            f->cur += rune_len_at(f->buf, f->cur);
        }
    } else if (cp == FT_KEY_HOME || cp == 0x01) {
        f->cur = 0;
    } else if (cp == FT_KEY_END || cp == 0x05) {
        f->cur = f->len;
    } else {
        field_insert(f, cp);
    }
}

/* A screen with a field takes the keys that belong to typing: runes,
   erasing, and — once there is text to move over — the caret's own moves
   (Left, Right, Home, End, Delete). With the field empty those still
   reach the key hook, so a screen may steer by arrow until typing starts;
   Up, Down and anything with a modifier always do. */
bool field_takes(const field *f, uint32_t cp) {
    if (!f->on) {
        return false;
    }
    if (cp == '\r' || cp == '\n' || cp == 0x08 || cp == 0x7F || cp == 0x15 || cp == 0x01 ||
        cp == 0x05) {
        return true;
    }
    if (f->len > 0 && (cp == FT_KEY_LEFT || cp == FT_KEY_RIGHT || cp == FT_KEY_HOME ||
                       cp == FT_KEY_END || cp == FT_KEY_DEL)) {
        return true;
    }
    if (cp < 0x20 || ft_key_is_code(cp)) {
        return false;
    }
    return true;
}

bool field_submits(const field *f, uint32_t cp, bool pasting) {
    if (cp != '\r' && cp != '\n') {
        return false;
    }
    if (f->rows <= 1) {
        return true;
    }
    if (cp == '\n' || pasting) {
        return false;
    }
    return true;
}
