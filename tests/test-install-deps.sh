#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# tools/clawt-install-deps.sh holds the one table that maps a pkg-config
# name or a command to a Fedora, Debian/Ubuntu and Arch package.  A table
# like that drifts silently: a dependency added to PKG_DEPS and nowhere
# else installs fine on the author's machine and fails for the next
# person with "Package gudev-1.0 was not found" -- which is how gudev and
# both gstreamer modules were already missing from the Fedora table in
# docs/getting-started.org when this test was written.
#
# So this checks three things, none of which touches a package manager:
# distribution detection from os-release files; that every pkg-config
# name any PKG_DEPS list names has a row; and that every package the
# script can install is named in docs/getting-started.org.  The one run
# that goes past --print uses --dry-run, so a broken ostree check prints
# a command rather than running one.
set -eu

CDPATH='' cd "$(dirname "$0")/.."
root="$(pwd)"
script="${root}/tools/clawt-install-deps.sh"
doc="${root}/docs/getting-started.org"
fixture="$(mktemp -d "${TMPDIR:-/tmp}/clawt-install-deps-XXXXXX")"

cleanup () {
	rm -rf -- "${fixture}"
}
trap cleanup EXIT

fail () {
	echo "test-install-deps: $*" >&2
	exit 1
}

# ── detection ─────────────────────────────────────────────────────────
#
# expect <name> <family-or-empty> <os-release lines...>
expect () {
	name="$1"
	want="$2"
	shift 2
	printf '%s\n' "$@" > "${fixture}/${name}"
	if [ -z "${want}" ]
	then
		if bash "${script}" --print --os-release "${fixture}/${name}" > /dev/null 2>&1
		then
			fail "${name} was accepted; it should be unsupported"
		fi
		return 0
	fi
	got="$(bash "${script}" --print --os-release "${fixture}/${name}")" ||
		fail "${name} was refused; expected ${want}"
	ref="$(bash "${script}" --print --distro "${want}")"
	[ "${got}" = "${ref}" ] || fail "${name} did not detect as ${want}"
}

expect fedora        fedora 'NAME="Fedora Linux"' 'ID=fedora' 'VERSION_ID=45'
expect debian        debian 'ID=debian' 'VERSION_ID="13"'
expect ubuntu        debian 'ID=ubuntu' 'ID_LIKE=debian'
expect mint          debian 'ID=linuxmint' 'ID_LIKE="ubuntu debian"'
expect arch          arch   'ID=arch'
expect endeavouros   arch   'ID=endeavouros' 'ID_LIKE=arch'
expect centos        ''     'ID="centos"' 'ID_LIKE="rhel fedora"'
expect opensuse      ''     'ID="opensuse-tumbleweed"' 'ID_LIKE="opensuse suse"'
expect missing-keys  ''     'NAME=nothing'

# ── machine-specific qemu ─────────────────────────────────────────────
bash "${script}" --print --distro fedora --arch x86_64 | grep -qx qemu-system-x86-core ||
	fail 'x86_64 did not get qemu-system-x86-core'
if bash "${script}" --print --distro fedora --arch x86_64 | grep -qx qemu-system-aarch64-core
then
	fail 'x86_64 was given the aarch64 qemu'
fi
bash "${script}" --print --distro debian --arch aarch64 | grep -qx qemu-system-arm ||
	fail 'aarch64 Debian did not get qemu-system-arm'

# --build-only installs nothing that only runs things.
if bash "${script}" --print --distro arch --build-only | grep -qx podman
then
	fail '--build-only included podman'
fi

# ── an ostree host is refused, and a plain one runs the right command ─
: > "${fixture}/ostree-booted"
set +e
bash "${script}" --dry-run --distro fedora --ostree-marker "${fixture}/ostree-booted" \
	> "${fixture}/ostree.out" 2>&1
status=$?
set -e
[ "${status}" -eq 2 ] || fail "ostree host exited ${status}, expected 2"
grep -q 'would run' "${fixture}/ostree.out" && fail 'ostree host reached the package manager'
grep -q 'Layer these packages' "${fixture}/ostree.out" || fail 'ostree refusal did not list packages'

bash "${script}" --dry-run --yes --distro arch --ostree-marker "${fixture}/absent" \
	> "${fixture}/arch.out" 2>&1
grep -q 'would run: .*pacman -S --needed --noconfirm .*glib2' "${fixture}/arch.out" ||
	fail 'arch dry run did not build a pacman command'

# ── every pkg-config name has a row ───────────────────────────────────
#
# Join backslash continuations, then take the right-hand side of every
# *PKG_DEPS assignment in clawtilla's config.mk and every bundled dep's.
# The libregnum client's LRG_PKG_DEPS is opt-in and documented apart
# (docs/lrg-client.org), so it is left out on purpose.
bash "${script}" --keys > "${fixture}/keys"
for mk in "${root}/config.mk" "${root}"/deps/libreclaw/config.mk "${root}"/deps/libreclaw/deps/*/config.mk
do
	[ -f "${mk}" ] || continue
	sed -e ':a' -e '/\\$/N; s/\\\n//; ta' "${mk}" |
		grep -E '^[[:space:]]*(GTK_)?PKG_DEPS[[:space:]]*[:?]?=' |
		sed -e 's/^[^=]*=//' |
		tr -s '[:blank:]' '\n' |
		grep -v '^$' > "${fixture}/names" || true
	while IFS= read -r name
	do
		grep -qxF "${name}" "${fixture}/keys" ||
			fail "${mk#"${root}"/} needs ${name}, which clawt-install-deps.sh does not map"
	done < "${fixture}/names"
done

# ── every installable package is documented ───────────────────────────
for distro in fedora debian arch
do
	for machine in x86_64 aarch64
	do
		bash "${script}" --print --distro "${distro}" --arch "${machine}"
	done
done | sort -u > "${fixture}/packages"
while IFS= read -r pkg
do
	grep -qF "~${pkg}~" "${doc}" ||
		fail "${pkg} can be installed but docs/getting-started.org does not name it"
done < "${fixture}/packages"

echo 'test-install-deps: OK'
