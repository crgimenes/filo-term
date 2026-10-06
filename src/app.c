#include "app.h"

#include <string.h>

#include "filo_math.h"
#include "filo_nolibc.h"
#include "filo_strings.h"
#include "keys.h"
#include "paint.h"

#ifdef APP_LIBM
#include "filo_libc.h"
#endif

static const term_app core;

app *app_of(filo_ctx *ctx) {
    return ctx->host.user;
}

static canvas *canvas_of(filo_ctx *ctx) {
    return &app_of(ctx)->target;
}

/* The error line, built without a libc: the parts in order, cut at the
   end of the buffer. */
static void say(char *dst, size_t cap, const char *const *parts, size_t n) {
    size_t at = 0;
    for (size_t i = 0; i < n && at + 1 < cap; i++) {
        for (const char *p = parts[i]; *p != '\0' && at + 1 < cap; p++) {
            dst[at++] = *p;
        }
    }
    dst[at] = '\0';
}

static const char *u32_text(uint32_t v, char buf[12]) {
    char *p = buf + 11;
    *p = '\0';
    do {
        *--p = (char)('0' + (v % 10U));
        v /= 10U;
    } while (v > 0);
    return p;
}

/* splitmix64: one multiply-xorshift step a number, and a stream good enough
   for a game that two runs should not play alike. */
