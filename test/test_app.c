/* The app core driven the way a terminal drives it: a small program
   compiled here, its bytes fed in, what reaches the terminal checked. */

#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif

#include "app.h"
#include "filo_fmt.h"
#include "filo_nolibc.h"
#include "filo_strings.h"
#include "keys.h"
#include "paint.h"
#include "tbx.h"
#include "tty.h"

enum { SRC_MAX = 8, UNIT_CAP = 64 * 1024 };

const app_spec app_program = {"t", "1.0", false, 0, 0, 0, NULL};

static app A;
static const app_spec *spec = &app_program; /* what build and start load */
static filo_ctx C;                          /* the compiler's, apart from the app's */
static uint8_t cpersistent[1U << 20U];
static uint8_t crun[1U << 20U];
static uint8_t unit[UNIT_CAP];
static uint8_t fbb[UNIT_CAP];
static size_t fbb_len;
static uint8_t out[TERM_OUT_CAP + 1];
static size_t nfail;

static void check(bool ok, const char *what) {
    if (ok) {
        return;
    }
    printf("FAIL: %s\n", what);
    nfail++;
}

static const char *drain(void) {
    size_t n = term_out_read(&A.t, out, sizeof(out) - 1);
    out[n] = 0;
    return (const char *)out;
}

/* A row of what the program painted, as text (ASCII is all these draw). */
static const char *row(int32_t r) {
    static char line[TERM_COLS_MAX + 1];
    size_t n = 0;
    for (uint16_t c = 0; c < A.target.cols; c++) {
        uint32_t cp = A.target.cells[r][c].cp;
        line[n++] = cp > 0 && cp < 0x80 ? (char)cp : ' ';
    }
    line[n] = '\0';
    return line;
}

typedef struct {
    const char *entry;
    const char *src;
} entry_src;

/* The program built as the CLI builds it: compiled against the base's
   names, which the context of a spec-less app holds. */
static bool build(const entry_src *e, size_t n) {
    static filo_prog progs[SRC_MAX];
    static filo_bc_entry entries[SRC_MAX];
    char why[128];
    if (!app_context(&A, spec, why, sizeof(why))) {
        printf("context: %s\n", why);
        return false;
    }
    filo_init(&C, &A.ctx.host, cpersistent, sizeof(cpersistent), crun, sizeof(crun));
    for (size_t i = 0; i < n; i++) {
        if (filo_compile(&C, (const uint8_t *)e[i].src, strlen(e[i].src), &progs[i]) != FILO_OK) {
            printf("compile %s: %s\n", e[i].entry, filo_error(&C));
            return false;
        }
        entries[i].name = e[i].entry;
        entries[i].prog = &progs[i];
    }
    size_t ulen = 0;
    if (filo_bc_build(&C, entries, (uint32_t)n, unit, sizeof(unit), &ulen) != FILO_OK) {
        printf("build: %s\n", filo_error(&C));
        return false;
    }
    filo_bundle_member m = {"t", unit, ulen};
    return filo_bundle_build(&C, &m, 1, fbb, sizeof(fbb), &fbb_len) == FILO_OK;
}

static bool start(void) {
    char why[256];
    A.seed = 42;
    A.arg = "hello";
    if (!app_start(&A, spec, fbb, fbb_len, 40, 10, why, sizeof(why))) {
        printf("start: %s\n", why);
        return false;
    }
    return true;
}

static void key(uint8_t c) {
    app_input(&A, &c, 1);
}

static void test_paint_and_done(void) {
    static const entry_src p[] = {
        {"init", "(def n 0)"},
        {"draw", "(print-at 0 0 (str-fmt \"n=%d %s %s\" n ARG VERSION))"},
        {"key", "(if (= KEY KEY_ESC) (done) (set n (+ n 1)))"},
    };
    check(build(p, 3), "builds");
    check(start(), "starts");
    const char *s = drain();
    check(strstr(s, "\x1b[?1049h") != NULL, "takes the alternate screen");
    check(strstr(s, "n=0 hello 1.0") != NULL, "first paint");
    key('x');
    check(strstr(row(0), "n=1") != NULL, "a key runs key, then draw");
    key(0x03);
    check(app_done(&A), "Ctrl-C is Esc, and Esc leaves");
    check(strstr(drain(), "\x1b[?1049l") != NULL, "gives the terminal back");
}

/* The kit and a common file, when the bundle carries them, run before
   init: a program built on the kit draws the same here as in rocchetto. */
static void test_kit_and_common(void) {
    static const entry_src p[] = {
        {"kit", "(def kit-word (fn () \"from the kit\"))"},
        {"common", "(def layer-word (str-concat (kit-word) \", then common\"))"},
        {"init", "(def title layer-word)"},
        {"draw", "(print-at 0 0 title)"},
    };
    check(build(p, 4), "builds with a kit");
    check(start(), "starts with a kit");
    check(strstr(row(0), "from the kit, then common") != NULL, "kit, common, then init");
}

/* A click reaches the key entry as KEY_CLICK with its cell in MX and MY:
   the kit's menus and tables are built on them, so every app has them. */
static void test_click(void) {
    static const entry_src p[] = {
        {"init", "(def at \"none\")"},
        {"draw", "(print-at 0 0 at)"},
        {"key", "(if (= KEY KEY_CLICK) (set at (str-fmt \"click %d,%d\" MX MY)) #t)"},
    };
    check(build(p, 3), "builds with a click");
    check(start(), "starts with a click");
    static const char sgr[] = "\x1b[<0;10;3M";
    app_input(&A, (const uint8_t *)sgr, sizeof(sgr) - 1);
    check(strstr(row(0), "click 9,2") != NULL, "a click names its cell");
}

static char score_track[PAINT_TRACK_MAX];
static int32_t score_value = -1;

static void took_score(app *a, const char *track, int32_t value) {
    (void)a;
    snprintf(score_track, sizeof(score_track), "%s", track);
    score_value = value;
}

/* report-score hands a game's result to the host that keeps scores, with
   its track when it has one; without such a host it is quietly #f; a score
   that is no whole number from 0, or a track that is not a short word, is
   the program's error. */
