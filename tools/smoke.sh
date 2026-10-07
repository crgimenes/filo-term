#!/bin/sh
# The binary as shipped, on a terminal: what make test cannot see, since it
# drives the bundles and never a program's main (a renamed program once
# looked for a bundle member that was not there). BIN runs on a pty, by
# script(1); a second later each KEY (a printf format) is typed, a third of
# a second apart, and BIN must have drawn something and be gone within five
# seconds, with status 0.
set -eu
usage='usage: smoke.sh BIN KEY...'
bin=${1:?$usage}
shift
out=$(mktemp)
trap 'rm -f "$out"' EXIT
keys() {
    sleep 1
    for k in "$@"; do
        # shellcheck disable=SC2059 # the key is the format
        printf "$k"
        sleep 0.3
    done
}
status=0
if script --version >/dev/null 2>&1; then # util-linux
    keys "$@" | perl -e 'alarm 5; exec @ARGV' script -qefc "$bin" /dev/null >"$out" 2>&1 || status=$?
else
    keys "$@" | perl -e 'alarm 5; exec @ARGV' script -q /dev/null "$bin" >"$out" 2>&1 || status=$?
fi
if [ "$status" != 0 ]; then
    echo "smoke: $bin: status $status (142: still running after 5s)" >&2
    tr -d '\033' <"$out" | tail -5 >&2
    exit 1
fi
if ! grep -q "$(printf '\033')" "$out"; then
    echo "smoke: $bin: drew nothing" >&2
    exit 1
fi
echo "smoke: $bin: ok"
