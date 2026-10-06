# filo-term: the terminal a Filo program runs on, apart from any program —
# the output ring and the compositor (term, canvas), UTF-8, the text buffer
# (tbuf), the key decoder (keyin), and the Filo builtins over them (paint,
# field, tbx), with the pager and the markdown renderer (pager, md, hl),
# and a program's own terminal: the app core (app) and its POSIX loop
# (tty). Programs build these sources as their own; this is their gate.
CC ?= cc
CLANG_FORMAT ?= clang-format
CLANG_TIDY ?= clang-tidy
FILO ?= ../clang_filo

WARN = -Wall -Wextra -Werror -Wshadow -Wconversion -Wdouble-promotion -Wundef
# C11: programs build these as C11 and the BBS as C23, so they are held to
# both. tty is POSIX, and glibc and musl hide some of it (cfmakeraw) under
# a strict -std; the BSDs and macOS ignore the macro.
FLAGS = -std=c11 -D_DEFAULT_SOURCE $(WARN) -Isrc -I$(FILO)
SRC = $(addprefix src/,term.c canvas.c utf8.c tbuf.c keyin.c paint.c field.c tbx.c pager.c md.c hl.c app.c tty.c)
FILOSRC = $(addprefix $(FILO)/,filo.c filo_math.c filo_strings.c filo_nolibc.c)
APPSRC = $(addprefix src/,term.c canvas.c utf8.c keyin.c paint.c field.c app.c)
HDRS = $(wildcard src/*.h) $(FILO)/filo.h
OBJ = $(patsubst src/%.c,build/%.o,$(SRC))
TIDY_CHECKS = bugprone-*,cert-*,clang-analyzer-*,readability-*,-readability-identifier-length,-readability-function-cognitive-complexity,-readability-magic-numbers,-cert-err33-c,-readability-else-after-return,-readability-simplify-boolean-expr,-bugprone-easily-swappable-parameters,-clang-analyzer-optin.performance.Padding

.PHONY: all test fmt fmt-check tidy check qa clean

all: $(OBJ)

build/%.o: src/%.c $(HDRS)
	@mkdir -p build
	$(CC) -O2 $(FLAGS) -c -o $@ $<

# The runtime driven without any shell: a framed box, a status bar, the
# text buffer, under the sanitizers.
test: test/test_runtime.c test/test_app.c $(SRC) $(HDRS)
	@mkdir -p build
	$(CC) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all $(FLAGS) \
		-o build/test_runtime src/term.c src/canvas.c src/utf8.c src/tbuf.c test/test_runtime.c
	./build/test_runtime
	$(CC) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all $(FLAGS) \
		-o build/test_app $(APPSRC) $(FILOSRC) test/test_app.c
	./build/test_app

fmt:
	$(CLANG_FORMAT) -i src/*.c src/*.h test/*.c tools/*.c

fmt-check:
	$(CLANG_FORMAT) --dry-run --Werror src/*.c src/*.h test/*.c tools/*.c

tidy:
	$(CLANG_TIDY) --quiet --warnings-as-errors='*' --checks='$(TIDY_CHECKS)' \
		$(SRC) tools/appvm.c -- -std=c11 -D_DEFAULT_SOURCE -Isrc -I$(FILO)

check:
	cppcheck --enable=warning,style,performance,portability --inline-suppr \
		--suppress=missingIncludeSystem --error-exitcode=1 -Isrc -I$(FILO) $(SRC) tools/appvm.c test/test_runtime.c test/test_app.c

qa: all fmt-check test tidy check

clean:
	rm -rf build
