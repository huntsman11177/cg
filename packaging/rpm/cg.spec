Name:           cg
Version:        1.3.2
Release:        1%{?dist}
Summary:        Fast multi-threaded POSIX/GNU-style grep

License:        MIT
URL:            https://github.com/huntsman11177/cg
Source0:        %{url}/releases/download/v%{version}/%{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  glibc-devel

%description
cg is a multi-threaded POSIX/GNU-style grep. The primary target is
high-throughput fixed-string and multi-pattern search. Supported regular
expressions use an internal lazy DFA, with the system regex library as a
fallback.

%prep
%autosetup -n %{name}-%{version}

%build
%make_build PREFIX=%{_prefix}

%install
%make_install PREFIX=%{_prefix}

%check
make check

%files
%license LICENSE
%doc README.md CHANGELOG.md RELEASE_NOTES_1.3.2.md
%{_bindir}/cg
%{_mandir}/man1/cg.1*

%changelog
* Sun Oct 11 2026 huntsman11177 <sahasubrata11177@gmail.com> - 1.3.2-1
- Package cg 1.3.2.
* Sat Oct 10 2026 huntsman11177 <sahasubrata11177@gmail.com> - 1.3.1-1
- Package cg 1.3.1.
* Sat Oct 10 2026 huntsman11177 <sahasubrata11177@gmail.com> - 1.3.0-1
- Package cg 1.3.0.
* Sat Oct 10 2026 huntsman11177 <sahasubrata11177@gmail.com> - 1.2.2-1
- Package cg 1.2.2.
