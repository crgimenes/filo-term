#include <string.h>

#include "hl.h"

/* Token colors, each a self-contained SGR (see the invariant in md.c). */
#define HL_SGR_PLAIN "\x1b[0m"
#define HL_SGR_COMMENT "\x1b[0;2m"
#define HL_SGR_STRING "\x1b[0;32m"
#define HL_SGR_NUMBER "\x1b[0;35m"
#define HL_SGR_KEYWORD "\x1b[0;36m"

/* Words are stored space-delimited so a lookup is one strstr of " word ". */
typedef struct {
    const char *names; /* fence info strings that select this language */
    const char *kw;
    const char *lc1;      /* line comment opener */
    const char *lc2;      /* second opener, or NULL */
    bool block_comment;   /* C style: slash-star ... star-slash */
    bool backtick_string; /* Go and JS raw strings span lines */
    bool single_quote;    /* ' opens a string rather than a char literal */
    bool lisp;            /* names hold - ? ! and friends; a " string spans lines */
    bool fold_case;       /* keywords match in either case */
} hl_def;

static const hl_def defs[] = {
    [HL_SHELL] =
        {
            .names = " bash sh shell zsh console ",
            .kw = " if then else elif fi for while do done case esac in function return local "
                  "export source alias set unset echo printf read cd exit trap shift eval "
                  "test true false break continue declare readonly ",
            .lc1 = "#",
            .single_quote = true,
        },
    [HL_GO] =
        {
            .names = " go golang ",
            .kw = " break case chan const continue default defer else fallthrough for func go "
                  "goto if import interface map package range return select struct switch type "
                  "var nil true false iota make new len cap append copy delete panic recover "
                  "string bool byte rune int int8 int16 int32 int64 uint uint8 uint16 uint32 "
                  "uint64 uintptr float32 float64 complex64 complex128 error any ",
            .lc1 = "//",
            .block_comment = true,
            .backtick_string = true,
        },
    [HL_C] =
        {
            .names = " c h cpp c++ ",
            .kw = " auto break case char const continue default do double else enum extern float "
                  "for goto if inline int long register restrict return short signed sizeof "
                  "static struct switch typedef union unsigned void volatile while bool true "
                  "false NULL size_t uint8_t uint16_t uint32_t uint64_t int8_t int16_t int32_t "
                  "int64_t include define ifndef endif pragma ",
            .lc1 = "//",
            .block_comment = true,
        },
    [HL_LUA] =
        {
            .names = " lua ",
            .kw = " and break do else elseif end false for function goto if in local nil not "
                  "or repeat return then true until while self require pairs ipairs print "
                  "type tostring tonumber table string math os io ",
            .lc1 = "--",
            .single_quote = true,
        },
    [HL_ASM] =
        {
            .names = " asm assembly nasm gas s ",
            .kw = " mov movl movq lea push pop add sub mul div imul idiv inc dec cmp test and "
                  "or xor not neg shl shr sal sar jmp je jne jz jnz jg jge jl jle ja jb call "
                  "ret int syscall nop loop enter leave hlt db dw dd dq resb resw resd equ "
                  "org section segment global extern bits times ",
            .lc1 = ";",
            .lc2 = "#",
            .single_quote = true,
        },
    [HL_JSON] =
        {
            .names = " json ",
            .kw = " true false null ",
        },
    [HL_SQL] =
        {
            .names = " sql psql postgres mysql sqlite ",
            .kw = " select from where insert into values update set delete create table drop "
                  "alter add column index view join left right inner outer full on group by "
                  "order having limit offset union all distinct as and or not null is like "
                  "between exists count sum avg min max primary key foreign references default "
                  "unique constraint begin commit rollback returning with ",
            .lc1 = "--",
            .block_comment = true,
            .single_quote = true,
        },
    [HL_JS] =
        {
            .names = " javascript js typescript ts node ",
            .kw = " async await break case catch class const continue debugger default delete "
                  "do else export extends finally for function if import in instanceof let new "
                  "of return static super switch this throw try typeof var void while with "
                  "yield null true false undefined ",
            .lc1 = "//",
            .block_comment = true,
            .backtick_string = true,
            .single_quote = true,
        },
    [HL_FILO] =
        {
            .names = " filo ",
            .kw = " def fn let letv if cond else do set and or return exit ",
            .lc1 = ";",
            .lisp = true,
        },
    [HL_REDCODE] =
        {
            .names = " redcode red ",
            .kw = " dat mov add sub mul div mod jmp jmz jmn djn cmp seq sne slt spl nop org end "
                  "equ for rof pin ldp stp ",
            .lc1 = ";",
            .fold_case = true,
        },
};

