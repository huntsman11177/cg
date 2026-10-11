# Install cg

`cg` 1.3.2 is MIT-licensed. Source and release tags live at
<https://github.com/huntsman11177/cg>.

The binary installs as `cg` and the manual page as `man cg`.

## From source

```sh
git clone https://github.com/huntsman11177/cg.git
cd cg
make
make check
sudo make install
```

`PREFIX` defaults to `/usr/local`. Packagers should stage with `DESTDIR` and
set `PREFIX=/usr`.

## Arch Linux and pacman

The AUR package name is `cg`. After it is published:

```sh
git clone https://aur.archlinux.org/cg.git
cd cg
makepkg -si
```

`makepkg -si` builds the package and installs it with pacman. An AUR helper
does the same thing:

```sh
yay -S cg
```

To build from a local checkout before the AUR push:

```sh
cd packaging/arch
makepkg -si
```

The `PKGBUILD` on the default branch carries the SHA-256 of the
`cg-1.3.2.tar.gz` asset on the GitHub release. Official Arch repositories
(`core`, `extra`) are a separate submission. This release is the AUR package,
which pacman installs after `makepkg` or an AUR helper.

## Fedora, RHEL, and dnf

The spec is `packaging/rpm/cg.spec`. On a Fedora machine:

```sh
sudo dnf install gcc make glibc-devel rpm-build rpmdevtools
git clone https://github.com/huntsman11177/cg.git
cd cg/packaging/rpm
spectool -g -R cg.spec
rpmbuild -ba cg.spec
sudo dnf install ~/rpmbuild/RPMS/*/cg-1.3.2-1.*.rpm
```

`dnf install` of that local RPM is the supported path for this release.
Getting `cg` into the official Fedora repositories requires a Fedora package
review. That review is not part of this tag.

## GitHub

```sh
git clone https://github.com/huntsman11177/cg.git
```

Tagged release: <https://github.com/huntsman11177/cg/releases/tag/v1.3.2>

Continuous integration builds `cg` and runs `make check` on every push and
pull request (`.github/workflows/ci.yml`).
