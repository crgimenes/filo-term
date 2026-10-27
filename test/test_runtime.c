/* The boundary gate: this links the runtime alone — term, canvas, utf8,
   tbuf — and no host at all. It draws a framed box with a status bar and
   checks the bytes that reach the terminal. If this file ever needs a host
   to compile, the boundary leaked and the build says so here. */

#include <stdio.h>
#include <string.h>

#include "canvas.h"
#include "hl.h"
#include "keys.h"
#include "md.h"
#include "pager.h"
#include "tbuf.h"
#include "term.h"
#include "utf8.h"

static term T;
static canvas SHOWN;
static canvas WORK;
static uint8_t OUT[TERM_OUT_CAP];
static size_t NFAIL;

static void check(bool ok, const char *what) {
    if (ok) {
        return;
    }
    printf("FAIL: %s\n", what);
    NFAIL++;
}

/* The drained bytes, NUL-terminated so the checks can use strstr. */
static const char *drain(void) {
    size_t n = term_out_read(&T, OUT, sizeof(OUT) - 1);
    OUT[n] = 0;
    return (const char *)OUT;
}

static void test_box_and_bar(void) {
    static const cv_text border[8] = {
        {(const uint8_t *)"+", 1}, {(const uint8_t *)"-", 1}, {(const uint8_t *)"+", 1},
        {(const uint8_t *)"|", 1}, {(const uint8_t *)"+", 1}, {(const uint8_t *)"-", 1},
        {(const uint8_t *)"+", 1}, {(const uint8_t *)"|", 1},
    };
    term_init(&T, 40, 8);
    cv_reset(&SHOWN, T.rows, T.cols);
    cv_reset(&WORK, T.rows, T.cols);

    cv_box(&WORK, 0, 0, 6, 20, border);
    cv_pen(&WORK, 2, CV_COLOR_DEFAULT, CV_A_BOLD);
    cv_put(&WORK, 2, 2, cv_cstr("caixa"));
    cv_pen_reset(&WORK);
    cv_pen(&WORK, CV_COLOR_DEFAULT, 4, 0);
    cv_put(&WORK, 7, 0, cv_cstr("status"));
    cv_pen_reset(&WORK);

    cv_flush(&T, &SHOWN, &WORK);
    const char *s = drain();
    check(strstr(s, "+------") != NULL, "box top drawn");
    check(strstr(s, "caixa") != NULL, "text drawn");
    check(strstr(s, "\x1b[0;1;38;5;2m") != NULL, "bold green pen emitted");
    check(strstr(s, "\x1b[0;48;5;4m") != NULL, "status background emitted");
    check(strstr(s, "status") != NULL, "status bar drawn");

    /* Nothing changed: the diff must emit nothing but the reset it opens
       with. That property is the whole reason the compositor exists — it is
       what makes a serial line usable. */
    cv_flush(&T, &SHOWN, &WORK);
    s = drain();
    check(strcmp(s, "\x1b[0m\x1b[0m") == 0, "unchanged frame emits no cells");

    cv_put(&WORK, 2, 2, cv_cstr("CAIXA"));
    cv_flush(&T, &SHOWN, &WORK);
    s = drain();
    check(strstr(s, "CAIXA") != NULL, "changed cell repainted");
    check(strstr(s, "status") == NULL, "untouched row not repainted");
}

static void test_output_and_width(void) {
    term_init(&T, 40, 8);
    term_puts(&T, "ok ");
    term_put_u32(&T, 4096);
    term_put_at(&T, 3, 7);
    term_put_spaces(&T, 2);
    check(strcmp(drain(), "ok 4096\x1b[7;3H  ") == 0, "output primitives");

    /* UTF-8 is a requirement here, not a feature: a cut must fall on a rune
       boundary and count display columns, not bytes. */
    term_init(&T, 40, 8);
    size_t w = term_put_fit(&T, "ação de guerra", 6);
    check(w == 6, "put_fit returns columns written");
    check(strcmp(drain(), "ação \xe2\x80\xa6") == 0, "put_fit cuts on a rune boundary");

    char buf[8];
    check(term_fmt_human(buf, 999) == 3 && strcmp(buf, "999") == 0, "fmt_human bytes");
    check(term_fmt_human(buf, 4403) == 4 && strcmp(buf, "4.2K") == 0, "fmt_human K");

    check(cv_text_width(cv_cstr("ação")) == 4, "text width counts runes");
    check(utf8_swidth("日本") == 4, "wide runes take two columns");
    check(utf8_width(0x2705) == 2 && utf8_width(0x231A) == 2 && utf8_width(0x1F004) == 2 &&
              utf8_width(0x1F7EB) == 2 && utf8_width(0x2713) == 1 && utf8_width(0x3FFFE) == 1,
          "emoji presentation runes take two columns");
}

/* A sink that takes everything, the shape a redirected command uses. */
static char SUNK[64];
static size_t NSUNK;

static bool take_all(void *ctx, const uint8_t *data, size_t n) {
    size_t *count = ctx;
    if (NSUNK + n < sizeof(SUNK)) {
        memcpy(SUNK + NSUNK, data, n);
        NSUNK += n;
        SUNK[NSUNK] = 0;
    }
    (*count)++;
    return true;
}

static void test_sink_diverts_output(void) {
    static size_t calls;
    calls = 0;
    NSUNK = 0;
    term_init(&T, 40, 8);
    term_puts(&T, "to the grid");
    check(term_out_pending(&T) == 11, "no sink: bytes reach the grid");
    (void)drain();

    T.sink = take_all;
    T.sink_ctx = &calls;
    term_puts(&T, "diverted");
    check(term_out_pending(&T) == 0, "sink: nothing reaches the grid");
    check(strcmp(SUNK, "diverted") == 0, "sink got the bytes");
    check(calls == 1, "sink called once per write");
}

static void test_tbuf_counts_runes(void) {
    static tbuf B;
    tb_init(&B);
    check(tb_load(&B, (const uint8_t *)"ação\nduas", 11), "tbuf loads");
    check(tb_line_of(&B, 9) == 1, "second line found past a multibyte rune");
    /* byte 5 is the 'o': three runes precede it, five bytes do */
    check(tb_col_of(&B, 5) == 3, "column counts runes, not bytes");
}

static bool text_is(const tbuf *t, const char *want) {
    return t->len == strlen(want) && memcmp(t->text, want, t->len) == 0;
}

static void type(tbuf *t, const char *s) {
    while (*s != '\0') {
        (void)tb_insert(t, (const uint8_t *)s, 1);
        s++;
    }
}

/* Built with a log of 512 bytes, as a small device would be: the oldest
   records go, and what is left still undoes to a text that existed. */
