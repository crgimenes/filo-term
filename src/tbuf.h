#ifndef FT_TBUF_H
#define FT_TBUF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "term_config.h"

/* The editor's text: one flat byte array, a table of line starts rebuilt
   after every edit (64 KB is nothing to scan), a cursor as a byte offset,
   and an anchor when there is a selection. Columns are display columns:
   what the terminal shows, tabs to the next stop, wide runes two. No
   undo: neither had EDIT. */
enum {
    TB_CAP = FT_CFG_TB_CAP,
    TB_LINES_MAX = FT_CFG_TB_LINES_MAX,
    TB_CLIP = FT_CFG_TB_CLIP,
    TB_TAB = 4,
    TB_FIND_MAX = 128,
};

typedef struct {
    uint8_t text[TB_CAP];
    size_t len;
    size_t lines[TB_LINES_MAX]; /* byte offset where each line starts */
    size_t nlines;              /* at least 1: the empty text is one line */
    size_t cur;
    size_t goal; /* column wanted when moving up and down */
    bool anchored;
    size_t anchor;
    uint8_t clip[TB_CLIP];
    size_t clip_len;
    size_t top;  /* first line in view */
    size_t left; /* first column in view */
    bool dirty;
    /* the lowest offset an edit touched since the highlighter last looked
       (SIZE_MAX: none): what it knew of the lines before is still true */
    size_t changed;
} tbuf;

void tb_init(tbuf *t);
/* False when it does not fit (bytes or lines). */
bool tb_load(tbuf *t, const uint8_t *data, size_t n);

size_t tb_line_of(const tbuf *t, size_t off);
size_t tb_col_of(const tbuf *t, size_t off);
/* Start and length of line i, without its newline. */
void tb_line(const tbuf *t, size_t i, const uint8_t **p, size_t *n);
/* Byte offset at display column col of line i, or the line's end. */
size_t tb_off_at(const tbuf *t, size_t i, size_t col);

/* Editing at the cursor; a selection is replaced. */
bool tb_insert(tbuf *t, const uint8_t *bytes, size_t n);
void tb_backspace(tbuf *t);
void tb_delete(tbuf *t);
void tb_delete_line(tbuf *t);

/* Movement; with select the anchor stays (or is set), without it goes. */
void tb_left(tbuf *t, bool select);
void tb_right(tbuf *t, bool select);
void tb_up(tbuf *t, size_t n, bool select);
void tb_down(tbuf *t, size_t n, bool select);
void tb_home(tbuf *t, bool select);
void tb_end(tbuf *t, bool select);
void tb_doc_home(tbuf *t, bool select);
void tb_doc_end(tbuf *t, bool select);
void tb_goto_line(tbuf *t, size_t i);

/* The selection as an ordered byte range; false when there is none. */
bool tb_selection(const tbuf *t, size_t *a, size_t *b);
void tb_copy(tbuf *t);
void tb_cut(tbuf *t);
bool tb_paste(tbuf *t);

/* Moves top and left so the cursor shows in rows x cols. */
void tb_view(tbuf *t, size_t rows, size_t cols);

/* Next match after the cursor, wrapping; selects it. */
bool tb_find(tbuf *t, const uint8_t *needle, size_t n);

/* The byte at off replaced, or added at the end: how a hex view edits.
   False when it does not fit — past the end of a full buffer, or a newline
   the line table has no room for. */
bool tb_set_byte(tbuf *t, size_t off, uint8_t v);

/* The cursor to a byte offset, clamped to the text. A hex view puts it on
   any byte; at_rune backs it off to the start of the rune it falls in, which
   the line view needs. */
void tb_seek(tbuf *t, size_t off, bool at_rune);
/* The whole text replaced by data as one edit (what a formatter does), the
   cursor to cur, backed off to the start of its rune; the clipboard and the
   view stay. False, nothing changed, when it does not fit. */
bool tb_replace(tbuf *t, const uint8_t *data, size_t n, size_t cur);

#endif
