#include "paint.h"

#include <string.h>

/* The canvas the builtins draw on; the host says which when it registers
   them, and a builtin has no other way to reach its program's state. */
static canvas *(*canvas_of)(filo_ctx *ctx) = NULL;

/* ---- arguments ---- */

static bool whole(double x, int32_t *out) {
    if (!(x > -2147483648.0 && x < 2147483648.0)) {
        return false;
    }
    if ((double)(int64_t)x != x) {
        return false;
    }
    *out = (int32_t)x;
    return true;
}

int paint_arg_whole(filo_ctx *ctx, const filo_value *v, const char *what, int32_t *out) {
    double x = 0;
    if (filo_arg_num(ctx, v, &x) != FILO_OK) {
        return FILO_ERR;
    }
    if (!whole(x, out)) {
        return filo_fail2(ctx, what, " must be a whole number");
    }
    return FILO_OK;
}

/* A coordinate or a size names a cell, and half a column does not exist, so
   it takes the cell the position falls in. Centring divides by two and
   placing by three, and which of those come out whole depends on the size of
   the terminal: refusing the fraction would break a screen only on some
   windows, which is the worst way for it to break. */
int paint_arg_cell(filo_ctx *ctx, const filo_value *v, const char *what, int32_t *out) {
    double x = 0;
    if (filo_arg_num(ctx, v, &x) != FILO_OK) {
        return FILO_ERR;
    }
    if (!(x > -2147483648.0 && x < 2147483648.0)) {
        return filo_fail2(ctx, what, " is not a position on the screen");
    }
    int32_t n = (int32_t)x;
    if ((double)n > x) {
        n--; /* the cast rounds toward zero; cells go down */
    }
    *out = n;
    return FILO_OK;
}

int paint_arg_text(filo_ctx *ctx, const filo_value *v, cv_text *out) {
    filo_str s = {NULL, 0};
    if (filo_arg_str(ctx, v, &s) != FILO_OK) {
        return FILO_ERR;
    }
    out->ptr = s.ptr;
    out->len = s.len;
    return FILO_OK;
}

/* -1 is the terminal's own colour; 0..255 index the xterm palette. */
int paint_arg_color(filo_ctx *ctx, const filo_value *v, int16_t *out) {
    int32_t n = 0;
    if (paint_arg_whole(ctx, v, "colour", &n) != FILO_OK) {
        return FILO_ERR;
    }
    if (n < 0) {
        *out = CV_COLOR_DEFAULT;
        return FILO_OK;
    }
    if (n > 255) {
        return filo_fail(ctx, "colour must be between -1 and 255");
    }
    *out = (int16_t)n;
    return FILO_OK;
}

static int b_print_at(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    if (n != 3) {
        return filo_fail(ctx, "print-at expects a row, a column and text");
    }
    int32_t row = 0;
    int32_t col = 0;
    cv_text t = {NULL, 0};
    if (paint_arg_cell(ctx, &a[0], "row", &row) != FILO_OK ||
        paint_arg_cell(ctx, &a[1], "column", &col) != FILO_OK ||
        paint_arg_text(ctx, &a[2], &t) != FILO_OK) {
        return FILO_ERR;
    }
    *out = filo_num(cv_put(canvas_of(ctx), row, col, t));
    return FILO_OK;
}

static int rect_args(filo_ctx *ctx, const filo_value *a, int32_t *row, int32_t *col, int32_t *rows,
                     int32_t *cols) {
    if (paint_arg_cell(ctx, &a[0], "row", row) != FILO_OK ||
        paint_arg_cell(ctx, &a[1], "column", col) != FILO_OK ||
        paint_arg_cell(ctx, &a[2], "height", rows) != FILO_OK ||
        paint_arg_cell(ctx, &a[3], "width", cols) != FILO_OK) {
        return FILO_ERR;
    }
    return FILO_OK;
}

