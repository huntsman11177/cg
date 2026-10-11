# Benchmarks

## Principles

`cg` is optimized primarily for large fixed-string and multi-pattern workloads. Performance should therefore be reported with the exact workload and command line.

Do not publish a single speedup number as if it applied to all grep workloads.

At minimum, record:

- CPU model and core/thread topology.
- OS and kernel.
- Compiler and version.
- Compiler flags.
- `cg` version/commit.
- Competitor version.
- Corpus size and source.
- Pattern count and pattern-generation method.
- Corpus checksum.
- Pattern-file checksum.
- Filesystem/storage.
- Cold-cache or warm-cache state.
- Thread count.
- Number of repetitions.
- Result counts.

## Current benchmark snapshot

A 9.3 GB English-text corpus was searched with 189,916 fixed-string patterns.

Both tools returned the same selected-line count:

```text
336142718
```

One direct comparison was:

```text
./cg -F -j1 -c -f unique-189916.txt en.txt
336142718
real=7.14
user=6.91
sys=0.21
```

versus:

```text
rg --no-config -F -f unique-189916.txt -c en.txt
336142718
real=27.39
user=27.15
sys=0.15
```

The measured wall-clock ratio for that run is approximately:

```text
27.39 / 7.14 = 3.84x
```

This is a workload-specific result. Cache state, machine, compiler, tool version, pattern distribution, and thread configuration can materially change the result.

## Thread scaling

A warm-cache scaling snapshot for the same large-pattern workload showed approximately:

| Workers | `cg` wall time |
|---:|---:|
| 1 | 3.43 s |
| 2 | 1.80 s |
| 4 | 1.10 s |
| 8 | 0.73 s |
| 16 | 0.57 s |

These measurements are useful for understanding scaling, but should be regenerated for every published release.

## Recommended benchmark matrix

### Multi-pattern fixed strings

```sh
cg -F -j1 -c -f patterns.txt corpus.txt
cg -F -jN -c -f patterns.txt corpus.txt

rg --no-config -F -f patterns.txt -c corpus.txt
grep -F -f patterns.txt -c corpus.txt
ugrep -F -f patterns.txt -c corpus.txt
```

Verify that every implementation produces the same count before comparing timing.

### Single literal

```sh
cg -F -c 'needle' corpus.txt
rg --no-config -F -c 'needle' corpus.txt
grep -F -c 'needle' corpus.txt
```

### Regex

Use a separate matrix for `-G` and `-E`. Regex engines have different feature sets and optimization strategies, so regex results should not be mixed with fixed-string results.

### Output-heavy workloads

Benchmarking `-c` isolates matching and counting much better than printing millions of lines. Also test normal output separately because formatting and terminal/pipe throughput can dominate.

## Cache discipline

A reproducible benchmark should explicitly state whether the corpus is warm in page cache.

For warm-cache measurements:

1. Run a warm-up pass.
2. Run the exact command multiple times.
3. Report median and range.

For cold-cache measurements, use a controlled environment and document the cache-clearing procedure. Do not mix cold and warm results in one table.

## Reporting

Prefer tables such as:

| Tool | Version | Threads | Time | User | Sys | Count |
|---|---|---:|---:|---:|---:|---:|
| cg | X.Y.Z | 1 | ... | ... | ... | ... |
| ripgrep | X.Y.Z | — | ... | ... | ... | ... |
| GNU grep | X.Y | — | ... | ... | ... | ... |

Never omit the result count. Equal output is part of the benchmark.

## 1.3.0 snapshot (2026-10-10)

Machine: AMD Ryzen 7 7730U, 16 threads, 22 GiB RAM. Corpus: `en.txt`, 9.3 GB. Output discarded with `>/dev/null`, so these times include formatting the matches. ripgrep 15.2.0 uses its default thread count. `cg` 1.3.0 was timed with `-j1` and `-j8`.

`cg` defaults to basic regular expressions, like GNU grep. `+` is literal unless `-E` is set. ripgrep treats `+` as a quantifier. The two `-w` commands below exited 1 from `cg` because the pattern was a basic expression; that is the same result as `grep -w`, and it is not a faster search. With `-E`, both patterns match. On a 2 MiB prefix, `-E -w -c '[a-z]+ing'` returned 8355 from both `cg` 1.3.0 and ripgrep. `-E -w -c 'th[a-z]+'` returned 17239 from `cg` and 17238 from ripgrep; the extra `cg` line is `Le théâtre brule!`, which GNU grep also selects in this UTF-8 locale.

| Command | cg 1.3.0 -j1 | cg 1.3.0 -j8 | ripgrep default |
|---|---:|---:|---:|
| `-w '[a-z]+ing'` | 0.94 s, exit 1 | 0.51 s, exit 1 | 7.67 s |
| `-E -w '[a-z]+ing'` | 8.22 s |  | 7.67 s |
| `-w 'th[a-z]+'` | 17.02 s, exit 1 | 3.28 s, exit 1 | 12.55 s |
| `-E -w 'th[a-z]+'` | 14.14 s |  | 12.55 s |
| `-n 'e.e.e'` | 11.63 s | 2.67 s | 11.71 s |
| `-n zzyzx` | 1.21 s | 0.85 s | 0.91 s |
| `-o 'e.e.e'` | 11.73 s | 2.64 s | 11.75 s |
| `-v -n 'e.e.e'` | 14.92 s | 8.52 s | 41.42 s |
| `-n 'a.*z'` | 12.01 s | 2.66 s | 12.30 s |

