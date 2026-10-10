# Architecture

`cg` is structured as a grep-compatible front end around several matching engines and specialized literal scanners.

## Processing pipeline

```text
command line
    |
    v
pattern loading / option parsing
    |
    v
pattern classification
    |
    +--> FAST literal engine
    |
    +--> internal NFA -> lazy DFA
    |
    `--> libc regex fallback
    |
    v
file discovery / recursive traversal
    |
    v
small-file reader OR mmap/chunk scheduler
    |
    v
worker threads
    |
    v
per-file / per-chunk results
    |
    v
ordered printer
    |
    v
stdout / stderr + exit status
```

## File processing

Files of 1 MiB or less are read completely and processed by one worker. This avoids mmap/munmap and cross-thread result handoff for small inputs.

Larger files are memory-mapped and divided into work chunks. The default chunk size is approximately 4 MiB and can be adjusted with `CG_CHUNK_KIB`.

Chunks own lines that start inside their byte range. Workers therefore do not need a global pre-scan merely to establish chunk boundaries.

A printer stage preserves argument/file order while allowing workers to execute out of order.

## Matching engines

### FAST

The FAST path is selected for single fixed strings and for pure literal sets that can be handled by the literal machinery.

The single-literal scanner uses byte-oriented search with optimized `memchr`/`memcmp` behavior and x86-64 SIMD paths where applicable.

### Internal DFA

For supported ERE/BRE expressions, `cg` parses patterns into its own expression representation and constructs a Thompson NFA.

The NFA is converted lazily into DFA states while searching. DFA state caches are thread-local, avoiding a lock on the hot matching path.

Multiple patterns can be merged into one automaton rather than running a separate libc regex for every pattern.

The implementation also recognizes pure literal alternations before building the NFA. This is important for very large expressions such as thousands of literal alternatives.

### libc regex fallback

Patterns requiring functionality outside the internal engine can use the system `regcomp`/`regexec` implementation.

Each worker owns its regex state. The source explicitly avoids sharing a `regex_t` between worker threads because that can serialize execution inside the regex implementation.

## Literal prefilters

The literal matcher uses several levels of filtering.

### Small literal sets

For a small number of literals, the implementation uses specialized AVX2 first/last-byte filtering.

### Teddy-style filtering

For larger literal sets within the Teddy range, literals are mapped into buckets using short fingerprints. AVX2 classifies candidate windows before exact verification.

### Larger literal sets

When the literal set is larger than the Teddy threshold, a widened prefix hash is used instead of allowing Teddy buckets to saturate.

### Long literals

When all literals are sufficiently long, each literal can retain a rare four-byte anchor. An AVX2 reject bloom classifies windows before the anchor chain is followed.

### Short literals

One-byte literals use a byte-membership scan. Other literals shorter than three bytes use a `memchr`-based side path.

Aho–Corasick remains available when the hash-filter construction cannot be used.

## Correctness model

Prefilters are candidate generators. They are not the final authority unless the selected pure-literal path has established an exact literal hit.

For regex branches, candidate locations are verified by the actual matcher.

This separation is important: aggressive SIMD filtering may reject impossible candidates, but it must not turn a false positive into a reported match.

## Parallelism

Workers process independent file/chunk jobs. The output stage serializes presentation rather than search.

This design provides parallel search without allowing result ordering to depend on worker scheduling.

The `-j` option controls the number of workers, from 1 through 256.

On Linux, the default worker count is based on physical CPU cores rather than simply logical CPU count when topology information is available.

## Line numbering

Line-number accounting is lazy. The implementation only performs the additional line-counting work needed when `-n` is requested and only up to the relevant hit.

This keeps the default fixed-string path from paying for line-number bookkeeping it does not need.

## Important tuning boundaries

The source intentionally keeps the following boundaries explicit:

- Small files: 1 MiB.
- Default large-file chunk: approximately 4 MiB.
- Maximum internal NFA size: 50,000 states, with special handling for large pure-literal alternations.
- Maximum Teddy literal table capacity: 128 literals.
- Default Teddy threshold: 32 literals.
- Maximum requested worker count: 256.
- `CG_CHUNK_KIB`: 64 KiB–1 GiB.

These are implementation details and may change between releases.
