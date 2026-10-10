# cg 1.2.2

Fast multi-threaded POSIX/GNU-style grep, licensed under the MIT License.

## This release

- Multi-threaded file processing with ordered output.
- Fixed-string and multi-pattern fast paths.
- Internal Thompson-NFA/lazy-DFA for the supported BRE/ERE subset.
- libc regex fallback for patterns outside that subset.
- AVX2 literal prefilters on x86-64, with portable fallbacks.
- Recursive traversal and GNU-style output options, including `--color`.
- `-P` (Perl regex) and `-z` (NUL-separated records) are rejected.

Install paths for source, Arch/pacman, and Fedora/dnf are in `docs/INSTALL.md`.

Source: https://github.com/huntsman11177/cg
Tag: `v1.2.2`
