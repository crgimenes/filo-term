#ifndef FT_HL_H
#define FT_HL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Syntax highlighting: one generic tokenizer (comments, strings, numbers)
   plus a keyword table per language, for the pager's fenced code blocks
   and the editor's text. A fence with no language stays uncolored — most
   of those carry terminal output, and coloring that wrong reads worse than
   not coloring it. */

typedef enum {
    HL_NONE = 0,
    HL_SHELL,
    HL_GO,
    HL_C,
    HL_LUA,
    HL_ASM,
    HL_JSON,
    HL_SQL,
    HL_JS,
    HL_FILO,
    HL_REDCODE,
} hl_lang;

/* Carried across lines: block comments and raw strings do not end at \n. */
typedef enum {
    HL_ST_NORMAL = 0,
    HL_ST_BLOCK_COMMENT,
    HL_ST_RAW_STRING,
    HL_ST_STRING, /* a string that spans lines, where the language allows it */
} hl_carry;

/* What a stretch of a line is. */
typedef enum {
    HL_PLAIN = 0,
    HL_COMMENT,
    HL_STRING,
    HL_NUMBER,
    HL_KEYWORD,
} hl_class;

typedef struct {
    uint8_t lang;  /* hl_lang */
    uint8_t carry; /* hl_carry */
} hl_state;

/* Where rendered bytes go: the host's sink, with its own pointer. */
typedef void (*hl_emit)(void *user, const uint8_t *data, size_t n);

/* Where a line's stretches go: bytes [from, to) of the line are cls. */
typedef void (*hl_span)(void *user, hl_class cls, size_t from, size_t to);

/* Maps a fence info string ("go", "bash", "golang"...) to a language. */
hl_lang hl_lang_of(const char *info);

/* The same, from a file's name: what follows the last dot reads as a
   fence's info string, so "init.filo" is filo and "mars.c" is c. */
hl_lang hl_lang_of_name(const char *name);

void hl_begin(hl_state *h, hl_lang lang);

/* Emits one code line, colored, through emit. The line carries no newline. */
void hl_line(hl_state *h, const uint8_t *line, size_t n, hl_emit emit, void *user);

/* Tells span each stretch of one line, in order and covering it whole;
   span may be NULL, to carry h past a line nobody shows. */
void hl_spans(hl_state *h, const uint8_t *line, size_t n, hl_span span, void *user);

#endif