static void test_report_score(void) {
    static const entry_src p[] = {
        {"init", "(def said \"\")"},
        {"draw", "(print-at 0 0 said)"},
        {"key", "(cond ((= KEY 97) (set said (string (report-score 42)))) "
                "((= KEY 98) (report-score 7 \"level3\")) "
                "((= KEY 99) (report-score -1)) "
                "((= KEY 100) (report-score 1 \"Level 3\")) (else #t))"},
    };
    check(build(p, 3), "builds a scoring one");
    check(start(), "starts a scoring one");
    key('a');
    check(strstr(row(0), "#f") != NULL, "no host that keeps scores: #f");
    A.score = took_score;
    key('a');
    check(score_value == 42 && score_track[0] == '\0', "the host takes the score");
    check(strstr(row(0), "#t") != NULL, "and says so");
    key('b');
    check(score_value == 7 && strcmp(score_track, "level3") == 0, "a track goes with it");
    score_value = -1;
    key('c');
    check(score_value == -1 && A.error[0] != '\0', "no negative score");
    A.error[0] = '\0';
    key('d');
    check(score_value == -1 && A.error[0] != '\0', "a track is a short word");
    A.score = NULL;
}

/* input-set seeds the field, and inside the input entry what it put
   stays: a form moves its one field to the next value, keeping the text of
   the one it leaves. */
static void test_input_set(void) {
    static const entry_src p[] = {
        {"init", "(def got \"\") (input-set \"ação\")"},
        {"draw", "(print-at 0 0 (str-concat \"got:\" got)) (input-at 2 0 20 40 #f)"},
        {"input", "(set got (input-text)) (input-set \"next\")"},
    };
    check(build(p, 3), "builds the form");
    check(start(), "starts the form");
    check(A.in.len == strlen("ação") && memcmp(A.in.buf, "ação", A.in.len) == 0,
          "input-set seeds the field, runes whole");
    key('!');
    key('\r');
    check(strstr(row(0), "got:a  o!") != NULL, "Enter hands the text to input");
    check(A.in.len == 4 && memcmp(A.in.buf, "next", 4) == 0, "what input set stays");
    key('\r');
    check(strstr(row(0), "got:next") != NULL, "and is the next value");
}

static void test_keep_canvas_and_tick(void) {
    static const entry_src p[] = {
        {"init", "(keep-canvas) (def t 0) (def r (random 1000))"},
        {"draw", "(if FRESH (print-at 1 0 \"fresh\")) (print-at 2 0 (str-fmt \"t=%d\" t))"},
        {"tick", "(set t (+ t MS))"},
    };
    check(build(p, 3), "builds the animated one");
    check(start(), "starts the animated one");
    check(app_animates(&A), "a tick means it animates");
    (void)drain();
    app_tick(&A, 30);
    check(strstr(row(2), "t=30") != NULL, "tick gets MS and paints");
    check(strstr(drain(), "fresh") == NULL, "the kept canvas is not blanked");
    check(A.target.cells[1][0].cp == 'f', "what the first paint drew is still there");
    app_resize(&A, 50, 12);
    check(strstr(drain(), "fresh") != NULL, "a resize starts from blank cells");
}

static void test_error_line(void) {
    static const entry_src p[] = {
        {"init", "(def ok #t)"},
        {"draw", "(print-at 0 0 \"body\")"},
        {"key", "(if (= KEY 98) (+ 1 \"a\") (set ok #t))"},
    };
    check(build(p, 3), "builds the failing one");
    check(start(), "starts the failing one");
    (void)drain();
    key('b');
    const char *s = drain();
    check(strstr(s, "t: key.filo:1:") != NULL, "the failure is on the bottom row");
    check(A.target.cells[0][0].cp == 'b', "and not in the program's cells");
    key('a');
    check(A.error[0] == '\0', "the next key clears it");
}

static void test_field(void) {
    static const entry_src p[] = {
        {"init", "(def got \"\")"},
        {"draw", "(input-at 0 0 10 0 #f) (print-at 1 0 got)"},
        {"input", "(set got (str-concat \"<\" (input-text) \">\"))"},
    };
    check(build(p, 3), "builds the field");
    check(start(), "starts the field");
    key('h');
    key('i');
    key('\r');
    check(strstr(drain(), "<hi>") != NULL, "Enter hands the line to input");
}

static void keys(const char *s) {
    for (; *s != '\0'; s++) {
        key((uint8_t)*s);
    }
}

/* An area: the text breaks at spaces over the rows, and the caret, moved
   into the middle of it, lands on the right line and column. */
static void test_area(void) {
    static const entry_src p[] = {
        {"init", "(def got \"\")"},
        {"draw", "(input-at 0 0 10 0 #f 3) (print-at 4 0 got)"},
        {"input", "(set got (input-text))"},
    };
    check(build(p, 3), "builds the area");
    check(start(), "starts the area");
    keys("hello wide world");
    (void)drain();
    check(strncmp(row(0), "hello     ", 10) == 0, "the first line ends at the space");
    check(strncmp(row(1), "wide      ", 10) == 0, "the second too");
    check(strncmp(row(2), "world     ", 10) == 0, "the last word is the last line");
    check(A.caret_row == 2 && A.caret_col == 5, "the caret is after the last word");
    keys("\x1b[D\x1b[D\x1b[D");
    key('X');
    (void)drain();
    check(strncmp(row(2), "woXrld    ", 10) == 0, "a rune lands where the caret is");
    check(A.caret_row == 2 && A.caret_col == 3, "and the caret follows it there");
    keys("\x1b[H");
    key('Y');
    (void)drain();
    check(strncmp(row(0), "Yhello    ", 10) == 0 && A.caret_row == 0 && A.caret_col == 1,
          "Home goes to the first line");
    key('\r');
    check(strstr(drain(), "Yhello wide woXrld") != NULL, "Enter hands the whole text over");
}

/* In an area a line break is text when it is Ctrl-J or comes in a paste
   (a CRLF counts once); Enter typed is CR and sends. */
