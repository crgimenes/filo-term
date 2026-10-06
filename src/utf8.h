#ifndef FT_UTF8_H
#define FT_UTF8_H

#include <stddef.h>
#include <stdint.h>

enum {
    UTF8_REPLACEMENT = 0xFFFD,
    UTF8_MAX_BYTES = 4,
};

/* Incremental decoder: feed one byte at a time; a completed rune is reported
   together with the exact bytes that produced it. */
typedef struct {
    uint32_t cp;
    uint8_t need;
    uint8_t have;
    uint8_t bytes[UTF8_MAX_BYTES];
} utf8_dec;

typedef enum {
    UTF8_MORE,  /* byte consumed, rune not complete yet */
    UTF8_RUNE,  /* rune completed; see out fields */
    UTF8_ERROR, /* invalid sequence; decoder reset, byte NOT consumed when resync */
} utf8_result;

void utf8_dec_init(utf8_dec *d);

/* Feeds one byte. On UTF8_RUNE, *cp is the code point and bytes/len describe
   its encoding. On UTF8_ERROR the caller should treat the pending bytes as
   U+FFFD; if resync is nonzero the same byte must be fed again (it starts a
   new sequence). */
utf8_result utf8_dec_feed(utf8_dec *d, uint8_t b, uint32_t *cp, int *resync);

/* Terminal cell width: 0 for combining/zero-width, 2 for wide/fullwidth and
   emoji presentation, 1 otherwise. Practical wcwidth subset mirrored from
   compterm's mterm — cell arithmetic must agree with what xterm/tmux render. */
int utf8_width(uint32_t cp);

/* Display width of a UTF-8 string (invalid bytes count as width 1). */
size_t utf8_swidth(const char *s);

/* Byte length of the last rune in buf[0..len); 0 when len == 0. Used by
   backspace to remove a whole rune. */
size_t utf8_last_rune_len(const uint8_t *buf, size_t len);

/* Encodes cp into buf; returns the byte count (1..4). Invalid code points
   encode as U+FFFD. */
size_t utf8_encode(uint8_t buf[UTF8_MAX_BYTES], uint32_t cp);

#endif
