#ifndef FT_PAINT_H
#define FT_PAINT_H

#include <stdint.h>

#include "canvas.h"
#include "filo.h"

/* What a Filo program paints with: the builtins over a canvas, apart from
   any host — print-at, link-at, fill, box, fg, bg, attr, text-width, and the canvas
   read back as memory (tag, cell-at, tag-at). The host registers them with
   the function that gives the canvas of the program running; screens of
   a shell and programs of their own draw the same way. */
void paint_register(filo_ctx *ctx, canvas *(*canvas_fn)(filo_ctx *ctx));

/* The argument rules the painting builtins keep, for a host's own: a cell
   rounds down to the one a position falls in, a whole number refuses a
   fraction, a colour is -1 (the terminal's) or 0..255. */
int paint_arg_whole(filo_ctx *ctx, const filo_value *v, const char *what, int32_t *out);
int paint_arg_cell(filo_ctx *ctx, const filo_value *v, const char *what, int32_t *out);
int paint_arg_text(filo_ctx *ctx, const filo_value *v, cv_text *out);
int paint_arg_color(filo_ctx *ctx, const filo_value *v, int16_t *out);

/* (report-score value [track]): a game's result as it ends, for whatever
   board keeps scores (a host with none ignores it). value is a whole number
   from 0; track, when a game has more than one board (a level), is up to
   PAINT_TRACK_MAX - 1 letters a-z and digits. */
enum { PAINT_TRACK_MAX = 16 };
int paint_arg_score(filo_ctx *ctx, const filo_value *a, uint32_t n, int32_t *value,
                    char track[PAINT_TRACK_MAX]);

#endif