static void test_tbuf_undo(void) {
    static tbuf B;
    check(tb_load(&B, (const uint8_t *)"abc", 3), "tbuf loads");
    check(!tb_undo(&B), "nothing to undo after a load");
    tb_doc_end(&B, false);
    type(&B, "de\nfg");
    check(tb_undo(&B) && text_is(&B, "abcde\n"), "typing undoes a line at a time");
    check(B.cur == 6, "the cursor goes back to where the typing began");
    check(tb_undo(&B) && text_is(&B, "abc") && !tb_undo(&B), "then the line before");

    tb_doc_end(&B, false);
    tb_backspace(&B);
    tb_backspace(&B);
    tb_doc_home(&B, false);
    tb_delete(&B);
    check(text_is(&B, ""), "all deleted");
    check(tb_undo(&B) && text_is(&B, "a") && B.cur == 0, "a delete undoes on its own");
    check(tb_undo(&B) && text_is(&B, "abc") && B.cur == 3, "backspaces undo together");

    tb_doc_home(&B, false);
    tb_right(&B, true);
    tb_right(&B, true);
    type(&B, "X");
    check(text_is(&B, "Xc"), "the selection replaced");
    check(tb_undo(&B) && text_is(&B, "abc") && !tb_undo(&B), "and back in one step");

    check(tb_set_byte(&B, 1, 'Y') && tb_set_byte(&B, 1, 'Z') && tb_set_byte(&B, 2, 'W') &&
              tb_set_byte(&B, 3, 'V'),
          "bytes overwritten and one added");
    check(text_is(&B, "aZWV"), "the bytes set");
    check(tb_undo(&B) && text_is(&B, "aZW"), "the added byte goes");
    check(tb_undo(&B) && text_is(&B, "abc") && !tb_undo(&B), "the overwrites undo together");

    check(tb_replace(&B, (const uint8_t *)"new text", 8, 0), "replaced whole");
    check(tb_undo(&B) && text_is(&B, "abc") && !tb_undo(&B), "a replace undoes whole");

    for (int i = 0; i < 200; i++) {
        tb_doc_home(&B, false);
        type(&B, i % 2 == 0 ? "x" : "y");
    }
    check(B.undo_len <= TB_UNDO, "the log stays in its bytes");
    int undone = 0;
    while (tb_undo(&B)) {
        undone++;
    }
    check(undone > 0 && undone < 200, "the oldest records went");
    check(B.len == 3 + (size_t)(200 - undone), "what is left undoes to a text that was");

    uint8_t big[600];
    memset(big, 'z', sizeof(big));
    check(tb_replace(&B, big, sizeof(big), 0), "replaced by more than the log holds");
    check(tb_replace(&B, (const uint8_t *)"abc", 3, 0), "and that replaced in turn");
    check(!tb_undo(&B), "empties it rather than undo to a text that never was");
}

/* A toy app: it only records what it was handed, which is enough to prove
   the stack routes to the top one and hands back the right context. */
typedef struct {
    char name;
    uint32_t last_key;
    size_t repaints;
} toy;

static void toy_key(void *ctx, uint32_t cp) {
    toy *a = ctx;
    a->last_key = cp;
}

static void toy_resize(void *ctx) {
    toy *a = ctx;
    a->repaints++;
}

static const term_app TOY_APP = {.on_key = toy_key, .on_resize = toy_resize};

/* The line table against one rebuilt from the text. */
static bool lines_hold(const tbuf *t) {
    size_t n = 1;
    for (size_t i = 0; i < t->len; i++) {
        if (t->text[i] == '\n') {
            if (n >= t->nlines || t->lines[n] != i + 1) {
                return false;
            }
            n++;
        }
    }
    return n == t->nlines && t->lines[0] == 0;
}

static void test_tbuf_lines_follow_edits(void) {
    static tbuf B;
    check(tb_load(&B, (const uint8_t *)"one\ntwo\n\nfour", 13), "tbuf loads");
    uint32_t seed = 7;
    bool ok = true;
    for (int step = 0; step < 4000 && ok; step++) {
        seed = (seed * 1664525U) + 1013904223U;
        uint32_t r = seed >> 8;
        size_t pos = r % (B.len + 1);
        switch (r % 10) {
        case 0:
        case 1:
            tb_seek(&B, pos, false);
            tb_insert(&B, (const uint8_t *)"a\nb\n", 1 + ((r >> 4) % 4));
            break;
        case 2:
            tb_seek(&B, pos, false);
            tb_backspace(&B);
            break;
        case 3:
            tb_seek(&B, pos, false);
            tb_delete(&B);
            break;
        case 4:
            tb_seek(&B, pos, false);
            tb_delete_line(&B);
            break;
        case 5:
            tb_set_byte(&B, pos, (r >> 4) % 2 == 0 ? '\n' : 'x');
            break;
        case 6:
            tb_undo(&B);
            break;
        case 7:
            tb_seek(&B, pos, false);
            tb_right(&B, true);
            tb_right(&B, true);
            tb_cut(&B);
            break;
        case 8:
            tb_paste(&B);
            break;
        default:
            tb_replace(&B, (const uint8_t *)"x\n\ny", 4, 2);
            break;
        }
        ok = lines_hold(&B);
    }
    check(ok, "the line table follows every edit");
}

static void test_app_stack(void) {
    static toy menu, reader;
    menu = (toy){'m', 0, 0};
    reader = (toy){'r', 0, 0};
    term_init(&T, 40, 8);

    check(term_app_top(&T) == NULL, "empty stack has no top");
    check(term_app_enter(&T, &TOY_APP, &menu), "first app enters");
    check(T.alt, "entering takes the alternate screen");
    check(strstr(drain(), "\x1b[?1049h") != NULL, "alternate screen requested");

    check(term_app_enter(&T, &TOY_APP, &reader), "second app enters");
    check(term_app_ctx(&T) == &reader, "top is the newest app");
    term_app_top(&T)->on_key(term_app_ctx(&T), 'x');
    check(reader.last_key == 'x' && menu.last_key == 0, "keys reach the top app only");

    term_flash(&T, "said\r\n");
    check(strcmp(T.note, "said") == 0, "flash keeps one line");
    check(reader.repaints == 1, "flash repaints the top app");

    check(!term_app_leave(&T), "leaving with one left does not restore");
    check(term_app_ctx(&T) == &menu, "the app below is back on top");
    check(T.alt, "the screen stays alternate while an app is up");
    (void)drain();

    check(term_app_leave(&T), "leaving the last app restores");
    check(!T.alt, "the alternate screen was given back");
    const char *s = drain();
    check(strstr(s, "\x1b[?1049l") != NULL, "main buffer restored");
    check(strstr(s, "\x1b[0m") != NULL, "attributes reset on the way out");

    size_t i = 0;
    while (i < TERM_APPS_MAX) {
        check(term_app_enter(&T, &TOY_APP, &menu), "stack accepts up to its depth");
        i++;
    }
    check(!term_app_enter(&T, &TOY_APP, &menu), "a full stack refuses instead of overflowing");
}

/* Clicks are reported while the app on top wants them; a reader over a
   menu leaves the mouse to the terminal so a drag selects, and the menu
   takes it back when the reader closes. */