static void test_area_line_breaks(void) {
    static const entry_src p[] = {
        {"init", "(def got \"\")"},
        {"draw", "(input-at 0 0 10 0 #f 4) (print-at 5 0 (str-replace \"\\n\" \"/\" got))"},
        {"input", "(set got (input-text))"},
    };
    check(build(p, 3), "builds the area");
    check(start(), "starts the area");
    keys("ab\ncd");
    (void)drain();
    check(strncmp(row(0), "ab        ", 10) == 0 && strncmp(row(1), "cd        ", 10) == 0,
          "Ctrl-J breaks the line");
    keys("\x1b[200~ /\\\r\n( o\r\x1b[201~");
    (void)drain();
    check(strncmp(row(1), "cd /\\     ", 10) == 0, "a pasted line stays on its line");
    check(strncmp(row(2), "( o       ", 10) == 0, "a pasted CRLF breaks once");
    check(A.caret_row == 3 && A.caret_col == 0, "a pasted CR at the end opens a line");
    key('\r');
    check(strstr(drain(), "ab/cd /\\/( o/") != NULL, "Enter typed sends it all");
}

/* (wrap): the lines a field would break the text into — at spaces, the
   other spaces kept, a lone long word cut. */
static void test_wrap(void) {
    static const entry_src p[] = {
        {"init", "(def z 0)"},
        {"draw", "(print-at 0 0 (str-join \"|\" (wrap \"hello wide world\" 9)))"
                 "(print-at 1 0 (str-join \"|\" (wrap \"  a  b\" 9)))"
                 "(print-at 2 0 (str-join \"|\" (wrap \"abcdefghijkl\" 5)))"
                 "(print-at 3 0 (str-fmt \"%d\" (length (wrap \"\" 5))))"},
    };
    check(build(p, 2), "builds the wrapper");
    check(start(), "starts the wrapper");
    (void)drain();
    check(strncmp(row(0), "hello|wide|world ", 17) == 0, "breaks at the spaces");
    check(strncmp(row(1), "  a  b ", 7) == 0, "keeps the spaces that are not breaks");
    check(strncmp(row(2), "abcde|fghij|kl ", 15) == 0, "cuts a word wider than the line");
    check(strncmp(row(3), "1 ", 2) == 0, "an empty text is one empty line");
}

/* A line break ends a line wherever it is, and a text ending in one ends
   on an empty line. */
static void test_wrap_line_breaks(void) {
    static const entry_src p[] = {
        {"init", "(def z 0)"},
        {"draw", "(print-at 0 0 (str-join \"|\" (wrap \" /\\\\_/\\\\\\n( o.o )\\n\" 20)))"},
    };
    check(build(p, 2), "builds the art wrapper");
    check(start(), "starts the art wrapper");
    (void)drain();
    check(strncmp(row(0), " /\\_/\\|( o.o )| ", 16) == 0, "breaks at line breaks, spaces kept");
}

/* (lend): a builtin that lends the terminal out, as one running $EDITOR
   does, and takes it back: the size comes in again while key is still
   on the VM. */
static int b_lend(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *res) {
    (void)ctx;
    (void)a;
    (void)n;
    app_resize(&A, 40, 10);
    *res = filo_cstring("");
    return FILO_OK;
}

static bool lend_extend(app *a, filo_ctx *ctx) {
    (void)a;
    return filo_register_builtin(ctx, "lend", b_lend) == FILO_OK;
}

static const app_spec lending = {"t", "1.0", false, 0, 0, 0, lend_extend};

/* Painting there would run draw inside key, on one VM; the paint after key
   is the one that answers the resize. */
static void test_resize_inside_an_entry(void) {
    static const entry_src p[] = {
        {"init", "(def n 0)"},
        {"draw", "(print-at 0 0 (str-fmt \"n=%d\" n))"},
        {"key", "(let ((r (lend))) (set n (+ n 1)) r)"},
    };
    spec = &lending;
    check(build(p, 3), "builds the lending one");
    check(start(), "starts the lending one");
    (void)drain();
    key('x');
    check(A.error[0] == '\0', "no failure after the terminal comes back");
    check(strstr(row(0), "n=1") != NULL, "key finished, then one paint");
    spec = &app_program;
}

static void test_refuses_missing(void) {
    static const entry_src p[] = {
        {"init", "(def x 0)"},
        {"draw", "(nope 1)"},
    };
    check(build(p, 2), "builds against a name no host has");
    char why[256];
    check(!app_start(&A, &app_program, fbb, fbb_len, 40, 10, why, sizeof(why)),
          "a missing name refuses the load");
    check(strstr(why, "nope") != NULL, "and says which");
}

static void test_profile(void) {
    char why[128];
    static char text[16 * 1024];
    check(app_context(&A, &app_program, why, sizeof(why)), "context");
    check(app_profile(&A.ctx, text, sizeof(text)) > 0, "profile fits");
    check(strstr(text, "print-at\n") != NULL, "lists paint");
    check(strstr(text, "done\n") != NULL, "lists the base");
    check(strstr(text, "\nglobal KEY_ESC\n") != NULL, "lists the globals as values");
    check(strstr(text, "sqrt\n") == NULL, "no libm without asking");
}

/* ---- the text buffer's builtins ---- */

static tbuf TB;
static uint8_t tb_file[4096]; /* what the file holds, for saved_fn */
static size_t tb_file_len;
static filo_ctx E; /* a context of its own, the builtins driven expression by expression */
static uint8_t epersistent[1U << 20U];
static uint8_t erun[1U << 18U];

static tbuf *buf_fn(filo_ctx *ctx) {
    (void)ctx;
    return &TB;
}

static bool saved_fn(filo_ctx *ctx) {
    (void)ctx;
    return TB.len == tb_file_len && memcmp(TB.text, tb_file, TB.len) == 0;
}

static bool tb_extend(app *a, filo_ctx *ctx) {
    (void)a;
    tbx_register(ctx, buf_fn, saved_fn);
    return true;
}

static const app_spec editing = {"t", "1.0", false, 0, 0, 0, tb_extend};

static void tb_open(const void *data, size_t n) {
    tb_init(&TB);
    (void)tb_load(&TB, data, n);
    memcpy(tb_file, data, n);
    tb_file_len = n;
}

static void tb_text(const char *s) {
    tb_open(s, strlen(s));
}

static void eval_context(void) {
    filo_init(&E, &filo_nolibc_host, epersistent, sizeof(epersistent), erun, sizeof(erun));
    (void)filo_strings_register(&E, &filo_nolibc_strings);
    tbx_register(&E, buf_fn, saved_fn);
}

