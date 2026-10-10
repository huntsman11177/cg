# cg 1.3.0

Fast multi-threaded POSIX/GNU-style grep, licensed under the MIT License.

## Changes

- Streaming DFA now keeps matching lines, so normal output uses the interleaved scan.
- Plain output is written straight into the output buffer when context, `-o`, color, and `-m` are off.
- AVX2 newline counting when `-n` needs line numbers.
- `-w` for a supported pattern is built into the DFA. Patterns that can match an empty string stay on the previous path.
- `-E -w` with a range such as `[a-z]` follows GNU grep in a UTF-8 locale. ripgrep keeps `[a-z]` as ASCII, so a few lines can differ.

`cg` is a basic regular expression by default. A pattern like `[a-z]+ing` needs `-E` before `+` is a quantifier. Without `-E`, that search matches nothing, same as GNU grep.

The 189,916-pattern fixed-string count on the 9.3 GB corpus is unchanged from 1.2.2. Output-heavy regex timings are in `docs/BENCHMARKS.md`.

Source: https://github.com/huntsman11177/cg
Tag: `v1.3.0`
