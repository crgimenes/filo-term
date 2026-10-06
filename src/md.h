#ifndef FT_MD_H
#define FT_MD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hl.h"
#include "term_config.h"

/* Markdown to ANSI, streaming and line buffered. Only the renderer ever
   emits escapes: every ESC in the source is dropped, so a file cannot drive
   the terminal. Output carries no width-dependent layout, so the pager stays
   free to re-wrap it at any size. */

enum {
    MD_LINE_MAX = FT_CFG_MD_LINE,
    MD_URL_MAX = 512,
    /* a horizontal rule is a fixed short run, never the terminal width: the
       pager re-wraps its rows but never re-renders them */
    MD_RULE_WIDTH = 24,
};

/* Hugo's ref shortcode names a page by slug: the host says where that page
   lives ("/posts/x.md"), or false when it has no such page. */
typedef bool (*md_slug_fn)(void *user, const uint8_t *slug, size_t n, char *path, size_t cap);

typedef struct {
    /* the host's, kept across md_reset: where rendered bytes go, the site a
       "/..." link points into ("" for none: the link stays text), and the
       slugs */
    hl_emit emit;
    void *user;
    const char *base;
    md_slug_fn slug;

    bool enabled; /* markdown files only; .txt and friends stream verbatim */
    bool source;  /* a source file: no markdown, every line highlighted */
    uint8_t line[MD_LINE_MAX];
    size_t line_len;
    bool line_open; /* the block prefix for this line is already out */
    uint8_t esc;    /* strips escapes arriving from the source */

    bool in_code;
    char fence_ch;
    size_t fence_len;
    hl_state hl;

    uint8_t block;   /* md_block */
    uint8_t heading; /* 1..6 while block is a heading */
    bool bold;
    bool italic;
    bool code;
    bool link;
} md;

/* Starts a document, keeping the host's fields. */
void md_reset(md *r, bool enabled);

/* Not markdown, but not plain either: every line goes through the
   highlighter as the language says. HL_NONE is the same as md_reset(r,
   false), which streams the bytes as they came. */
void md_source(md *r, hl_lang lang);
void md_feed(md *r, const uint8_t *data, size_t n);
void md_end(md *r);

#endif
