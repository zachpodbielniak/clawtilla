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
# module a config.mk, Makefile or *.mk passes to pkg-config (any nesting
# under deps/ included), and every name in a PKG_DEPS-style list, has a
# row; and that every package the script can install is named in
# docs/getting-started.org.  The one run that goes past --print uses
# --dry-run, so a broken ostree check prints a command rather than
# running one.
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
# Every config.mk, Makefile and *.mk in the tree, deps/ at every nesting
# included.  Two sources, both of which have to have a row:
#   a module passed to pkg-config / $(PKG_CONFIG) after --cflags, --libs,
#   --exists or --modversion
#   a literal name in a PKG_DEPS-style list (PKG_DEPS, GTK_PKG_DEPS,
#   CLIENT_PKG_DEPS, DEPS_REQUIRED, NCURSES_DEPS, ONNXRUNTIME_DEPS, ...)
# An assignment whose variable starts with LRG_ is the opt-in libregnum
# client (docs/lrg-client.org) and is left out, same as LRG_PKG_DEPS was.
# A name that is only probed still needs a row; the kind says whether the
# default install pulls the package.
cat > "${fixture}/scan.awk" <<'END_AWK'
function is_ident_char(c) {
	return (c ~ /^[A-Za-z0-9_.-]$/)
}

function is_pc(tok,    i, rest, prev) {
	i = index(tok, "pkg-config")
	if (i > 0) {
		rest = substr(tok, i + 11)
		if (rest == "" || !is_ident_char(substr(rest, 1, 1))) {
			if (i == 1) return 1
			prev = substr(tok, i - 1, 1)
			if (!is_ident_char(prev)) return 1
		}
	}
	i = index(tok, "$(PKG_CONFIG)")
	if (i > 0) {
		rest = substr(tok, i + 14)
		if (rest == "" || substr(rest, 1, 1) !~ /^[A-Za-z0-9_]$/) {
			if (i == 1) return 1
			prev = substr(tok, i - 1, 1)
			if (prev !~ /^[A-Za-z0-9_]$/) return 1
		}
	}
	return 0
}

function is_flag(tok) {
	return (tok == "--cflags" || tok == "--libs" || tok == "--exists" || tok == "--modversion")
}

