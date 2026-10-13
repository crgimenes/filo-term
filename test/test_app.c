/* The app core driven the way a terminal drives it: a small program
   compiled here, its bytes fed in, what reaches the terminal checked. */

#include <stdio.h>
#include <string.h>

#include "app.h"
#include "keys.h"

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
    if (nfail > 0) {
        printf("%zu app tests failed\n", nfail);
        return 1;
    }
    puts("all app tests passed");
    return 0;
}
