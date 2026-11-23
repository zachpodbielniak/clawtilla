#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# test-skill.sh - keep the /clawtilla skill (data/skills/clawtilla) true.
#
# The skill is the manual a chief of staff or a custodian reads before
# changing the fleet, and they read it on a machine with no docs/ beside
# it.  A skill that is merely out of date is worse than none: it is
# followed.  So this fails the suite when the skill stops describing
# what is built:
#
#   - the front matter loads (clawtilla's own reader refuses an angle
#     bracket or a description over 1024 characters);
#   - every local link resolves, and every reference file is reachable;
#   - every clawtilla_* tool registered in src/mcp/clawt-mcp-tools.c is
#     in references/mcp-tools*.md;
#   - every top-level CLI command is in references/cli*.md;
#   - every docs/*.org is named somewhere in the skill, so a new feature's
#     doc cannot land without the skill learning it exists;
#   - `make install-skill` and `make uninstall-skill` round-trip into a
#     temporary HOME, with symlinks.
#
# The generated config reference is checked by test-config-schema.c.
#
# Usage: bash tests/test-skill.sh
# Example: make test

# make test runs every tests/test-*.sh with `sh`, which is dash on Debian
# and bash in POSIX mode on Fedora; this one needs bash itself.
if [ -z "${BASH_VERSION:-}" ] || shopt -qo posix
then
	exec bash "$0" "$@"
fi

set -euo pipefail

root="$(CDPATH='' cd -- "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
skill="${root}/data/skills/clawtilla"
failures=0

fail () {
	echo "test-skill: $*" >&2
	failures=$((failures + 1))
}

check_frontmatter () {
	local file="${skill}/SKILL.md"
	local description
	local section

	[[ -f "${file}" ]] || { fail "missing ${file}"; return; }
	[[ "$(head -n1 "${file}")" == "---" ]] || fail "SKILL.md must open with --- front matter"
	grep -qx 'name: clawtilla' "${file}" || fail "front matter name must be exactly 'clawtilla'"
	# The folded description: the indented lines after `description: >`.
	description="$(awk '/^description:/ {on=1; next} on && /^  / {print; next} on {exit}' "${file}")"
	[[ -n "${description}" ]] || fail "front matter has no description"
	[[ ${#description} -le 1024 ]] || fail "description is ${#description} characters (limit 1024)"
	[[ "${description}" != *[\<\>]* ]] || fail "description holds an angle bracket; the loader refuses it"
	for section in '## When to Use' '## Instructions' '## Output Format' '## Examples' '## Constraints'
	do
		grep -q "^${section}" "${file}" || fail "SKILL.md lacks '${section}'"
	done
}

# Every relative markdown link in a file, minus any #fragment.
links_in () {
	grep -oE '\]\([^)[:space:]]+\)' "${1}" | sed -E 's/^\]\(//; s/\)$//; s/#.*$//' |
		grep -vE '^(https?:|mailto:|$)' || true
}

check_links () {
	local file
	local link
	local target
	declare -A reached=()

	while IFS= read -r file
	do
		while IFS= read -r link
		do
			target="$(realpath -m "$(dirname "${file}")/${link}")"
			if [[ ! -e "${target}" ]]
			then
				fail "${file#"${root}"/}: broken link ${link}"
				continue
			fi
			reached["${target}"]=1
		done < <(links_in "${file}")
	done < <(find "${skill}" -name '*.md' | sort)

	# A reference nothing links to is a reference nobody is sent to.
	while IFS= read -r file
	do
		[[ -n "${reached["$(realpath "${file}")"]+x}" ]] ||
			fail "${file#"${skill}"/} is not linked from anywhere in the skill"
	done < <(find "${skill}/references" -name '*.md' | sort)
}

check_tools () {
	local name
	local corpus

	corpus="$(cat "${skill}"/references/mcp-tools*.md)"
	while IFS= read -r name
	do
		[[ "${corpus}" == *"${name}"* ]] ||
			fail "clawtilla tool ${name} is registered but not in references/mcp-tools*.md"
	done < <(bash "${skill}/scripts/list-mcp-tools.sh" "${root}")
}

check_cli () {
	local name
	local corpus

	corpus="$(cat "${skill}"/references/cli*.md)"
	while IFS= read -r name
	do
		[[ "${corpus}" == *"clawtilla ${name}"* ]] ||
			fail "CLI command 'clawtilla ${name}' is not in references/cli*.md"
	done < <(bash "${skill}/scripts/list-cli-commands.sh" "${root}")
}

check_docs () {
	local doc
	local name
	local corpus

	corpus="$(find "${skill}" -name '*.md' -exec cat {} +)"
	for doc in "${root}"/docs/*.org
	do
		name="$(basename "${doc}")"
		[[ "${corpus}" == *"${name}"* ]] ||
			fail "docs/${name} is named nowhere in the skill; add what it covers to a reference"
	done
}

check_install () {
	local home
	local args

	home="$(mktemp -d "${TMPDIR:-/tmp}/clawt-skill-home-XXXXXX")"
	args=(HOME="${home}" AGENTS_HOME="${home}/.agents" CLAUDE_DIR="${home}/.claude" GROK_DIR="${home}/.grok")

	make -s --no-print-directory -C "${root}" install-skill "${args[@]}" > /dev/null ||
		fail "make install-skill failed"
	[[ -L "${home}/.agents/skills/clawtilla" ]] || fail "install-skill did not symlink ~/.agents/skills/clawtilla"
	[[ "$(readlink "${home}/.agents/skills/clawtilla")" == "${skill}" ]] ||
		fail "the .agents/skills/clawtilla link does not point at this checkout's skill"
	[[ -f "${home}/.claude/skills/clawtilla/SKILL.md" ]] || fail "the ~/.claude link does not resolve"
	[[ -f "${home}/.grok/skills/clawtilla/SKILL.md" ]] || fail "the ~/.grok link does not resolve"

	# A second install replaces its own links rather than refusing them.
	make -s --no-print-directory -C "${root}" install-skill "${args[@]}" > /dev/null ||
		fail "a second make install-skill failed"

	make -s --no-print-directory -C "${root}" uninstall-skill "${args[@]}" > /dev/null ||
		fail "make uninstall-skill failed"
	[[ ! -e "${home}/.agents/skills/clawtilla" && ! -L "${home}/.agents/skills/clawtilla" ]] ||
		fail "uninstall-skill left ~/.agents/skills/clawtilla"
	[[ ! -L "${home}/.claude/skills/clawtilla" ]] || fail "uninstall-skill left the ~/.claude link"
	[[ -e "${skill}/SKILL.md" ]] || fail "uninstall-skill removed the checkout's own skill"

	rm -r -- "${home}"
}

check_frontmatter
check_links
check_tools
check_cli
check_docs
check_install

if [[ ${failures} -gt 0 ]]
then
	echo "test-skill: ${failures} problem(s)" >&2
	exit 1
fi
echo "  TEST    /clawtilla skill: front matter, links, tools, CLI, docs, install round trip"