/* src run in E, its value as Filo reads it, or the failure. */
static const char *ev(const char *src) {
    static char text[1024];
    filo_prog prog;
    if (filo_compile(&E, (const uint8_t *)src, strlen(src), &prog) != FILO_OK) {
        snprintf(text, sizeof(text), "compile: %s", filo_error(&E));
        return text;
    }
    filo_value v;
    if (filo_run(&E, &prog, NULL, &v) != FILO_OK) {
        snprintf(text, sizeof(text), "error: %s", filo_error(&E));
        return text;
    }
    size_t n = 0;
    if (filo_value_repr(&E, &v, text, sizeof(text) - 1, &n) != FILO_OK) {
        return "repr: does not fit";
    }
    text[n] = '\0';
    return text;
}

static void is(const char *src, const char *want) {
    const char *got = ev(src);
    if (strcmp(got, want) == 0) {
        return;
    }
    printf("FAIL: %s\n  want: %s\n  got:  %s\n", src, want, got);
    nfail++;
}

static void fails(const char *src) {
    const char *got = ev(src);
    if (strncmp(got, "error: ", 7) == 0) {
        return;
    }
    printf("FAIL: %s should fail, got %s\n", src, got);
    nfail++;
}

static void press(uint32_t k, unsigned page) {
    char src[64];
    snprintf(src, sizeof(src), "(tb-key %u %u)", (unsigned)k, page);
    is(src, "#t");
}

static void test_tbx_lines(void) {
    eval_context();
    tb_text("ab\tc \xe6\x97\xa5\xe6\x9c\xac\nsecond line\nthird");
    is("(tb-lines)", "3");
    is("(tb-line 0 0 20)", "(tuple \"ab  c \xe6\x97\xa5\xe6\x9c\xac\" -1 -1)");
    is("(tb-line 0 5 3)", "(tuple \" \xe6\x97\xa5\" -1 -1)");
    is("(tb-line 1 2 4)", "(tuple \"cond\" -1 -1)");
    is("(tb-line 1 20 4)", "(tuple \"\" -1 -1)");
    is("(tb-line 9 0 4)", "(tuple \"\" -1 -1)");
    is("(tb-cursor)", "(tuple 0 0)");
    is("(tb-goto 3)", "#t");
    is("(tb-cursor)", "(tuple 2 0)");
    is("(tb-view 2 10)", "(tuple 1 0)");
    is("(tb-goto 0)", "#t");
    is("(tb-view 2 10)", "(tuple 0 0)");
    press(FT_KEY_DOWN, 1);
    press(FT_KEY_END, 1);
    is("(tb-cursor)", "(tuple 1 11)");
    is("(tb-view 2 5)", "(tuple 0 7)");
    is("(tb-line 1 7 5)", "(tuple \"line\" -1 -1)");
    is("(tb-goto 1)", "#t");
    is("(tb-find \"line\")", "#t");
    is("(tb-cursor)", "(tuple 1 7)");
    is("(tb-line 1 0 20)", "(tuple \"second line\" 7 11)");
    is("(tb-line 1 8 20)", "(tuple \"ine\" 0 3)");
    is("(tb-find \"nowhere\")", "#f");
    is("(tb-find \"\")", "#f");
    is("(tb-find \"ab\")", "#t");
    is("(tb-cursor)", "(tuple 0 0)");
    tb_text("\xe6\x97\xa5\xe6\x9c\xac");
    is("(tb-seek 1)", "1");
    is("(tb-seek 4 #t)", "3");
    is("(tb-seek 99)", "6");
}

static void test_tbx_dirty(void) {
    eval_context();
    tb_text("abc");
    is("(tb-dirty)", "#f");
    press('x', 1);
    is("(tb-line 0 0 10)", "(tuple \"xabc\" -1 -1)");
    is("(tb-dirty)", "#t");
    is("(tb-undo)", "#t");
    is("(tb-line 0 0 10)", "(tuple \"abc\" -1 -1)");
    is("(tb-dirty)", "#f");
    is("(tb-undo)", "#f");
    tbx_register(&E, buf_fn, NULL);
    press('x', 1);
    is("(tb-undo)", "#t");
    is("(tb-dirty)", "#t");
    tbx_register(&E, buf_fn, saved_fn);
}

static void test_tbx_keys(void) {
    eval_context();
    tb_text("one\ntwo\nthree\nfour\nfive");
    press('h', 1);
    press(0xE9, 1);
    is("(tb-line 0 0 10)", "(tuple \"h\xc3\xa9one\" -1 -1)");
    is("(tb-cursor)", "(tuple 0 2)");
    press(0x7f, 1);
    is("(tb-line 0 0 10)", "(tuple \"hone\" -1 -1)");
    press(FT_KEY_DEL, 1);
    is("(tb-line 0 0 10)", "(tuple \"hne\" -1 -1)");
    press(FT_KEY_RIGHT, 1);
    press('\r', 1);
    is("(tb-lines)", "6");
    is("(tb-line 0 0 10)", "(tuple \"hn\" -1 -1)");
    is("(tb-line 1 0 10)", "(tuple \"e\" -1 -1)");
    press('\t', 1);
    is("(tb-line 1 0 10)", "(tuple \"    e\" -1 -1)");
    is("(tb-cursor)", "(tuple 1 4)");
    press(FT_KEY_HOME, 1);
    is("(tb-cursor)", "(tuple 1 0)");
    press(FT_KEY_END, 1);
    is("(tb-cursor)", "(tuple 1 5)");
    press(FT_KEY_LEFT, 1);
    press(FT_KEY_UP, 1);
    is("(tb-cursor)", "(tuple 0 2)");
    press(FT_KEY_UP, 1);
    is("(tb-cursor)", "(tuple 0 2)");
    press(FT_KEY_PGDN, 3);
    is("(tb-cursor)", "(tuple 3 4)"); /* the goal column, 4, is kept */
    press(FT_KEY_PGDN, 0);
    is("(tb-cursor)", "(tuple 4 4)");
    press(FT_KEY_PGUP, 3);
    is("(tb-cursor)", "(tuple 1 4)");
    press(FT_KEY_END | FT_KEY_CTRL, 1);
    is("(tb-cursor)", "(tuple 5 4)");
    press(FT_KEY_HOME | FT_KEY_CTRL, 1);
    is("(tb-cursor)", "(tuple 0 0)");
    press(0x19, 1);
    is("(tb-lines)", "5");
    is("(tb-line 0 0 10)", "(tuple \"    e\" -1 -1)");
    press(FT_KEY_RIGHT | FT_KEY_SHIFT, 1);
    press(FT_KEY_RIGHT | FT_KEY_SHIFT, 1);
    is("(tb-line 0 0 10)", "(tuple \"    e\" 0 2)");
    is("(tb-key 57472 1)", "#f");
    is("(tb-key 1 1)", "#f");
    is("(tb-line 0 0 10)", "(tuple \"    e\" 0 2)");
}

