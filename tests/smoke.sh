#!/bin/sh
set -eu
CG=${1:-./cg}
TMP=${TMPDIR:-/tmp}/cg-smoke-$$
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
mkdir -p "$TMP/dir/sub"
printf '%s\n' 'alpha' 'beta' 'ALPHA' 'gamma delta' 'alphabet' > "$TMP/input.txt"
printf '%s\n' 'alpha' 'gamma' > "$TMP/patterns.txt"
printf '%s\n' 'sub-alpha' > "$TMP/dir/sub/nested.txt"

[ "$($CG --version | head -n1)" = 'cg 1.3.2' ]

printf '%s\n' 'alpha' 'alphabet' > "$TMP/expected"
$CG -F alpha "$TMP/input.txt" | diff -u "$TMP/expected" -
printf '%s\n' alpha ALPHA alphabet > "$TMP/expected-i"
$CG -Fi alpha "$TMP/input.txt" | diff -u "$TMP/expected-i" -
$CG -F -f "$TMP/patterns.txt" "$TMP/input.txt" >/dev/null
[ "$($CG -Fc alpha "$TMP/input.txt")" = 2 ]
[ "$($CG -Fl alpha "$TMP/input.txt")" = "$TMP/input.txt" ]
$CG -Er 'alpha|gamma' "$TMP/input.txt" >/dev/null
$CG -Fr alpha "$TMP/dir" >/dev/null

if $CG -P alpha "$TMP/input.txt" >/dev/null 2>&1; then
    echo 'ERROR: -P unexpectedly succeeded' >&2
    exit 1
fi
if $CG -z alpha "$TMP/input.txt" >/dev/null 2>&1; then
    echo 'ERROR: -z unexpectedly succeeded' >&2
    exit 1
fi

echo 'cg smoke tests: PASS'