static int b_fill(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    if (n != 5) {
        return filo_fail(ctx, "fill expects a row, a column, a height, a width and text");
    }
    int32_t row = 0;
    int32_t col = 0;
    int32_t rows = 0;
    int32_t cols = 0;
    cv_text t = {NULL, 0};
    if (rect_args(ctx, a, &row, &col, &rows, &cols) != FILO_OK ||
        paint_arg_text(ctx, &a[4], &t) != FILO_OK) {
        return FILO_ERR;
    }
    cv_fill(canvas_of(ctx), row, col, rows, cols, t);
    *out = filo_bool(true);
    return FILO_OK;
}

/* (scroll row col rows cols by): the rectangle moves inside the canvas,
   by rows, negative for up, and what it came off goes blank. */
static int b_scroll(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    if (n != 5) {
        return filo_fail(ctx, "scroll expects a row, a column, a height, a width and a distance");
    }
    int32_t row = 0;
    int32_t col = 0;
    int32_t rows = 0;
    int32_t cols = 0;
    int32_t by = 0;
    if (rect_args(ctx, a, &row, &col, &rows, &cols) != FILO_OK ||
        paint_arg_cell(ctx, &a[4], "distance", &by) != FILO_OK) {
        return FILO_ERR;
    }
    cv_scroll(canvas_of(ctx), row, col, rows, cols, by);
    *out = filo_bool(true);
    return FILO_OK;
}

static int b_box(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    if (n != 5) {
        return filo_fail(ctx, "box expects a row, a column, a height, a width and 8 border pieces");
    }
    int32_t row = 0;
    int32_t col = 0;
    int32_t rows = 0;
    int32_t cols = 0;
    filo_seq l = {NULL, 0};
    if (rect_args(ctx, a, &row, &col, &rows, &cols) != FILO_OK ||
        filo_arg_list(ctx, &a[4], &l) != FILO_OK) {
        return FILO_ERR;
    }
    if (l.len != 8) {
        return filo_fail(ctx, "box expects 8 border pieces, clockwise from the top-left corner");
    }
    cv_text border[8];
    uint32_t i = 0;
    while (i < 8) {
        if (paint_arg_text(ctx, &l.items[i], &border[i]) != FILO_OK) {
            return FILO_ERR;
        }
        i++;
    }
    cv_box(canvas_of(ctx), row, col, rows, cols, border);
    *out = filo_bool(true);
    return FILO_OK;
}

static int b_fg(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    if (n != 1) {
        return filo_fail(ctx, "fg expects one colour");
    }
    canvas *c = canvas_of(ctx);
    int16_t v = 0;
    if (paint_arg_color(ctx, &a[0], &v) != FILO_OK) {
        return FILO_ERR;
    }
    cv_pen(c, v, (int16_t)c->pen.bg, (uint8_t)c->pen.attr);
    *out = filo_bool(true);
    return FILO_OK;
}

static int b_bg(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    if (n != 1) {
        return filo_fail(ctx, "bg expects one colour");
    }
    canvas *c = canvas_of(ctx);
    int16_t v = 0;
    if (paint_arg_color(ctx, &a[0], &v) != FILO_OK) {
        return FILO_ERR;
    }
    cv_pen(c, (int16_t)c->pen.fg, v, (uint8_t)c->pen.attr);
    *out = filo_bool(true);
    return FILO_OK;
}

static int b_attr(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    if (n != 1) {
        return filo_fail(ctx, "attr expects the attributes to use, 0 for none");
    }
    canvas *c = canvas_of(ctx);
    int32_t v = 0;
    if (paint_arg_whole(ctx, &a[0], "attributes", &v) != FILO_OK) {
        return FILO_ERR;
    }
    if (v < 0 || v > (int32_t)(CV_A_BOLD | CV_A_DIM | CV_A_REV)) {
        return filo_fail(ctx, "attributes are a sum of A_BOLD, A_DIM and A_REV");
    }
    cv_pen(c, (int16_t)c->pen.fg, (int16_t)c->pen.bg, (uint8_t)v);
    *out = filo_bool(true);
    return FILO_OK;
}

