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
