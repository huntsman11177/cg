# RPM package

`cg.spec` builds cg 1.3.3 for dnf. It is not in the official Fedora
repositories. Install the built RPM locally.

```sh
sudo dnf install gcc make glibc-devel rpm-build rpmdevtools
spectool -g -R cg.spec
rpmbuild -ba cg.spec
sudo dnf install ~/rpmbuild/RPMS/*/cg-1.3.3-1.*.rpm
```

`spectool` stores the GitHub release archive in `~/rpmbuild/SOURCES`. The
archive root is `cg-1.3.3`, which matches `%autosetup -n cg-1.3.3`.

A Fedora package review is required before `dnf install cg` works from the
official Fedora repos. Track that separately from this spec.
