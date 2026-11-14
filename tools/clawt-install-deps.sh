#!/usr/bin/env bash
# clawt-install-deps.sh - Install clawtilla's build and runtime packages.
#
# Copyright (C) 2026
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Works out the distribution family from /etc/os-release (ID, then
# ID_LIKE) and installs the packages for Fedora, Debian/Ubuntu or Arch
# with that family's own package manager.  `make install-deps` runs it.
#
# The table below is the one place a pkg-config name or a command is
# mapped to a package.  tests/test-install-deps.sh fails when a name in
# any PKG_DEPS list has no row here, and when a package this script would
# install is missing from docs/getting-started.org -- a hand-written list
# of packages is exactly the kind that drifts, so it is checked rather
# than trusted.
#
# An ostree-booted host (Silverblue, Immutablue, Hyacinth Macaw, ...) is
# refused: its /usr is read-only, and the packages belong in the image
# build.  The script prints them for that instead of installing anything.

set -euo pipefail

readonly LICENSE_TEXT='clawt-install-deps.sh is part of clawtilla.
Copyright (C) 2026 Zach Podbielniak

This program is free software: you can redistribute it and/or modify it
under the terms of the GNU Affero General Public License as published by
the Free Software Foundation, either version 3 of the License, or (at
your option) any later version.

This program is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Affero
General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.'

# key,kind,fedora,debian,arch
#
# key   a pkg-config module for build rows, a command for runtime rows
# kind  toolchain | build | optional (the build skips it with a message
#       when absent) | runtime | runtime-x86_64 | runtime-aarch64
#
# A cell may hold several packages separated by spaces.  Every name was
# checked against the distribution's own metadata (Fedora 45, Ubuntu
# 24.04, Arch) on 2026-11-14.  Arch has no libpq package of its own;
# postgresql-libs carries it.
readonly PACKAGE_TABLE='
gcc,toolchain,gcc make pkgconf-pkg-config git,build-essential pkg-config git,base-devel git
glib-2.0,build,glib2-devel,libglib2.0-dev,glib2
gobject-2.0,build,glib2-devel,libglib2.0-dev,glib2
gio-2.0,build,glib2-devel,libglib2.0-dev,glib2
gio-unix-2.0,build,glib2-devel,libglib2.0-dev,glib2
gmodule-2.0,build,glib2-devel,libglib2.0-dev,glib2
libsoup-3.0,build,libsoup3-devel,libsoup-3.0-dev,libsoup3
json-glib-1.0,build,json-glib-devel,libjson-glib-dev,json-glib
libcmark,build,cmark-devel,libcmark-dev,cmark
sqlite3,build,sqlite-devel,libsqlite3-dev,sqlite
libetpan,build,libetpan-devel,libetpan-dev,libetpan
libpq,build,libpq-devel,libpq-dev,postgresql-libs
libdex-1,build,libdex-devel,libdex-dev,libdex
libxml-2.0,build,libxml2-devel,libxml2-dev,libxml2
gstreamer-1.0,build,gstreamer1-devel,libgstreamer1.0-dev,gstreamer
gstreamer-app-1.0,build,gstreamer1-plugins-base-devel,libgstreamer-plugins-base1.0-dev,gst-plugins-base-libs
yaml-0.1,build,libyaml-devel,libyaml-dev,libyaml
gudev-1.0,build,libgudev-devel,libgudev-1.0-dev,libgudev
libvirt,build,libvirt-devel,libvirt-dev,libvirt
gtk4,optional,gtk4-devel,libgtk-4-dev,gtk4
libadwaita-1,optional,libadwaita-devel,libadwaita-1-dev,libadwaita
gobject-introspection-1.0,optional,gobject-introspection-devel,libgirepository1.0-dev gobject-introspection,gobject-introspection
podman,runtime,podman,podman,podman
distrobox,runtime,distrobox,distrobox,distrobox
bwrap,runtime,bubblewrap,bubblewrap,bubblewrap
virsh,runtime,libvirt-client,libvirt-clients,libvirt
libvirtd,runtime,libvirt-daemon-kvm,libvirt-daemon-system,libvirt
qemu-system-x86_64,runtime-x86_64,qemu-system-x86-core,qemu-system-x86,qemu-system-x86
qemu-system-aarch64,runtime-aarch64,qemu-system-aarch64-core,qemu-system-arm,qemu-system-aarch64
qemu-img,runtime,qemu-img,qemu-utils,qemu-img
virtiofsd,runtime,virtiofsd,virtiofsd,virtiofsd
passt,runtime,passt,passt,passt
xorrisofs,runtime,xorriso,xorriso,libisoburn
Xvfb,runtime,xorg-x11-server-Xvfb,xvfb,xorg-server-xvfb
'

usage () {
	cat <<'EOF'
Usage: clawt-install-deps.sh [OPTION]...

Install the packages clawtilla needs to build and run, using the package
manager of the distribution it detects: Fedora (dnf), Debian/Ubuntu
(apt-get) or Arch (pacman).  Runs the package manager through sudo when
not already root.  An ostree-booted host is refused, with the package
list printed for its image build instead.

Options:
  --build-only          only what is needed to compile (no podman, qemu, ...)
  --runtime-only        only the runtime tools for computers and browsers
  --print               print the package list and install nothing
  --keys                print every pkg-config name and command the table maps
  -y, --yes             do not ask the package manager for confirmation
  --dry-run             print the package-manager commands instead of running them
  --distro FAMILY       skip detection: fedora, debian or arch
  --os-release FILE     read FILE instead of /etc/os-release
  --ostree-marker FILE  treat the host as ostree-booted when FILE exists
                        (default /run/ostree-booted)
  --arch MACHINE        use MACHINE instead of `uname -m` for the qemu package
  -h, --help            show this help
  --license             show the license

Examples:
  make install-deps                            # everything, with confirmation
  make install-deps INSTALL_DEPS_FLAGS=--build-only
  tools/clawt-install-deps.sh --print          # what would be installed here
  tools/clawt-install-deps.sh --print --distro arch --build-only
  tools/clawt-install-deps.sh --yes            # unattended, e.g. in a Containerfile
EOF
}

# Print the family (fedora, debian or arch) named by an os-release file,
# or nothing when it names none of them.  ID is tried before ID_LIKE so
# that a distribution which is itself one of the three wins over what it
# resembles; ID_LIKE covers Ubuntu, Mint, Pop!_OS, EndeavourOS, Manjaro.
detect_family () {
	if [[ $# -ne 1 ]]
	then
		echo 'detect_family() requires 1 positional argument' >&2
		echo 'detect_family <os-release-file>' >&2
		exit 1
	fi

	local os_release="${1}"
	local id=''
	local id_like=''
	local candidate
	local line

	if [[ ! -r "${os_release}" ]]
	then
		return 0
	fi

	# Read the two keys by hand rather than sourcing the file: os-release
	# is shell-compatible by specification, not by guarantee.
	while IFS= read -r line
	do
		case "${line}" in
			ID=*)
				id="${line#ID=}"
				;;
			ID_LIKE=*)
				id_like="${line#ID_LIKE=}"
				;;
		esac
	done < "${os_release}"
	id="${id//\"/}"
	id_like="${id_like//\"/}"

	# Enterprise Linux says ID_LIKE="rhel fedora", and is not Fedora minus
	# things: libetpan and distrobox are in EPEL or nowhere, and several
	# names differ.  Answering "fedora" would start a dnf run that fails
	# halfway, so it is unsupported here rather than wrongly supported.
	case "${id}" in
		rhel|centos|rocky|almalinux|ol)
			return 0
			;;
	esac

	for candidate in ${id} ${id_like}
	do
		case "${candidate}" in
			fedora)
				echo fedora
				return 0
				;;
			debian|ubuntu)
				echo debian
				return 0
				;;
			arch|archlinux)
				echo arch
				return 0
				;;
		esac
	done
}

