#ifndef FT_TBX_H
#define FT_TBX_H

#include "filo.h"
#include "tbuf.h"

/* The text buffer (tbuf) as Filo builtins, apart from any host: the lines
   as shown (plain, or in highlighted stretches), the cursor, the view, the
   editing keys, undo, find, go to a line, and the clipboard of copy, cut
   and paste — and the bytes, for a hex view (tb-size, tb-offset, tb-seek,
   tb-byte-at, tb-set-byte, tb-hex-row, tb-find-bytes, hex, bin, cp437) and
   tb-format, the text laid out as filofmt lays out Filo.
   Where the text comes from and where a save goes are the host's (tb-path
   and tb-save), and so are the function that gives the buffer of the
   program running and the one that says whether its text is what the file
   already holds (NULL: never), which is what lets tb-dirty forget an edit
   undone by hand. */
void tbx_register(filo_ctx *ctx, tbuf *(*buf_fn)(filo_ctx *ctx), bool (*saved_fn)(filo_ctx *ctx));

#endif