static int b_text_width(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    if (n != 1) {
        return filo_fail(ctx, "text-width expects one string");
    }
    cv_text t = {NULL, 0};
    if (paint_arg_text(ctx, &a[0], &t) != FILO_OK) {
        return FILO_ERR;
    }
    *out = filo_num((double)cv_text_width(t));
    return FILO_OK;
}

/* The bits a cell has left over, carried by the pen like a colour: after
   (tag n) every rune drawn holds n where no one sees it, and tag-at reads it
   back — the attribute byte of text mode, used for data. */
enum { PAINT_TAG_MAX = 0x3FFFF }; /* the eighteen bits of cv_cell.spare */

static int b_tag(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    if (n != 1) {
        return filo_fail(ctx, "tag expects one number, 0 for none");
    }
    int32_t v = 0;
    if (paint_arg_whole(ctx, &a[0], "tag", &v) != FILO_OK) {
        return FILO_ERR;
    }
    if (v < 0 || v > PAINT_TAG_MAX) {
        return filo_fail(ctx, "tag must be between 0 and 262143");
    }
    canvas_of(ctx)->pen.spare = (uint64_t)v;
    *out = filo_bool(true);
    return FILO_OK;
}

/* The cell a read names, or NULL off the canvas. A read there answers -1,
   which a game can take as a wall without a bounds check of its own. */
static int cell_ref(filo_ctx *ctx, const filo_value *a, uint32_t n, const char *name,
                    const cv_cell **cell) {
    if (n != 2) {
        return filo_fail2(ctx, name, " expects a row and a column");
    }
    int32_t row = 0;
    int32_t col = 0;
    if (paint_arg_cell(ctx, &a[0], "row", &row) != FILO_OK ||
        paint_arg_cell(ctx, &a[1], "column", &col) != FILO_OK) {
        return FILO_ERR;
    }
    const canvas *c = canvas_of(ctx);
    *cell = NULL;
    if (row >= 0 && col >= 0 && row < (int32_t)c->rows && col < (int32_t)c->cols) {
        *cell = &c->cells[row][col];
    }
    return FILO_OK;
}

/* (cell-at row col): the code point drawn there; 0 is the right half of a
   wide rune. */
static int b_cell_at(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    const cv_cell *cell = NULL;
    if (cell_ref(ctx, a, n, "cell-at", &cell) != FILO_OK) {
        return FILO_ERR;
    }
    *out = filo_num(cell != NULL ? (double)cell->cp : -1.0);
    return FILO_OK;
}

static int b_tag_at(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *out) {
    const cv_cell *cell = NULL;
    if (cell_ref(ctx, a, n, "tag-at", &cell) != FILO_OK) {
        return FILO_ERR;
    }
    *out = filo_num(cell != NULL ? (double)cell->spare : -1.0);
    return FILO_OK;
}

void paint_register(filo_ctx *ctx, canvas *(*canvas_fn)(filo_ctx *ctx)) {
    canvas_of = canvas_fn;
    (void)filo_register_builtin(ctx, "print-at", b_print_at);
    (void)filo_register_builtin(ctx, "fill", b_fill);
    (void)filo_register_builtin(ctx, "box", b_box);
    (void)filo_register_builtin(ctx, "scroll", b_scroll);
    (void)filo_register_builtin(ctx, "fg", b_fg);
    (void)filo_register_builtin(ctx, "bg", b_bg);
    (void)filo_register_builtin(ctx, "attr", b_attr);
    (void)filo_register_builtin(ctx, "text-width", b_text_width);
    (void)filo_register_builtin(ctx, "tag", b_tag);
    (void)filo_register_builtin(ctx, "cell-at", b_cell_at);
    (void)filo_register_builtin(ctx, "tag-at", b_tag_at);
}