static void test_app_mouse_follows_the_top(void) {
    static const term_app reader_app = {
        .on_key = toy_key, .on_resize = toy_resize, .selects = true};
    static toy menu, reader;
    menu = (toy){'m', 0, 0};
    reader = (toy){'r', 0, 0};
    term_init(&T, 40, 8);
    (void)drain();

    check(term_app_enter(&T, &TOY_APP, &menu), "a menu enters");
    check(T.mouse && strstr(drain(), "\x1b[?1000h\x1b[?1006h") != NULL, "a menu gets the clicks");
    check(term_app_enter(&T, &reader_app, &reader), "a reader over it");
    check(!T.mouse && strstr(drain(), "\x1b[?1006l\x1b[?1000l") != NULL,
          "a reader leaves the drag to the terminal");
    check(!term_app_leave(&T), "the reader closes");
    check(T.mouse && strstr(drain(), "\x1b[?1000h") != NULL, "the menu has the clicks again");
    check(term_app_enter(&T, &TOY_APP, &menu), "another menu over it");
    check(strstr(drain(), "\x1b[?1000h") == NULL, "already reported: nothing said twice");
    (void)term_app_leave(&T);
    check(term_app_leave(&T), "the last app leaves");
    check(!T.mouse && strstr(drain(), "\x1b[?1000l") != NULL, "the prompt selects as terminals do");

    check(term_app_enter(&T, &reader_app, &reader), "a reader alone");
    check(!T.mouse && strstr(drain(), "\x1b[?1000h") == NULL, "never asks for the clicks");
}

/* The ceiling: past it the grid stops growing instead of being clipped, so
   the size the layout reasons about and the grid it draws into are the same
   number. Clipping was the old behaviour and it lost whatever a screen
   placed from the right edge. */
static void test_size_has_a_ceiling(void) {
    term_init(&T, 4000, 3000);
    check(T.cols == TERM_COLS_MAX, "a huge window is capped, not clipped");
    check(T.rows == TERM_ROWS_MAX, "the same for rows");

    term_resize(&T, 100, 30);
    check(T.cols == 100 && T.rows == 30, "a normal window is taken as it is");

    term_resize(&T, TERM_COLS_MAX + 1, TERM_ROWS_MAX + 1);
    check(T.cols == TERM_COLS_MAX && T.rows == TERM_ROWS_MAX,
          "one past the ceiling is the ceiling");

    /* the grid agrees with the size, which is the whole point */
    cv_reset(&WORK, T.rows, T.cols);
    check(WORK.cols == T.cols && WORK.rows == T.rows, "the canvas matches the capped size");
    cv_fill(&WORK, 0, 0, 1, T.cols, cv_cstr("-"));
    check(WORK.cells[0][T.cols - 1].cp == '-', "the last column is real, not off the grid");
}

/* The packed cell. raw is how the compositor compares cells — once per cell
   per frame — so what matters is that every field survives the round trip
   and that raw notices a change in any of them. A padding bit would break
   the second half silently. */
static void test_cell_packs_into_one_word(void) {
    check(sizeof(cv_cell) == 8, "a cell is one word");

    term_init(&T, 40, 8);
    cv_reset(&WORK, T.rows, T.cols);
    cv_pen(&WORK, CV_COLOR_DEFAULT, 255, CV_A_BOLD | CV_A_DIM | CV_A_REV);
    (void)cv_put(&WORK, 0, 0, cv_cstr("\xf4\x8f\xbf\xbf")); /* U+10FFFF, the last code point */

    const cv_cell *c = &WORK.cells[0][0];
    check(c->cp == 0x10FFFF, "the highest code point survives 22 bits");
    check(c->fg == CV_COLOR_DEFAULT, "a signed field keeps the default colour");
    check(c->bg == 255, "the top palette index survives");
    check(c->attr == (CV_A_BOLD | CV_A_DIM | CV_A_REV), "every attribute bit survives");

    /* the same cell written by another path compares equal through raw:
       no indeterminate bit is left over */
    cv_reset(&SHOWN, T.rows, T.cols);
    cv_pen(&SHOWN, CV_COLOR_DEFAULT, 255, CV_A_REV | CV_A_BOLD | CV_A_DIM);
    (void)cv_put(&SHOWN, 0, 0, cv_cstr("\xf4\x8f\xbf\xbf"));
    check(SHOWN.cells[0][0].raw == c->raw, "equal cells have equal raw");

    /* and raw notices a change in any one field */
    uint64_t was = c->raw;
    cv_pen(&WORK, CV_COLOR_DEFAULT, 255, CV_A_BOLD | CV_A_DIM);
    (void)cv_put(&WORK, 0, 0, cv_cstr("\xf4\x8f\xbf\xbf"));
    /* cv_put wrote the cell c points at: cppcheck does not follow it there */
    // cppcheck-suppress knownConditionTrueFalse
    check(c->raw != was, "raw sees a change of attribute");
    cv_pen(&WORK, 1, 255, CV_A_BOLD | CV_A_DIM);
    (void)cv_put(&WORK, 0, 0, cv_cstr("\xf4\x8f\xbf\xbf"));
    uint64_t fg1 = c->raw;
    cv_pen(&WORK, 2, 255, CV_A_BOLD | CV_A_DIM);
    (void)cv_put(&WORK, 0, 0, cv_cstr("\xf4\x8f\xbf\xbf"));
    // cppcheck-suppress knownConditionTrueFalse
    check(c->raw != fg1, "raw sees a change of colour");
}

/* Art drawn with true colour (38;2;r;g;b, 48;2;...), as converters write
   it, lands as the nearest of the 256; its numbers are never taken for
   codes of their own (the 2 was dim, a 0 a reset); a fg and a bg in true
   colour together fit one sequence. The capture also tells how far it drew. */
static void test_capture_true_colour(void) {
    cv_reset(&WORK, 4, 20);
    cv_capture cap;
    cv_capture_begin_at(&cap, &WORK, 0, 0);
    static const char art[] = "\x1b[38;2;255;0;0;48;2;0;0;0mR"
                              "\x1b[0;38;2;128;128;128mG"
                              "\x1b[1;38;2;255;175;0mO"
                              "\x1b[0;38;2;169;85;0;40mB";
    cv_capture_feed(&cap, (const uint8_t *)art, sizeof(art) - 1);
    const cv_cell *c = WORK.cells[0];
    check(c[0].cp == 'R' && c[0].fg == 196 && c[0].bg == 16, "red on black: 196 on 16");
    check(c[0].attr == 0, "the 2 of 38;2 is no dim");
    check(c[1].fg == 244 && c[1].bg == CV_COLOR_DEFAULT, "a mid grey is a grey of the ramp");
    check(c[2].fg == 214 && c[2].attr == CV_A_BOLD, "amber, bold kept");
    check(c[3].fg == 130 && c[3].bg == 0, "a brown, then a 16-colour ground in the same sequence");
    check(cap.rows == 1 && cap.cols == 4, "the capture measures what it drew");
    check(cv_rgb_to_256(255, 255, 255) == 231 && cv_rgb_to_256(0, 0, 0) == 16,
          "the ends of the cube");
}

/* A link goes out as OSC 8 around exactly its cells and is closed before
   anything else; a frame that drops it rewrites those cells without it even
   though their runes did not change; a URL that could end the escape early
   is refused. */
