#!/usr/bin/env bash
#
# Upstream forecast for a Strata pull request, run by .github/workflows/strata-upstream-forecast.yml.
#
# 1. Conflicts: predicts with `git merge-tree` the merge of QGIS upstream into the base branch and
#    into the PR result, and reports when the PR adds conflicts for the next upstream sync.
# 2. Footprint: fails when the PR modifies a file that exists upstream and is not listed in the
#    allowlist, unless the PR carries the override label. The allowlist is read from the base
#    branch, so a PR cannot approve itself by adding its own files to it.
#
# A file counts as modified upstream when the PR result differs from the upstream commit it is
# based on: upstream changes brought in by a sync PR do not count, Strata changes do.
#
# Usage: strata-upstream-forecast.sh                       check HEAD (the PR merge commit)
#        strata-upstream-forecast.sh --generate-allowlist  rewrite the allowlist from HEAD
#
# Environment:
#   HEAD_SHA                PR result; default HEAD (the PR merge commit checked out by actions/checkout)
#   BASE_SHA                base branch; default HEAD^1 when HEAD is a merge, else origin/master
#   UPSTREAM_REF            default refs/remotes/qgis-upstream/master
#   ALLOWLIST_FILE          default .strata/upstream-allowlist.txt
#   OVERRIDE_LABEL          default upstream-touch
#   OVERRIDE_LABEL_PRESENT  true when the PR carries the override label
#   REPORT_FILE             Markdown report; printed to stdout when unset
#   GITHUB_OUTPUT           when set, receives adds_conflicts, violations and comment
# Compatible with the bash 3.2 shipped with macOS.

set -Eeuo pipefail

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
# shellcheck source=scripts/ci/strata-upstream-lib.sh
source "${SCRIPT_DIR}/strata-upstream-lib.sh"

export LC_ALL=C

UPSTREAM_REF="${UPSTREAM_REF:-refs/remotes/qgis-upstream/master}"
ALLOWLIST_FILE="${ALLOWLIST_FILE:-.strata/upstream-allowlist.txt}"
OVERRIDE_LABEL="${OVERRIDE_LABEL:-upstream-touch}"
OVERRIDE_LABEL_PRESENT="${OVERRIDE_LABEL_PRESENT:-false}"
REPORT_FILE="${REPORT_FILE:-}"
COMMENT_MARKER="<!-- strata-upstream-forecast -->"

WORK_DIR="$( mktemp -d )"
trap 'rm -rf "${WORK_DIR}"' EXIT

log()
{
  printf '==> %s\n' "$*" >&2
}

set_output()
{
  if [[ -n "${GITHUB_OUTPUT:-}" ]]; then
    echo "$1=$2" >> "${GITHUB_OUTPUT}"
  fi
}

count_lines()
{
  grep -c . "$1" || true
}

# Writes the upstream files modified by <commit> to <out>: paths that exist upstream (at the
# merge base with upstream or at the upstream tip) and differ between that merge base and <commit>.
list_footprint()
{
  local commit="$1"
  local out="$2"
  local mb

  mb="$( git merge-base "${commit}" "${UPSTREAM_REF}" )"
  {
    git ls-tree -r --name-only "${mb}"
    git ls-tree -r --name-only "${UPSTREAM_REF}"
  } | sort -u > "${WORK_DIR}/upstream-files"
  git diff --name-only --no-renames "${mb}" "${commit}" | sort -u > "${WORK_DIR}/changed-since-upstream"
  comm -12 "${WORK_DIR}/changed-since-upstream" "${WORK_DIR}/upstream-files" > "${out}"
}

generate_allowlist()
{
  local head
  head="$( git rev-parse "${HEAD_SHA:-HEAD}" )"
  list_footprint "${head}" "${WORK_DIR}/footprint"

  mkdir -p "$( dirname "${ALLOWLIST_FILE}" )"
  {
    echo "# Upstream QGIS files that Strata modifies on purpose."
    echo "#"
    echo "# .github/workflows/strata-upstream-forecast.yml fails a PR to master that modifies a file"
    echo "# that exists upstream and is not listed here, unless the PR has the '${OVERRIDE_LABEL}' label."
    echo "# Every file listed here can conflict at the next upstream sync: prefer new files in Strata"
    echo "# directories and one-line hooks in upstream files."
    echo "#"
    echo "# A PR that has to touch a new upstream file adds it here, gets the '${OVERRIDE_LABEL}' label"
    echo "# from a reviewer, and is checked against the list of the base branch."
    echo "# Generated from $( git rev-parse --short=11 "${head}" ) with:"
    echo "#   scripts/ci/strata-upstream-forecast.sh --generate-allowlist"
    echo "# One path per line, as printed by git; lines starting with # are ignored."
    echo
    cat "${WORK_DIR}/footprint"
  } > "${ALLOWLIST_FILE}"

  log "Wrote $( count_lines "${WORK_DIR}/footprint" ) upstream files to ${ALLOWLIST_FILE}."
}

