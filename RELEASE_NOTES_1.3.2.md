# cg 1.3.2

Fast multi-threaded POSIX/GNU-style grep, licensed under the MIT License.

## Changes

- The regex prefilter keeps the rarest literal that every match must contain. Basic `th[a-z]+` scans for the literal `+`. An alternation contributes only a shared literal prefix. `a.*z` stays on the streaming DFA.
- A pattern whose every branch is `\bWORD\b` or `\<WORD\>` is rewritten to `-w` of those words.
- ASCII `-F -i` lowercases the literals. Sparse sets use the existing byte scanners on a lowercased window. Dense word lists stay on the case-folded DFA.
- Simple one-to-one Unicode case pairs in Latin, Greek, and Cyrillic are compiled into the DFA. `ß`, `İ`, and `ſ` still use libc.

On a warm 9.97 GB `en.txt`, basic `th[a-z]+` went from 15.04 s to 0.46 s of user time at one thread. Counts for the patterns in `docs/BENCHMARKS.md` match 1.3.1, including `HARD_REAL_10000` at 11744376.

`cg` is a basic regular expression by default. A pattern like `[a-z]+ing` needs `-E` before `+` is a quantifier. Without `-E`, `+` is a literal, same as GNU grep.

Source: https://github.com/huntsman11177/cg
Tag: `v1.3.2`