static void test_tbx_clipboard(void) {
    eval_context();
    tb_text("hello world");
    is("(tb-clip)", "\"\"");
    is("(tb-paste)", "#f");
    for (int i = 0; i < 5; i++) {
        press(FT_KEY_RIGHT | FT_KEY_SHIFT, 1);
    }
    is("(tb-copy)", "#t");
    is("(tb-clip)", "\"hello\"");
    press(FT_KEY_END, 1);
    is("(tb-paste)", "#t");
    is("(tb-line 0 0 20)", "(tuple \"hello worldhello\" -1 -1)");
    press(FT_KEY_HOME, 1);
    for (int i = 0; i < 6; i++) {
        press(FT_KEY_RIGHT | FT_KEY_SHIFT, 1);
    }
    is("(tb-cut)", "#t");
    is("(tb-clip)", "\"hello \"");
    is("(tb-line 0 0 20)", "(tuple \"worldhello\" -1 -1)");
    press(FT_KEY_END | FT_KEY_SHIFT, 1);
    press(FT_KEY_INS | FT_KEY_CTRL, 1);
    is("(tb-clip)", "\"worldhello\"");
    press(FT_KEY_DEL | FT_KEY_SHIFT, 1);
    is("(tb-line 0 0 20)", "(tuple \"\" -1 -1)");
    press(FT_KEY_INS | FT_KEY_SHIFT, 1);
    press(FT_KEY_INS | FT_KEY_SHIFT, 1);
    is("(tb-line 0 0 30)", "(tuple \"worldhelloworldhello\" -1 -1)");
    press(FT_KEY_INS, 1);
    is("(tb-line 0 0 30)", "(tuple \"worldhelloworldhello\" -1 -1)");
}

static void test_tbx_bytes(void) {
    eval_context();
    static const uint8_t raw[] = {'a', 'b', 'c', 0x80, 0xE9, 0xDB, 0x01, 0x0A, 'z'};
    tb_open(raw, sizeof(raw));
    is("(tb-size)", "9");
    is("(tb-offset)", "0");
    is("(tb-seek 4)", "4");
    is("(tb-offset)", "4");
    is("(tb-seek 99)", "9");
    is("(tb-byte-at 3)", "128");
    is("(tb-byte-at 8)", "122");
    is("(tb-byte-at 9)", "-1");
    is("(tb-hex-row 0 9 #f)", "(tuple \"61 62 63 80 E9 DB 01 0A  7A\" \"abc.....z\")");
    is("(tb-hex-row 0 9 #t)", "(tuple \"61 62 63 80 E9 DB 01 0A  7A\" "
                              "\"abc\xc3\x87\xce\x98\xe2\x96\x88\xe2\x98\xba\xe2\x97\x99z\")");
    is("(tb-hex-row 7 4 #f)", "(tuple \"0A 7A      \" \".z  \")");
    is("(tb-hex-row 9 2 #t)", "(tuple \"     \" \"  \")");
    is("(tb-hex-row 0 0 #f)", "(tuple \"\" \"\")");
    is("(tb-set-byte 3 65)", "#t");
    is("(tb-byte-at 3)", "65");
    is("(tb-set-byte 4 (+ (* 4 16) (% (tb-byte-at 4) 16)))", "#t");
    is("(tb-byte-at 4)", "73");
    is("(tb-set-byte 4 (+ (- (tb-byte-at 4) (% (tb-byte-at 4) 16)) 15))", "#t");
    is("(tb-byte-at 4)", "79");
    is("(tb-lines)", "2");
    is("(tb-set-byte 9 10)", "#t");
    is("(tb-size)", "10");
    is("(tb-lines)", "3");
    is("(tb-set-byte 11 1)", "#f");
    is("(tb-dirty)", "#t");
    is("(tb-seek 0)", "0");
    is("(tb-find-bytes \"41 4f\")", "#t");
    is("(tb-offset)", "3");
    is("(tb-find-bytes \"7A0A\")", "#t");
    is("(tb-offset)", "8");
    is("(tb-find-bytes \"7a 0a\")", "#t");
    is("(tb-offset)", "8");
    is("(tb-find-bytes \"ff\")", "#f");
    is("(tb-find-bytes \"61 62 63 41 4F DB 01 0A 7A 0A 00\")", "#f");
    is("(hex 255 4)", "\"00FF\"");
    is("(hex 0 1)", "\"0\"");
    is("(hex 4096 0)", "\"1000\"");
    is("(hex 1 64)", "\"0000000000000000000000000000000000000000000000000000000000000001\"");
    is("(bin 5 8)", "\"00000101\"");
    is("(bin 0 0)", "\"0\"");
    is("(cp437 65)", "\"A\"");
    is("(cp437 0)", "\" \"");
    is("(cp437 1)", "\"\xe2\x98\xba\"");
    is("(cp437 128)", "\"\xc3\x87\"");
    is("(cp437 219)", "\"\xe2\x96\x88\"");
    is("(cp437 233)", "\"\xce\x98\"");
    is("(cp437 255)", "\"\xc2\xa0\"");
}

