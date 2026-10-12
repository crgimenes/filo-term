#ifndef FT_FIELD_H
#define FT_FIELD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "canvas.h"
#include "term_config.h"

/* A line a program types into: its text lives here, in C, and a program
   reads it when the line is done (input-text), so it never has to keep it.
   The keys edit it as a line is edited anywhere; the draw places it on a
   canvas and says where the caret is. */
enum { FIELD_CAP = FT_CFG_FIELD_CAP };

typedef struct {
    uint8_t buf[FIELD_CAP];
    size_t len;
    size_t cur; /* byte offset of the caret, on a rune boundary */
    uint32_t runes;
    uint32_t max; /* runes it takes; 0 is as many as fit */
    bool on;      /* the paint in progress placed it */
} field;

void field_draw(field *f, canvas *c, int32_t row, int32_t col, int32_t width, uint32_t most,
                bool secret, int32_t *caret_row, int32_t *caret_col);

/* One line of a text as wrapped: where it starts and ends in the bytes.
   A space the text broke at is neither line's. */
typedef struct {
    size_t start;
    size_t end;
} field_line;

/* Breaks text into lines no wider than maxw cells: at the last space of
   the line when it has one, inside the word when it has none; every other
   space stays, so a drawing made of spaces keeps its shape. The last line
   is always there, empty or not. Returns how many, at most cap. */
size_t field_wrap(const uint8_t *buf, size_t len, size_t maxw, field_line *out, size_t cap);

/* The same text over rows lines of width, broken at spaces, the caret
   anywhere in it: an area. More lines than rows scroll so the caret's
   line shows. With one row, or a secret, it is field_draw. */
void field_draw_area(field *f, canvas *c, int32_t row, int32_t col, int32_t width, int32_t rows,
                     uint32_t most, bool secret, int32_t *caret_row, int32_t *caret_col);

/* Whether a key is the field's: runes, erasing, and the caret's moves once
   there is text to move over. The rest go to the program. */
bool field_takes(const field *f, uint32_t cp);
void field_key(field *f, uint32_t cp);
void field_clear(field *f);

#endif
