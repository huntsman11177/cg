# Changelog

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
