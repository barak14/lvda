# SPDX-License-Identifier: GPL-2.0
#
# Build:
#   ./makerpm.sh              # stages a clean tarball + runs rpmbuild
#   sudo dnf install ./RPMS/x86_64/lvda-dkms-<ver>.x86_64.rpm
#
# Requires the matching kernel-devel/kernel-headers package on the install
# target so DKMS can build the module against the running kernel.

# Distro fallbacks for path macros not defined on every RPM family
# (notably _modulesloaddir on older openSUSE).
%{!?_sysusersdir:    %global _sysusersdir    %{_prefix}/lib/sysusers.d}
%{!?_tmpfilesdir:    %global _tmpfilesdir    %{_prefix}/lib/tmpfiles.d}
%{!?_udevrulesdir:   %global _udevrulesdir   %{_prefix}/lib/udev/rules.d}
%{!?_modulesloaddir: %global _modulesloaddir %{_prefix}/lib/modules-load.d}

%global module_name    lvda
%global module_version 0.1.0

Name:           %{module_name}-dkms
Version:        %{module_version}
Release:        1%{?dist}
Summary:        Virtual display driver for Sunshine/Apollo streaming (DKMS sources)

License:        GPL-2.0-only
URL:            https://github.com/_/lvda
Source0:        %{module_name}-%{version}.tar.gz

# Userspace lvda-ctl is built at package time; the kernel module is built by
# DKMS at install time against the running kernel.
BuildRequires:  gcc
BuildRequires:  make
ExclusiveArch:  x86_64

Requires:          dkms
Requires(post):    dkms
Requires(preun):   dkms
Recommends:        kernel-devel
%if 0%{?suse_version}
Recommends:        kernel-default-devel
%endif

Conflicts:      %{module_name}
Provides:       %{module_name} = %{version}-%{release}

%description
lvda is a Linux DRM kernel module that exposes a persistent virtual GPU
(/dev/dri/cardN) carrying a pool of virtual monitors. The monitors start
disconnected; a control ioctl lights one up at an exact resolution, refresh
rate, and HDR mode on demand and hotplug-connects it. A streaming host
(Sunshine / Apollo + Moonlight) can then serve a client at its native mode.

This package ships the kernel module source (built per-kernel via DKMS), the
lvda-ctl userspace daemon, and the udev/sysusers/tmpfiles/modules-load
drop-ins required to use the driver.

%prep
%setup -q -n %{module_name}-%{version}

%build
%{__make} -C tools/lvda-ctl

%install
# DKMS source tree (module + uapi + dkms.conf).
install -dm755 %{buildroot}%{_usrsrc}/%{module_name}-%{version}
cp -a module %{buildroot}%{_usrsrc}/%{module_name}-%{version}/module
cp -a uapi   %{buildroot}%{_usrsrc}/%{module_name}-%{version}/uapi
install -Dm644 packaging/dkms.conf \
    %{buildroot}%{_usrsrc}/%{module_name}-%{version}/dkms.conf

# Userspace CLI + UAPI header.
install -Dm755 tools/lvda-ctl/lvda-ctl %{buildroot}%{_bindir}/lvda-ctl
install -Dm644 uapi/lvda.h             %{buildroot}%{_includedir}/lvda/lvda.h

# systemd drop-ins.
install -Dm644 packaging/sysusers.d/lvda.conf \
    %{buildroot}%{_sysusersdir}/lvda.conf
install -Dm644 packaging/tmpfiles.d/lvda.conf \
    %{buildroot}%{_tmpfilesdir}/lvda.conf
install -Dm644 packaging/udev/60-lvda.rules \
    %{buildroot}%{_udevrulesdir}/60-lvda.rules
install -Dm644 packaging/modules-load.d/lvda.conf \
    %{buildroot}%{_modulesloaddir}/lvda.conf

%post
# Register sources with DKMS, build, and install against the running kernel.
if [ -x /usr/sbin/dkms ] || [ -x /usr/bin/dkms ]; then
    dkms add -m %{module_name} -v %{version} --rpm_safe_upgrade ||:
    dkms build -m %{module_name} -v %{version} ||:
    dkms install -m %{module_name} -v %{version} --force ||:
fi
systemd-sysusers ||:
systemd-tmpfiles --create %{_tmpfilesdir}/lvda.conf ||:
udevadm control --reload-rules ||:
udevadm trigger --subsystem-match=misc ||:

%preun
# On uninstall (not upgrade), unregister from DKMS so the .ko is wiped.
if [ "$1" = "0" ]; then
    if [ -x /usr/sbin/dkms ] || [ -x /usr/bin/dkms ]; then
        dkms remove -m %{module_name} -v %{version} --all --rpm_safe_upgrade ||:
    fi
fi

%postun
udevadm control --reload-rules ||:

%files
%license LICENSE
%dir %{_usrsrc}/%{module_name}-%{version}
%{_usrsrc}/%{module_name}-%{version}/dkms.conf
%{_usrsrc}/%{module_name}-%{version}/module
%{_usrsrc}/%{module_name}-%{version}/uapi
%{_bindir}/lvda-ctl
%dir %{_includedir}/lvda
%{_includedir}/lvda/lvda.h
%{_sysusersdir}/lvda.conf
%{_tmpfilesdir}/lvda.conf
%{_udevrulesdir}/60-lvda.rules
%{_modulesloaddir}/lvda.conf

%changelog
* Sun May 31 2026 lvda contributors <none@example.com> - 0.1.0-1
- Initial release.
