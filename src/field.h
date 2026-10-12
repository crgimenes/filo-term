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
    uint32_t max;  /* runes it takes; 0 is as many as fit */
    int32_t rows;  /* the rows the last paint gave it: more than one is an area */
    bool on;       /* the paint in progress placed it */
    bool after_cr; /* the last key was a CR made a line break: an LF after it is the same one */
} field;

void field_draw(field *f, canvas *c, int32_t row, int32_t col, int32_t width, uint32_t most,
                bool secret, int32_t *caret_row, int32_t *caret_col);

/* One line of a text as wrapped: where it starts and ends in the bytes,
   and where the next one starts. The space or line break it ended on is
   neither line's; next is past the text when this is the last line. */
typedef struct {
    size_t start;
    size_t end;
    size_t next;
} field_line;

/* The line that starts at from, no wider than maxw cells: it ends at a line
   break, or at its last space when it has one, or inside the word when it
   has none; every other space stays, so a drawing made of spaces keeps its
   shape. Walk a text with from = 0, then each line's next, while from is
   not past the end: an empty text, or one ending in a break, ends on an
   empty line. */
field_line field_wrap_line(const uint8_t *buf, size_t len, size_t maxw, size_t from);

/* The same text over rows lines of width, broken at spaces, the caret
   anywhere in it: an area. More lines than rows scroll so the caret's
   line shows. With one row, or a secret, it is field_draw. */
void field_draw_area(field *f, canvas *c, int32_t row, int32_t col, int32_t width, int32_t rows,
                     uint32_t most, bool secret, int32_t *caret_row, int32_t *caret_col);

/* Whether a key is the field's: runes, erasing, and the caret's moves once
   there is text to move over. The rest go to the program. */
bool field_takes(const field *f, uint32_t cp);

/* Whether a line-ending key hands the text over, as Enter does, rather
   than going into it. In an area a line break is text when it is Ctrl-J
   (LF) or comes inside a paste; the Enter key typed is CR and sends. */
bool field_submits(const field *f, uint32_t cp, bool pasting);
void field_key(field *f, uint32_t cp);
void field_clear(field *f);

#endif
