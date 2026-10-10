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
