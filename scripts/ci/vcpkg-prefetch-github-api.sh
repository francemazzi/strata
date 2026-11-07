#!/usr/bin/env bash
# Prefetch vcpkg downloads that go through the GitHub REST API.
#
# pcre2 10.49#2 with the "jit" feature downloads the deps/sljit gitlink from
# api.github.com without a token. Hosted runners share egress IPs, so the
# anonymous rate limit answers 403 and vcpkg does not retry it ("Not a
# transient network error"): the macOS arm64 build fails while rebuilding the
# dependencies after a baseline bump.
#
# The API returns the same bytes with a token, so this script fetches the
# file authenticated into the vcpkg downloads directory. vcpkg_download_distfile
# uses an existing file when its SHA512 matches and skips the network.
#
# It never fails the build: on any problem it prints a warning and vcpkg
# downloads the file itself, as before.
#
# Usage: vcpkg-prefetch-github-api.sh <vcpkg.json> [downloads-dir]
# Env:   GITHUB_TOKEN (required), VCPKG_DOWNLOADS or VCPKG_ROOT

set -uo pipefail

warn() {
  echo "::warning title=vcpkg prefetch::$*"
  exit 0
}

manifest="${1:-}"
[[ -f "${manifest}" ]] || warn "manifest '${manifest}' not found"
[[ -n "${GITHUB_TOKEN:-}" ]] || warn "GITHUB_TOKEN is not set"
command -v jq >/dev/null 2>&1 || warn "jq is not installed"

downloads="${2:-${VCPKG_DOWNLOADS:-${VCPKG_ROOT:+${VCPKG_ROOT}/downloads}}}"
[[ -n "${downloads}" ]] || warn "set VCPKG_DOWNLOADS or VCPKG_ROOT"

sha512() {
  if command -v sha512sum >/dev/null 2>&1; then
    sha512sum "$1" | cut -d' ' -f1
  else
    shasum -a 512 "$1" | cut -d' ' -f1
  fi
}

# GET a GitHub API path with the token; extra curl arguments follow the path.
gh_api() {
  local api_path="$1"
  shift
  curl -fsSL --retry 3 \
    -H "Authorization: Bearer ${GITHUB_TOKEN}" \
    -H "X-GitHub-Api-Version: 2022-11-28" \
    "$@" "https://api.github.com/${api_path}"
}

baseline=$(jq -er '."vcpkg-configuration"."default-registry".baseline' "${manifest}") \
  || warn "no default-registry baseline in ${manifest}"

# Read the pcre2 version and portfile at that baseline, so the script follows
# future pcre2 bumps without edits.
raw=(-H "Accept: application/vnd.github.raw")
version=$(gh_api "repos/microsoft/vcpkg/contents/versions/baseline.json?ref=${baseline}" "${raw[@]}" \
  | jq -er '.default.pcre2.baseline') \
  || warn "cannot read the pcre2 version at vcpkg ${baseline}"
portfile=$(gh_api "repos/microsoft/vcpkg/contents/ports/pcre2/portfile.cmake?ref=${baseline}" "${raw[@]}") \
  || warn "cannot read the pcre2 portfile at vcpkg ${baseline}"

expected=$(printf '%s\n' "${portfile}" \
  | sed -n 's/.*set(sljit_gitlink_hash SHA512 \([0-9a-f]\{128\}\)).*/\1/p' | head -n 1)
if [[ -z "${expected}" ]]; then
  echo "pcre2 ${version} does not download the sljit gitlink: nothing to prefetch."
  exit 0
fi

# Same URL and file name as ports/pcre2/portfile.cmake.
ref="pcre2-${version}"
target="${downloads}/pcre2-${ref}-sljit-gitlink.json"
if [[ -f "${target}" && "$(sha512 "${target}")" == "${expected}" ]]; then
  echo "Already present: ${target}"
  exit 0
fi

mkdir -p "${downloads}" || warn "cannot create ${downloads}"
partial="${target}.part"
gh_api "repos/PCRE2Project/pcre2/contents/deps/sljit?ref=${ref}" -o "${partial}" \
  || { rm -f "${partial}"; warn "cannot download the sljit gitlink of ${ref}"; }

actual=$(sha512 "${partial}")
if [[ "${actual}" != "${expected}" ]]; then
  rm -f "${partial}"
  warn "sljit gitlink of ${ref} has SHA512 ${actual}, the port expects ${expected}"
fi

mv -f "${partial}" "${target}"
echo "Prefetched ${target}"
