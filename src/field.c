#include "field.h"

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
}

/* The field's own keys, besides the runes: the caret moves and erases
   where it is, the way a line is edited anywhere. */
void field_key(field *f, uint32_t cp) {
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