static void test_tbx_spans(void) {
    eval_context();
    tb_text("int x = 42; /* a\nb */ \"s\" y\n\tfor");
    is("(tb-spans 0 0 40 \"c\")",
       "(list (tuple \"int\" 4 #f) (tuple \" x = \" 0 #f) (tuple \"42\" 3 #f) (tuple \"; \" 0 #f) "
       "(tuple \"/* a\" 1 #f))");
    is("(tb-spans 1 0 40 \"c\")", "(list (tuple \"b */\" 1 #f) (tuple \" \" 0 #f) (tuple "
                                  "\"\\\"s\\\"\" 2 #f) (tuple \" y\" 0 #f))");
    is("(tb-spans 2 0 40 \"c\")", "(list (tuple \"    \" 0 #f) (tuple \"for\" 4 #f))");
    /* a rune astride the left edge is not shown, as tb-line does not show it */
    is("(tb-spans 2 2 3 \"c\")", "(list (tuple \"f\" 4 #f))");
    is("(tb-line 2 2 3)", "(tuple \"f\" -1 -1)");
    is("(tb-spans 0 0 40 \"nope\")", "(list (tuple \"int x = 42; /* a\" 0 #f))");
    is("(tb-spans 9 0 40 \"c\")", "(list)");
    is("(tb-spans 0 0 0 \"c\")", "(list)");
    is("(tb-goto 1)", "#t");
    press(FT_KEY_RIGHT | FT_KEY_SHIFT, 1);
    press(FT_KEY_RIGHT | FT_KEY_SHIFT, 1);
    is("(tb-spans 0 0 40 \"c\")",
       "(list (tuple \"in\" 4 #t) (tuple \"t\" 4 #f) (tuple \" x = \" 0 #f) (tuple \"42\" 3 #f) "
       "(tuple \"; \" 0 #f) (tuple \"/* a\" 1 #f))");
    /* an edit above a line changes what the highlighter carries into it */
    press(FT_KEY_HOME, 1);
    press('/', 1);
    press('/', 1);
    is("(tb-spans 0 0 40 \"c\")", "(list (tuple \"//int x = 42; /* a\" 1 #f))");
    is("(tb-spans 1 0 40 \"c\")",
       "(list (tuple \"b */ \" 0 #f) (tuple \"\\\"s\\\"\" 2 #f) (tuple \" y\" 0 #f))");
}

static void test_tbx_format(void) {
    eval_context();
    static const char src[] = "(def x   1)\n(if a\n  b c)\n";
    static uint8_t mem[4096];
    size_t want_len = 0;
    const char *want = filo_fmt(src, sizeof(src) - 1, 2, 80, mem, sizeof(mem), &want_len);
    check(want != NULL && (want_len != sizeof(src) - 1 || memcmp(want, src, want_len) != 0),
          "filofmt lays the sample out differently");
    tb_text(src);
    is("(tb-seek 20)", "20");
    check(TB.text[20] == 'b', "the cursor is on b");
    is("(tb-format)", "\"\"");
    check(TB.len == want_len && memcmp(TB.text, want, want_len) == 0, "the text is filofmt's");
    check(TB.cur < TB.len && TB.text[TB.cur] == 'b', "the cursor stays on b");
    is("(tb-dirty)", "#t");
    is("(tb-undo)", "#t");
    check(TB.len == sizeof(src) - 1 && memcmp(TB.text, src, TB.len) == 0, "undo takes it back");
    is("(tb-format)", "\"\"");
    is("(tb-format)", "\"\"");
    check(TB.len == want_len, "formatting twice is formatting once");
    tb_text("(def x \"open");
    is("(tb-format)", "\"a string or a paren does not close\"");
    tb_text("(def x 1))");
    is("(tb-format)", "\"a string or a paren does not close\"");
    tb_text("(def x 1)\n");
    is("(tb-format)", "\"\"");
    is("(tb-dirty)", "#f");
}

static void test_tbx_errors(void) {
    eval_context();
    tb_text("abc\n");
    static const char *const bad[] = {
        "(tb-lines 1)",
        "(tb-line 0 0)",
        "(tb-line \"a\" 0 0)",
        "(tb-line 0 -1 0)",
        "(tb-line 0 0 #t)",
        "(tb-spans 0 0 0)",
        "(tb-spans 0 0 0 1)",
        "(tb-spans \"a\" 0 0 \"c\")",
        "(tb-cursor 1)",
        "(tb-view 1)",
        "(tb-view \"a\" 1)",
        "(tb-key 1)",
        "(tb-key \"a\" 1)",
        "(tb-key 97 -1)",
        "(tb-dirty 1)",
        "(tb-find)",
        "(tb-find 1)",
        "(tb-goto)",
        "(tb-goto \"x\")",
        "(tb-clip 1)",
        "(tb-copy 1)",
        "(tb-cut 1)",
        "(tb-paste 1)",
        "(tb-undo 1)",
        "(tb-size 1)",
        "(tb-offset 1)",
        "(tb-seek)",
        "(tb-seek 1 #t 3)",
        "(tb-seek \"a\")",
        "(tb-byte-at)",
        "(tb-byte-at #t)",
        "(tb-set-byte 1)",
        "(tb-set-byte 1 \"a\")",
        "(tb-set-byte 0 256)",
        "(tb-hex-row 0 8)",
        "(tb-hex-row 0 8 1)",
        "(tb-hex-row \"a\" 8 #t)",
        "(tb-hex-row 0 65 #t)",
        "(tb-find-bytes)",
        "(tb-find-bytes 1)",
        "(tb-find-bytes \"\")",
        "(tb-find-bytes \"4\")",
        "(tb-find-bytes \"zz\")",
        "(tb-find-bytes \"41 4\")",
        "(tb-format 1)",
        "(hex 1)",
        "(hex -1 2)",
        "(hex 1.5 2)",
        "(hex 1 65)",
        "(hex \"a\" 1)",
        "(bin 1)",
        "(bin \"a\" 1)",
        "(cp437)",
        "(cp437 256)",
        "(cp437 -1)",
        "(cp437 \"a\")",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        fails(bad[i]);
    }
    is("(tb-line 0 0 10)", "(tuple \"abc\" -1 -1)");
    is("(tb-dirty)", "#f");
}

/* An editor: the key entry hands the keys to the buffer, draw shows its
   first line, the builtins reached through the program's extend hook. */
