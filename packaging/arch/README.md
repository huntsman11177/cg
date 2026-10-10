# Arch package

`PKGBUILD` builds `cg` from the GitHub tag archive and installs it with
`PREFIX=/usr` so pacman can track the files.

```sh
makepkg -si
```

The SHA-256 in `PKGBUILD` is the checksum of

```text
https://github.com/huntsman11177/cg/releases/download/v1.2.2/cg-1.2.2.tar.gz
```

That digest is filled in on the default branch after the tag exists. The copy
of this file inside the `v1.2.2` archive may still say `SKIP`. Use the
`PKGBUILD` from the default branch, or from the AUR repository, to build.

## AUR

The package name `cg` was unused on the AUR when 1.2.2 was tagged. Publishing
it requires an AUR account and an SSH key registered at
<https://aur.archlinux.org/account>.

```sh
git clone ssh://aur@aur.archlinux.org/cg.git aur-cg
cp PKGBUILD .SRCINFO aur-cg/
cd aur-cg
git add PKGBUILD .SRCINFO
git commit -m "Initial import of cg 1.2.2"
git push origin master
```

Then install with pacman through `makepkg -si` or an AUR helper (`yay -S cg`).