static void test_links(void) {
    term_init(&T, 20, 3);
    cv_reset(&SHOWN, 0, 0);
    cv_reset(&WORK, 3, 20);
    (void)cv_put(&WORK, 0, 0, cv_cstr("git: "));
    int32_t cols = cv_put(&WORK, 0, 5, cv_cstr("github"));
    check(cv_link_add(&WORK, 0, 5, cols, cv_cstr("https://github.com/crgimenes")), "link added");
    cv_flush(&T, &SHOWN, &WORK);
    const char *out = drain();
    check(strstr(out, "git: \x1b]8;;https://github.com/crgimenes\x1b\\github\x1b]8;;\x1b\\") !=
              NULL,
          "the link opens before its text and closes right after");

    cv_flush(&T, &SHOWN, &WORK);
    out = drain();
    check(strstr(out, "]8;") == NULL, "an unchanged link is not sent again");

    cv_clear(&WORK);
    (void)cv_put(&WORK, 0, 0, cv_cstr("git: github"));
    cv_flush(&T, &SHOWN, &WORK);
    out = drain();
    check(strstr(out, "github") != NULL && strstr(out, "]8;;h") == NULL,
          "cells that lose their link are rewritten plain");

    check(!cv_link_add(&WORK, 1, 0, 3, cv_cstr("https://x\x1b]0;evil")), "no escape in a URL");
    check(!cv_link_add(&WORK, 1, 0, 3, cv_cstr("")), "no empty URL");
    int i = 0;
    while (i < CV_LINKS_MAX) {
        check(cv_link_add(&WORK, 2, i, 1, cv_cstr("https://a")), "room for every link");
        i++;
    }
    check(!cv_link_add(&WORK, 1, 0, 1, cv_cstr("https://b")), "a full table says so");
    check(cv_link_add(&WORK, 2, 0, 1, cv_cstr("https://c")), "the same cell replaces its link");
}

/* ---- the pager ---- */

static pager P;

static void load(const char *s) {
    pager_load(&P, (const uint8_t *)s, strlen(s));
}

static size_t occurrences(const char *s, const char *sub) {
    size_t n = 0;
    size_t k = strlen(sub);
    for (const char *c = s; (c = strstr(c, sub)) != NULL; c += k) {
        n++;
    }
    return n;
}

static void test_pager_loads_and_shows(void) {
    term_init(&T, 80, 24);
    pager_reset(&P, "notes.txt");
    load("one\r\ntwo\x01three\tfour\x7f\n\x1b[1mx\x1b[0m\n");
    const char *want = "one\ntwothree    four\n\x1b[1mx\x1b[0m\n";
    check(P.text_len == strlen(want) && memcmp(P.text, want, P.text_len) == 0,
          "load drops \\r and control bytes, expands tabs, keeps escapes");
    check(!P.truncated, "a small text is whole");

    pager_show(&P, &T);
    const char *s = drain();
    check(strstr(s, "\x1b[?25l") != NULL, "show hides the cursor");
    check(strstr(s, "one\x1b]8;;\x1b\\\x1b[0m") != NULL, "every row closes any link");
    check(strstr(s, "twothree    four") != NULL, "the lines show");
    check(strstr(s, "\x1b[7m notes.txt  100%") != NULL, "the status names the file, all shown");
    check(strstr(s, "q quit") != NULL, "the status hints at q");
    check(P.nrows == 3, "one row a line");
    pager_hide(&T);
    check(strstr(drain(), "\x1b[?25h") != NULL, "hide shows the cursor again");

    pager_reset(&P, "");
    pager_show(&P, &T);
    (void)drain();
    check(P.nrows == 1 && P.rows[0].len == 0, "an empty text is one empty row");
}

static void load_line(size_t i) {
    char line[16];
    snprintf(line, sizeof(line), "line %02zu\n", i);
    load(line);
}

static void test_pager_scrolls(void) {
    term_init(&T, 80, 24); /* 23 rows of text */
    pager_reset(&P, "long");
    for (size_t i = 0; i < 60; i++) {
        load_line(i);
    }
    pager_show(&P, &T);
    const char *s = drain();
    check(P.nrows == 60 && P.top == 0, "sixty rows, at the top");
    check(strstr(s, "  38%") != NULL, "23 of 60 is 38%");

    check(pager_key(&P, &T, 'j') && P.top == 1, "j moves a line");
    s = drain();
    check(strstr(s, "line 00") == NULL && strstr(s, "line 23") != NULL, "the screen moved");
    check(pager_key(&P, &T, 'G') && P.top == 37, "G goes to the bottom");
    check(strstr(drain(), "  100%") != NULL, "the bottom is 100%");
    check(pager_key(&P, &T, 'g') && P.top == 0, "g goes to the top");
    check(pager_key(&P, &T, FT_KEY_PGDN) && P.top == 22, "page down keeps one row");
    check(pager_key(&P, &T, FT_KEY_DOWN) && P.top == 23, "arrow down moves a line");
    check(pager_key(&P, &T, 'k') && P.top == 22, "k moves back");
    check(pager_key(&P, &T, FT_KEY_PGUP) && P.top == 0, "page up");
    check(pager_key(&P, &T, 'd') && P.top == 11, "d is half a page");
    check(pager_key(&P, &T, 'u') && P.top == 0, "u is half a page back");
    check(pager_key(&P, &T, FT_KEY_END) && P.top == 37, "End is the bottom");
    check(pager_key(&P, &T, 'j') && P.top == 37, "past the bottom stays");
    check(pager_key(&P, &T, FT_KEY_HOME) && P.top == 0, "Home is the top");
    (void)drain();
    check(pager_key(&P, &T, 'x'), "an unknown key is taken");
    check(term_out_pending(&T) == 0, "and paints nothing");
    check(!pager_key(&P, &T, 'q'), "q closes");
    check(!pager_key(&P, &T, FT_KEY_ESC), "Esc closes");
}

/* A document's margin: the text wraps inside it and every row starts past
   it; a phone keeps one column, a tiny screen none, the status the width. */
static void test_pager_margin(void) {
    static char line[102];
    memset(line, 'a', 100);
    line[100] = '\n';
    line[101] = '\0';
    term_init(&T, 80, 24);
    pager_reset(&P, "doc.md");
    check(P.margin == 0, "a reset pager has no margin");
    P.margin = 2;
    load(line);
    pager_show(&P, &T);
    const char *s = drain();
    check(P.rows[0].len == 76 && P.rows[1].off == 76,
          "the text wraps at the width less both margins");
    check(strstr(s, "\x1b[0m  aaaa") != NULL, "a row starts past the margin");
    check(strstr(s, "\x1b[7m doc.md") != NULL, "the status keeps the whole width");

    term_resize(&T, 40, 24);
    pager_resize(&P, &T);
    (void)drain();
    check(P.rows[0].len == 38, "a narrow screen keeps one column each side");

    term_resize(&T, 16, 24);
    pager_resize(&P, &T);
    (void)drain();
    check(P.rows[0].len == 16, "a tiny one keeps none");
    pager_reset(&P, "x");
    check(P.margin == 0, "and the next text starts without one");
}

static void test_pager_wraps_and_resizes(void) {
    static char line[202];
    memset(line, 'a', 100);
    line[100] = ' ';
    memset(line + 101, 'b', 99);
    line[200] = '\n';
    line[201] = '\0';
    term_init(&T, 80, 24);
    pager_reset(&P, "wide");
    load(line);
    pager_show(&P, &T);
    (void)drain();
    size_t at80 = P.nrows;
    check(at80 >= 3, "200 bytes wrap into three rows or more at 80");
    check(P.rows[0].len == 80 && P.rows[1].off == 80, "no space: the first row cuts at the width");
    check(P.rows[1].len == 21 && P.rows[2].off == 101, "the second breaks after the space");

    term_resize(&T, 40, 24);
    pager_resize(&P, &T);
    (void)drain();
    check(P.nrows > at80, "narrower gives more rows");

    pager_reset(&P, "cjk");
    for (int i = 0; i < 30; i++) {
        load("\xe6\x97\xa5");
    }
    pager_show(&P, &T);
    (void)drain();
    check(P.nrows == 2 && P.rows[0].len == 60, "wide runes wrap by column, not by byte");
    term_resize(&T, 80, 24);
}

