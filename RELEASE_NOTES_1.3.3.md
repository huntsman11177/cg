# cg 1.3.3

Fast multi-threaded POSIX/GNU-style grep, licensed under the MIT License.

## Changes

- `-w` on the word-bounded DFA streams. A byte at or above `0x80` in a multibyte locale still ends that line.
- When every pattern atom is plain ASCII, a non-ASCII `-w` line is decided by that DFA. Each UTF-8 character becomes one placeholder byte (word or non-word). Invalid UTF-8, a non-ASCII literal, a negated class, or a named class still uses libc.
- Whole-word sets pack tokens of at most 8 bytes into one key. On AVX2 the scan classifies 64-byte blocks. A token that ends on a block boundary no longer drops the later tokens in that block.

On a warm 9.97 GB `en.txt`, one thread, `-c -E -w`:

| Pattern | cg 1.3.3 | ripgrep 15.2.0 | cg lines | ripgrep lines |
|---|---:|---:|---:|---:|
| `[a-z]+e` | 9.57 s | 19.97 s | 164108077 | 164108072 |
| `[a-z]+s` | 8.03 s | 19.31 s | 90951565 | 90951559 |

Turning the streamer off left the counts unchanged and took 18.19 s and 20.53 s. The literal scanners were not changed. `HARD_REAL_10000` was not retimed.

Source: https://github.com/huntsman11177/cg
Tag: `v1.3.3`
