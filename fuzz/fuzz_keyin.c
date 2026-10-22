/* libFuzzer on the key decoder: any byte string is what a terminal sends,
   and none may fault or leave the decoder in a state it cannot leave. Time
   passes every few bytes, so a lone ESC becomes a key and a control string
   cut short stops eating the keyboard. */
#include <stdint.h>
#include <stdlib.h>

#include "keyin.h"

static void emit(void *user, keyin_event ev, uint32_t cp) {
    size_t *n = user;
    (*n)++;
    if (ev > KEYIN_MOUSE) {
        abort();
    }
    if (ev == KEYIN_KEY && cp > 0x10FFFF) { /* a rune, or a code of keys.h */
        abort();
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    keyin k;
    keyin_init(&k);
    size_t n = 0;
    for (size_t i = 0; i < size; i++) {
        keyin_feed(&k, data[i], emit, &n);
        if (k.csi_len > KEYIN_CSI_MAX || k.esc > 4) {
            abort();
        }
        if ((data[i] & 7) == 0) {
            keyin_tick(&k, 60, emit, &n);
        }
    }
    keyin_tick(&k, KEYIN_ESC_TIMEOUT_MS, emit, &n);
    keyin_tick(&k, KEYIN_ESC_TIMEOUT_MS, emit, &n);
    if (k.esc == 1) { /* the timeout has passed: a lone ESC is out by now */
        abort();
    }
    return 0;
}