static void test_pager_wraps_styles(void) {
    term_init(&T, 20, 24);
    pager_reset(&P, "styled");
    load("\x1b[1maaaa bbbb cccc dddd eeee ffff\x1b[0m tail\n"
         "\x1b]8;;https://example.com/\x1b\\llll mmmm nnnn oooo pppp\x1b]8;;\x1b\\ rest\n");
    pager_show(&P, &T);
    const char *o = drain();
    check(P.nrows >= 4, "both lines wrapped");

    size_t styled = 0;
    size_t linked = 0;
    for (size_t i = 0; i < P.nrows; i++) {
        if (P.rows[i].sgr > 0) {
            styled++;
        }
        if (P.rows[i].link > 0) {
            linked++;
        }
    }
    check(styled > 0, "a row starting inside bold records it");
    check(linked > 0, "a row starting inside a link records it");
    check(occurrences(o, "\x1b]8;;https://example.com/\x1b\\") == 2,
          "the link is reopened where the row starts");
    check(strstr(o, "\x1b[0m\x1b[1m") != NULL, "the bold is reopened where the row starts");

    bool open = false;
    size_t opens = 0;
    bool nested = false;
    for (const char *c = o; (c = strstr(c, "\x1b]8;;")) != NULL; c += 5) {
        if (c[5] == '\x1b') {
            open = false;
            continue;
        }
        if (open) {
            nested = true;
        }
        open = true;
        opens++;
    }
    check(opens > 0 && !nested && !open, "no link opens over another or is left dangling");
    term_resize(&T, 80, 24);
}

static void test_pager_hangs_lists(void) {
    term_init(&T, 20, 24);
    pager_reset(&P, "lists");
    load("\xe2\x80\xa2 aaaa bbbb cccc dddd eeee\n"
         "12. aaaa bbbb cccc dddd\n"
         "aaaa bbbb cccc dddd eeee\n"
         "    aaaa bbbb cccc dddd\n"
         "\x1b[0;36m3)\x1b[0m aaaa bbbb cccc ddd\n"
         "123456789 aaaa bbbb cccc\n");
    pager_show(&P, &T);
    const char *o = drain();
    check(P.nrows == 12, "every line wrapped in two");
    check(P.rows[0].indent == 0 && P.rows[1].indent == 2, "a bullet item hangs under its text");
    check(P.rows[2].indent == 0 && P.rows[3].indent == 4, "a numbered item too");
    check(P.rows[4].indent == 0 && P.rows[5].indent == 0, "a paragraph does not");
    check(P.rows[6].indent == 0 && P.rows[7].indent == 4, "an indented line keeps its indent");
    check(P.rows[8].indent == 0 && P.rows[9].indent == 3, "a marker in colour still counts");
    check(P.rows[10].indent == 0 && P.rows[11].indent == 0, "a number without its dot is text");
    check(strstr(o, "\x1b[0m  dddd eeee") != NULL, "the hang is painted as blanks");
    check(strstr(o, "\x1b[0m    dddd") != NULL, "the indent is painted as blanks");
    term_resize(&T, 80, 24);
}

static void test_pager_searches(void) {
    term_init(&T, 80, 24);
    pager_reset(&P, "search");
    for (size_t i = 0; i < 60; i++) {
        if (i == 30) {
            load("the Needle\n");
        } else if (i == 45) {
            load("a needle\n");
        } else {
            load_line(i);
        }
    }
    pager_show(&P, &T);
    (void)drain();

    check(pager_key(&P, &T, '/') && P.typing, "/ starts typing");
    check(strstr(drain(), "\x1b[7m /") != NULL, "the status shows the slash");
    const char *typed = "needlx";
    while (*typed != '\0') {
        check(pager_key(&P, &T, (uint32_t)*typed), "a byte of the pattern");
        typed++;
    }
    check(pager_key(&P, &T, 0x7f) && pager_key(&P, &T, 'e'), "backspace and the last letter");
    check(P.patlen == 6 && memcmp(P.pat, "needle", 6) == 0, "the pattern is what was typed");
    check(strstr(drain(), "\x1b[7m /needle \x1b[27m") != NULL, "the status shows the pattern");
    check(pager_key(&P, &T, '\r') && !P.typing && P.top == 30, "Enter finds the first match");
    const char *o = drain();
    check(strstr(o, "the \x1b[7mNeedle\x1b[27m") != NULL, "the match is shown reversed");
    check(strstr(o, "\x1b[7ma needle") == NULL, "the status is back to the name");
    check(pager_key(&P, &T, 'n') && P.top == 37, "n finds the next, at the bottom");
    check(strstr(drain(), "a \x1b[7mneedle\x1b[27m") != NULL, "lower case found either case");
    check(pager_key(&P, &T, 'N') && P.top == 30, "N goes back to the one before");
    (void)drain();
    check(pager_key(&P, &T, 'n') && pager_key(&P, &T, 'n') && !P.not_found && P.top == 37,
          "a match below the top is found again, as in less");
    check(pager_key(&P, &T, 'g') && P.top == 0, "back to the top");

    check(pager_key(&P, &T, '/') && P.patlen == 0, "/ starts a new pattern");
    check(pager_key(&P, &T, 'z') && pager_key(&P, &T, FT_KEY_ESC) && !P.typing, "Esc gives up");
    check(P.patlen == 6 && memcmp(P.pat, "needle", 6) == 0, "the pattern before stays");

    check(pager_key(&P, &T, '/') && pager_key(&P, &T, '\r') && P.top == 30,
          "/ and Enter alone look for the last pattern again");
    (void)drain();

    check(pager_key(&P, &T, 'g') && pager_key(&P, &T, '/'), "a capital pattern");
    typed = "Needle";
    while (*typed != '\0') {
        (void)pager_key(&P, &T, (uint32_t)*typed);
        typed++;
    }
    check(pager_key(&P, &T, '\n') && P.top == 30, "finds the capital");
    check(pager_key(&P, &T, 'n') && P.not_found && P.top == 30, "and only the capital");
    check(strstr(drain(), "Pattern not found") != NULL, "the status says so");
    check(pager_key(&P, &T, 'j') && !P.not_found && P.top == 31,
          "the next key clears it and counts");
    check(strstr(drain(), "Pattern not found") == NULL, "the status is back");
}

