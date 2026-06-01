# Copyright 2026 lvda contributors
# Distributed under the terms of the GNU General Public License v2
# SPDX-License-Identifier: GPL-2.0

EAPI=8

inherit tmpfiles toolchain-funcs

DESCRIPTION="Virtual display driver for Sunshine/Apollo streaming (DKMS sources)"
HOMEPAGE="https://github.com/_/lvda"
SRC_URI="https://github.com/_/lvda/archive/refs/tags/v${PV}.tar.gz -> lvda-${PV}.tar.gz"
S="${WORKDIR}/lvda-${PV}"

LICENSE="GPL-2"
SLOT="0"
KEYWORDS="~amd64"

# The kernel module is built per-kernel by DKMS at install time (matching the
# .deb/.rpm/Arch packages); only the userspace CLI is compiled from this ebuild.
RDEPEND="sys-kernel/dkms"

src_compile() {
	emake -C tools/lvda-ctl CC="$(tc-getCC)"
}

src_install() {
	local dkmsroot="/usr/src/lvda-${PV}"

	# DKMS source tree (module + uapi + dkms.conf).
	insinto "${dkmsroot}"
	doins packaging/dkms.conf
	doins -r module uapi

	# Userspace CLI + UAPI header for third-party consumers.
	dobin tools/lvda-ctl/lvda-ctl
	insinto /usr/include/lvda
	doins uapi/lvda.h

	# Drop-ins (group, device perms, rendezvous dir, boot autoload).
	insinto /usr/lib/sysusers.d
	doins packaging/sysusers.d/lvda.conf
	insinto /usr/lib/tmpfiles.d
	doins packaging/tmpfiles.d/lvda.conf
	insinto /usr/lib/udev/rules.d
	doins packaging/udev/60-lvda.rules
	insinto /usr/lib/modules-load.d
	doins packaging/modules-load.d/lvda.conf

	dodoc README.md BUILD.md
}

pkg_postinst() {
	# No systemd-sysusers on OpenRC profiles — create the group ourselves.
	getent group lvda >/dev/null 2>&1 || groupadd --system lvda || true

	# Register sources with DKMS, build, and install against the running kernel.
	if [[ -x ${EROOT}/usr/sbin/dkms || -x ${EROOT}/usr/bin/dkms ]]; then
		dkms add     -m lvda -v "${PV}" || true
		dkms build   -m lvda -v "${PV}" || true
		dkms install -m lvda -v "${PV}" --force || true
	fi

	tmpfiles_process lvda.conf

	elog "Add the streaming user to the 'lvda' group, then re-login:"
	elog "    gpasswd -a <user> lvda"
}

pkg_prerm() {
	# On uninstall (not upgrade), unregister from DKMS so the .ko is wiped.
	if [[ -z ${REPLACED_BY_VERSION} ]]; then
		if [[ -x ${EROOT}/usr/sbin/dkms || -x ${EROOT}/usr/bin/dkms ]]; then
			dkms remove -m lvda -v "${PV}" --all || true
		fi
	fi
}
