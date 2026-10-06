#include "term.h"

#include <string.h>

#include "utf8.h"

void term_init(term *t, uint16_t cols, uint16_t rows) {
    memset(t, 0, sizeof(*t));
    term_resize(t, cols, rows);
}

void term_resize(term *t, uint16_t cols, uint16_t rows) {
    t->cols = cols < TERM_COLS_MAX ? cols : TERM_COLS_MAX;
    t->rows = rows < TERM_ROWS_MAX ? rows : TERM_ROWS_MAX;
}

void term_write(term *t, const uint8_t *data, size_t n) {
    if (t->sink != NULL && t->sink(t->sink_ctx, data, n)) {
        return;
    }
    if (n > TERM_OUT_CAP - t->out_len) {
        t->out_overflow = true; /* host broke the drain contract; drop, don't corrupt */
        return;
    }
    memcpy(t->out + t->out_len, data, n);
    t->out_len += n;
}

void term_puts(term *t, const char *s) {
    term_write(t, (const uint8_t *)s, strlen(s));
}

void term_put_u32(term *t, uint32_t n) {
    char buf[10];
    size_t i = sizeof(buf);
    if (n == 0) {
        term_puts(t, "0");
        return;
    }
    while (n > 0 && i > 0) {
        i--;
        buf[i] = (char)('0' + (n % 10));
        n /= 10;
    }
    term_write(t, (const uint8_t *)(buf + i), sizeof(buf) - i);
}

size_t term_out_pending(const term *t) {
    return t->out_len;
}

size_t term_out_read(term *t, uint8_t *dst, size_t max) {
    size_t n = t->out_len < max ? t->out_len : max;
    memcpy(dst, t->out, n);
    memmove(t->out, t->out + n, t->out_len - n);
    t->out_len -= n;
    return n;
}

bool term_app_enter(term *t, const term_app *app, void *ctx) {
    if (t->napps >= TERM_APPS_MAX) {
        return false;
    }
    if (t->napps == 0) {
        term_puts(t, "\x1b[?2004h"); /* pastes arrive marked while an app is up */
    }
    if (!t->alt) {
        term_puts(t, "\x1b[?1049h"); /* the stack owns the alternate screen */
        t->alt = true;
    }
    t->apps[t->napps] = app;
    t->ctxs[t->napps] = ctx;
    t->napps++;
    return true;
}

bool term_app_leave(term *t) {
    if (t->napps > 0) {
        t->napps--;
    }
    if (t->napps > 0) {
        return false;
    }
    /* leave no attribute behind: SGR, cursor visibility and wheel mode all
       survive the buffer switch and would bleed into whatever had the
       terminal before. */
    term_puts(t, "\x1b[0m\x1b[?25h\x1b[?1007l\x1b[?2004l\x1b[?1049l\x1b[0m");
    t->alt = false;
    t->pasting = false;
    return true;
}

const term_app *term_app_top(const term *t) {
    return t->napps > 0 ? t->apps[t->napps - 1] : NULL;
}

void *term_app_ctx(const term *t) {
    return t->napps > 0 ? t->ctxs[t->napps - 1] : NULL;
}

void term_flash(term *t, const char *note) {
    t->note[0] = '\0';
    if (note != NULL) {
        size_t n = strlen(note);
        while (n > 0 && (note[n - 1] == '\r' || note[n - 1] == '\n')) {
            n--; /* flash lines are single-line by definition */
        }
        if (n >= sizeof(t->note)) {
            n = sizeof(t->note) - 1;
        }
        memcpy(t->note, note, n);
        t->note[n] = '\0';
    }
    const term_app *app = term_app_top(t);
    if (app != NULL && app->on_resize != NULL) {
        app->on_resize(term_app_ctx(t));
    }
}

void term_put_spaces(term *t, size_t n) {
    while (n > 0) {
        term_puts(t, " ");
        n--;
    }
}

void term_put_at(term *t, uint16_t col, uint16_t row) {
    term_puts(t, "\x1b[");
    term_put_u32(t, row);
    term_puts(t, ";");
    term_put_u32(t, col);
    term_puts(t, "H");
}

size_t term_put_fit(term *t, const char *s, size_t maxw) {
    size_t sw = utf8_swidth(s);
    if (sw <= maxw) {
        term_puts(t, s);
        return sw;
    }
    if (maxw == 0) {
        return 0;
    }
    utf8_dec d;
    utf8_dec_init(&d);
    size_t w = 0;
    size_t i = 0;
    size_t last_fit = 0; /* byte offset that fits in maxw-1 columns */
    while (s[i] != '\0') {
        uint32_t cp = 0;
        int resync = 0;
        utf8_result r = utf8_dec_feed(&d, (uint8_t)s[i], &cp, &resync);
        i++;
        if (r != UTF8_RUNE) {
            if (r == UTF8_ERROR) {
                break;
            }
            continue;
        }
        size_t cw = (size_t)utf8_width(cp);
        if (w + cw > maxw - 1) {
            break;
        }
        w += cw;
        last_fit = i;
    }
    term_write(t, (const uint8_t *)s, last_fit);
    term_puts(t, "\xe2\x80\xa6"); /* … */
    return w + 1;
}

size_t term_fmt_human(char buf[8], uint32_t n) {
    uint32_t tenths;
    char unit;
    if (n < 1024) {
        tenths = n * 10U;
        unit = '\0';
    } else if (n < 1024U * 1024U) {
        tenths = (n * 10U) / 1024U;
        unit = 'K';
    } else {
        tenths = ((n / 1024U) * 10U) / 1024U;
        unit = 'M';
    }
    uint32_t whole = tenths / 10U;
    size_t len = 0;
    char digits[10];
    size_t nd = 0;
    do {
        digits[nd] = (char)('0' + (whole % 10U));
        nd++;
        whole /= 10U;
    } while (whole > 0);
    while (nd > 0) {
        nd--;
        buf[len] = digits[nd];
        len++;
    }
    if (unit != '\0') {
        buf[len] = '.';
        buf[len + 1] = (char)('0' + (tenths % 10U));
        buf[len + 2] = unit;
        len += 3;
    }
    buf[len] = '\0';
    return len;
}

void term_clipboard(term *t, const uint8_t *p, size_t len) {
    term_puts(t, "\x1b]52;c;");
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0;
    while (i < len) {
        uint32_t v = (uint32_t)p[i] << 16U;
        size_t have = 1;
        if (i + 1 < len) {
            v |= (uint32_t)p[i + 1] << 8U;
            have++;
        }
        if (i + 2 < len) {
            v |= p[i + 2];
            have++;
        }
        char q[4];
        q[0] = b64[(v >> 18U) & 63U];
        q[1] = b64[(v >> 12U) & 63U];
        q[2] = have > 1 ? b64[(v >> 6U) & 63U] : '=';
        q[3] = have > 2 ? b64[v & 63U] : '=';
        term_write(t, (const uint8_t *)q, 4);
        i += 3;
    }
    term_puts(t, "\x1b\\");
}