static uint64_t next_random(uint64_t *s) {
    uint64_t z = (*s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
}

/* ---- the base's builtins ---- */

static int b_input_at(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    if (n != 5) {
        return filo_fail(ctx, "input-at expects a row, a column, a width, a maximum and "
                              "whether to hide what is typed");
    }
    int32_t row = 0;
    int32_t col = 0;
    int32_t width = 0;
    int32_t most = 0;
    if (paint_arg_cell(ctx, &a[0], "row", &row) != FILO_OK ||
        paint_arg_cell(ctx, &a[1], "column", &col) != FILO_OK ||
        paint_arg_cell(ctx, &a[2], "width", &width) != FILO_OK ||
        paint_arg_cell(ctx, &a[3], "maximum", &most) != FILO_OK) {
        return FILO_ERR;
    }
    if (a[4].kind != FILO_BOOL) {
        return filo_fail(ctx, "input-at expects #t or #f for hiding what is typed");
    }
    if (width < 1) {
        return filo_fail(ctx, "a field needs at least one column");
    }
    app *p = app_of(ctx);
    field_draw(&p->in, &p->target, row, col, width, most > 0 ? (uint32_t)most : 0, a[4].u.b,
               &p->caret_row, &p->caret_col);
    *out = filo_bool(true);
    return FILO_OK;
}

static int b_input_text(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    (void)a;
    if (n != 0) {
        return filo_fail(ctx, "input-text takes no arguments");
    }
    const app *p = app_of(ctx);
    *out = filo_string(p->in.buf, (uint32_t)p->in.len);
    return FILO_OK;
}

static int b_cursor_at(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    if (n != 2) {
        return filo_fail(ctx, "cursor-at expects a row and a column");
    }
    app *p = app_of(ctx);
    if (paint_arg_cell(ctx, &a[0], "row", &p->caret_row) != FILO_OK ||
        paint_arg_cell(ctx, &a[1], "column", &p->caret_col) != FILO_OK) {
        return FILO_ERR;
    }
    p->has_cursor = true;
    *out = filo_bool(true);
    return FILO_OK;
}

enum { CLIP_MAX = 64 * 1024 };

static int b_clipboard(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    if (n != 1 || a[0].kind != FILO_STRING) {
        return filo_fail(ctx, "clipboard expects a string");
    }
    size_t len = a[0].u.str.len;
    term_clipboard(&app_of(ctx)->t, a[0].u.str.ptr, len > CLIP_MAX ? CLIP_MAX : len);
    *out = filo_bool(true);
    return FILO_OK;
}

/* random: a whole number in [0, n). The host's and not the language's: the
   corpus is shared with the Go engine, and a corpus case cannot have an
   answer that changes every run. */
static int b_random(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    if (n != 1) {
        return filo_fail(ctx, "random expects one bound");
    }
    int32_t bound = 0;
    if (paint_arg_whole(ctx, &a[0], "bound", &bound) != FILO_OK) {
        return FILO_ERR;
    }
    if (bound <= 0) {
        return filo_fail(ctx, "random expects a bound above zero");
    }
    *out = filo_num((double)(next_random(&app_of(ctx)->seed) % (uint64_t)bound));
    return FILO_OK;
}

/* (keep-canvas): the cells stay from one paint to the next instead of being
   blanked before each draw, so a program can keep its state in them. */
static int b_keep_canvas(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    (void)a;
    if (n != 0) {
        return filo_fail(ctx, "keep-canvas takes no argument");
    }
    app_of(ctx)->keep_canvas = true;
    *out = filo_bool(true);
    return FILO_OK;
}

static int b_done(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    (void)a;
    if (n != 0) {
        return filo_fail(ctx, "done takes no argument");
    }
    app_of(ctx)->done = true;
    *out = filo_bool(true);
    return FILO_OK;
}

/* ---- running the program ---- */

static void set_num(app *a, const char *name, double v) {
    (void)filo_set_global(&a->ctx, name, filo_num(v));
}

static uint32_t steps(uint32_t chosen, uint32_t fallback) {
    return chosen > 0 ? chosen : fallback;
}

/* One entry of the program. A failure is kept for the bottom row, said the
   way a compiler says it. */
static bool run(app *a, const char *entry, uint32_t budget) {
    filo_limits limits = {budget, 0};
    filo_value v;
    if (filo_bc_run(&a->ctx, a->unit, entry, &limits, &v) == FILO_OK) {
        return true;
    }
    uint32_t line = 0;
    uint32_t col = 0;
    char lbuf[12];
    char cbuf[12];
    if (filo_error_at(&a->ctx, &line, &col)) {
        const char *parts[] = {
            a->spec->name,
            ": ",
            entry,
            ".filo:",
            u32_text(line, lbuf),
            ":",
            u32_text(col, cbuf),
            ": ",
            filo_error(&a->ctx),
        };
        say(a->error, sizeof(a->error), parts, sizeof(parts) / sizeof(parts[0]));
        return false;
    }
    const char *parts[] = {
        a->spec->name, ": ", entry, ".filo: ", filo_error(&a->ctx),
    };
    say(a->error, sizeof(a->error), parts, sizeof(parts) / sizeof(parts[0]));
    return false;
}

static bool has(const app *a, const char *entry) {
    return filo_bc_has(a->unit, entry);
}

static bool on_top(const app *a) {
    return term_app_top(&a->t) == &core;
}

static void paint(app *a) {
    if (!on_top(a) || a->t.pasting) {
        return; /* a view over it has the screen; a paste ends with one paint */
    }
    bool fresh = a->fresh;
    if (!a->keep_canvas) {
        fresh = true;
    }
    if (a->target.rows != a->t.rows || a->target.cols != a->t.cols) {
        fresh = true;
    }
    if (fresh) {
        cv_reset(&a->target, a->t.rows, a->t.cols);
    } else {
        cv_pen_reset(&a->target);
    }
    a->fresh = false;
    (void)filo_set_global(&a->ctx, "FRESH", filo_bool(fresh));
    set_num(a, "W", a->t.cols);
    set_num(a, "H", a->t.rows);
    a->in.on = false;
    a->has_cursor = false;
    (void)run(a, "draw", steps(a->spec->steps_draw, APP_STEPS_DRAW));
    cv_flush(&a->t, &a->shown, &a->target);
    if (a->error[0] != '\0') {
        /* the failure goes over the bottom row and not into the cells, which
           a program that keeps its canvas is still using */
        term_put_at(&a->t, 1, a->t.rows);
        term_puts(&a->t, "\x1b[0;38;5;15;48;5;1m\x1b[2K ");
        (void)term_put_fit(&a->t, a->error, a->t.cols > 1 ? a->t.cols - 1U : 0);
        term_puts(&a->t, "\x1b[0m");
        a->shown.rows = 0; /* the next flush paints that row back */
    }
    if (a->in.on || a->has_cursor) {
        term_put_at(&a->t, (uint16_t)(a->caret_col + 1), (uint16_t)(a->caret_row + 1));
        term_puts(&a->t, "\x1b[?25h");
    } else {
        term_puts(&a->t, "\x1b[?25l");
    }
}

/* The program is over: the terminal goes back to whoever had it. */
static void finish(app *a) {
    if (on_top(a)) {
        (void)term_app_leave(&a->t);
    }
}

/* A key goes to the field when the last paint placed one and the key is
   the field's (Enter hands its text to the input entry), and to the key
   entry otherwise; then the screen is painted again. */
static void core_key(void *ctx, uint32_t cp) {
    app *a = ctx;
    a->error[0] = '\0';
    uint32_t budget = steps(a->spec->steps_event, APP_STEPS_EVENT);
    if (field_takes(&a->in, cp)) {
        if (cp == '\r' || cp == '\n') {
            if (has(a, "input")) {
                (void)run(a, "input", budget);
            }
            field_clear(&a->in);
        } else {
            field_key(&a->in, cp);
        }
    } else if (has(a, "key")) {
        set_num(a, "KEY", cp);
        (void)run(a, "key", budget);
    }
    if (a->done) {
        finish(a);
        return;
    }
    paint(a);
}

static void core_tick(void *ctx, uint32_t ms) {
    app *a = ctx;
    if (!has(a, "tick")) {
        return;
    }
    set_num(a, "MS", ms);
    (void)run(a, "tick", steps(a->spec->steps_draw, APP_STEPS_DRAW));
    if (a->done) {
        finish(a);
        return;
    }
    paint(a);
}

static void core_resize(void *ctx) {
    app_repaint(ctx);
}

static const term_app core = {
    .on_key = core_key,
    .on_resize = core_resize,
    .on_tick = core_tick,
};

void app_repaint(app *a) {
    a->shown.rows = 0; /* whatever the terminal holds now is not to be trusted */
    a->fresh = true;
    paint(a);
}

static void on_input(void *user, keyin_event ev, uint32_t cp) {
    app *a = user;
    const term_app *top = term_app_top(&a->t);
    switch (ev) {
    case KEYIN_PASTE_BEGIN:
        a->t.pasting = true;
        return;
    case KEYIN_PASTE_END:
        a->t.pasting = false;
        paint(a);
        return;
    case KEYIN_ESC:
        cp = FT_KEY_ESC;
        break;
    case KEYIN_KEY:
        if (cp == 0x03) {
            cp = FT_KEY_ESC; /* raw mode turns Ctrl-C into a byte; it leaves, as Esc does */
        }
        break;
    }
    if (top != NULL) {
        top->on_key(term_app_ctx(&a->t), cp);
    }
}

void app_input(app *a, const uint8_t *data, size_t n) {
    for (size_t i = 0; i < n && !a->done; i++) {
        keyin_feed(&a->kin, data[i], on_input, a);
    }
}

void app_tick(app *a, uint32_t ms) {
    keyin_tick(&a->kin, ms, on_input, a);
    const term_app *top = term_app_top(&a->t);
    if (top != NULL && !a->done) {
        top->on_tick(term_app_ctx(&a->t), ms);
    }
}

void app_resize(app *a, uint16_t cols, uint16_t rows) {
    term_resize(&a->t, cols, rows);
    a->shown.rows = 0;
    const term_app *top = term_app_top(&a->t);
    if (top != NULL) {
        top->on_resize(term_app_ctx(&a->t));
    }
}

bool app_done(const app *a) {
    return a->done;
}

bool app_animates(const app *a) {
    if (a->unit == NULL) {
        return false;
    }
    return has(a, "tick");
}

/* ---- starting ---- */

static void globals(app *a) {
    static const struct {
        const char *name;
        double v;
    } g[] = {
        {"KEY", 0},
        {"MS", 0},
        {"A_BOLD", CV_A_BOLD},
        {"A_DIM", CV_A_DIM},
        {"A_REV", CV_A_REV},
        {"C_DEFAULT", -1},
        {"KEY_UP", FT_KEY_UP},
        {"KEY_DOWN", FT_KEY_DOWN},
        {"KEY_LEFT", FT_KEY_LEFT},
        {"KEY_RIGHT", FT_KEY_RIGHT},
        {"KEY_ENTER", '\r'},
        {"KEY_HOME", FT_KEY_HOME},
        {"KEY_END", FT_KEY_END},
        {"KEY_PGUP", FT_KEY_PGUP},
        {"KEY_PGDN", FT_KEY_PGDN},
        {"KEY_DEL", FT_KEY_DEL},
        {"KEY_INS", FT_KEY_INS},
        {"KEY_ESC", FT_KEY_ESC},
        {"KEY_SHIFT", FT_KEY_SHIFT},
        {"KEY_ALT", FT_KEY_ALT},
        {"KEY_CTRL", FT_KEY_CTRL},
    };
    for (size_t i = 0; i < sizeof(g) / sizeof(g[0]); i++) {
        set_num(a, g[i].name, g[i].v);
    }
    set_num(a, "W", a->t.cols);
    set_num(a, "H", a->t.rows);
    (void)filo_set_global(&a->ctx, "FRESH", filo_bool(true));
    (void)filo_set_global(&a->ctx, "ARG", filo_cstring(a->arg != NULL ? a->arg : ""));
    (void)filo_set_global(&a->ctx, "VERSION",
                          filo_cstring(a->spec->version != NULL ? a->spec->version : ""));
}

static bool fail(char *why, size_t cap, const char *a, const char *b, const char *c) {
    const char *parts[] = {a, b, c};
    say(why, cap, parts, 3);
    return false;
}

bool app_context(app *a, const app_spec *spec, char *why, size_t cap) {
    a->spec = spec;
    filo_host host = filo_nolibc_host;
    const filo_strings_fns *strings = &filo_nolibc_strings;
    const filo_math_fns *math = NULL; /* floor and its neighbours, computed without libm */
#ifdef APP_LIBM
    if (spec->libm) {
        filo_libc_install();
        host = filo_libc_host;
        strings = &filo_libc_strings;
        math = &filo_libc_math;
    }
#else
    if (spec->libm) {
        return fail(why, cap, spec->name, ": ", "built without libm (APP_LIBM)");
    }
#endif
    host.user = a;
    filo_init(&a->ctx, &host, a->persistent, sizeof(a->persistent), a->run, sizeof(a->run));
    (void)filo_strings_register(&a->ctx, strings);
    (void)filo_math_register(&a->ctx, math);
    paint_register(&a->ctx, canvas_of);
    (void)filo_register_builtin(&a->ctx, "input-at", b_input_at);
    (void)filo_register_builtin(&a->ctx, "input-text", b_input_text);
    (void)filo_register_builtin(&a->ctx, "cursor-at", b_cursor_at);
    (void)filo_register_builtin(&a->ctx, "clipboard", b_clipboard);
    (void)filo_register_builtin(&a->ctx, "random", b_random);
    (void)filo_register_builtin(&a->ctx, "keep-canvas", b_keep_canvas);
    /* a full table refuses the last ones first: this one standing means
       they all do */
    if (filo_register_builtin(&a->ctx, "done", b_done) != FILO_OK) {
        return fail(why, cap, spec->name, ": ", filo_error(&a->ctx));
    }
    globals(a);
    if (spec->extend != NULL && !spec->extend(a, &a->ctx)) {
        return fail(why, cap, spec->name, ": ", filo_error(&a->ctx));
    }
    return true;
}

bool app_start(app *a, const app_spec *spec, const uint8_t *fbb, size_t fbb_len, uint16_t cols,
               uint16_t rows, char *why, size_t cap) {
    memset(&a->in, 0, sizeof(a->in));
    a->has_cursor = false;
    a->keep_canvas = false;
    a->fresh = true;
    a->done = false;
    a->error[0] = '\0';
    a->unit = NULL;
    term_init(&a->t, cols, rows);
    cv_reset(&a->target, a->t.rows, a->t.cols);
    cv_reset(&a->shown, 0, 0);
    keyin_init(&a->kin);
    if (!app_context(a, spec, why, cap)) {
        return false;
    }
    const uint8_t *member = NULL;
    size_t len = 0;
    if (filo_bundle_find(&a->ctx, fbb, fbb_len, spec->name, &member, &len) != FILO_OK ||
        filo_bc_load(&a->ctx, member, len, &a->unit) != FILO_OK) {
        return fail(why, cap, spec->name, ": ", filo_error(&a->ctx));
    }
    if (!has(a, "init") || !has(a, "draw")) {
        return fail(why, cap, spec->name, ": ", "the program needs an init and a draw");
    }
    if (!run(a, "init", steps(spec->steps_init, APP_STEPS_INIT))) {
        return fail(why, cap, a->error, "", "");
    }
    /* From here the program's names are all there are: one typed wrong in
       a later edit fails loudly instead of making a new global. */
    filo_seal_globals(&a->ctx);
    if (!term_app_enter(&a->t, &core, a)) {
        return fail(why, cap, spec->name, ": ", "no room for the program on the terminal");
    }
    term_puts(&a->t, "\x1b[H\x1b[2J");
    paint(a);
    return true;
}

size_t app_profile(const filo_ctx *ctx, char *dst, size_t cap) {
    static const char global[] = "global ";
    size_t at = 0;
    for (uint32_t i = 0; i < ctx->nbuiltins + ctx->nsymbols; i++) {
        const char *s = NULL;
        const char *kind = "";
        if (i < ctx->nbuiltins) {
            s = ctx->builtins[i].name;
        } else if (ctx->defined[i - ctx->nbuiltins]) {
            s = ctx->symbols[i - ctx->nbuiltins];
            kind = global;
        }
        if (s == NULL) {
            continue;
        }
        size_t k = strlen(kind);
        size_t n = strlen(s);
        if (at + k + n + 1 >= cap) {
            return 0;
        }
        memcpy(dst + at, kind, k);
        memcpy(dst + at + k, s, n);
        dst[at + k + n] = '\n';
        at += k + n + 1;
    }
    dst[at] = '\0';
    return at;
}
