# cg 1.3.1

Fast multi-threaded POSIX/GNU-style grep, licensed under the MIT License.

## Changes

- With more than one worker, plain output is formatted in the worker that searched the chunk. The printer writes finished buffers in order. A single worker still formats in the printer, so search and output overlap.
- The streaming DFA keeps a non-end-anchor newline inside the four-way loop. End-anchor newlines, accepting states, and dead states still leave that loop.
- Plain `-v` with more than one worker keeps the sparse list of matching lines and writes the other lines from the file. It does not build a match record for every output line.

Fixed-string search is unchanged. Output-heavy timings are in `docs/BENCHMARKS.md`.

`cg` is a basic regular expression by default. A pattern like `[a-z]+ing` needs `-E` before `+` is a quantifier. Without `-E`, that search matches nothing, same as GNU grep.

Source: https://github.com/huntsman11177/cg
Tag: `v1.3.1`