static void test_pager_truncates(void) {
    static char chunk[1001];
    memset(chunk, 'x', 999);
    chunk[999] = '\n';
    chunk[1000] = '\0';
    term_init(&T, 80, 24);
    pager_reset(&P, "big");
    for (size_t i = 0; i * 999 < PAGER_TEXT_CAP + 1000; i++) {
        load(chunk);
    }
    check(P.truncated && P.text_len == PAGER_TEXT_CAP, "past the cap the rest is dropped");
    pager_show(&P, &T);
    check(strstr(drain(), "[truncated]") != NULL, "the status says so");
    check(pager_key(&P, &T, 'G') && P.top + 23 == P.nrows, "the end is reachable");
    (void)drain();

    pager_reset(&P, "lines");
    static char nl[1024];
    memset(nl, '\n', sizeof(nl));
    size_t fed = 0;
    while (fed < (size_t)PAGER_ROWS_MAX + 4096) {
        pager_load(&P, (const uint8_t *)nl, sizeof(nl));
        fed += sizeof(nl);
    }
    check(!P.truncated && P.text_len == fed, "more lines than the tables hold still load");
    pager_show(&P, &T);
    (void)drain();
    check(P.nrows == PAGER_ROWS_MAX && P.truncated, "the rows stop at the ceiling, flagged");
    pager_show(&P, &T);
    check(strstr(drain(), "[truncated]") != NULL, "the status says lines were dropped");
    check(pager_key(&P, &T, 'G') && P.top == PAGER_ROWS_MAX - 23, "the last rows are reachable");
    (void)drain();

    pager_reset(&P, "one line");
    memset(nl, 'x', sizeof(nl));
    for (size_t i = 0; i < 200; i++) {
        pager_load(&P, (const uint8_t *)nl, sizeof(nl));
    }
    term_resize(&T, 2, 24); /* the narrowest the wrap allows */
    pager_show(&P, &T);
    (void)drain();
    check(P.nrows == PAGER_ROWS_MAX && P.truncated, "one line past the ceiling is flagged");
    term_resize(&T, 80, 24);
}

/* ---- markdown ---- */

static md R;
static char MDOUT[32 * 1024];
static size_t NMDOUT;

static void md_take(void *user, const uint8_t *data, size_t n) {
    (void)user;
    if (n > sizeof(MDOUT) - 1 - NMDOUT) {
        n = sizeof(MDOUT) - 1 - NMDOUT;
    }
    memcpy(MDOUT + NMDOUT, data, n);
    NMDOUT += n;
    MDOUT[NMDOUT] = 0;
}

static bool slug_of(void *user, const uint8_t *slug, size_t n, char *path, size_t cap) {
    (void)user;
    if (n != 5 || memcmp(slug, "kutta", 5) != 0) {
        return false;
    }
    snprintf(path, cap, "/pub/kutta.md");
    return true;
}

static const char *render(bool enabled, const char *src) {
    NMDOUT = 0;
    MDOUT[0] = 0;
    R.emit = md_take;
    md_reset(&R, enabled);
    md_feed(&R, (const uint8_t *)src, strlen(src));
    md_end(&R);
    return MDOUT;
}

static void test_md_inline(void) {
    R.base = "";
    R.slug = NULL;
    const char *o = render(true, "# Title\n");
    check(strcmp(o, "\x1b[0;1;36;4mTitle\x1b[0m\n") == 0, "a heading is bold cyan underlined");
    o = render(true, "## Sub\n### Deep\n");
    check(strstr(o, "\x1b[0;1;36mSub") != NULL && strstr(o, "\x1b[0;1mDeep") != NULL,
          "lower headings lose the underline, then the colour");
    o = render(true, "a **bold** b\n");
    check(strcmp(o, "\x1b[0ma \x1b[0;1mbold\x1b[0m b\x1b[0m\n") == 0, "bold opens and closes");
    check(strstr(render(true, "*it*\n"), "\x1b[0;3mit") != NULL, "italic");
    check(strstr(render(true, "__it__ _x_\n"), "\x1b[0;1mit\x1b[0m \x1b[0;3mx") != NULL,
          "underscores too");
    check(strstr(render(true, "`code`\n"), "\x1b[0;33mcode") != NULL, "code is yellow");
    o = render(true, "**`both`**\n");
    check(strstr(o, "\x1b[0;1;33mboth") != NULL, "styles nest in one SGR");
    o = render(true, "snake_case_here and 2 * 3 * 4\n");
    check(strstr(o, "\x1b[0;3m") == NULL && strstr(o, "\x1b[0;1m") == NULL,
          "an identifier and arithmetic are not emphasis");
    check(strstr(o, "snake_case_here and 2 * 3 * 4") != NULL, "and keep their marks");
    o = render(true, "a \\*b\\* c\n");
    check(strstr(o, "a *b* c") != NULL, "a backslash keeps the mark as text");
}

static void test_md_blocks(void) {
    R.base = "";
    const char *o = render(true, "- one\n");
    check(strcmp(o, "\x1b[0;36m\xe2\x80\xa2\x1b[0m \x1b[0mone\x1b[0m\n") == 0, "a bullet");
    o = render(true, "2. two\n");
    check(strcmp(o, "\x1b[0;36m2.\x1b[0m \x1b[0mtwo\x1b[0m\n") == 0, "a number keeps itself");
    o = render(true, "  * nested\n");
    check(strncmp(o, "  \x1b[0;36m\xe2\x80\xa2", 9) == 0, "a nested item keeps its indent");
    o = render(true, "> quoted\n");
    check(strcmp(o, "\x1b[0;2m\xe2\x94\x82\x1b[0m \x1b[0;2;3mquoted\x1b[0m\n") == 0,
          "a quote has its bar and is dim italic");
    o = render(true, "---\n");
    check(occurrences(o, "\xe2\x94\x80") == MD_RULE_WIDTH && strstr(o, "\x1b[0;2m") == o,
          "a rule is a fixed run");
    check(occurrences(render(true, "***\n___\n"), "\xe2\x94\x80") == 2 * MD_RULE_WIDTH,
          "stars and underscores rule too");
    check(strcmp(render(true, "a\n\nb\n"), "\x1b[0ma\x1b[0m\n\n\x1b[0mb\x1b[0m\n") == 0,
          "a blank line stays blank");
    check(strcmp(render(true, "tail"), "\x1b[0mtail\x1b[0m\n") == 0,
          "md_end closes a line without its newline");
}

static void test_md_links(void) {
    R.base = "";
    R.slug = NULL;
    const char *o = render(true, "[abs](https://example.com/x)\n");
    check(strcmp(o, "\x1b[0m\x1b]8;;https://example.com/x\x1b\\\x1b[0;4;36mabs\x1b[0m\x1b]8;;"
                    "\x1b\\\x1b[0m\n") == 0,
          "an absolute link opens, styles, closes");
    o = render(true, "[rel](/pub/)\n");
    check(strstr(o, "]8;;") == NULL && strstr(o, "rel") != NULL, "no base: a site link is text");
    check(strstr(o, "/pub/") == NULL, "and its target is gone");
    R.base = "https://shell.test";
    o = render(true, "[rel](/pub/)\n");
    check(strstr(o, "\x1b]8;;https://shell.test/pub/\x1b\\rel") != NULL ||
              strstr(o, "\x1b]8;;https://shell.test/pub/\x1b\\\x1b[0;4;36mrel") != NULL,
          "with a base the site link resolves");
    o = render(true, "[none](foo.md) [m](mailto:a@b.c)\n");
    check(strstr(o, "]8;;foo") == NULL && strstr(o, "none") != NULL, "a bare path is not linked");
    check(strstr(o, "\x1b]8;;mailto:a@b.c\x1b\\") != NULL, "mailto is");
    o = render(true, "![alt](https://example.com/x.png)\n");
    check(strstr(o, "[img] alt") != NULL, "an image is its alt, marked");
    check(strstr(render(true, "[open](x\n"), "[open](x") != NULL, "an unclosed link is text");
}