function is_stop(tok) {
	if (tok == "&&" || tok == "||" || tok == ";" || tok == "|") return 1
	if (index(tok, ";") > 0) return 1
	if (tok ~ /^#/) return 1
	if (tok ~ /^2?>/ || tok ~ /^</) return 1
	return 0
}

function clean_mod(tok) {
	gsub(/^[("']+/, "", tok)
	gsub(/[)"',;]+$/, "", tok)
	return tok
}

function is_module(tok) {
	if (tok == "" || tok == "pkg-config" || tok == "pkgconf") return 0
	if (tok == "echo" || tok == "yes" || tok == "y" || tok == "no") return 0
	if (tok == "if" || tok == "then" || tok == "else" || tok == "fi") return 0
	if (tok == "for" || tok == "in" || tok == "do" || tok == "done") return 0
	if (tok == "while" || tok == "until" || tok == "case" || tok == "esac") return 0
	if (tok == "OK" || tok == "MISSING") return 0
	if (index(tok, "$") > 0) return 0
	if (tok ~ /^-/ || tok ~ /\//) return 0
	return (tok ~ /^[A-Za-z][A-Za-z0-9_.+-]*$/)
}

function list_name(var) {
	if (var == "" || var ~ /^LRG_/) return 0
	if (var == "DEPS_REQUIRED" || var == "NCURSES_DEPS" || var == "ONNXRUNTIME_DEPS") return 1
	if (var ~ /PKG_DEPS$/) return 1
	return 0
}

function emit(path, line, name) {
	print path ":" line ":" name
}

function consider(path, line, tok) {
	tok = clean_mod(tok)
	if (is_module(tok)) emit(path, line, tok)
}

function process_group(path,    i, j, n, tok, var, rest, seen, collecting, first, past, eq, skip, pre) {
	first = seg[1]
	sub(/^[[:space:]]+/, "", first)
	if (first ~ /^#/) return
	var = ""
	if (match(first, /^[A-Za-z_][A-Za-z0-9_]*/)) {
		var = substr(first, RSTART, RLENGTH)
		rest = substr(first, RSTART + RLENGTH)
		sub(/^[[:space:]]+/, "", rest)
		if (rest !~ /^[:?+]?=/) var = ""
	}
	if (var ~ /^LRG_/) return

	collecting = 0
	seen = 0
	skip = 0
	for (i = 1; i <= nseg; i++) {
		if (skip) break
		n = split(seg[i], toks, /[[:space:]]+/)
		for (j = 1; j <= n; j++) {
			tok = toks[j]
			if (tok == "") continue
			if (tok ~ /^#/) {
				skip = 1
				break
			}
			if (index(tok, ";") > 0) {
				pre = tok
				sub(/;.*/, "", pre)
				if (collecting && seen) consider(path, segln[i], pre)
				collecting = 0
				seen = 0
				continue
			}
			if (is_stop(tok)) {
				collecting = 0
				seen = 0
				continue
			}
			if (is_pc(tok)) {
				collecting = 1
				seen = 0
				continue
			}
			if (collecting && is_flag(tok)) {
				seen = 1
				continue
			}
			if (collecting && seen) consider(path, segln[i], tok)
		}
	}

	if (!list_name(var)) return
	past = 0
	skip = 0
	for (i = 1; i <= nseg; i++) {
		if (skip) break
		n = split(seg[i], toks, /[[:space:]]+/)
		for (j = 1; j <= n; j++) {
			tok = toks[j]
			if (tok == "" || tok ~ /^#/) {
				if (tok ~ /^#/) skip = 1
				if (skip) break
				continue
			}
			if (i == 1 && !past) {
				eq = index(tok, "=")
				if (eq == 0) continue
				past = 1
				tok = substr(tok, eq + 1)
				if (tok == "") continue
			}
			consider(path, segln[i], tok)
		}
	}
}

FNR == 1 {
	if (nseg > 0) process_group(cur)
	nseg = 0
	cur = FILENAME
	sub(/^\.\//, "", cur)
}

{
	raw = $0
	cont = (raw ~ /\\[[:space:]]*$/)
	sub(/\\[[:space:]]*$/, "", raw)
	nseg++
	seg[nseg] = raw
	segln[nseg] = FNR
	if (!cont) {
		process_group(cur)
		nseg = 0
	}
}

END {
	if (nseg > 0) process_group(cur)
}
END_AWK

bash "${script}" --keys > "${fixture}/keys"
find . \( -name .git -o -name trees \) -prune -o \
	-type f \( -name config.mk -o -name Makefile -o -name '*.mk' \) -print \
	| sed 's|^\./||' | sort > "${fixture}/mk-files"
[ -s "${fixture}/mk-files" ] || fail 'no makefiles found'
set --
while IFS= read -r mk
do
	set -- "$@" "${mk}"
done < "${fixture}/mk-files"
awk -f "${fixture}/scan.awk" "$@" | sort -u > "${fixture}/uses"
grep -q ':glib-2.0$' "${fixture}/uses" || fail 'scanner found no glib-2.0 use'
grep -qxF 'deps/libreclaw/deps/podomation/deps/crispy/config.mk:106:readline' "${fixture}/uses" ||
	fail 'scanner missed crispy config.mk:106 readline'
grep -qxF 'deps/libreclaw/deps/podomation/modules/udev/Makefile:10:libudev' "${fixture}/uses" ||
	fail 'scanner missed udev Makefile:10 libudev'
: > "${fixture}/missing"
while IFS= read -r use
do
	name="${use##*:}"
	place="${use%:*}"
	if ! grep -qxF "${name}" "${fixture}/keys"
	then
		printf '%s needs %s, which clawt-install-deps.sh does not map\n' \
			"${place}" "${name}"
	fi
done < "${fixture}/uses" > "${fixture}/missing"
[ ! -s "${fixture}/missing" ] || {
	cat "${fixture}/missing" >&2
	fail 'pkg-config names above have no row'
}

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
