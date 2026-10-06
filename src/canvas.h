#ifndef FT_CANVAS_H
#define FT_CANVAS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "term.h"

/* The screen as a grid of cells. Everything that draws writes cells here and
   only cv_flush turns cells into bytes, so no painter has to know about
   escape sequences, and what reaches the terminal is only what changed. */

enum {
    /* Jumping the cursor costs about eight bytes, so a short run of
       unchanged cells is cheaper to rewrite than to skip. It also keeps
       words whole in the stream, which matters when reading a trace. */
    CV_GAP_MAX = 8,
};

#define CV_A_BOLD 0x1U
#define CV_A_DIM 0x2U
#define CV_A_REV 0x4U

/* Every byte value 0..255 is a real palette index, so the "no colour of its
   own" mark cannot be one of them: it is -1, the same value a screen writes as
   C_DEFAULT. Colour 255 used to collide with it and came out of art as the
   terminal's default. */
#define CV_COLOR_DEFAULT (-1)

/* cp == 0 marks the right half of a double-width rune: it belongs to the
   cell on its left and is never emitted on its own.

   Nine bytes of information were costing twelve, and a grid is a million of
   them, so the fields are bit-fields packed into one word. Three rules hold
   this together and breaking any of them is silent:

   - every field's base type has the same size and alignment. uint64_t and
     int64_t share a storage unit; put a 32-bit type among them and each
     change of width opens a new one, growing the cell to sixteen bytes —
     worse than the struct this replaced.
   - fg and bg are spelled signed on purpose, because CV_COLOR_DEFAULT is
     -1 and plain int bit-fields have implementation-defined signedness.
   - the widths sum to exactly 64, spare included. A padding bit is
     indeterminate, and raw — which is how two cells are compared, once per
     cell per frame — would report equal cells as different.

   raw therefore means something only inside one build: bit-field order is
   implementation-defined, so it is for comparing and copying, never for a
   file or a wire. */
typedef union {
    uint64_t raw;
    struct {
        uint64_t cp : 22; /* Unicode ends at 0x10FFFF */
        int64_t fg : 10;  /* 0..255 index the xterm palette, CV_COLOR_DEFAULT is none */
        int64_t bg : 10;
        uint64_t attr : 4;
        uint64_t spare : 18;
    };
} cv_cell;

/* _Static_assert, not static_assert: the keyword spelling works in C11, C17
   and C23 alike, and the macro would need <assert.h>, which a freestanding
   build does not have. */
_Static_assert(sizeof(cv_cell) == 8, "a cell is one word; check the field widths");

/* Text as bytes and a length: what a script hands over is not terminated,
   and neither is the text inside a piece of ANSI art. */
typedef struct {
    const uint8_t *ptr;
    size_t len;
} cv_text;

cv_text cv_cstr(const char *s);

/* Display columns the text occupies, which is not its length in runes. */
size_t cv_text_width(cv_text s);

typedef struct {
    cv_cell cells[TERM_ROWS_MAX][TERM_COLS_MAX];
    uint16_t rows;
    uint16_t cols;
    cv_cell pen; /* cp unused: the colors and attributes drawing calls use */
} canvas;

/* The three buffers a session needs: what the painter composed, what the
   effect chain turned it into, and what the terminal is showing. A
   transition reads shown as the old screen and target as the new one. */
typedef struct {
    canvas target;
    canvas work;
    canvas shown;
} compositor;

/* Sizes the canvas (clamping to the maxima), blanks it and resets the pen. */
void cv_reset(canvas *c, uint16_t rows, uint16_t cols);
void cv_clear(canvas *c);
void cv_pen_reset(canvas *c);
void cv_pen(canvas *c, int16_t fg, int16_t bg, uint8_t attr);

/* Positions are row before column and start at zero; sizes are rows before
   columns. Anything outside the canvas is clipped, including negatives.
   cv_put writes the text once and returns the columns it advanced; cv_fill
   and cv_box repeat it along the run, so a two-rune string draws "-=-=-=".
   Empty text leaves the cells untouched. */
int32_t cv_put(canvas *c, int32_t row, int32_t col, cv_text s);
void cv_fill(canvas *c, int32_t row, int32_t col, int32_t rows, int32_t cols, cv_text s);

/* Moves a rectangle of cells by `by` rows inside the canvas — negative is
   up — and blanks the rows it came off, with the pen, as cv_fill would.
   The display is memory a program can address, and this is the one move
   it cannot write cell by cell without paying for every one of them. */
void cv_scroll(canvas *c, int32_t row, int32_t col, int32_t rows, int32_t cols, int32_t by);

/* border holds 8 strings clockwise from the top-left corner: corner, top,
   corner, right, corner, bottom, corner, left. An empty element leaves that
   part alone, which is how a box gets no bottom or sits over art. */
void cv_box(canvas *c, int32_t row, int32_t col, int32_t rows, int32_t cols,
            const cv_text border[8]);

void cv_blit(canvas *dst, int32_t row, int32_t col, const canvas *src);

/* Copies the used region only: the grids are large and the frame loop runs
   this on every tick. */
void cv_copy(canvas *dst, const canvas *src);

/* Emits where next differs from shown, then copies next into shown. A size
   change repaints everything. */
void cv_flush(term *t, canvas *shown, const canvas *next);

/* Reads ANSI into cells: absolute and relative cursor moves, erase, the
   sixteen colours and the 256-colour pair, CR/LF and UTF-8. That is what the
   host's own painters emit and also what a drawing program writes, which is
   how a piece of art becomes cells.

   With an origin (cv_capture_begin_at) the bytes land as a fragment at that
   position, clipped to the canvas, and screen-wide erases are ignored: art
   dropped into a corner must not wipe what is around it. */
typedef struct {
    canvas *c;
    int32_t row;
    int32_t col;
    int32_t row0;
    int32_t col0;
    bool fragment;
    cv_cell pen;
} cv_capture;

void cv_capture_begin(cv_capture *cap, canvas *c);
void cv_capture_begin_at(cv_capture *cap, canvas *c, int32_t row, int32_t col);
void cv_capture_feed(cv_capture *cap, const uint8_t *data, size_t n);

#endif