hl_lang hl_lang_of(const char *info) {
    if (info == NULL || info[0] == '\0') {
        return HL_NONE;
    }
    char probe[24];
    size_t n = 0;
    probe[n] = ' ';
    n++;
    size_t i = 0;
    while (info[i] != '\0' && n + 2 < sizeof(probe)) {
        char c = info[i];
        i++;
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        probe[n] = c;
        n++;
    }
    probe[n] = ' ';
    n++;
    probe[n] = '\0';
    for (size_t l = HL_SHELL; l < sizeof(defs) / sizeof(defs[0]); l++) {
        if (defs[l].names != NULL && strstr(defs[l].names, probe) != NULL) {
            return (hl_lang)l;
        }
    }
    return HL_NONE;
}

hl_lang hl_lang_of_name(const char *name) {
    if (name == NULL) {
        return HL_NONE;
    }
    size_t i = strlen(name);
    while (i > 0 && name[i - 1] != '/' && name[i - 1] != '.') {
        i--;
    }
    if (i == 0 || name[i - 1] != '.') {
        return HL_NONE;
    }
    return hl_lang_of(name + i);
}

void hl_begin(hl_state *h, hl_lang lang) {
    h->lang = (uint8_t)lang;
    h->carry = HL_ST_NORMAL;
}

