#ifndef FT_PAGER_H
#define FT_PAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "term.h"
#include "term_config.h"

enum {
    PAGER_TEXT_CAP = FT_CFG_PAGER_TEXT_CAP,
    PAGER_LINES_MAX = FT_CFG_PAGER_LINES_MAX,
    PAGER_ROWS_MAX = FT_CFG_PAGER_ROWS_MAX,
    PAGER_TAB_WIDTH = 4,
    PAGER_NAME_MAX = 256,
    PAGER_PATTERN_MAX = 64,
};

/* One wrapped display row: byte range into text. A row holds at most cols
   runes, so len always fits 16 bits. sgr and link carry the style in effect
   where the row starts (offset + 1, 0 meaning none): a row that begins
   inside bold text, or inside a hyperlink, has to reopen it — an OSC 8 left
   dangling would turn the rest of the screen into a link. */
typedef struct {
    uint32_t off;
    uint32_t sgr;
    uint32_t link;
    uint16_t len;
    uint16_t indent; /* blank columns before it: a wrapped list item hangs */
} pager_row;

typedef struct {
    char name[PAGER_NAME_MAX];
    uint8_t text[PAGER_TEXT_CAP];
    size_t text_len;
    bool truncated;
    uint32_t lstart[PAGER_LINES_MAX];
    size_t nlines;
    pager_row rows[PAGER_ROWS_MAX];
    size_t nrows;
    size_t top;  /* first visible display row */
    size_t sgr;  /* wrap cursor: style in effect, offset + 1 */
    size_t link; /* wrap cursor: open hyperlink, offset + 1 */
    uint8_t esc; /* escape state carried across load calls */
    /* /text, as less: what is looked for (shown reversed where it is),
       whether it is being typed, and whether the last look found nothing */
    uint8_t pat[PAGER_PATTERN_MAX];
    size_t patlen;
    bool typing;
    bool not_found;
} pager;

void pager_reset(pager *p, const char *name);

/* Appends rendered bytes: keeps the renderer's escape sequences, drops \r
   and other control bytes, expands tabs. Content beyond the cap is dropped
   and flagged as truncated. */
void pager_load(pager *p, const uint8_t *data, size_t n);

/* Wraps at the terminal's width and paints from the top, over whatever
   screen the caller gave it (an alternate one); pager_hide puts back the
   terminal modes it set. */
void pager_show(pager *p, term *t);
void pager_hide(term *t);

/* A key while it is showing: false when the reader closed it (q, Esc), and
   the caller takes the screen back. / looks for text as less does (Enter
   to look, Esc to give up), n and N go to the next and the one before;
   lower case finds either case, a capital only itself. */
bool pager_key(pager *p, term *t, uint32_t cp);
void pager_resize(pager *p, term *t);

#endif