static void test_tbx_app(void) {
    static const entry_src p[] = {
        {"init", "(def z 0)"},
        {"draw", "(letv (text a b) (tb-line 0 0 W) (print-at 0 0 text)) "
                 "(print-at 1 0 (string (tb-cursor)))"},
        {"key", "(if (= KEY KEY_ESC) (done) (tb-key KEY 5))"},
    };
    spec = &editing;
    tb_text("abc");
    check(build(p, 3), "builds the editor");
    check(start(), "starts the editor");
    check(strncmp(row(0), "abc ", 4) == 0, "draw shows the buffer");
    key('X');
    check(strncmp(row(0), "Xabc ", 5) == 0, "a key reaches the buffer");
    keys("\x1b[D");
    key('Y');
    check(strncmp(row(0), "YXabc ", 6) == 0, "the arrows move it");
    check(strncmp(row(1), "(tuple 0 1)", 11) == 0, "the cursor follows");
    key(0x7f);
    check(strncmp(row(0), "Xabc ", 5) == 0, "backspace too");
    key(0x1b);
    app_tick(&A, 200);
    check(app_done(&A), "Esc still leaves");
    spec = &app_program;
}

/* ---- the POSIX loop, on a pty ---- */

enum {
    PTY_COLS = 50,
    PTY_ROWS = 12,
    WAIT_MS = 5000,
};

static int b_tty_lend(filo_ctx *ctx, const filo_value *a, uint32_t n, filo_value *res) {
    (void)ctx;
    (void)a;
    (void)n;
    tty_suspend();
    tty_resume(&A);
    *res = filo_cstring("");
    return FILO_OK;
}

static bool tty_extend(app *a, filo_ctx *ctx) {
    (void)a;
    return filo_register_builtin(ctx, "lend", b_tty_lend) == FILO_OK;
}

static const app_spec on_tty = {"t", "1.0", false, 0, 0, 0, tty_extend};

/* Each paint lands on a row of its own, with no blank in it: the whole
   text goes to the terminal in one piece, so the master can read it. */
static const entry_src tty_program[] = {
    {"init", "(def n 0) (def last \"none\")"},
    {"draw", "(print-at (% n H) 0 (str-fmt \"%dx%d:%d:%s\" W H n last))"},
    {"key", "(set n (+ n 1)) (cond ((= KEY 113) (done)) ((= KEY 115) (lend)) "
            "((= KEY KEY_ESC) (set last \"esc\")) (else (set last (str-fmt \"%d\" KEY))))"},
};

/* The child's side: the slave is its terminal, the program runs on it as
   a main would run it, and the exit status says how it ended. It leaves
   through exit, not _exit: a coverage build appends the child's counters
   to the profile at exit, and _exit would lose them. */
static void on_slave(int slave) {
    (void)alarm(10);
    (void)setsid();
    (void)ioctl(slave, TIOCSCTTY, 0);
    (void)dup2(slave, STDIN_FILENO);
    (void)dup2(slave, STDOUT_FILENO);
    (void)dup2(slave, STDERR_FILENO);
    if (slave > STDERR_FILENO) {
        (void)close(slave);
    }
    uint16_t cols = 0;
    uint16_t rows = 0;
    char why[128];
    if (!tty_open(&cols, &rows, why, sizeof(why))) {
        exit(3);
    }
    if (cols != PTY_COLS || rows != PTY_ROWS) {
        exit(4);
    }
    if (!app_start(&A, spec, fbb, fbb_len, cols, rows, why, sizeof(why))) {
        exit(5);
    }
    bool finished = tty_run(&A);
    tty_close();
    exit(finished ? 0 : 1);
}

static pid_t spawn_on_pty(int *master) {
    int m = -1;
    int s = -1;
    struct winsize ws = {PTY_ROWS, PTY_COLS, 0, 0};
    if (openpty(&m, &s, NULL, NULL, &ws) != 0) {
        return -1;
    }
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        (void)close(m);
        on_slave(s);
    }
    (void)close(s);
    *master = m;
    return pid;
}

static char seen[64 * 1024]; /* everything the master read, NULs as blanks */
static size_t seen_len;
static size_t seen_from; /* where the last expectation ended */

/* Reads the master until needle arrives after the last match, or WAIT_MS
   pass, or the terminal ends. */
static bool expect(int master, const char *needle) {
    struct timespec t0;
    (void)clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        const char *at = strstr(seen + seen_from, needle);
        if (at != NULL) {
            seen_from = (size_t)(at - seen) + strlen(needle);
            return true;
        }
        struct timespec now;
        (void)clock_gettime(CLOCK_MONOTONIC, &now);
        long spent = ((now.tv_sec - t0.tv_sec) * 1000L) + ((now.tv_nsec - t0.tv_nsec) / 1000000L);
        if (spent >= WAIT_MS) {
            printf("  waited for %s, have: %s\n", needle, seen + seen_from);
            return false;
        }
        struct pollfd p = {master, POLLIN, 0};
        int r = poll(&p, 1, (int)(WAIT_MS - spent));
        if (r <= 0) {
            continue;
        }
        if (seen_len + 1 >= sizeof(seen)) {
            return false;
        }
        ssize_t got = read(master, seen + seen_len, sizeof(seen) - 1 - seen_len);
        if (got <= 0) {
            return false;
        }
        for (ssize_t i = 0; i < got; i++) {
            if (seen[seen_len + (size_t)i] == '\0') {
                seen[seen_len + (size_t)i] = ' ';
            }
        }
        seen_len += (size_t)got;
        seen[seen_len] = '\0';
    }
}

static void type(int master, const char *s) {
    (void)write(master, s, strlen(s));
}

static int exit_status(pid_t pid) {
    int st = 0;
    if (waitpid(pid, &st, 0) != pid) {
        return -1;
    }
    if (WIFEXITED(st)) {
        return WEXITSTATUS(st);
    }
    return 100 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
}

static bool tty_ready(int *master, pid_t *pid) {
    seen_len = 0;
    seen_from = 0;
    seen[0] = '\0';
    spec = &on_tty;
    if (!build(tty_program, 3)) {
        return false;
    }
    *pid = spawn_on_pty(master);
    return *pid > 0;
}

