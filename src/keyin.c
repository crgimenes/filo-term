#include "keyin.h"

#include "keys.h"

void keyin_init(keyin *k) {
    utf8_dec_init(&k->dec);
    k->esc = 0;
    k->esc_ms = 0;
    k->csi_len = 0;
    k->ss3 = false;
}

/* Maps a finished CSI/SS3 sequence to a synthetic key; 0 = not a key.
   Parameters are "n" or "n;mod": the first names the key for '~', the
   second is 1 + the modifier bits (shift 1, alt 2, ctrl 4). */
static uint32_t csi_key(const keyin *k, uint8_t final) {
    uint32_t p[2] = {0, 0};
    size_t np = 0;
    size_t i = 0;
    while (i < k->csi_len && np < 2) {
        uint8_t c = k->csi[i];
        i++;
        if (c == ';') {
            np++;
            continue;
        }
        if (c < '0' || c > '9') {
            return 0;
        }
        p[np] = (p[np] * 10) + (uint32_t)(c - '0');
    }
    if (k->csi_len > 0) {
        np++;
    }
    uint32_t mods = 0;
    if (np == 2 && p[1] >= 2 && p[1] <= 8) {
        mods = (p[1] - 1U) << 4U;
    }
    uint32_t key = 0;
    switch (final) {
    case 'A':
        key = FT_KEY_UP;
        break;
    case 'B':
        key = FT_KEY_DOWN;
        break;
    case 'C':
        key = FT_KEY_RIGHT;
        break;
    case 'D':
        key = FT_KEY_LEFT;
        break;
    case 'H':
        key = FT_KEY_HOME;
        break;
    case 'P':
    case 'Q':
    case 'R':
    case 'S':
        /* F1 to F4: SS3 P to S, or CSI 1;mod P to S with a modifier */
        key = FT_KEY_F1 + (uint32_t)(final - 'P');
        break;
    case 'F':
        key = FT_KEY_END;
        break;
    case '~':
        if (p[0] == 5) {
            key = FT_KEY_PGUP;
        } else if (p[0] == 6) {
            key = FT_KEY_PGDN;
        } else if (p[0] == 1 || p[0] == 7) {
            key = FT_KEY_HOME;
        } else if (p[0] == 4 || p[0] == 8) {
            key = FT_KEY_END;
        } else if (p[0] == 3) {
            key = FT_KEY_DEL;
        } else if (p[0] == 2) {
            key = FT_KEY_INS;
        } else if (p[0] >= 11 && p[0] <= 24) {
            /* F1 to F12 as CSI n~: 11 to 15, then 17 to 21 and 23, 24 (the
               numbers skip where VT220 keys sat) */
            static const uint8_t fkey[14] = {1, 2, 3, 4, 5, 0, 6, 7, 8, 9, 10, 0, 11, 12};
            if (fkey[p[0] - 11] != 0) {
                key = FT_KEY_F1 + fkey[p[0] - 11] - 1U;
            }
        }
        break;
    default:
        break;
    }
    if (key == 0) {
        return 0;
    }
    return key | mods;
}

/* CSI 200~ and 201~ bracket a paste; true when the sequence was one. */
static bool paste_mark(const keyin *k, keyin_emit emit, void *user) {
    if (k->csi_len != 3 || k->csi[0] != '2' || k->csi[1] != '0') {
        return false;
    }
    if (k->csi[2] == '0') {
        emit(user, KEYIN_PASTE_BEGIN, 0);
        return true;
    }
    if (k->csi[2] == '1') {
        emit(user, KEYIN_PASTE_END, 0);
        return true;
    }
    return false;
}

void keyin_feed(keyin *k, uint8_t b, keyin_emit emit, void *user) {
    if (k->esc == 1) {
        if (b == '[' || b == 'O') {
            k->esc = 2;
            k->csi_len = 0;
            k->ss3 = b == 'O';
            return;
        }
        if (b == ']' || b == 'P' || b == 'X' || b == '^' || b == '_') {
            /* OSC/DCS/SOS/PM/APC on input can only be the terminal answering
               a query embedded in relayed output (an editor on a live stream
               asking for colours): nobody here asked, so it is swallowed */
            k->esc = 3;
            k->esc_ms = 0;
            return;
        }
        k->esc = 0;
        emit(user, KEYIN_ESC, 0);
        /* this byte goes on as ordinary input below */
    } else if (k->esc == 3) {
        if (b == 0x07) {
            k->esc = 0;
            return;
        }
        if (b == 0x1B) {
            k->esc = 4;
            return;
        }
        k->esc_ms = 0;
        return;
    } else if (k->esc == 4) {
        if (b == '\\') {
            k->esc = 0;
            return;
        }
        k->esc = 3; /* a stray ESC inside the string: keep swallowing */
        k->esc_ms = 0;
        return;
    } else if (k->esc == 2) {
        if (k->ss3 || (b >= 0x40 && b <= 0x7E)) {
            k->esc = 0;
            if (b == '~' && paste_mark(k, emit, user)) {
                return;
            }
            uint32_t key = csi_key(k, b);
            if (key != 0) {
                emit(user, KEYIN_KEY, key); /* unknown sequences are swallowed */
            }
            return;
        }
        if (k->csi_len < KEYIN_CSI_MAX) {
            k->csi[k->csi_len] = b;
            k->csi_len++;
        }
        return;
    }
    if (b == 0x1B) {
        k->esc = 1;
        k->esc_ms = 0;
        return;
    }
    uint32_t cp = 0;
    int resync = 0;
    utf8_result r = utf8_dec_feed(&k->dec, b, &cp, &resync);
    if (r == UTF8_RUNE) {
        emit(user, KEYIN_KEY, cp);
        return;
    }
    if (r == UTF8_ERROR && resync) {
        r = utf8_dec_feed(&k->dec, b, &cp, &resync);
        if (r == UTF8_RUNE) {
            emit(user, KEYIN_KEY, cp);
        }
    }
    /* invalid bytes are dropped */
}

void keyin_tick(keyin *k, uint32_t ms, keyin_emit emit, void *user) {
    if (k->esc == 1) {
        k->esc_ms += ms;
        if (k->esc_ms >= KEYIN_ESC_TIMEOUT_MS) {
            k->esc = 0;
            emit(user, KEYIN_ESC, 0);
        }
    }
    if (k->esc >= 3) {
        k->esc_ms += ms;
        if (k->esc_ms >= KEYIN_ESC_TIMEOUT_MS) {
            k->esc = 0;
        }
    }
}
