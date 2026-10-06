#ifndef FT_TERM_H
#define FT_TERM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "term_config.h"

/* The terminal a session talks to: how big the grid is and the bytes on
   their way out to it. Nothing here knows what a shell, a file or a program
   is, so the hosts — posix, wasm, a UART on a board — differ only in who
   drains the buffer. */

enum {
    /* The grid stops growing here. A window stretched past three hundred
       columns is not a terminal anyone reads, and every cell past the
       ceiling costs 36 bytes across the three canvases. Above it the size
       is clamped rather than clipped, so the layout arithmetic and the grid
       always agree on one number — the shell's line editor included, which
       is only wrong for a typed line longer than the ceiling itself.
       Measured against real windows: a 4K screen shrunk to the smallest
       readable type gives 235x66, an ordinary one 176x49. */
    TERM_COLS_MAX = FT_CFG_COLS_MAX,
    TERM_ROWS_MAX = FT_CFG_ROWS_MAX,

    TERM_OUT_CAP = FT_CFG_OUT_CAP,
    TERM_APPS_MAX = 4,
    TERM_NOTE_MAX = 64,
};

/* A full-screen app: it owns the keys while it is on top and paints the
   alternate screen. ctx is whatever the caller handed to term_app_enter,
   so the runtime never needs a type for the state an app keeps. */
typedef struct {
    void (*on_key)(void *ctx, uint32_t cp);
    void (*on_resize)(void *ctx);
    void (*on_tick)(void *ctx, uint32_t ms);
} term_app;

typedef struct term {
    uint16_t cols;
    uint16_t rows;
    /* the alternate screen is up; SGR is NOT buffer-local, so leaving it
       always resets */
    bool alt;

    /* between CSI 200~ and 201~: apps hold their repaint to the end */
    bool pasting;

    /* The app stack: menu over area over reader. Empty means whoever owns
       the session (a shell, a firmware's main loop) has the keys. */
    const term_app *apps[TERM_APPS_MAX];
    void *ctxs[TERM_APPS_MAX];
    size_t napps;
    char note[TERM_NOTE_MAX]; /* one line the app on top shows */

    uint8_t out[TERM_OUT_CAP];
    size_t out_len;
    bool out_overflow;

    /* Output going somewhere that is not the terminal — a command whose
       stdout was redirected to a file. True means the sink took the bytes
       and nothing reaches the grid. */
    bool (*sink)(void *ctx, const uint8_t *data, size_t n);
    void *sink_ctx;
} term;

void term_init(term *t, uint16_t cols, uint16_t rows);

/* The new size, clamped to the ceiling, and nothing else: telling the apps
   belongs to whoever owns them. */
void term_resize(term *t, uint16_t cols, uint16_t rows);

void term_write(term *t, const uint8_t *data, size_t n);
void term_puts(term *t, const char *s);
void term_put_u32(term *t, uint32_t n);
void term_put_spaces(term *t, size_t n);

/* Moves the cursor to a 1-based column and row. */
void term_put_at(term *t, uint16_t col, uint16_t row);

/* Writes s truncated to maxw display columns (ellipsis when cut). Returns
   the columns actually written. */
size_t term_put_fit(term *t, const char *s, size_t maxw);

/* Formats n as "999", "4.3K" or "1.2M" into buf; returns the length. */
size_t term_fmt_human(char buf[8], uint32_t n);

/* Pushes an app and gives it the alternate screen. False when the stack is
   full and nothing was pushed: a host that loses a screen is better than
   one that scribbles past the end. */
bool term_app_enter(term *t, const term_app *app, void *ctx);

/* Pops the top app. True when the stack emptied, which is when the
   terminal was restored and the caller has the keys back. */
bool term_app_leave(term *t);

const term_app *term_app_top(const term *t);
void *term_app_ctx(const term *t);

/* Keeps note as the flash line and repaints the app on top. */
void term_flash(term *t, const char *note);

size_t term_out_pending(const term *t);
size_t term_out_read(term *t, uint8_t *dst, size_t max);

/* Hands text to the terminal's clipboard the way terminals agree on: OSC
   52, base64. The browser glue answers it with the clipboard API; iTerm2,
   kitty, WezTerm and Alacritty answer it themselves; Terminal.app ignores
   it. */
void term_clipboard(term *t, const uint8_t *p, size_t len);

#endif
