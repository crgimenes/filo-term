#ifndef FILO_TERM_APP_H
#define FILO_TERM_APP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "canvas.h"
#include "field.h"
#include "filo.h"
#include "keyin.h"
#include "term.h"

/* A Filo program that owns a terminal: a bundle member with the entries
   init (required: it declares the program's globals, which are sealed when
   it returns), draw (required), and key, input and tick when it has them.
   This is the core, with no terminal of its own: bytes come in (app_input),
   bytes for the terminal go out through the term, and a test drives it the
   same way a tty does (tty.h is the POSIX one).

   Every program gets the same base, on every host that runs one: the core,
   strings and the math computed without libm; paint; input-at,
   input-text, cursor-at, clipboard, random, keep-canvas, done; and the
   globals W, H, KEY, MS, FRESH, ARG, VERSION, A_BOLD, A_DIM, A_REV,
   C_DEFAULT and KEY_*. What a program needs past that it registers itself
   (app_spec.extend), and a host that lacks a name refuses the bundle when
   it loads, so no program asks which host it is on. */

/* The base's budgets are those of the smallest host that runs these
   programs (rocchetto's screens): a program that keeps to them runs on the
   desktop and there alike. */
enum {
    APP_STEPS_INIT = 200000,
    APP_STEPS_EVENT = 20000, /* a key or a line only moves state around */
    APP_STEPS_DRAW = 200000, /* a paint, or the tick of one that animates */
    APP_ERROR_MAX = 256,
};

#ifndef APP_CFG_PERSISTENT
#define APP_CFG_PERSISTENT (512U * 1024U) /* the program, its globals */
#endif
#ifndef APP_CFG_RUN
#define APP_CFG_RUN (256U * 1024U) /* what one event or one paint makes */
#endif

typedef struct app app;

/* What a program is, apart from any run of it. The program defines one
   named app_program, which is how tools/appvm finds it. */
typedef struct {
    const char *name;    /* errors say it; it is also the bundle member */
    const char *version; /* VERSION */
    bool libm;           /* sqrt, sin and the rest: a small host has none */
    uint32_t steps_init; /* 0 for each: the base's */
    uint32_t steps_event;
    uint32_t steps_draw;
    /* the program's own builtins and globals, after the base; false (with
       the context's error) refuses the start */
    bool (*extend)(app *a, filo_ctx *ctx);
} app_spec;

extern const app_spec app_program;

struct app {
    /* the caller's, set before app_start */
    void *user;
    const char *arg; /* ARG; NULL is "" */
    uint64_t seed;   /* random's stream */

    const app_spec *spec;
    filo_ctx ctx;
    uint8_t persistent[APP_CFG_PERSISTENT];
    uint8_t run[APP_CFG_RUN];
    const filo_unit *unit;

    term t;
    canvas target; /* what the program painted */
    canvas shown;  /* what the terminal has */
    keyin kin;

    field in;
    bool has_cursor; /* cursor-at: the caret without a field */
    int32_t caret_row;
    int32_t caret_col;

    bool keep_canvas; /* the cells stay from one paint to the next */
    bool fresh;       /* the next paint starts from blank cells anyway */
    bool running;     /* an entry is on the VM: a paint now would re-enter it */
    bool done;
    char error[APP_ERROR_MAX]; /* the last failure, on the bottom row */
};

/* The program's context with the base and the program's own builtins, and
   no program loaded: what app_start builds on, and what tools/appvm lists
   for filo check. */
bool app_context(app *a, const app_spec *spec, char *why, size_t cap);

/* Loads the spec's member of fbb (which must outlive a), runs init, takes
   the terminal at cols x rows and paints. False with why when the bundle
   lacks what it needs, init fails, or a name is missing. */
bool app_start(app *a, const app_spec *spec, const uint8_t *fbb, size_t fbb_len, uint16_t cols,
               uint16_t rows, char *why, size_t cap);

void app_input(app *a, const uint8_t *data, size_t n);
void app_tick(app *a, uint32_t ms);
void app_resize(app *a, uint16_t cols, uint16_t rows);
bool app_done(const app *a);

/* Whether the program animates: it has a tick, so time matters to it. */
bool app_animates(const app *a);

/* The app a builtin of the program runs under. */
app *app_of(filo_ctx *ctx);

/* Paints the program again over a terminal that cannot be trusted: what a
   full-screen view pushed over it (term_app_enter) calls once it leaves. */
void app_repaint(app *a);

/* ctx's names, one a line, builtins then "global NAME" for each global:
   the profile filo build -vm compiles against and filo check -vm holds a
   bundle to. The length written, 0 when it does not fit. */
size_t app_profile(const filo_ctx *ctx, char *dst, size_t cap);

#endif
