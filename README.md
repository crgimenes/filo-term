# filo-term

The terminal a Filo program runs on, apart from any program: the pieces a
host links to give a [Filo](https://github.com/crgimenes/clang_filo) program
a screen, keys and a text to work on, and the app core and POSIX loop that
make a Filo program a program of its own (`app`, `tty`). The
[rocchetto](https://github.com/crgimenes/rocchetto) shell, the
[edt](https://github.com/crgimenes/edt) editor,
[corewar](https://github.com/crgimenes/corewar) and
[filo-games](https://github.com/crgimenes/filo-games) build these sources as
their own.

| file | what it is |
| --- | --- |
| `term` | the output ring the host drains, the terminal's size, the stack of full-screen apps, OSC 52 clipboard |
| `canvas` | cells with colour and attributes, flushed to the terminal as the diff from what it shows |
| `utf8` | an incremental decoder and a small wcwidth |
| `tbuf` | a text buffer of bytes with a line table, selection, find, and byte access for a hex view |
| `keyin`, `keys.h` | bytes in, keys out: UTF-8, CSI/SS3 with modifiers, F1–F12, bracketed paste, a lone Esc told apart by time |
| `paint` | Filo builtins that draw on a canvas (`print-at`, `fill`, `fg`, `bg`, `attr`, ...) |
| `field` | a one-line input field |
| `tbx` | Filo builtins over the text buffer (`tb-*`, `tb-spans` for its lines highlighted, and `hex`, `bin`, `cp437` for a hex view) |
| `pager` | a less-like pager over rendered text |
| `md`, `hl` | markdown to ANSI, streaming; syntax highlighting for fenced code and the editor's text (C, Go, JS, Lua, shell, SQL, JSON, asm, Filo, Redcode) |
| `app` | a Filo program that owns a terminal: the entries (`init`, `draw`, `key`, `input`, `tick`), the builtins every host gives one, the error line |
| `tty` | the app on a POSIX terminal: raw mode, resize, signals, the loop, `tty_main` for a program that is its bundle |

Each registration function takes a getter (`paint_register(ctx, canvas_fn)`,
`tbx_register(ctx, buf_fn, saved_fn)`), so the library never knows the host
it runs in. Sizes are in `src/term_config.h`, overridden with `-D`.

## Build

```
make           # every source compiled as C11 (hosts use C11 or C23)
make test      # the runtime driven without any shell, under ASan/UBSan
make qa        # the above, clang-format, clang-tidy, cppcheck
```

`paint` and `tbx` need `filo.h`: `FILO ?= ../clang_filo`.

## License

MIT