static void test_md_shortcodes(void) {
    R.base = "https://shell.test";
    R.slug = slug_of;
    const char *o = render(true, "{{< youtube abc123 >}}\n");
    check(strstr(o, "\x1b]8;;https://www.youtube.com/watch?v=abc123\x1b\\") != NULL,
          "youtube becomes a link");
    check(strstr(o, "video") != NULL && strstr(o, "{{<") == NULL, "with a word and no template");
    o = render(true, "{{< anim src=\"a.webp\" href=\"https://example.com/r\" alt=\"Kutta\" >}}\n");
    check(strstr(o, "\x1b]8;;https://example.com/r\x1b\\") != NULL, "anim links to its href");
    check(strstr(o, "[anim] Kutta") != NULL, "and shows its alt");
    o = render(true, "see [it]({{< ref \"kutta\" >}}) here\n");
    check(strstr(o, "\x1b]8;;https://shell.test/pub/kutta/\x1b\\") != NULL,
          "ref resolves through the host's slugs");
    o = render(true, "see [it]({{< ref \"nothing\" >}}) here\n");
    check(strstr(o, "\x1b]8;;") == NULL && strstr(o, "see it here") != NULL,
          "an unknown slug is text, not a bad link");
    o = render(true, "a {{< mystery x=1 >}} b\n");
    check(strstr(o, "mystery") == NULL && strstr(o, "a  b") != NULL, "an unknown one vanishes");
    o = render(true, "a {{% unclosed\n");
    check(strstr(o, "{{% unclosed") != NULL, "one not closed on its line is text");
    R.slug = NULL;
}

static void test_md_code_fences(void) {
    R.base = "";
    const char *o = render(true, "```go\nfunc f() { s := \"x\" } // c\n```\nafter\n");
    check(strstr(o, "\x1b[0;36mfunc") != NULL, "a keyword");
    check(strstr(o, "\x1b[0;32m\"x\"") != NULL, "a string");
    check(strstr(o, "\x1b[0;2m// c") != NULL, "a comment");
    check(strstr(o, "```") == NULL && strstr(o, "go\n") == NULL, "the fence lines take no row");
    check(strstr(o, "\x1b[0mafter") != NULL, "markdown resumes after the fence");

    o = render(true, "```c\n/* one\ntwo */\nint x;\n```\n");
    check(strstr(o, "\x1b[0;2m/* one") != NULL && strstr(o, "\x1b[0;2mtwo */") != NULL,
          "a block comment carries across lines");
    check(strstr(o, "\x1b[0;36mint") != NULL, "and ends");

    o = render(true, "1. step:\n   ```go\n   func f() {}\n   ```\n   done\n");
    check(strstr(o, "`") == NULL, "an indented fence is a fence");
    check(strstr(o, "\x1b[0;36mfunc") != NULL && strstr(o, "done") != NULL, "its code is coloured");

    o = render(true, "```\nfunc f() **x**\n```\n");
    check(strstr(o, "\x1b[0;36m") == NULL && strstr(o, "\x1b[0;1m") == NULL,
          "an unlabeled fence is neither coloured nor markdown");
    check(strstr(o, "\x1b[0mfunc f() **x**") != NULL, "its text is plain");

    o = render(true, "~~~~\n```\n~~~~\nout\n");
    check(strstr(o, "```") != NULL && strstr(o, "~") == NULL, "a tilde fence holds backticks");
    o = render(true, "```\nnever closed");
    check(strstr(o, "never closed") != NULL, "an unclosed fence still shows its code");
}

static void test_md_verbatim_and_escapes(void) {
    R.base = "";
    const char *src = "# not a heading\n**not bold**\n";
    check(strcmp(render(false, src), src) == 0, "disabled, the bytes stream as they came");
    const char *o = render(true, "\x1b[31mred\x1b[0m and \x1b]0;title\x07kept \x1b_ap\x1b\\ one\n");
    check(strstr(o, "[31m") == NULL && strstr(o, "title") == NULL && strstr(o, "ap") == NULL,
          "the source's escapes are dropped whole");
    check(strcmp(o, "\x1b[0mred and kept  one\x1b[0m\n") == 0, "the text around is kept");
    o = render(true, "a\rb\n");
    check(strcmp(o, "\x1b[0mab\x1b[0m\n") == 0, "a \\r is dropped");
}

static void test_md_source(void) {
    NMDOUT = 0;
    MDOUT[0] = 0;
    R.emit = md_take;
    md_source(&R, HL_C);
    const char *src = "#define X 1\nint a; // c\n";
    md_feed(&R, (const uint8_t *)src, strlen(src));
    md_end(&R);
    check(strcmp(MDOUT, "\x1b[0m#\x1b[0;36mdefine\x1b[0m X \x1b[0;35m1\x1b[0m\n"
                        "\x1b[0;36mint\x1b[0m a; \x1b[0;2m// c\x1b[0m\n") == 0,
          "a source file: every line highlighted, none read as markdown");

    NMDOUT = 0;
    md_source(&R, HL_NONE);
    md_feed(&R, (const uint8_t *)src, strlen(src));
    md_end(&R);
    check(strcmp(MDOUT, src) == 0, "no language: the bytes as they came");
}

static void test_md_long_line(void) {
    static char line[MD_LINE_MAX + 600];
    memset(line, 'a', sizeof(line) - 2);
    line[sizeof(line) - 2] = '\n';
    line[sizeof(line) - 1] = '\0';
    R.base = "";
    const char *o = render(true, line);
    check(occurrences(o, "a") == sizeof(line) - 2, "a line past the buffer comes out whole");
    check(o[strlen(o) - 1] == '\n', "and ends");
}

/* ---- the highlighter ---- */

static char SPANS[256];

static void take_span(void *user, hl_class cls, size_t from, size_t to) {
    (void)user;
    size_t n = strlen(SPANS);
    snprintf(SPANS + n, sizeof(SPANS) - n, "%s%c%zu-%zu", n > 0 ? " " : "", "PCSNK"[cls], from, to);
}

static const char *spans(hl_state *h, const char *line) {
    SPANS[0] = 0;
    hl_spans(h, (const uint8_t *)line, strlen(line), take_span, NULL);
    return SPANS;
}

static void test_hl_languages(void) {
    check(hl_lang_of("go") == HL_GO && hl_lang_of("golang") == HL_GO, "go");
    check(hl_lang_of("bash") == HL_SHELL && hl_lang_of("sh") == HL_SHELL, "shell");
    check(hl_lang_of("c") == HL_C && hl_lang_of("h") == HL_C, "c");
    check(hl_lang_of("lua") == HL_LUA && hl_lang_of("asm") == HL_ASM, "lua, asm");
    check(hl_lang_of("json") == HL_JSON && hl_lang_of("sql") == HL_SQL, "json, sql");
    check(hl_lang_of("js") == HL_JS && hl_lang_of("javascript") == HL_JS, "js");
    check(hl_lang_of("filo") == HL_FILO && hl_lang_of("redcode") == HL_REDCODE, "filo, redcode");
    check(hl_lang_of("Go") == HL_GO, "case does not matter");
    check(hl_lang_of("brainfuck") == HL_NONE && hl_lang_of("") == HL_NONE &&
              hl_lang_of(NULL) == HL_NONE && hl_lang_of("g") == HL_NONE,
          "unknown, empty, none, a part of a name");
    check(hl_lang_of("a-very-long-info-string-that-overflows") == HL_NONE, "a long one is cut");
    check(hl_lang_of_name("init.filo") == HL_FILO && hl_lang_of_name("mars.c") == HL_C,
          "a file's extension");
    check(hl_lang_of_name("README") == HL_NONE && hl_lang_of_name("v1.0/README") == HL_NONE,
          "no extension, even with a dot in the path");
    check(hl_lang_of_name("a.b/c.go") == HL_GO && hl_lang_of_name(NULL) == HL_NONE,
          "the last dot of the name");
}

