#ifndef FT_KEYIN_H
#define FT_KEYIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "utf8.h"

/* A terminal's input decoded into keys: UTF-8 into runes, CSI and SS3
   sequences into the synthetic codes of keys.h (modifiers included), ESC
   alone told apart from the start of a sequence by time, bracketed paste
   marked, and whatever the terminal sends back unasked (OSC, DCS and their
   kin) swallowed whole. Who gets each key is the caller's business. */
enum {
    KEYIN_ESC_TIMEOUT_MS = 100, /* after ESC, what arrives later was typed */
    KEYIN_CSI_MAX = 16,
};

typedef enum {
    KEYIN_KEY,         /* cp is a rune or a code of keys.h */
    KEYIN_ESC,         /* ESC, alone */
    KEYIN_PASTE_BEGIN, /* what follows was pasted, not typed */
    KEYIN_PASTE_END,
} keyin_event;

typedef void (*keyin_emit)(void *user, keyin_event ev, uint32_t cp);

typedef struct {
    utf8_dec dec; /* its bytes are the rune just decoded */
    uint8_t esc;  /* 0 none, 1 after ESC, 2 in CSI/SS3, 3 in a control string, 4 ESC in one */
    uint32_t esc_ms;
    uint8_t csi[KEYIN_CSI_MAX];
    size_t csi_len;
    bool ss3;
} keyin;

void keyin_init(keyin *k);
void keyin_feed(keyin *k, uint8_t b, keyin_emit emit, void *user);
/* Time passing: a lone ESC becomes a key, a control string cut short
   stops eating the keyboard. */
void keyin_tick(keyin *k, uint32_t ms, keyin_emit emit, void *user);

#endif