# Writes the allowlist entries to <out> and sets ALLOWLIST_SOURCE.
load_allowlist()
{
  local out="$1"
  local source_commit=""

  if git cat-file -e "${BASE_SHA}:${ALLOWLIST_FILE}" 2>/dev/null; then
    source_commit="${BASE_SHA}"
    ALLOWLIST_SOURCE="base branch"
  elif git cat-file -e "${HEAD_SHA}:${ALLOWLIST_FILE}" 2>/dev/null; then
    # Only the PR that introduces the allowlist gets here.
    source_commit="${HEAD_SHA}"
    ALLOWLIST_SOURCE="this PR (the base branch has no allowlist yet)"
  fi

  if [[ -z "${source_commit}" ]]; then
    ALLOWLIST_SOURCE="none: \`${ALLOWLIST_FILE}\` is missing"
    : > "${out}"
    return 0
  fi

  git show "${source_commit}:${ALLOWLIST_FILE}" \
    | sed -e '/^[[:space:]]*#/d' -e 's/[[:space:]]*$//' -e '/^$/d' \
    | sort -u > "${out}"
}

# Prints "<path> <base regions> <PR regions>" for every path whose conflict regions differ.
compare_conflicts()
{
  local base_files="$1"
  local base_tree="$2"
  local head_files="$3"
  local head_tree="$4"
  local single="${WORK_DIR}/single"
  local path
  local base_count
  local head_count

  sort -u "${base_files}" "${head_files}" | while IFS= read -r path; do
    [[ -z "${path}" ]] && continue
    printf '%s\n' "${path}" > "${single}"
    base_count=0
    head_count=0
    if grep -Fxq -- "${path}" "${base_files}"; then
      base_count="$( count_conflict_regions "${base_tree}" "${single}" )"
    fi
    if grep -Fxq -- "${path}" "${head_files}"; then
      head_count="$( count_conflict_regions "${head_tree}" "${single}" )"
    fi
    if [[ "${base_count}" != "${head_count}" ]]; then
      echo "${path} ${base_count} ${head_count}"
    fi
  done
}

