#ifndef FT_KEYS_H
#define FT_KEYS_H

#include <stdbool.h>
#include <stdint.h>

/* The keys a program receives, as code points: a rune is itself, and the
   keys that are not runes are synthetic codes in the private use area, with
   the modifiers or-ed in. It is the contract between a Filo program (its
   KEY and KEY_* globals) and whatever host decodes the terminal, a shell
   or a program of its own, so it lives apart from both. */
enum {
    FT_KEY_UP = 0xE000,
    FT_KEY_DOWN = 0xE001,
    FT_KEY_RIGHT = 0xE002,
    FT_KEY_LEFT = 0xE003,
    FT_KEY_PGUP = 0xE004,
    FT_KEY_PGDN = 0xE005,
    FT_KEY_HOME = 0xE006,
    FT_KEY_END = 0xE007,
    FT_KEY_DEL = 0xE008,
    FT_KEY_INS = 0xE009,
    FT_KEY_ESC = 0xE00F,
    /* F1 to F12 are FT_KEY_F1 + 0 to 11: codes with bits 4 to 6 clear, as
       every key's, since those carry the modifiers */
    FT_KEY_F1 = 0xE080,
    /* modifiers, or-ed into the code the way xterm reports them (CSI 1;2 A
       is Shift+Up): apps that only know the bare keys ignore the rest */
    FT_KEY_SHIFT = 0x10,
    FT_KEY_ALT = 0x20,
    FT_KEY_CTRL = 0x40,
};

#define FT_KEY_BASE(cp) ((cp) & ~0x70U)
#define FT_KEY_MODS(cp) ((cp) & 0x70U)

/* Whether cp is one of the codes above, not a rune: they all fall in the
   first 256 of the private use area, 0xE000 to 0xE0FF, and every other code
   point is a rune — the emoji past 0xFFFF too, which a test for "at or past
   FT_KEY_UP" once took for keys and dropped. */
static inline bool ft_key_is_code(uint32_t cp) {
    if (cp < 0xE000U) {
        return false;
    }
    return cp <= 0xE0FFU;
}

#endif