# Print the packages for one family, one per line, deduplicated in table
# order.  The kinds wanted are passed as a space-separated list.
packages_for () {
	if [[ $# -ne 3 ]]
	then
		echo 'packages_for() requires 3 positional arguments' >&2
		echo 'packages_for <family> <kinds> <machine>' >&2
		exit 1
	fi

	local family="${1}"
	local kinds="${2}"
	local machine="${3}"
	local column
	local key kind fedora debian arch cell pkg
	declare -A seen=()

	case "${family}" in
		fedora) column=fedora ;;
		debian) column=debian ;;
		arch)   column=arch ;;
		*)
			echo "packages_for: unknown family '${family}'" >&2
			exit 1
			;;
	esac

	while IFS=, read -r key kind fedora debian arch
	do
		[[ -z "${key}" ]] && continue

		# A qemu row belongs to one machine; it is wanted when the
		# runtime is wanted and the machine matches.
		case "${kind}" in
			runtime-*)
				[[ "${kind#runtime-}" == "${machine}" ]] || continue
				kind=runtime
				;;
		esac
		[[ " ${kinds} " == *" ${kind} "* ]] || continue

		case "${column}" in
			fedora) cell="${fedora}" ;;
			debian) cell="${debian}" ;;
			arch)   cell="${arch}" ;;
		esac

		for pkg in ${cell}
		do
			if [[ -z "${seen[${pkg}]+x}" ]]
			then
				seen[${pkg}]=1
				echo "${pkg}"
			fi
		done
	done <<< "${PACKAGE_TABLE}"
}

