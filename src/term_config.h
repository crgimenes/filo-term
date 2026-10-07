#ifndef TERM_CONFIG_H
#define TERM_CONFIG_H

/* Every limit of filo-term that decides how much memory a build takes: the
   defaults for a desktop or the web. A build overrides what it needs with
   -D and changes nothing else (a small device sets them small). */

/* The terminal, and with it the three canvases of the compositor at eight
   bytes a cell, plus the programs that paint their own grid. Past this the
   size is clamped, not clipped: see term.h. 300x100 costs 720 KB. */
#ifndef FT_CFG_COLS_MAX
#define FT_CFG_COLS_MAX 300
#endif
#ifndef FT_CFG_ROWS_MAX
#define FT_CFG_ROWS_MAX 100
#endif

/* Bytes waiting for the host to drain. A host that drains every tick needs
   far less than one that does not. 256 KB. */
#ifndef FT_CFG_OUT_CAP
#define FT_CFG_OUT_CAP (256 * 1024)
#endif

/* The pager: the article being read, its line table and its wrapped rows.
   The largest single part of a session after the user's files, and it is
   sized for the longest article on the site. 2.2 MB. */
#ifndef FT_CFG_PAGER_TEXT_CAP
#define FT_CFG_PAGER_TEXT_CAP (1024 * 1024)
#endif
#ifndef FT_CFG_PAGER_LINES_MAX
#define FT_CFG_PAGER_LINES_MAX 32768
#endif
#ifndef FT_CFG_PAGER_ROWS_MAX
#define FT_CFG_PAGER_ROWS_MAX 65536
#endif

/* The editor's buffer and its line table. 128 KB together. */
#ifndef FT_CFG_TB_CAP
#define FT_CFG_TB_CAP (64 * 1024)
#endif
#ifndef FT_CFG_TB_LINES_MAX
#define FT_CFG_TB_LINES_MAX 8192
#endif

/* What the editor's cut and copy hold: a build without the full-screen
   editor never fills it. 16 KB. */
#ifndef FT_CFG_TB_CLIP
#define FT_CFG_TB_CLIP (16 * 1024)
#endif

/* The editor's undo: what its edits took away, and where. Typing costs
   one record a line, a deletion the bytes it deleted; when it fills, the
   oldest half goes. Reformatting a whole file needs the file's size. A
   small device sets it small: 4 KB still undoes the last few edits. 64 KB. */
#ifndef FT_CFG_TB_UNDO
#define FT_CFG_TB_UNDO (64 * 1024)
#endif

/* The markdown renderer's line: a longer one is drawn in pieces, and a
   style open across the cut breaks there. 8 KB. */
#ifndef FT_CFG_MD_LINE
#define FT_CFG_MD_LINE 8192
#endif

/* What tb-format lays Filo source out in: about 21 bytes for each byte of
   ordinary source, so 2 MB formats files up to some 90 KB, and a larger one
   is refused. */
#ifndef FT_CFG_TB_FMT_MEM
#define FT_CFG_TB_FMT_MEM (2 * 1024 * 1024)
#endif

#endif
