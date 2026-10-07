/* The boundary gate: this links the runtime alone — term, canvas, utf8,
   tbuf — and no host at all. It draws a framed box with a status bar and
   checks the bytes that reach the terminal. If this file ever needs a host
   to compile, the boundary leaked and the build says so here. */

#include <stdio.h>
#include <string.h>

#include "canvas.h"
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

static const term_app TOY_APP = {toy_key, toy_resize, NULL};

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

int main(void) {
    test_box_and_bar();
    test_output_and_width();
    test_sink_diverts_output();
    test_tbuf_counts_runes();
    test_tbuf_undo();
    test_app_stack();
    test_size_has_a_ceiling();
    test_cell_packs_into_one_word();
    if (NFAIL > 0) {
        printf("%zu runtime checks failed\n", NFAIL);
        return 1;
    }
    printf("all runtime tests passed\n");
    return 0;
}
