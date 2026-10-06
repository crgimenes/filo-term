#ifndef FILO_TERM_TTY_H
#define FILO_TERM_TTY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app.h"

/* An app on a POSIX terminal: raw keys in, the core's bytes out, and the
   terminal put back as it was however it ends. A program's main is
   tty_open, app_start, tty_run, tty_close, and whatever it does with what
   was left (edt writes a rescue file). */

/* Raw mode and the terminal's size. False with why when stdin and stdout
   are not a terminal or its settings cannot be read. */
bool tty_open(uint16_t *cols, uint16_t *rows, char *why, size_t cap);

/* Hands the app the terminal until the program is done (true) or the
   terminal goes away: hangup, terminate, end of input (false). */
bool tty_run(app *a);

/* The terminal as tty_open found it. Safe to call twice, and called at
   exit. */
void tty_close(void);

/* The terminal lent to another program (an editor) and taken back: cooked
   mode and the main screen until tty_resume, which puts the app back on
   the alternate screen at the terminal's size now and repaints it. */
void tty_suspend(void);
void tty_resume(app *a);

/* A seed that differs between runs, for app.seed. */
uint64_t tty_seed(void);

/* The whole main of a program that is its bundle and nothing else: usage
   on -h (stdout, 0) or on a wrong call (stderr, 2), the one optional
   argument as ARG, a seed, the terminal, the run. 0 when the program
   ended itself, 1 when it could not start or the terminal went away. */
int tty_main(int argc, char **argv, app *a, const app_spec *spec, const uint8_t *fbb,
             size_t fbb_len, const char *usage);

#endif