The `-j8` column is the one that shows the 1.3.0 output path pulling ahead of ripgrep on `e.e.e`, `-o`, and `a.*z`. The invert run is faster even at one thread (14.92 s versus 41.42 s).

The fixed-string count used for 1.2.2 was remeasured warm, same corpus, 189,916 patterns, `-F -j1 -c`. Both 1.2.2 and 1.3.0 returned `336142718` in 3.83–3.84 s. That path did not change. The 7.14 s figure above is an earlier run, not this remeasure.

## 1.3.1 snapshot (2026-10-10)

Same machine and warm `en.txt` as the 1.3.0 snapshot. One run each, stdout discarded with `>/dev/null`. `cg` 1.3.1 was built with `-O3` and timed with `-j1` and `-j8`. ripgrep 15.2.0 was `rg --no-config` at its default thread count. The two ripgrep `-w` commands were run twice back to back; the table shows that range. Every other cell is a single run.

`cg` without `-E` still treats `+` as a literal, so `-w '[a-z]+ing'` and `-w 'th[a-z]+'` exit 1, same as GNU grep. ripgrep compiles those patterns as regex. `-E -w` is the comparable search. Those `-w` patterns are end-anchored, so a newline still leaves the four-way loop; the `-j1` times match the 1.3.0 snapshot. The unanchored rows are the ones 1.3.1 speeds up.

| Command | cg 1.3.1 -j1 | cg 1.3.1 -j8 | ripgrep default |
|---|---:|---:|---:|
| `-w '[a-z]+ing'` | 0.95 s, exit 1 | 0.53 s, exit 1 | 7.91–8.00 s |
| `-E -w '[a-z]+ing'` | 8.18 s | 2.09 s | 7.91–8.00 s |
| `-w 'th[a-z]+'` | 17.15 s, exit 1 | 3.51 s, exit 1 | 12.94–13.43 s |
| `-E -w 'th[a-z]+'` | 14.27 s | 3.41 s | 12.94–13.43 s |
| `-n 'e.e.e'` | 5.94 s | 1.43 s | 11.95 s |
| `-n zzyzx` | 1.24 s | 0.85 s | 0.92 s |
| `-o 'e.e.e'` | 5.71 s | 1.59 s | 11.92 s |
| `-v -n 'e.e.e'` | 9.68 s | 2.90 s | 42.31 s |
| `-n 'a.*z'` | 6.07 s | 1.47 s | 12.31 s |

Against the 1.3.0 snapshot above, `-n 'e.e.e'` went from 11.63 s / 2.67 s to 5.94 s / 1.43 s, and `-v -n 'e.e.e'` went from 14.92 s / 8.52 s to 9.68 s / 2.90 s. `-o 'e.e.e'` and `-n 'a.*z'` dropped by about the same factor at one thread. `-n zzyzx` did not move. Those two snapshots are separate warm runs, not a paired retime of the 1.3.0 binary.

The 189,916-pattern fixed-string count, `-F -j1 -c`, returned `336142718` in 3.85 s. `-F -j8 -c the` returned `63651604` in 0.75 s. The literal scanners were not changed.

## 1.3.2 snapshot (2026-10-11)

Same machine, powersave governor, warm `en.txt` (9968530111 bytes). `cg` 1.3.2 and a 1.3.1 binary built with the same `-O3 -Wall -Wextra -pthread` flags were timed back to back with `/usr/bin/time`. The table is user time, then wall time. Counts matched between the two binaries at `-j1` and `-j8`. ripgrep 15.2.0 is one run, default threads, from the same warm cache as the first block of rows.

| Command | 1.3.2 -j1 | 1.3.1 -j1 | 1.3.2 -j8 wall | 1.3.1 -j8 wall | Lines |
|---|---:|---:|---:|---:|---:|
| `th[a-z]+` | 0.46 s (0.56 s) | 15.04 s (15.18 s) | 0.48 s | 3.06 s | 1 |
| `-E -w '[a-z]+ing'` | 8.03 s (8.16 s) | 7.96 s (8.10 s) | 1.76 s | 1.75 s | 43627832 |
| `-E 'a.*z'` | 5.41 s (5.52 s) | 5.47 s (5.58 s) | 1.27 s | 1.26 s | 3406053 |
| `-F -i -f` 100 common words | 9.79 s (9.91 s) | 10.01 s (10.14 s) | 2.36 s | 2.39 s | 176537739 |
| `-F -i -w -f` 100 common words | 127.88 s (128.52 s) | 125.44 s (126.10 s) | 30.95 s | 30.35 s | 159568380 |
| `-E` 100× `\bword\b` | 208.57 s (209.55 s) | 214.82 s (215.73 s) | 47.30 s | 50.63 s | 135492849 |
| `-E -f HARD_REAL_10000.regex` | 11.66 s (11.81 s) | 12.16 s (12.31 s) | 2.78 s | 2.77 s | 11744376 |

`th[a-z]+` is a basic expression, so `+` is literal. That is the same search GNU grep runs, and it is not the ripgrep search. ripgrep on the comparable rows in this session: `-E -w '[a-z]+ing'` 6.60 s wall (43626703 lines; ripgrep's `[a-z]` is ASCII), `-E 'a.*z'` 11.93 s wall (same 3406053 lines), `-F -i -f` 100 words 16.10 s wall (same count). No other row in the paired run was more than 3% slower in user time than 1.3.1. The literal scanners were not retuned.
