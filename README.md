# cg

`cg` is a multi-threaded, POSIX/GNU-style grep implementation written in C, designed around high-throughput fixed-string and multi-pattern searching while retaining a broad grep-compatible command-line interface.

The implementation combines parallel file processing with specialized literal scanners, a lazy DFA for its supported ERE/BRE subset, and a libc-regex fallback for patterns that need it.

## Status

Current source version: **1.3.2**.

This repository documentation describes the current source tree. It intentionally does not claim full GNU grep compatibility: unsupported features are called out explicitly below.

## Build

A direct build is:

```sh
cc -O3 -Wall -Wextra -pthread -o cg cg.c
```

For a production package, build with the distribution/compiler defaults and run the test suite before installation.

On x86-64, the source contains AVX2-accelerated scanners. The implementation also contains non-AVX2 paths, but performance characteristics depend on the target CPU and compiler.

## Basic usage

```sh
# Search a file
./cg 'needle' file.txt

# Fixed-string search
./cg -F 'needle' file.txt

# Multiple fixed strings from a file
./cg -F -f patterns.txt file.txt

# Extended regular expression
./cg -E 'foo|bar' file.txt

# Count selected lines
./cg -c -F 'needle' file.txt

# Show line numbers
./cg -n -F 'needle' file.txt

# Use multiple workers
./cg -j 8 -F -f patterns.txt large.txt

# Recursive search
./cg -r -F 'needle' src/

# Only filenames containing a match
./cg -l -F 'needle' src/

# Only filenames without a match
./cg -L -F 'needle' src/
```

When no input file is supplied, `cg` reads standard input. `-` can also be used explicitly for standard input.

## Pattern modes

### Fixed strings

Use `-F` when patterns are literal strings. This is the primary high-throughput path.

Patterns supplied with `-f` are interpreted one per line.

```sh
cg -F -f words.txt large.txt
```

### Basic and extended regular expressions

`-G` selects basic regular expressions and is the default. `-E` selects extended regular expressions.

The implementation has its own parser/NFA/lazy-DFA path for a supported subset. Patterns that require features outside that engine can fall back to the system regex implementation.

### Case-insensitive matching

`-i` / `--ignore-case` enables case-insensitive matching.

### Whole words and whole lines

```sh
cg -w 'word' file.txt
cg -x 'complete line' file.txt
```

`-x` takes precedence over `-w`.

## Output and control

Common output options include:

| Option | Meaning |
|---|---|
| `-c` | Print selected-line counts |
| `-n` | Print line numbers |
| `-b` | Print byte offsets |
| `-H` | Always print filenames |
| `-h` | Suppress filenames |
| `-l` | Print files with selected lines |
| `-L` | Print files without selected lines |
| `-o` | Print only matching portions |
| `-q` | Suppress normal output |
| `-s` | Suppress error messages |
| `-v` | Select non-matching lines |
| `-m N` | Stop after N selected lines per file |
| `-A N` | Print N lines of trailing context |
| `-B N` | Print N lines of leading context |
| `-C N` | Print N lines of context |
| `--color[=WHEN]` | Highlight matches |
| `-Z` | NUL-terminate filenames |

Context can also be requested with `-NUM`.

## Binary files

The implementation supports:

```sh
-a
-I
--binary-files=binary
--binary-files=text
--binary-files=without-match
```

Binary detection is based on the implementation's binary-file handling path; it is not a claim of byte-for-byte behavioral equivalence with every GNU grep release.

## Recursive search

```sh
cg -r PATTERN directory/
cg -R PATTERN directory/
```

`-r` recursively searches directories without following symlinks. `-R` enables dereference-recursive behavior.

File and directory filtering is available with:

```sh
--include=GLOB
--exclude=GLOB
--exclude-dir=GLOB
```

## Parallelism

`cg` processes large files in chunks and uses worker threads. The default worker count is the number of detected physical CPU cores on Linux, with an online-CPU fallback.

Override it with:

```sh
cg -j 4 ...
```

The accepted range is 1–256 workers.

The environment variable `CG_CHUNK_KIB` can tune the large-file work-unit size. The accepted range is 64 KiB through 1 GiB:

```sh
CG_CHUNK_KIB=8192 cg -j 8 -F -f patterns.txt large.txt
```

This is a tuning knob, not normally something applications need to set.

## How the implementation is organized

At startup, `cg` selects an appropriate matching engine:

1. **FAST** — single fixed string / pure literal paths.
2. **DFA** — the supported internal ERE/BRE implementation.
3. **REGEX** — libc `regcomp`/`regexec` fallback.

Large files are memory-mapped and divided into approximately 4 MiB chunks by default. Small files (up to 1 MiB) are read completely by one worker.

Workers process chunks independently. A printer stage preserves file/argument order so parallel searching does not reorder normal output.

The literal fast paths include AVX2 first/last-byte filtering, Teddy-style fingerprint filtering, widened prefix hashing, rare-anchor filtering for long literals, byte-membership scanning, and Aho–Corasick fallback where required.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Exit status

`cg` follows the documented grep-style status model:

- `0` — at least one selected line was found.
- `1` — no selected line was found.
- `2` — an error occurred.

With `-q`, a matching result returns `0` and normal output is suppressed.

## Unsupported features

The current command-line interface explicitly rejects:

- `-P` / Perl-compatible regular expressions.
- `-z` / NUL-separated input records.

`-D` is accepted for compatibility with `read` and `skip` device modes.

This is a compatibility-oriented grep implementation, not a promise of complete GNU grep semantic equivalence. See [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md).

## Performance

The primary optimization target is large fixed-string and multi-pattern workloads.

One benchmark snapshot on a 9.3 GB English-text corpus used 189,916 fixed-string patterns. Both `cg` and ripgrep produced the same count, `336142718`, in the compared run. In that run:

```text
cg -F -j1 -c -f unique-189916.txt en.txt
real=7.14
user=6.91
sys=0.21

rg --no-config -F -f unique-189916.txt -c en.txt
real=27.39
user=27.15
sys=0.15
```

That is approximately 3.8× lower wall-clock time for `cg` in that specific run.

These numbers are workload-, hardware-, compiler-, cache-, and version-dependent. They are not a claim that `cg` is universally faster than ripgrep. The 1.3.2 timings, measured on the same corpus, are in [docs/BENCHMARKS.md](docs/BENCHMARKS.md).

## Development

Keep performance-critical byte scanners stable unless a benchmark demonstrates a measurable improvement. Behavioral changes should be made in option parsing, pattern setup, matching semantics, or output handling.

Recommended warning-enabled build:

```sh
cc -O3 -Wall -Wextra -pthread -o cg cg.c
```

Before a release, compare representative results against a trusted grep implementation and test exit statuses as well as output.

See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

[MIT](LICENSE). Copyright (c) 2026 huntsman11177.

## Repository layout

- `cg.c` — implementation.
- `Makefile` — build, test, installation and uninstall targets.
- `man/cg.1` — manual page (`man cg` after installation).
- `tests/` — smoke checks.
- `docs/` — architecture, compatibility, benchmarks, release notes, and install guides.
- `packaging/arch/` — Arch/AUR `PKGBUILD` for pacman.
- `packaging/rpm/` — Fedora/RHEL spec for dnf.
- `.github/workflows/` — continuous integration.

## Installation

Source, pacman/AUR, and dnf instructions are in [docs/INSTALL.md](docs/INSTALL.md).

```sh
make
make check
sudo make install
man cg
```

The default prefix is `/usr/local`. Packagers set `PREFIX=/usr` and stage with `DESTDIR`.
