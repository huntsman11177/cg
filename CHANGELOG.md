# Changelog

## 1.3.0

Source drop previously built as `cg1`.

- The streaming DFA records matching lines, so normal output can use the same interleaved scan as count-only runs.
- Plain output (no context, `-o`, color, or `-m`) is formatted directly into the output buffer.
- AVX2 newline counting on x86-64 when line numbers are required.
- `-w` on a supported expression is compiled into the DFA as `(^|\W) P (\W|$)`, instead of checking every candidate line with libc regex. Empty-matching patterns stay on the previous path.
- In a UTF-8 locale, `-E -w 'th[a-z]+'` follows GNU grep and can match letters outside ASCII `a-z` (for example `théâtre`). ripgrep's `[a-z]` stays ASCII, so counts can differ by those lines.

The large fixed-string count on `en.txt` is unchanged from 1.2.2. See `docs/BENCHMARKS.md`.

## 1.2.2

Current documented release corresponding to the supplied `cg.c` source.

- Multi-threaded grep-style file processing.
- Fixed-string and multi-pattern fast paths.
- Internal Thompson-NFA/lazy-DFA path for supported BRE/ERE patterns.
- libc regex fallback for patterns outside the internal engine.
- AVX2 literal prefilters on x86-64 where available.
- Recursive traversal and GNU-style output/context options documented in `--help`.
- Explicit rejection of unsupported `-P` and `-z` modes.

Performance claims and benchmark methodology are documented in `docs/BENCHMARKS.md`.