static void test_hl_spans(void) {
    hl_state h;
    hl_begin(&h, HL_GO);
    check(strcmp(spans(&h, "return \"a\" // c"), "K0-6 P6-7 S7-10 P10-11 C11-15") == 0, "go");
    check(strcmp(spans(&h, "x = 42"), "P0-4 N4-6") == 0, "a number after a word");
    check(strcmp(spans(&h, "x1 = 2"), "P0-5 N5-6") == 0, "a digit in a word is not a number");
    check(strcmp(spans(&h, "s := \"a\\\"b\""), "P0-5 S5-11") == 0, "an escaped quote");
    check(strcmp(spans(&h, "s := `a"), "P0-5 S5-7") == 0 && h.carry == HL_ST_RAW_STRING,
          "a raw string opens");
    check(strcmp(spans(&h, "b` + 1"), "S0-2 P2-5 N5-6") == 0 && h.carry == HL_ST_NORMAL,
          "and carries to where it closes");
    check(strcmp(spans(&h, "\"open"), "S0-5") == 0 && h.carry == HL_ST_NORMAL,
          "a plain string ends with its line");

    hl_begin(&h, HL_C);
    check(strcmp(spans(&h, "/* a */ int"), "C0-7 P7-8 K8-11") == 0, "c block comment");
    check(strcmp(spans(&h, "int /* a"), "K0-3 P3-4 C4-8") == 0 && h.carry == HL_ST_BLOCK_COMMENT,
          "an open block comment carries");
    check(strcmp(spans(&h, "mid"), "C0-3") == 0 && h.carry == HL_ST_BLOCK_COMMENT,
          "a line inside is all comment");
    check(strcmp(spans(&h, "b */ x"), "C0-4 P4-6") == 0 && h.carry == HL_ST_NORMAL, "and closes");
    check(strcmp(spans(&h, "'a' 1"), "P0-4 N4-5") == 0, "a char literal is not a string in c");
    hl_spans(&h, (const uint8_t *)"/* x", 4, NULL, NULL);
    check(h.carry == HL_ST_BLOCK_COMMENT, "a line nobody shows still carries");

    hl_begin(&h, HL_SHELL);
    check(strcmp(spans(&h, "echo 'hi' # c"), "K0-4 P4-5 S5-9 P9-10 C10-13") == 0, "shell");
    hl_begin(&h, HL_LUA);
    check(strcmp(spans(&h, "local x = 1 -- c"), "K0-5 P5-10 N10-11 P11-12 C12-16") == 0, "lua");
    hl_begin(&h, HL_ASM);
    check(strcmp(spans(&h, "push 1 # c"), "K0-4 P4-5 N5-6 P6-7 C7-10") == 0, "asm, second opener");
    hl_begin(&h, HL_JSON);
    check(strcmp(spans(&h, "{\"a\": true}"), "P0-1 S1-4 P4-6 K6-10 P10-11") == 0, "json");
    hl_begin(&h, HL_SQL);
    check(strcmp(spans(&h, "select 1 -- c"), "K0-6 P6-7 N7-8 P8-9 C9-13") == 0, "sql");
    hl_begin(&h, HL_JS);
    check(strcmp(spans(&h, "let s = 'a' /* c"), "K0-3 P3-8 S8-11 P11-12 C12-16") == 0, "js");
    hl_begin(&h, HL_REDCODE);
    check(strcmp(spans(&h, "MOV 0, 1 ; c"), "K0-3 P3-4 N4-5 P5-7 N7-8 P8-9 C9-12") == 0,
          "redcode folds case");
    hl_begin(&h, HL_FILO);
    check(strcmp(spans(&h, "(def x-y \"s"), "P0-1 K1-4 P4-9 S9-11") == 0 && h.carry == HL_ST_STRING,
          "filo: a name with a dash, a string that spans lines");
    check(strcmp(spans(&h, "t\" 1"), "S0-2 P2-3 N3-4") == 0 && h.carry == HL_ST_NORMAL,
          "and closes on the next");
    check(strcmp(spans(&h, "; c"), "C0-3") == 0, "filo comments");
    hl_begin(&h, HL_NONE);
    check(strcmp(spans(&h, "func x"), "P0-6") == 0, "no language: one plain span");
    check(strcmp(spans(&h, ""), "") == 0, "an empty line tells nothing");
}

static void test_hl_line(void) {
    hl_state h;
    NMDOUT = 0;
    MDOUT[0] = 0;
    hl_begin(&h, HL_GO);
    hl_line(&h, (const uint8_t *)"func f() { s := \"x\" } // c", 26, md_take, NULL);
    check(strcmp(MDOUT, "\x1b[0;36mfunc\x1b[0m f() { s := \x1b[0;32m\"x\"\x1b[0m } "
                        "\x1b[0;2m// c\x1b[0m") == 0,
          "each span in its colour, the line closed");
    NMDOUT = 0;
    hl_line(&h, (const uint8_t *)"x = 1", 5, md_take, NULL);
    check(strcmp(MDOUT, "\x1b[0mx = \x1b[0;35m1\x1b[0m") == 0, "a number is magenta");
    NMDOUT = 0;
    hl_begin(&h, HL_NONE);
    hl_line(&h, (const uint8_t *)"plain", 5, md_take, NULL);
    check(strcmp(MDOUT, "\x1b[0mplain") == 0, "no language: reset and the bytes");
}

int main(void) {
    test_box_and_bar();
    test_output_and_width();
    test_sink_diverts_output();
    test_tbuf_counts_runes();
    test_tbuf_undo();
    test_tbuf_lines_follow_edits();
    test_app_stack();
    test_app_mouse_follows_the_top();
    test_size_has_a_ceiling();
    test_cell_packs_into_one_word();
    test_capture_true_colour();
    test_links();
    test_pager_loads_and_shows();
    test_pager_scrolls();
    test_pager_wraps_and_resizes();
    test_pager_margin();
    test_pager_wraps_styles();
    test_pager_hangs_lists();
    test_pager_searches();
    test_pager_truncates();
    test_md_inline();
    test_md_blocks();
    test_md_links();
    test_md_shortcodes();
    test_md_code_fences();
    test_md_verbatim_and_escapes();
    test_md_source();
    test_md_long_line();
    test_hl_languages();
    test_hl_spans();
    test_hl_line();
    if (NFAIL > 0) {
        printf("%zu runtime checks failed\n", NFAIL);
        return 1;
    }
    printf("all runtime tests passed\n");
    return 0;
}
