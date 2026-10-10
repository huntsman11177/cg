# Contributing

Thanks for contributing to `cg`.

## Development priorities

The project prioritizes:

1. Correct grep-compatible behavior.
2. Deterministic output and exit status.
3. High throughput on large fixed-string workloads.
4. Good scaling across worker threads.
5. Small, reviewable changes.

## Build

```sh
cc -O3 -Wall -Wextra -pthread -o cg cg.c
```

## Before submitting a change

At minimum:

```sh
./cg --help
./cg --version
```

Run semantic comparisons against GNU grep for the behavior you changed.

For performance-sensitive changes, benchmark before and after on the same corpus, pattern set, thread count, and cache state.

## Performance-sensitive code

The byte scanners are deliberately treated as frozen performance-critical components.

Do not change the scanner, anchor bloom, Teddy filter, or chunk scheduler merely because an alternative looks cleaner.

A performance change should include:

- Exact command lines.
- Before/after timings.
- Hardware.
- Compiler and flags.
- Workload description.
- Verification that output/counts are identical.

## Correctness

A prefilter must never become the final source of truth when it only produces candidates.

When changing literal filtering, test:

- No false negatives.
- No false positives.
- Overlapping literals.
- Prefix/suffix relationships.
- Very short literals.
- Duplicate patterns.
- Empty patterns where accepted.
- Large pattern sets.

## Threading

Changes to shared state must be reviewed for:

- Data races.
- Lifetime/ownership.
- Cancellation behavior.
- Ordered output.
- Error propagation.

The normal design is for workers to operate independently and for output ordering to be handled centrally.

## Compatibility

Do not silently add behavior that differs from grep semantics.

If a feature is intentionally unsupported, keep the command-line rejection and update `docs/COMPATIBILITY.md`.

## Documentation

Update documentation when:

- A command-line option changes.
- Compatibility changes.
- A tuning parameter changes.
- The matching architecture changes.
- A benchmark claim changes.

## Pull requests

A good pull request should explain:

- What changed.
- Why it changed.
- How correctness was verified.
- How performance was measured, if applicable.
- Any compatibility implications.