# Run a command as root: directly when we are root, through sudo when
# not, and refuse rather than guess when there is no sudo.
# Set by --dry-run: as_root prints the command it would run instead.
dry_run=0

as_root () {
	if [[ "${dry_run}" -eq 1 ]]
	then
		echo "would run: $*"
	elif [[ "${EUID}" -eq 0 ]]
	then
		"$@"
	elif command -v sudo > /dev/null 2>&1
	then
		sudo "$@"
	else
		echo 'clawt-install-deps: not root and no sudo; rerun as root' >&2
		exit 1
	fi
}

main () {
	local kinds='toolchain build optional runtime'
	local print_only=0
	local keys_only=0
	local assume_yes=0
	local family=''
	local os_release='/etc/os-release'
	local ostree_marker='/run/ostree-booted'
	local machine
	local -a packages=()

	machine="$(uname -m)"

	while [[ $# -gt 0 ]]
	do
		case "${1}" in
			--build-only)
				kinds='toolchain build optional'
				;;
			--runtime-only)
				kinds='runtime'
				;;
			--print)
				print_only=1
				;;
			--keys)
				keys_only=1
				;;
			-y|--yes)
				assume_yes=1
				;;
			--dry-run)
				dry_run=1
				;;
			--distro|--os-release|--ostree-marker|--arch)
				if [[ $# -lt 2 ]]
				then
					echo "clawt-install-deps: ${1} needs a value" >&2
					exit 1
				fi
				case "${1}" in
					--distro)        family="${2}" ;;
					--os-release)    os_release="${2}" ;;
					--ostree-marker) ostree_marker="${2}" ;;
					--arch)          machine="${2}" ;;
				esac
				shift
				;;
			-h|--help)
				usage
				exit 0
				;;
			--license)
				echo "${LICENSE_TEXT}"
				exit 0
				;;
			*)
				echo "clawt-install-deps: unknown option '${1}'" >&2
				usage >&2
				exit 1
				;;
		esac
		shift
	done

	if [[ "${keys_only}" -eq 1 ]]
	then
		while IFS=, read -r key _
		do
			[[ -n "${key}" ]] && echo "${key}"
		done <<< "${PACKAGE_TABLE}"
		exit 0
	fi

	if [[ -z "${family}" ]]
	then
		family="$(detect_family "${os_release}")"
	fi
	case "${family}" in
		fedora|debian|arch)
			;;
		'')
			echo "clawt-install-deps: ${os_release} names no supported distribution" >&2
			echo 'Supported: Fedora, Debian/Ubuntu and their derivatives, Arch and its derivatives.' >&2
			echo 'Use --distro fedora|debian|arch to choose one, or see docs/getting-started.org.' >&2
			exit 1
			;;
		*)
			echo "clawt-install-deps: unknown --distro '${family}' (fedora, debian or arch)" >&2
			exit 1
			;;
	esac

	mapfile -t packages < <(packages_for "${family}" "${kinds}" "${machine}")

	if [[ "${print_only}" -eq 1 ]]
	then
		printf '%s\n' "${packages[@]}"
		exit 0
	fi

	# An image-based host has a read-only /usr; installing onto it is
	# either impossible or a transient overlay nobody asked for.  Say what
	# to layer and stop -- this is not a failure of the script.
	if [[ -e "${ostree_marker}" ]]
	then
		echo 'clawt-install-deps: this host is ostree-booted (/usr is read-only).' >&2
		echo 'Layer these packages into the image build instead:' >&2
		printf '  %s\n' "${packages[@]}" >&2
		exit 2
	fi

	echo "clawt-install-deps: installing ${#packages[@]} ${family} packages"
	case "${family}" in
		fedora)
			if [[ "${assume_yes}" -eq 1 ]]
			then
				as_root dnf install -y "${packages[@]}"
			else
				as_root dnf install "${packages[@]}"
			fi
			;;
		debian)
			as_root apt-get update
			if [[ "${assume_yes}" -eq 1 ]]
			then
				as_root env DEBIAN_FRONTEND=noninteractive apt-get install -y "${packages[@]}"
			else
				as_root apt-get install "${packages[@]}"
			fi
			;;
		arch)
			# -S without -y: syncing the database without upgrading is a
			# partial upgrade, which Arch does not support.  A stale
			# database fails here and says so; run pacman -Syu first.
			if [[ "${assume_yes}" -eq 1 ]]
			then
				as_root pacman -S --needed --noconfirm "${packages[@]}"
			else
				as_root pacman -S --needed "${packages[@]}"
			fi
			;;
	esac
}

main "$@"