static bool word_byte(const hl_def *d, uint8_t b) {
    if ((b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || (b >= '0' && b <= '9') || b == '_') {
        return true;
    }
    if (d->lisp && b != 0 && strchr("-?!*<>=/+%", b) != NULL) {
        return true;
    }
    return false;
}

static bool is_keyword(const hl_def *d, const uint8_t *s, size_t n) {
    if (d->kw == NULL || n == 0 || n > 20) {
        return false;
    }
    char probe[24];
    probe[0] = ' ';
    for (size_t i = 0; i < n; i++) {
        uint8_t c = s[i];
        if (d->fold_case && c >= 'A' && c <= 'Z') {
            c = (uint8_t)(c - 'A' + 'a');
        }
        probe[i + 1] = (char)c;
    }
    probe[n + 1] = ' ';
    probe[n + 2] = '\0';
    return strstr(d->kw, probe) != NULL;
}

static bool starts_with(const uint8_t *s, size_t n, const char *pat) {
    if (pat == NULL || n == 0 || s[0] != (uint8_t)pat[0]) {
        return false; /* most bytes open nothing: settled before strlen */
    }
    size_t len = strlen(pat);
    if (len == 0 || n < len) {
        return false;
    }
    return memcmp(s, pat, len) == 0;
}

typedef struct {
    hl_span span;
    void *user;
} out;

/* Tells the sink [from, to) is cls, when there is anything to tell. */
static void tell(const out *o, hl_class cls, size_t from, size_t to) {
    if (to > from && o->span != NULL) {
        o->span(o->user, cls, from, to);
    }
}

/* The end of a string that opened before i, or the line's; *closed says
   which. Escapes do not close it, except in a raw one. */
static size_t string_end(const uint8_t *line, size_t n, size_t i, uint8_t quote, bool *closed) {
    while (i < n && line[i] != quote) {
        if (line[i] == '\\' && quote != '`' && i + 1 < n) {
            i++; /* an escaped quote does not close the string */
        }
        i++;
    }
    *closed = i < n;
    return *closed ? i + 1 : i;
}

void hl_spans(hl_state *h, const uint8_t *line, size_t n, hl_span span, void *user) {
    const out sink = {span, user};
    const out *o = &sink;
    const hl_def *d = &defs[h->lang];
    if (h->lang == HL_NONE) {
        tell(o, HL_PLAIN, 0, n);
        return;
    }
    size_t i = 0;
    size_t plain = 0; /* start of the pending default-colored run */
    if (h->carry == HL_ST_BLOCK_COMMENT) {
        while (i < n && !starts_with(line + i, n - i, "*/")) {
            i++;
        }
        if (i < n) {
            i += 2;
            h->carry = HL_ST_NORMAL;
        }
        tell(o, HL_COMMENT, 0, i);
        plain = i;
    }
    if (h->carry == HL_ST_RAW_STRING || h->carry == HL_ST_STRING) {
        bool closed = false;
        i = string_end(line, n, i, h->carry == HL_ST_RAW_STRING ? '`' : '"', &closed);
        if (closed) {
            h->carry = HL_ST_NORMAL;
        }
        tell(o, HL_STRING, plain, i);
        plain = i;
    }
    while (i < n) {
        uint8_t b = line[i];
        if (starts_with(line + i, n - i, d->lc1) || starts_with(line + i, n - i, d->lc2)) {
            tell(o, HL_PLAIN, plain, i);
            tell(o, HL_COMMENT, i, n);
            return;
        }
        if (d->block_comment && starts_with(line + i, n - i, "/*")) {
            tell(o, HL_PLAIN, plain, i);
            size_t start = i;
            i += 2;
            while (i < n && !starts_with(line + i, n - i, "*/")) {
                i++;
            }
            if (i < n) {
                i += 2;
            } else {
                h->carry = HL_ST_BLOCK_COMMENT;
            }
            tell(o, HL_COMMENT, start, i);
            plain = i;
            continue;
        }
        if (b == '"' || (b == '\'' && d->single_quote) || (b == '`' && d->backtick_string)) {
            tell(o, HL_PLAIN, plain, i);
            size_t start = i;
            bool closed = false;
            i = string_end(line, n, i + 1, b, &closed);
            if (!closed && b == '`') {
                h->carry = HL_ST_RAW_STRING; /* raw strings survive the newline */
            } else if (!closed && b == '"' && d->lisp) {
                h->carry = HL_ST_STRING; /* and a lisp's strings do */
            }
            tell(o, HL_STRING, start, i);
            plain = i;
            continue;
        }
        if (b >= '0' && b <= '9' && (i == plain || !word_byte(d, line[i - 1]))) {
            tell(o, HL_PLAIN, plain, i);
            size_t start = i;
            while (i < n && (word_byte(d, line[i]) || line[i] == '.')) {
                i++;
            }
            tell(o, HL_NUMBER, start, i);
            plain = i;
            continue;
        }
        if (word_byte(d, b) && (i == 0 || !word_byte(d, line[i - 1]))) {
            size_t start = i;
            while (i < n && word_byte(d, line[i])) {
                i++;
            }
            if (is_keyword(d, line + start, i - start)) {
                tell(o, HL_PLAIN, plain, start);
                tell(o, HL_KEYWORD, start, i);
                plain = i;
            }
            continue;
        }
        i++;
    }
    tell(o, HL_PLAIN, plain, n);
}

/* Each class's color, a self-contained SGR (see the invariant in md.c). */
static const char *const sgr[] = {
    [HL_PLAIN] = HL_SGR_PLAIN,   [HL_COMMENT] = HL_SGR_COMMENT, [HL_STRING] = HL_SGR_STRING,
    [HL_NUMBER] = HL_SGR_NUMBER, [HL_KEYWORD] = HL_SGR_KEYWORD,
};

typedef struct {
    const uint8_t *line;
    hl_emit emit;
    void *user;
} painter;

/* cppcheck-suppress constParameterCallback ; hl_span's shape */
static void paint(void *user, hl_class cls, size_t from, size_t to) {
    const painter *p = user;
    p->emit(p->user, (const uint8_t *)sgr[cls], strlen(sgr[cls]));
    p->emit(p->user, p->line + from, to - from);
}

void hl_line(hl_state *h, const uint8_t *line, size_t n, hl_emit emit, void *user) {
    painter p = {line, emit, user};
    if (h->lang == HL_NONE) {
        paint(&p, HL_PLAIN, 0, n);
        return;
    }
    hl_spans(h, line, n, paint, &p);
    emit(user, (const uint8_t *)HL_SGR_PLAIN, strlen(HL_SGR_PLAIN));
}
