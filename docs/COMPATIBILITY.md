# Compatibility

`cg` intentionally presents a POSIX/GNU-style grep interface, but it is not a drop-in replacement for every grep implementation.

## Supported interface

The current help text exposes:

### Pattern selection

- `-E`, `--extended-regexp`
- `-F`, `--fixed-strings`
- `-G`, `--basic-regexp`
- `-e`, `--regexp`
- `-f`, `--file`
- `-i`, `--ignore-case`
- `--no-ignore-case`
- `-y`
- `-w`, `--word-regexp`
- `-x`, `--line-regexp`

### Output

- `-c`, `--count`
- `-n`, `--line-number`
- `-b`, `--byte-offset`
- `-H`, `--with-filename`
- `-h`, `--no-filename`
- `--label`
- `-o`, `--only-matching`
- `-q`, `--quiet`, `--silent`
- `-l`, `--files-with-matches`
- `-L`, `--files-without-match`
- `-m`, `--max-count`
- `--line-buffered`
- `--color` / `--colour` (default `auto`; GNU grep defaults to `never`)
- `-Z`, `--null`

### Binary handling

- `-a`, `--text`
- `-I`
- `--binary-files=binary`
- `--binary-files=text`
- `--binary-files=without-match`

### Directory traversal

- `-d`, `--directories`
- `-D`, `--devices`
- `-r`, `--recursive`
- `-R`, `--dereference-recursive`
- `--include`
- `--exclude`
- `--exclude-dir`

### Context

- `-A`, `--after-context`
- `-B`, `--before-context`
- `-C`, `--context`
- `-NUM`

## Explicitly unsupported

The current source explicitly rejects:

```text
-P    Perl-compatible regular expressions
-z    NUL-separated input records
```

Do not document these as supported merely because GNU grep supports them.

## Regex implementation

The program contains three relevant regex behaviors:

1. Internal ERE/BRE parsing and matching for its supported subset.
2. Specialized recognition of pure literal expressions and literal alternations.
3. libc regex fallback for expressions that need functionality outside the internal matcher.

The source documentation specifically mentions fallback cases such as backreferences, intervals, and UTF-8 class behavior.

Because the exact boundary is implementation-defined, compatibility claims should be tested against the release binary rather than inferred solely from the option names.

## Compatibility testing policy

For each release, compare `cg` against GNU grep on a representative corpus.

Tests should include:

- Fixed strings.
- Multiple `-e` patterns.
- `-f` pattern files.
- Basic regex.
- Extended regex.
- Case-insensitive matching.
- Whole-word and whole-line matching.
- Inverted matches.
- Counts.
- Line numbers and byte offsets.
- Context.
- Recursive traversal.
- Include/exclude filters.
- Binary files.
- Empty files and empty patterns.
- Standard input.
- Multiple input files.
- Error cases and exit statuses.

For every test, compare both output and exit status.

Performance tests must be separate from semantic compatibility tests: a faster result is irrelevant if the selected lines differ.
