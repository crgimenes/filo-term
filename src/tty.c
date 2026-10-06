#include "tty.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

enum {
    POLL_MS = 50,  /* a lone Esc is told apart by time; nothing else waits */
    FRAME_MS = 16, /* a program that animates is ticked about this often */
    IN_CAP = 4096,
};

static struct termios saved;
static bool raw = false;
static volatile sig_atomic_t resized = 0;
static volatile sig_atomic_t ended = 0;

static void put(const uint8_t *data, size_t n) {
    while (n > 0) {
        ssize_t w = write(STDOUT_FILENO, data, n);
        if (w <= 0 && errno != EINTR) {
            return;
        }
        if (w > 0) {
            data += w;
            n -= (size_t)w;
        }
    }
}

static bool to_tty(void *ctx, const uint8_t *data, size_t n) {
    (void)ctx;
    put(data, n);
    return true;
}

static void on_winch(int sig) {
    (void)sig;
    resized = 1;
}

static void on_end(int sig) {
    (void)sig;
    ended = 1;
}

static void size_of(uint16_t *cols, uint16_t *rows) {
    struct winsize ws;
    *cols = 80;
    *rows = 24;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
    }
}

static void copy(char *why, size_t cap, const char *s) {
    size_t n = strlen(s);
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(why, s, n);
    why[n] = '\0';
}

void tty_close(void) {
    if (!raw) {
        return;
    }
    static const char reset[] = "\x1b[0m\x1b[?25h\x1b[?2004l\x1b[?1049l";
    put((const uint8_t *)reset, sizeof(reset) - 1);
    (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
    raw = false;
}

bool tty_open(uint16_t *cols, uint16_t *rows, char *why, size_t cap) {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        copy(why, cap, "needs a terminal");
        return false;
    }
    if (tcgetattr(STDIN_FILENO, &saved) != 0) {
        copy(why, cap, "cannot read the terminal's settings");
        return false;
    }
    size_of(cols, rows);
    struct termios t = saved;
    cfmakeraw(&t);
    (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);
    raw = true;
    (void)atexit(tty_close);
    (void)signal(SIGWINCH, on_winch);
    (void)signal(SIGTERM, on_end);
    (void)signal(SIGHUP, on_end);
    return true;
}

void tty_suspend(void) {
    tty_close();
}

void tty_resume(app *a) {
    struct termios t = saved;
    cfmakeraw(&t);
    (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);
    raw = true;
    static const char back[] = "\x1b[?1049h\x1b[?2004h\x1b[H\x1b[2J";
    put((const uint8_t *)back, sizeof(back) - 1);
    uint16_t cols = 0;
    uint16_t rows = 0;
    size_of(&cols, &rows);
    app_resize(a, cols, rows);
}

static uint32_t ms_since(struct timespec *last) {
    struct timespec now;
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    long ms = ((now.tv_sec - last->tv_sec) * 1000L) + ((now.tv_nsec - last->tv_nsec) / 1000000L);
    if (ms <= 0) {
        return 0; /* less than a millisecond: the time stays banked in last */
    }
    *last = now;
    return (uint32_t)ms;
}

bool tty_run(app *a) {
    uint8_t pending[TERM_OUT_CAP];
    size_t n = term_out_read(&a->t, pending, sizeof(pending)); /* the first paint */
    put(pending, n);
    a->t.sink = to_tty;
    int wait = app_animates(a) ? FRAME_MS : POLL_MS;
    struct timespec last;
    (void)clock_gettime(CLOCK_MONOTONIC, &last);
    while (!app_done(a) && !ended) {
        if (resized) {
            resized = 0;
            uint16_t cols = 0;
            uint16_t rows = 0;
            size_of(&cols, &rows);
            app_resize(a, cols, rows);
        }
        struct pollfd p = {STDIN_FILENO, POLLIN, 0};
        int r = poll(&p, 1, wait);
        if (r > 0 && ((unsigned)p.revents & (unsigned)(POLLIN | POLLHUP)) != 0) {
            uint8_t in[IN_CAP];
            ssize_t got = read(STDIN_FILENO, in, sizeof(in));
            if (got == 0 || (got < 0 && errno != EINTR && errno != EAGAIN)) {
                return false;
            }
            if (got > 0) {
                app_input(a, in, (size_t)got);
            }
        }
        app_tick(a, ms_since(&last));
    }
    return app_done(a);
}

uint64_t tty_seed(void) {
    struct timespec now;
    (void)clock_gettime(CLOCK_REALTIME, &now);
    return ((uint64_t)now.tv_sec << 32U) ^ (uint64_t)now.tv_nsec ^ ((uint64_t)getpid() << 16U);
}

static void said(const char *name, const char *why) {
    fputs(name, stderr);
    fputs(": ", stderr);
    fputs(why, stderr);
    fputs("\n", stderr);
}

int tty_main(int argc, char **argv, app *a, const app_spec *spec, const uint8_t *fbb,
             size_t fbb_len, const char *usage) {
    if (argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        fputs(usage, stdout);
        return 0;
    }
    if (argc > 2) {
        fputs(usage, stderr);
        return 2;
    }
    a->arg = argc == 2 ? argv[1] : NULL;
    a->seed = tty_seed();
    char why[512];
    uint16_t cols = 0;
    uint16_t rows = 0;
    if (!tty_open(&cols, &rows, why, sizeof(why))) {
        said(spec->name, why);
        return 1;
    }
    if (!app_start(a, spec, fbb, fbb_len, cols, rows, why, sizeof(why))) {
        tty_close();
        fputs(why, stderr);
        fputs("\n", stderr);
        return 1;
    }
    bool finished = tty_run(a);
    tty_close();
    return finished ? 0 : 1;
}