check()
{
  HEAD_SHA="$( git rev-parse "${HEAD_SHA:-HEAD}" )"
  if [[ -n "${BASE_SHA:-}" ]]; then
    BASE_SHA="$( git rev-parse "${BASE_SHA}" )"
  elif git rev-parse -q --verify "${HEAD_SHA}^2" >/dev/null; then
    BASE_SHA="$( git rev-parse "${HEAD_SHA}^1" )"
  else
    BASE_SHA="$( git rev-parse refs/remotes/origin/master )"
  fi
  local upstream_sha
  upstream_sha="$( git rev-parse "${UPSTREAM_REF}" )"
  log "Base ${BASE_SHA}, PR result ${HEAD_SHA}, upstream ${upstream_sha}."

  # --- Conflicts with upstream -------------------------------------------------
  local base_files="${WORK_DIR}/base-conflicts"
  local head_files="${WORK_DIR}/head-conflicts"
  local base_tree
  local head_tree
  local base_regions
  local head_regions

  predict_merge "${BASE_SHA}" "${upstream_sha}" "${base_files}" "${WORK_DIR}/base-tree" || true
  predict_merge "${HEAD_SHA}" "${upstream_sha}" "${head_files}" "${WORK_DIR}/head-tree" || true
  base_tree="$( cat "${WORK_DIR}/base-tree" )"
  head_tree="$( cat "${WORK_DIR}/head-tree" )"
  base_regions="$( count_conflict_regions "${base_tree}" "${base_files}" )"
  head_regions="$( count_conflict_regions "${head_tree}" "${head_files}" )"
  compare_conflicts "${base_files}" "${base_tree}" "${head_files}" "${head_tree}" > "${WORK_DIR}/conflict-delta"

  local adds_conflicts=false
  if [[ "${head_regions}" -gt "${base_regions}" ]] || awk '$3 > $2 { found = 1 } END { exit !found }' "${WORK_DIR}/conflict-delta"; then
    adds_conflicts=true
  fi
  log "Conflicts with upstream: base ${base_regions} regions, PR ${head_regions} regions."

  # --- Upstream files modified by the PR ---------------------------------------
  local touched="${WORK_DIR}/touched"
  local footprint="${WORK_DIR}/footprint"
  local candidates="${WORK_DIR}/candidates"
  local allowlist="${WORK_DIR}/allowlist"
  local violations="${WORK_DIR}/violations"

  git diff --name-only --no-renames "${BASE_SHA}" "${HEAD_SHA}" | sort -u > "${touched}"
  list_footprint "${HEAD_SHA}" "${footprint}"
  comm -12 "${touched}" "${footprint}" > "${candidates}"
  ALLOWLIST_SOURCE=""
  load_allowlist "${allowlist}"
  comm -23 "${candidates}" "${allowlist}" > "${violations}"

  local violation_count
  violation_count="$( count_lines "${violations}" )"
  local blocked=false
  if [[ "${violation_count}" -gt 0 && "${OVERRIDE_LABEL_PRESENT}" != "true" ]]; then
    blocked=true
  fi
  log "Upstream files modified by the PR: $( count_lines "${candidates}" ), outside the allowlist: ${violation_count}."

  # --- Report --------------------------------------------------------------------
  local report="${WORK_DIR}/report.md"
  local path
  local base_count
  local head_count
  {
    echo "${COMMENT_MARKER}"
    echo "## Upstream forecast"
    echo
    echo "Prediction of the next QGIS upstream sync (\`qgis/QGIS\` master at \`${upstream_sha:0:11}\`) with this PR merged."
    echo
    echo "| | Conflicting files | Conflict regions |"
    echo "|---|---|---|"
    echo "| base \`${BASE_SHA:0:11}\` | $( count_lines "${base_files}" ) | ${base_regions} |"
    echo "| with this PR | $( count_lines "${head_files}" ) | ${head_regions} |"
    echo
    if [[ "${adds_conflicts}" == "true" ]]; then
      echo "**This PR adds conflicts with QGIS upstream.** The next upstream sync will stop on these"
      echo "files until someone resolves them by hand:"
      echo
      echo "| File | Regions on base | Regions with this PR |"
      echo "|---|---|---|"
      while read -r path base_count head_count; do
        [[ -z "${path}" ]] && continue
        echo "| \`${path}\` | ${base_count} | ${head_count} |"
      done < "${WORK_DIR}/conflict-delta"
      echo
      echo "Where possible, move the change to a Strata file and leave a one-line hook in the upstream file."
    else
      echo "This PR adds no conflicts with QGIS upstream."
    fi
    echo
    echo "### Upstream files modified"
    echo
    echo "Allowlist: \`${ALLOWLIST_FILE}\` from ${ALLOWLIST_SOURCE}."
    echo
    if [[ "${violation_count}" -eq 0 ]]; then
      echo "Upstream files modified by this PR: $( count_lines "${candidates}" ), all in the allowlist."
    else
      echo "Upstream files modified by this PR and not in the allowlist (${violation_count}):"
      echo
      while IFS= read -r path; do
        echo "- \`${path}\`"
      done < "${violations}"
      echo
      if [[ "${blocked}" == "true" ]]; then
        echo "**The check fails.** Each of these files can conflict at every upstream sync. Either keep"
        echo "the change in Strata files, or add the paths to \`${ALLOWLIST_FILE}\` in this PR and ask"
        echo "a reviewer for the \`${OVERRIDE_LABEL}\` label."
      else
        echo "The \`${OVERRIDE_LABEL}\` label is set: the check passes. Add the paths to \`${ALLOWLIST_FILE}\`"
        echo "in this PR so that later PRs touching them pass without the label."
      fi
    fi
  } > "${report}"

  if [[ -n "${REPORT_FILE}" ]]; then
    cp "${report}" "${REPORT_FILE}"
  else
    cat "${report}"
  fi

  local comment=false
  if [[ "${adds_conflicts}" == "true" || "${violation_count}" -gt 0 ]]; then
    comment=true
  fi
  set_output adds_conflicts "${adds_conflicts}"
  set_output violations "${violation_count}"
  set_output comment "${comment}"

  if [[ "${blocked}" == "true" ]]; then
    echo "::error::This PR modifies upstream QGIS files outside ${ALLOWLIST_FILE}; see the report."
    return 1
  fi
}

case "${1:-}" in
  --generate-allowlist)
    generate_allowlist
    ;;
  "")
    check
    ;;
  *)
    echo "Usage: $0 [--generate-allowlist]" >&2
    exit 2
    ;;
esac
