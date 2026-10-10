# Release Checklist

Use this checklist before publishing a `cg` release.

## Source

- [ ] `cg.c` builds with `-O3 -Wall -Wextra -pthread`.
- [ ] `--help` is current.
- [ ] `--version` reports the intended release.
- [ ] No debug-only behavior remains.
- [ ] No benchmark-only paths or hard-coded corpus assumptions remain.

## Compatibility

- [ ] Fixed-string tests pass.
- [ ] Regex tests pass.
- [ ] `-e` and `-f` tests pass.
- [ ] Output modifiers pass.
- [ ] Recursive traversal tests pass.
- [ ] Binary-file tests pass.
- [ ] Context tests pass.
- [ ] Exit-status tests pass.
- [ ] Explicitly unsupported options still fail cleanly.

## Performance

- [ ] Single-literal benchmark.
- [ ] Multi-literal benchmark.
- [ ] Large `-f` pattern-set benchmark.
- [ ] Regex benchmark.
- [ ] Thread-scaling benchmark.
- [ ] Warm/cold cache state documented.
- [ ] Counts/output verified against a reference implementation.

## Packaging

- [ ] Git tag created.
- [ ] Release tarball generated from the tag.
- [ ] SHA256 checksum calculated.
- [ ] AUR `PKGBUILD` updated with the exact release source and checksum.
- [ ] Package builds in a clean Arch environment.
- [ ] Installed binary reports the release version.

## Repository

- [ ] README updated.
- [ ] Architecture documentation updated.
- [ ] Compatibility documentation updated.
- [ ] Benchmark documentation updated.
- [ ] Changelog/release notes written.
- [x] License file present (MIT).
