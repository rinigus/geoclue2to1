Name:           geoclue2to1
Version:        0.1.0
Release:        1
Summary:        GeoClue2 D-Bus API bridge backed by GeoClue1 on Sailfish OS
License:        BSD
Group:          System/Daemons
URL:            https://github.com/rinigus/geoclue2to1
Source0:        %{name}-%{version}.tar.bz2
Source1:        geoclue2to1.service
Source2:        geoclue2to1.conf

BuildRequires:  cmake
BuildRequires:  pkgconfig(dbus-glib-1)
BuildRequires:  pkgconfig(glib-2.0)
BuildRequires:  systemd

Requires(post):   systemd
Requires(preun):  systemd
Requires(postun): systemd

%description
geoclue2to1 is a small daemon that implements the org.freedesktop.GeoClue2
D-Bus API on Sailfish OS and forwards location requests to the existing
GeoClue1 service over D-Bus. It is intended to allow newer applications that
expect GeoClue2 to run on systems that only provide GeoClue1.

PackageName: GeoClue2to1
Categories:
  - Maps

%package test
Summary:        Test application for GeoClue2 D-Bus API bridge backed by GeoClue1 on Sailfish OS
Group:          System/Daemons
Requires:       %{name}%{?_isa} = %{version}-%{release}

%description test
%{summary}

%prep
%setup -q

%build
%cmake .
%cmake_build

%install
%cmake_install

%{__install} -Dp -m0644 %{SOURCE1} %{buildroot}%{_userunitdir}/geoclue2to1.service
%{__install} -Dp -m0644 %{SOURCE2} %{buildroot}%{_datadir}/dbus-1/system.d/geoclue2to1.conf

%preun
# in case of complete removal, stop and disable
if [ "$1" = "0" ]; then
  systemctl-user disable %{name} || true
  systemctl-user stop %{name} || true
fi

%post
systemctl-user daemon-reload || true
systemctl-user start %{name} || true
systemctl-user enable %{name} || true

%pre
# In case of update, stop first
if [ "$1" = "2" ]; then
  systemctl-user disable %{name} || true
  systemctl-user stop %{name} || true
fi

%files
%defattr(-,root,root,-)
%doc
/usr/bin/geoclue2to1
%{_userunitdir}/geoclue2to1.service
%{_datadir}/dbus-1/system.d/geoclue2to1.conf

%files test
/usr/bin/geoclue2-test-client