static void test_tty_run(void) {
    int master = -1;
    pid_t pid = 0;
    check(tty_ready(&master, &pid), "the program runs on a pty");
    if (pid <= 0) {
        spec = &app_program;
        return;
    }
    check(expect(master, "\x1b[?1049h"), "tty_open then app_start take the alternate screen");
    check(expect(master, "50x12:0:none"), "the first paint comes at the pty's size");
    type(master, "x");
    check(expect(master, "50x12:1:120"), "a key typed on the master reaches the program");
    struct winsize ws = {14, 60, 0, 0};
    check(ioctl(master, TIOCSWINSZ, &ws) == 0, "the master resizes");
    check(expect(master, "60x14:1:120"), "SIGWINCH repaints at the new size");
    type(master, "\x1b[200~ab\x1b[201~");
    check(expect(master, "60x14:3:98"), "a paste arrives whole");
    check(strstr(seen, "60x14:2:97") == NULL, "with no paint in the middle of it");
    type(master, "\x1b");
    check(expect(master, "60x14:4:esc"), "a lone Esc is a key once the wait passes");
    type(master, "s");
    check(expect(master, "\x1b[0m\x1b[?25h\x1b[?2004l\x1b[?1049l"),
          "suspend gives the terminal back");
    check(expect(master, "\x1b[?1049h\x1b[?2004h\x1b[H\x1b[2J"), "resume takes it again");
    check(expect(master, "60x14:5:esc"), "and repaints at its size");
    type(master, "q");
    check(expect(master, "\x1b[0m\x1b[?25h\x1b[?2004l\x1b[?1049l"),
          "done: the terminal is put back");
    check(exit_status(pid) == 0, "tty_run returned true: exit 0");
    (void)close(master);
    spec = &app_program;
}

static void test_tty_hangup(void) {
    int master = -1;
    pid_t pid = 0;
    check(tty_ready(&master, &pid), "the program runs on a second pty");
    if (pid <= 0) {
        spec = &app_program;
        return;
    }
    check(expect(master, "50x12:0:none"), "the first paint");
    (void)close(master);
    check(exit_status(pid) == 1, "the terminal gone, tty_run returned false: exit 1");
    spec = &app_program;
}

/* fn run with fd (1 or 2) on a pipe, and stdin on an empty one when
   no_tty: what it wrote, and what it returned. */
static int captured(int fd, bool no_tty, int (*fn)(int, char **), int argc, char **argv, char *text,
                    size_t cap) {
    int from[2];
    int in[2];
    if (pipe(from) != 0 || pipe(in) != 0) {
        return -1;
    }
    fflush(stdout);
    fflush(stderr);
    int saved_fd = dup(fd);
    int saved_in = dup(STDIN_FILENO);
    (void)dup2(from[1], fd);
    (void)close(from[1]);
    if (no_tty) {
        (void)dup2(in[0], STDIN_FILENO);
    }
    (void)close(in[0]);
    (void)close(in[1]);
    int rc = fn(argc, argv);
    fflush(stdout);
    fflush(stderr);
    (void)dup2(saved_fd, fd);
    (void)dup2(saved_in, STDIN_FILENO);
    (void)close(saved_fd);
    (void)close(saved_in);
    ssize_t n = read(from[0], text, cap - 1);
    text[n > 0 ? (size_t)n : 0] = '\0';
    (void)close(from[0]);
    return rc;
}

static int main_of(int argc, char **argv) {
    return tty_main(argc, argv, &A, spec, fbb, fbb_len, "usage: t [ARG]\n");
}

static int open_of(int argc, char **argv) {
    (void)argc;
    (void)argv;
    uint16_t cols = 0;
    uint16_t rows = 0;
    char why[64];
    bool ok = tty_open(&cols, &rows, why, sizeof(why));
    fputs(why, stderr);
    return ok ? 0 : 1;
}

static void test_tty_main(void) {
    static char text[1024];
    spec = &on_tty;
    check(build(tty_program, 3), "builds for tty_main");
    char *help[] = {"x", "-h", NULL};
    check(captured(STDOUT_FILENO, false, main_of, 2, help, text, sizeof(text)) == 0 &&
              strcmp(text, "usage: t [ARG]\n") == 0,
          "-h: usage on stdout, 0");
    char *help2[] = {"x", "--help", NULL};
    check(captured(STDOUT_FILENO, false, main_of, 2, help2, text, sizeof(text)) == 0 &&
              strcmp(text, "usage: t [ARG]\n") == 0,
          "--help too");
    char *version[] = {"x", "--version", NULL};
    check(captured(STDOUT_FILENO, false, main_of, 2, version, text, sizeof(text)) == 0 &&
              strcmp(text, "t 1.0\n") == 0,
          "--version: name and version");
    char *three[] = {"x", "a", "b", NULL};
    check(captured(STDERR_FILENO, false, main_of, 3, three, text, sizeof(text)) == 2 &&
              strcmp(text, "usage: t [ARG]\n") == 0,
          "three arguments: usage on stderr, 2");
    char *one[] = {"x", NULL};
    check(captured(STDERR_FILENO, true, main_of, 1, one, text, sizeof(text)) == 1 &&
              strcmp(text, "t: needs a terminal\n") == 0,
          "no terminal: said on stderr, 1");
    check(captured(STDERR_FILENO, true, open_of, 0, NULL, text, sizeof(text)) == 1 &&
              strcmp(text, "needs a terminal") == 0,
          "tty_open without a terminal says so");
    spec = &app_program;
}

int main(void) {
    test_paint_and_done();
    test_kit_and_common();
    test_input_set();
    test_keep_canvas_and_tick();
    test_error_line();
    test_field();
    test_area();
    test_area_line_breaks();
    test_wrap();
    test_wrap_line_breaks();
    test_resize_inside_an_entry();
    test_refuses_missing();
    test_profile();
    test_click();
    test_report_score();
    test_tbx_lines();
    test_tbx_dirty();
    test_tbx_keys();
    test_tbx_clipboard();
    test_tbx_bytes();
    test_tbx_spans();
    test_tbx_format();
    test_tbx_errors();
    test_tbx_app();
    test_tty_run();
    test_tty_hangup();
    test_tty_main();
    if (nfail > 0) {
        printf("%zu app tests failed\n", nfail);
        return 1;
    }
    puts("all app tests passed");
    return 0;
}
