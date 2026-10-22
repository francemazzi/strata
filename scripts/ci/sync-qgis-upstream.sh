#!/usr/bin/env bash
#
# Nightly sync of QGIS upstream into Strata.
#
# 1. Checks that the base branch still contains the upstream commit recorded by the last sync
#    merge (a squash or rebase merge of a sync PR drops it); opens an issue and stops if not.
# 2. Predicts the merge with `git merge-tree --write-tree`, without touching the working tree.
# 3. When the upstream tip conflicts, bisects the upstream first-parent history for the last
#    commit that still merges cleanly, opens a sync PR up to that commit and reports the
#    conflicts in an issue: for each file, the upstream commit that introduced the conflict
#    and the Strata commits that changed the same lines.
# Conflicts are never resolved automatically (no `-X ours` / `-X theirs`).
#
# Usage: sync-qgis-upstream.sh            (fetch, predict, merge, push, PR/issue)
#        sync-qgis-upstream.sh --guards-only   (only run the Strata safety guards on the working tree)
# DRY_RUN=true skips push, PR and issue updates and prints their bodies instead.
# Compatible with the bash 3.2 shipped with macOS: no mapfile, no associative arrays.

set -Eeuo pipefail

UPSTREAM_URL="${UPSTREAM_URL:-https://github.com/qgis/QGIS.git}"
UPSTREAM_BRANCH="${UPSTREAM_BRANCH:-master}"
UPSTREAM_WEB_URL="${UPSTREAM_WEB_URL:-https://github.com/qgis/QGIS}"
SYNC_BRANCH="${SYNC_BRANCH:-automation/qgis-upstream-master}"
BASE_BRANCH="${BASE_BRANCH:-master}"
CONFLICT_ISSUE_TITLE="${CONFLICT_ISSUE_TITLE:-QGIS upstream sync conflicts}"
ANCESTRY_ISSUE_TITLE="${ANCESTRY_ISSUE_TITLE:-QGIS upstream sync: upstream history missing from master}"
DRY_RUN="${DRY_RUN:-false}"

REMOTE_UPSTREAM_REF="refs/remotes/qgis-upstream/${UPSTREAM_BRANCH}"

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
# shellcheck source=scripts/ci/strata-upstream-lib.sh
source "${SCRIPT_DIR}/strata-upstream-lib.sh"

# git messages end up in issues: keep them in English whatever the runner locale.
export LC_ALL=C

WORK_DIR=""
REPORT_FILE=""
ISSUE_REF=""
BASE_SHA=""
CANDIDATES_FILE=""
CANDIDATE_COUNT=0
PREDICT_FILES=""
PREDICT_TREE=""
LAST_CLEAN_INDEX=0
FIRST_CONFLICT_INDEX=0

log()
{
  printf '==> %s\n' "$*"
}

abort_merge_if_active()
{
  if git rev-parse -q --verify MERGE_HEAD >/dev/null 2>&1; then
    git merge --abort || true
  fi
}

cleanup()
{
  local status=$?
  if [[ ${status} -ne 0 ]]; then
    abort_merge_if_active
  fi
  if [[ -n "${WORK_DIR}" && -d "${WORK_DIR}" ]]; then
    rm -rf "${WORK_DIR}"
  fi
}
trap cleanup EXIT

require_clean_tree()
{
  if [[ -n "$( git status --porcelain )" ]]; then
    git status --short
    echo "Working tree must be clean before syncing upstream." >&2
    exit 1
  fi
}

require_gh()
{
  if [[ "${DRY_RUN}" == "true" ]]; then
    return 0
  fi

  if ! command -v gh >/dev/null 2>&1; then
    echo "GitHub CLI 'gh' is required for PR/issue management." >&2
    exit 1
  fi
}

github_issue_number()
{
  local title="$1"
  gh issue list \
    --state open \
    --limit 100 \
    --json number,title \
    --jq ".[] | select(.title == \"${title}\") | .number" \
    | sed -n '1p'
}

# Creates the issue, or replaces the body of the open issue with the same title.
# Sets ISSUE_REF (e.g. "#47") for the PR body.
create_or_update_issue()
{
  local title="$1"
  local body_file="$2"

  if [[ "${DRY_RUN}" == "true" ]]; then
    log "Dry run: would create or update issue '${title}' with:"
    cat "${body_file}"
    ISSUE_REF="the '${title}' issue"
    return 0
  fi

  local issue_number
  issue_number="$( github_issue_number "${title}" || true )"

  if [[ -n "${issue_number}" ]]; then
    log "Updating existing issue #${issue_number}: ${title}."
    gh issue edit "${issue_number}" --body-file "${body_file}"
  else
    log "Creating issue: ${title}."
    local issue_url
    issue_url="$( gh issue create --title "${title}" --body-file "${body_file}" )"
    issue_number="${issue_url##*/}"
  fi
  ISSUE_REF="#${issue_number}"
}

close_issue_if_present()
{
  local title="$1"
  local comment="$2"

  if [[ "${DRY_RUN}" == "true" ]]; then
    return 0
  fi

  local issue_number
  issue_number="$( github_issue_number "${title}" || true )"
  if [[ -n "${issue_number}" ]]; then
    log "Closing resolved issue #${issue_number}: ${title}."
    gh issue close "${issue_number}" --comment "${comment}"
  fi
}

collect_conflicted_files()
{
  git diff --name-only --diff-filter=U
}

upstream_commit_link()
{
  local sha="$1"
  local line
  local full
  local short
  local date
  local subject
  line="$( git log -1 --date=short --format='%H%x09%h%x09%ad%x09%s' "${sha}" )"
  IFS=$'\t' read -r full short date subject <<< "${line}"
  printf '[%s](%s/commit/%s) (%s) `%s`' "${short}" "${UPSTREAM_WEB_URL}" "${full}" "${date}" "$( md_code "${subject}" )"
}

# ---------------------------------------------------------------------------
# Upstream ancestry
# ---------------------------------------------------------------------------

# Every sync merge records "Upstream SHA: <sha>". If the newest one on the base branch names a
# commit the base branch does not contain, a sync PR was squashed or rebased.
check_upstream_ancestry()
{
  local base_ref="$1"
  local sync_commit
  local recorded_sha

  sync_commit="$(
    git log -1 --format=%H -E --all-match \
      --grep='^chore\(sync\): merge qgis/QGIS' \
      --grep='^Upstream SHA: [0-9a-f]{40}$' \
      "${base_ref}"
  )"
  if [[ -z "${sync_commit}" ]]; then
    log "No previous sync merge found on ${BASE_BRANCH}: skipping the ancestry check."
    return 0
  fi

  recorded_sha="$( git log -1 --format=%B "${sync_commit}" | sed -n -E 's/^Upstream SHA: ([0-9a-f]{40})$/\1/p' | tail -n 1 )"
  if git merge-base --is-ancestor "${recorded_sha}" "${base_ref}" 2>/dev/null; then
    log "Ancestry check passed: ${BASE_BRANCH} contains upstream ${recorded_sha} (recorded by ${sync_commit})."
    return 0
  fi

  REPORT_FILE="${WORK_DIR}/ancestry-report.md"
  {
    echo "# QGIS upstream history missing from \`${BASE_BRANCH}\`"
    echo
    echo "\`${BASE_BRANCH}\` (\`$( git rev-parse "${base_ref}" )\`) contains the sync commit ${sync_commit}, which records:"
    echo
    echo '```'
    echo "Upstream SHA: ${recorded_sha}"
    echo '```'
    echo
    echo "but that upstream commit is not an ancestor of \`${BASE_BRANCH}\`."
    echo "This happens when a sync PR is merged with **Squash and merge** or **Rebase and merge**:"
    echo "the content arrives but the upstream history does not, so the next sync would replay every"
    echo "upstream change since the previous real merge and conflict on every file Strata modifies."
    echo
    echo "The nightly sync stays stopped until a person restores the ancestry, for example with a"
    echo "merge commit that has \`${recorded_sha}\` as a parent, after checking that \`${BASE_BRANCH}\`"
    echo "really contains that upstream state. Merge it with **Create a merge commit**."
    echo "This issue closes itself at the first sync run that finds the ancestry restored."
  } > "${REPORT_FILE}"
  return 1
}

# ---------------------------------------------------------------------------
# Merge prediction and bisection over the upstream first-parent history
# ---------------------------------------------------------------------------

candidate_at()
{
  sed -n "${1}p" "${CANDIDATES_FILE}"
}

# Predicts BASE_SHA + candidate <idx> and caches the result in WORK_DIR, so that the
# bisections below never run the same merge twice. Sets PREDICT_FILES and PREDICT_TREE.
predict_candidate()
{
  local idx="$1"
  local prefix="${WORK_DIR}/predict-${idx}"

  if [[ ! -f "${prefix}.status" ]]; then
    local status=0
    predict_merge "${BASE_SHA}" "$( candidate_at "${idx}" )" "${prefix}.files" "${prefix}.tree" || status=$?
    echo "${status}" > "${prefix}.status"
  fi

  PREDICT_FILES="${prefix}.files"
  PREDICT_TREE="$( cat "${prefix}.tree" )"
  return "$( cat "${prefix}.status" )"
}

# Bisects for the last candidate that merges cleanly. Assumes that a conflict, once it appears,
# stays (true unless upstream reverts the change): the commit found may then not be the very last
# clean one, but it always merges cleanly because it was tested directly.
# Requires the last candidate (the upstream tip) to conflict.
find_last_clean_candidate()
{
  local lo=0
  local hi="${CANDIDATE_COUNT}"
  local mid

  while (( hi - lo > 1 )); do
    mid=$(( (lo + hi) / 2 ))
    if predict_candidate "${mid}"; then
      lo="${mid}"
    else
      hi="${mid}"
    fi
  done

  LAST_CLEAN_INDEX="${lo}"
  FIRST_CONFLICT_INDEX="${hi}"
}

# Prints the index of the first candidate whose merge conflicts on <path>.
first_conflict_index_for_path()
{
  local path="$1"
  local lo=0
  local hi="${CANDIDATE_COUNT}"
  local mid

  while (( hi - lo > 1 )); do
    mid=$(( (lo + hi) / 2 ))
    predict_candidate "${mid}" || true
    if grep -Fxq -- "${path}" "${PREDICT_FILES}"; then
      hi="${mid}"
    else
      lo="${mid}"
    fi
  done

  echo "${hi}"
}

# Prints "<start> <end>" line ranges, in <ours>, of the Strata hunks of <path> that touch the same
# merge-base lines as an upstream hunk (overlapping or adjacent, as git merge sees them), or
# "deleted" for such hunks that only remove lines.
strata_zone_ranges()
{
  local mb="$1"
  local ours="$2"
  local theirs="$3"
  local path="$4"
  local hunk_re='^@@ -([0-9]+)(,([0-9]+))? \+([0-9]+)(,([0-9]+))? @@.*'

  {
    git diff -U0 --no-renames --no-color --no-ext-diff "${mb}" "${theirs}" -- "${path}" \
      | sed -n -E "s/${hunk_re}/U:\\1:\\3:\\4:\\6/p"
    git diff -U0 --no-renames --no-color --no-ext-diff "${mb}" "${ours}" -- "${path}" \
      | sed -n -E "s/${hunk_re}/S:\\1:\\3:\\4:\\6/p"
  } | awk -F: '
    function count(value) { return value == "" ? 1 : value + 0 }
    function last(start, n) { return n > 0 ? start + n - 1 : start }
    $1 == "U" { u++; us[u] = $2; ue[u] = last($2, count($3)); next }
    $1 == "S" {
      start = $2; end = last($2, count($3))
      for (i = 1; i <= u; i++) {
        if (start <= ue[i] + 1 && us[i] <= end + 1) {
          if (count($5) > 0) { print $4, last($4, count($5)) } else { print "deleted" }
          break
        }
      }
    }'
}

print_commit_items()
{
  local sha
  local date
  local author
  local subject

  while IFS=$'\t' read -r sha date author subject; do
    printf '  - %s (%s, %s) `%s`\n' "${sha}" "${date}" "$( md_code "${author}" )" "$( md_code "${subject}" )"
  done
}

# Prints a Markdown list of the Strata commits that last changed the conflicting lines of <path>
# (git blame limited to commits not in upstream). When the lines cannot be mapped (pure deletions,
# renames, binary files), lists the latest Strata commits on the file instead, and says so.
strata_commits_for_path()
{
  local mb="$1"
  local ours="$2"
  local theirs="$3"
  local path="$4"
  local shas_file="${WORK_DIR}/strata-shas"
  local ranges
  local start
  local end
  local -a shas=()
  local sha
  local latest

  : > "${shas_file}"
  ranges="$( strata_zone_ranges "${mb}" "${ours}" "${theirs}" "${path}" )"

  while read -r start end; do
    [[ -z "${start}" || "${start}" == "deleted" ]] && continue
    # Upstream lines show up as ^boundary commits and are dropped.
    git blame -l -s -L "${start},${end}" "${theirs}..${ours}" -- "${path}" 2>/dev/null \
      | awk '{ print $1 }' | grep -v '^\^' >> "${shas_file}" || true
  done <<< "${ranges}"

  while IFS= read -r sha; do
    shas+=( "${sha}" )
  done < <( sort -u "${shas_file}" )

  if [[ ${#shas[@]} -gt 0 ]]; then
    echo "- Strata commits that last changed the same lines:"
    git log --no-walk --date=short --format='%h%x09%ad%x09%an%x09%s' "${shas[@]}" | print_commit_items
    return 0
  fi

  latest="$( git log --no-merges -n 5 --date=short --format='%h%x09%ad%x09%an%x09%s' "${theirs}..${ours}" -- "${path}" )"
  if [[ -n "${latest}" ]]; then
    echo "- Latest Strata commits on this file (the conflicting lines could not be mapped):"
    printf '%s\n' "${latest}" | print_commit_items
  else
    echo "- No Strata commit on this path: the conflict comes from a rename or a move, see the git merge-tree messages below."
  fi
}

describe_upstream_culprit()
{
  local sha="$1"
  local path="$2"
  local short
  local subject

  echo "- Introduced upstream by $( upstream_commit_link "${sha}" )"
  if git rev-parse -q --verify "${sha}^2" >/dev/null; then
    git log --no-merges -n 5 --format='%h%x09%s' "${sha}^1..${sha}" -- "${path}" \
      | while IFS=$'\t' read -r short subject; do
          printf '  - merged branch commit `%s` `%s`\n' "${short}" "$( md_code "${subject}" )"
        done
  fi
}

write_conflict_report()
{
  local upstream_sha="$1"
  local tip_files="$2"
  local tip_tree="$3"
  local mb
  local regions
  local file_count
  local path
  local idx

  mb="$( git merge-base "${BASE_SHA}" "${upstream_sha}" )"
  regions="$( count_conflict_regions "${tip_tree}" "${tip_files}" )"
  file_count="$( grep -c . "${tip_files}" || true )"

  REPORT_FILE="${WORK_DIR}/conflict-report.md"
  {
    echo "# QGIS upstream sync conflicts"
    echo
    echo "Merging QGIS upstream into Strata conflicts. The nightly sync still merges upstream up to the"
    echo "last commit that merges cleanly, so only the conflicting part waits for a person."
    echo
    echo "- Base branch: \`${BASE_BRANCH}\` at \`${BASE_SHA}\`"
    echo "- Upstream: \`${UPSTREAM_URL} ${UPSTREAM_BRANCH}\` at \`${upstream_sha}\`"
    echo "- Upstream first-parent commits not yet in Strata: ${CANDIDATE_COUNT}"
    if (( LAST_CLEAN_INDEX > 0 )); then
      echo "- Partial sync: \`${SYNC_BRANCH}\` merges ${LAST_CLEAN_INDEX} of them, up to $( upstream_commit_link "$( candidate_at "${LAST_CLEAN_INDEX}" )" )"
    else
      echo "- Partial sync: none, the first upstream commit already conflicts"
    fi
    echo "- First conflicting upstream commit: $( upstream_commit_link "$( candidate_at "${FIRST_CONFLICT_INDEX}" )" )"
    echo
    echo "## Conflicting files at the upstream tip"
    echo
    echo "Conflicting files: ${file_count}. Conflict regions: ${regions}. For each file: the first upstream"
    echo "first-parent commit whose merge conflicts on it, and the Strata commits that last changed the same lines."
    echo
    while IFS= read -r path; do
      [[ -z "${path}" ]] && continue
      idx="$( first_conflict_index_for_path "${path}" )"
      echo "### \`${path}\`"
      echo
      describe_upstream_culprit "$( candidate_at "${idx}" )" "${path}"
      strata_commits_for_path "${mb}" "${BASE_SHA}" "${upstream_sha}" "${path}"
      echo
    done < "${tip_files}"
    echo "## git merge-tree messages"
    echo
    echo '```'
    merge_messages "${BASE_SHA}" "${upstream_sha}" | grep -v '^Auto-merging ' | sed -n '1,100p' || true
    echo '```'
    echo
    write_resolution_policy
  } > "${REPORT_FILE}"
}

write_resolution_policy()
{
  echo "## Resolution policy"
  echo
  echo "- Keep Strata release triggers, artifact names, signing/upload behavior, and Strata-specific scripts."
  echo "- Preserve \`ENABLE_AI_ASSISTANT\`, all AI sources/includes/tests, and ONNX/SentencePiece wiring."
  echo "- Accept upstream QGIS changes where compatible, but do not use automatic \`ours\` or \`theirs\` conflict resolution."
  echo "- Resolve conflicts manually in a branch (\`git merge --no-ff <upstream sha>\`), run CI, merge that PR with"
  echo "  **Create a merge commit**, and let this sync action run again."
}

write_blocked_report()
{
  local reason="$1"
  local details="$2"
  local upstream_sha="$3"

  REPORT_FILE="${WORK_DIR}/blocked-report.md"
  {
    echo "# QGIS upstream sync blocked"
    echo
    echo "The nightly Strata sync could not safely merge QGIS upstream."
    echo
    echo "- Base branch: \`${BASE_BRANCH}\`"
    echo "- Sync branch: \`${SYNC_BRANCH}\`"
    echo "- Upstream: \`${UPSTREAM_URL} ${UPSTREAM_BRANCH}\`"
    echo "- Base SHA: \`${BASE_SHA}\`"
    echo "- Upstream SHA: \`${upstream_sha}\`"
    echo "- Reason: ${reason}"
    echo
    echo "## Details"
    echo
    if [[ -n "${details}" ]]; then
      echo '```'
      echo "${details}"
      echo '```'
    else
      echo "No additional details were reported."
    fi
    echo
    write_resolution_policy
  } > "${REPORT_FILE}"
}

# ---------------------------------------------------------------------------
# Safety guards
# ---------------------------------------------------------------------------

declare -a GUARD_FAILURES=()

guard_fail()
{
  GUARD_FAILURES+=( "$*" )
}

assert_path_exists()
{
  local path="$1"
  if [[ ! -e "${path}" ]]; then
    guard_fail "Required Strata path is missing: ${path}"
  fi
}

assert_glob_has_matches()
{
  local pattern="$1"
  local -a matches=()
  local match
  # A while-read loop instead of mapfile keeps the guards runnable on the bash 3.2 shipped with macOS.
  while IFS= read -r match; do
    matches+=( "${match}" )
  done < <( compgen -G "${pattern}" || true )

  if [[ ${#matches[@]} -eq 0 ]]; then
    guard_fail "Required Strata file pattern has no matches: ${pattern}"
  fi
}

assert_git_grep()
{
  local pattern="$1"
  shift

  if ! git grep -n -- "${pattern}" -- "$@" >/dev/null 2>&1; then
    guard_fail "Required pattern '${pattern}' was not found in: $*"
  fi
}

assert_file_contains()
{
  local file="$1"
  local pattern="$2"

  if [[ ! -f "${file}" ]]; then
    guard_fail "Required file is missing: ${file}"
    return
  fi

  if ! grep -Eq "${pattern}" "${file}"; then
    guard_fail "Required pattern '${pattern}' was not found in ${file}"
  fi
}

assert_file_lacks()
{
  local file="$1"
  local pattern="$2"

  if [[ ! -f "${file}" ]]; then
    guard_fail "Required file is missing: ${file}"
    return
  fi

  if grep -Eq "${pattern}" "${file}"; then
    guard_fail "Forbidden regression pattern '${pattern}' was found in ${file}"
  fi
}

run_safety_guards()
{
  GUARD_FAILURES=()

  local conflict_markers
  conflict_markers="$( git grep -n -E '^(<{7}|>{7})([[:space:]].*)?$' -- . 2>/dev/null || true )"
  if [[ -n "${conflict_markers}" ]]; then
    guard_fail "Conflict markers remain in tracked files:
${conflict_markers}"
  fi

  assert_path_exists "src/app/ai"
  assert_glob_has_matches "src/app/ai/*.h"
  assert_glob_has_matches "src/app/ai/*.cpp"
  assert_glob_has_matches "tests/src/app/testqgsai*"
  assert_path_exists "resources/themes/Strata Auto"
  assert_path_exists "resources/themes/Strata Auto/style.qss"
  assert_path_exists ".github/workflows/release-strata.yml"
  assert_path_exists ".github/workflows/build-macos-qt6.yml"
  assert_path_exists ".github/workflows/windows-qt6.yml"
  assert_path_exists ".github/workflows/package-macos-release.yml"
  assert_path_exists "scripts/build-strata-app.sh"
  assert_path_exists "scripts/run-strata-dev.sh"
  assert_path_exists "scripts/patch-macos-bundle.sh"

  assert_git_grep "ENABLE_AI_ASSISTANT" CMakeLists.txt src/app/CMakeLists.txt tests/src/app/CMakeLists.txt
  assert_git_grep "HAVE_AI_ASSISTANT" src/app/CMakeLists.txt src/app/qgisapp.cpp src/app/qgisapp.h
  assert_git_grep "onnxruntime" CMakeLists.txt src/app/CMakeLists.txt vcpkg/vcpkg.json
  assert_git_grep "sentencepiece" CMakeLists.txt src/app/CMakeLists.txt vcpkg/vcpkg.json
  assert_git_grep "HAVE_AI_E5_EMBEDDINGS" src/app/CMakeLists.txt src/app/ai/index/qgsaiembeddingprovider.cpp
  assert_file_lacks "src/app/CMakeLists.txt" "onnxruntime[^\\n]*REQUIRED|sentencepiece[^\\n]*REQUIRED|onnxruntime_cxx_api\\.h[^\\n]*REQUIRED|sentencepiece_processor\\.h[^\\n]*REQUIRED"

  assert_file_contains ".github/workflows/build-macos-qt6.yml" "strata-app-"
  assert_file_contains ".github/workflows/build-macos-qt6.yml" "strata-sdk-"
  assert_file_contains ".github/workflows/windows-qt6.yml" "strata-windows-qt6"
  assert_file_contains ".github/workflows/windows-qt6.yml" "strata-sdk-x64-windows"
  assert_file_contains ".github/workflows/release-strata.yml" "strata-v"

  assert_file_lacks ".github/workflows/build-macos-qt6.yml" "qgis-app-|name:[[:space:]]*qgis-sdk"
  assert_file_lacks ".github/workflows/windows-qt6.yml" "name:[[:space:]]*qgis-windows-qt6|name:[[:space:]]*qgis-sdk"
  assert_file_lacks ".github/workflows/package-macos-release.yml" "qgis-app-"

  if [[ ${#GUARD_FAILURES[@]} -ne 0 ]]; then
    printf '%s\n' "${GUARD_FAILURES[@]}"
    return 1
  fi
}

# ---------------------------------------------------------------------------
# Sync branch and PR
# ---------------------------------------------------------------------------

# True when the remote sync branch already is the merge of <target> into the current base.
sync_branch_is_current()
{
  local target="$1"
  local remote_ref="refs/remotes/origin/${SYNC_BRANCH}"
  local remote_sha

  git rev-parse -q --verify "${remote_ref}" >/dev/null || return 1
  remote_sha="$( git ls-remote origin "refs/heads/${SYNC_BRANCH}" | cut -f1 )"
  [[ "${remote_sha}" == "$( git rev-parse "${remote_ref}" )" ]] || return 1
  [[ "$( git rev-parse -q --verify "${remote_ref}^1" || true )" == "${BASE_SHA}" ]] || return 1
  [[ "$( git rev-parse -q --verify "${remote_ref}^2" || true )" == "${target}" ]] || return 1
}

write_pr_body()
{
  local upstream_sha="$1"
  local target_sha="$2"
  local partial="$3"
  local body_file="$4"

  {
    echo "Automated nightly sync from \`qgis/QGIS:${UPSTREAM_BRANCH}\` into Strata."
    echo
    echo "- Base branch: \`${BASE_BRANCH}\`"
    echo "- Base SHA: \`${BASE_SHA}\`"
    echo "- Upstream SHA: \`${target_sha}\`"
    echo "- Sync branch: \`${SYNC_BRANCH}\`"
    echo
    if [[ "${partial}" == "true" ]]; then
      echo "## Partial sync"
      echo
      echo "The upstream tip \`${upstream_sha}\` conflicts with Strata. This PR merges the first"
      echo "${LAST_CLEAN_INDEX} of ${CANDIDATE_COUNT} upstream first-parent commits, up to"
      echo "$( upstream_commit_link "${target_sha}" ), the last one that merges cleanly."
      echo "The next one, $( upstream_commit_link "$( candidate_at "${FIRST_CONFLICT_INDEX}" )" ), conflicts:"
      echo "see ${ISSUE_REF} for the files, the upstream commits and the Strata commits involved."
      echo
    fi
    echo "Safety guards passed before this PR was created:"
    echo
    echo "- No conflict markers in tracked files"
    echo "- Strata AI sources, tests, theme, release scripts, and release workflows are still present"
    echo "- \`ENABLE_AI_ASSISTANT\`, \`onnxruntime\`, and \`sentencepiece\` wiring still exists"
    echo "- Strata release artifact names did not regress to QGIS artifact names"
    echo
    echo "This PR still requires normal CI and human review before merge."
    echo "Merge it with **Create a merge commit** only: a squash or rebase merge drops the upstream"
    echo "history, and the next sync would replay every upstream change since the previous merge."
  } > "${body_file}"
}

find_sync_pr_number()
{
  local head_selector="${SYNC_BRANCH}"
  local pr_number=""

  if [[ -n "${GITHUB_REPOSITORY_OWNER:-}" ]]; then
    head_selector="${GITHUB_REPOSITORY_OWNER}:${SYNC_BRANCH}"
  fi

  pr_number="$(
    gh pr list \
      --state open \
      --limit 100 \
      --base "${BASE_BRANCH}" \
      --head "${head_selector}" \
      --json number \
      --jq '.[0].number' 2>/dev/null || true
  )"

  if [[ -z "${pr_number}" && "${head_selector}" != "${SYNC_BRANCH}" ]]; then
    pr_number="$(
      gh pr list \
        --state open \
        --limit 100 \
        --base "${BASE_BRANCH}" \
        --head "${SYNC_BRANCH}" \
        --json number \
        --jq '.[0].number' 2>/dev/null || true
    )"
  fi

  echo "${pr_number}"
}

create_or_update_pr()
{
  local upstream_sha="$1"
  local target_sha="$2"
  local partial="$3"
  local title="chore(sync): merge qgis/QGIS ${UPSTREAM_BRANCH}"
  local body_file="${WORK_DIR}/pr-body.md"

  if [[ "${partial}" == "true" ]]; then
    title="${title} up to ${target_sha:0:11} (partial)"
  fi
  write_pr_body "${upstream_sha}" "${target_sha}" "${partial}" "${body_file}"

  if [[ "${DRY_RUN}" == "true" ]]; then
    log "Dry run: would create or update PR '${title}' from ${SYNC_BRANCH} to ${BASE_BRANCH} with:"
    cat "${body_file}"
    return 0
  fi

  local pr_number
  pr_number="$( find_sync_pr_number )"

  if [[ -n "${pr_number}" ]]; then
    log "Updating existing upstream sync PR #${pr_number}."
    gh pr edit "${pr_number}" --title "${title}" --body-file "${body_file}"
  else
    log "Creating upstream sync PR."
    gh pr create \
      --base "${BASE_BRANCH}" \
      --head "${SYNC_BRANCH}" \
      --title "${title}" \
      --body-file "${body_file}"
  fi
}

# Once the base branch contains the upstream tip (e.g. after a manual sync), an open sync PR
# would only add a redundant merge.
close_stale_sync_pr()
{
  if [[ "${DRY_RUN}" == "true" ]]; then
    log "Dry run: would close any open sync PR from ${SYNC_BRANCH}."
    return 0
  fi

  local pr_number
  pr_number="$( find_sync_pr_number )"
  if [[ -n "${pr_number}" ]]; then
    log "Closing upstream sync PR #${pr_number}: ${BASE_BRANCH} already contains the upstream tip."
    gh pr close "${pr_number}" --comment "\`${BASE_BRANCH}\` already contains the QGIS upstream tip: nothing left to merge from this PR."
  fi
}

guards_only()
{
  # Checks the current working tree (e.g. a manually resolved merge) without fetching or merging.
  # The EXIT trap is dropped first: it would otherwise abort the merge being resolved when a guard fails.
  trap - EXIT
  if ! run_safety_guards; then
    echo "Strata safety guards failed." >&2
    exit 1
  fi
  log "Strata safety guards passed."
}

main()
{
  if [[ "${1:-}" == "--guards-only" ]]; then
    guards_only
    return 0
  fi

  require_clean_tree
  require_gh
  WORK_DIR="$( mktemp -d )"

  log "Fetching base branch ${BASE_BRANCH} from origin."
  git fetch --prune origin "+refs/heads/${BASE_BRANCH}:refs/remotes/origin/${BASE_BRANCH}"
  git fetch --prune origin "+refs/heads/${SYNC_BRANCH}:refs/remotes/origin/${SYNC_BRANCH}" >/dev/null 2>&1 || true

  log "Fetching QGIS upstream ${UPSTREAM_BRANCH}."
  git fetch --no-tags "${UPSTREAM_URL}" "+refs/heads/${UPSTREAM_BRANCH}:${REMOTE_UPSTREAM_REF}"

  local base_ref="refs/remotes/origin/${BASE_BRANCH}"
  local upstream_sha
  BASE_SHA="$( git rev-parse "${base_ref}" )"
  upstream_sha="$( git rev-parse "${REMOTE_UPSTREAM_REF}" )"

  if ! check_upstream_ancestry "${base_ref}"; then
    create_or_update_issue "${ANCESTRY_ISSUE_TITLE}" "${REPORT_FILE}"
    echo "Upstream ancestry check failed: the last sync merge is not part of ${BASE_BRANCH} history." >&2
    exit 1
  fi
  close_issue_if_present "${ANCESTRY_ISSUE_TITLE}" "\`${BASE_BRANCH}\` contains the upstream commit recorded by the last sync merge again."

  if git merge-base --is-ancestor "${upstream_sha}" "${BASE_SHA}"; then
    log "No upstream changes to sync: ${BASE_BRANCH} already contains ${upstream_sha}."
    close_issue_if_present "${CONFLICT_ISSUE_TITLE}" "\`${BASE_BRANCH}\` already contains the QGIS upstream tip \`${upstream_sha}\`."
    close_stale_sync_pr
    return 0
  fi

  CANDIDATES_FILE="${WORK_DIR}/candidates"
  git rev-list --first-parent --reverse "${upstream_sha}" "^${BASE_SHA}" > "${CANDIDATES_FILE}"
  CANDIDATE_COUNT="$( wc -l < "${CANDIDATES_FILE}" | tr -d ' ' )"
  log "${CANDIDATE_COUNT} upstream first-parent commits are not in ${BASE_BRANCH} yet."

  local target_sha="${upstream_sha}"
  local partial=false

  log "Predicting the merge of ${upstream_sha} with git merge-tree."
  if predict_candidate "${CANDIDATE_COUNT}"; then
    log "The upstream tip merges cleanly."
  else
    local tip_files="${WORK_DIR}/tip-conflicts"
    local tip_tree="${PREDICT_TREE}"
    cp "${PREDICT_FILES}" "${tip_files}"
    log "The upstream tip conflicts in $( grep -c . "${tip_files}" || true ) files; bisecting the upstream first-parent history."
    find_last_clean_candidate
    write_conflict_report "${upstream_sha}" "${tip_files}" "${tip_tree}"
    create_or_update_issue "${CONFLICT_ISSUE_TITLE}" "${REPORT_FILE}"

    if (( LAST_CLEAN_INDEX == 0 )); then
      echo "The first upstream commit not in ${BASE_BRANCH} already conflicts: nothing to sync until the conflict is resolved." >&2
      exit 1
    fi
    target_sha="$( candidate_at "${LAST_CLEAN_INDEX}" )"
    partial=true
    log "Partial sync up to ${target_sha} (${LAST_CLEAN_INDEX} of ${CANDIDATE_COUNT} first-parent commits)."
  fi

  if sync_branch_is_current "${target_sha}"; then
    log "${SYNC_BRANCH} already merges ${target_sha} into ${BASE_SHA}: nothing to push."
  else
    git checkout -B "${SYNC_BRANCH}" "${base_ref}"

    log "Merging ${target_sha} into ${SYNC_BRANCH} without automatic conflict strategy."
    if ! git merge --no-ff --no-commit "${target_sha}"; then
      local conflicted_files
      conflicted_files="$( collect_conflicted_files )"
      abort_merge_if_active
      write_blocked_report "merge conflicts that git merge-tree did not predict" "${conflicted_files}" "${target_sha}"
      create_or_update_issue "${CONFLICT_ISSUE_TITLE}" "${REPORT_FILE}"
      exit 1
    fi

    if git diff --quiet && git diff --cached --quiet; then
      abort_merge_if_active
      log "No upstream changes to sync."
      close_issue_if_present "${CONFLICT_ISSUE_TITLE}" "The latest upstream sync found no changes to merge."
      return 0
    fi

    if ! run_safety_guards; then
      local guard_details
      guard_details="$( printf '%s\n' "${GUARD_FAILURES[@]}" )"
      abort_merge_if_active
      write_blocked_report "safety guard failure" "${guard_details}" "${target_sha}"
      create_or_update_issue "${CONFLICT_ISSUE_TITLE}" "${REPORT_FILE}"
      exit 1
    fi

    local -a commit_args=(
      -m "chore(sync): merge qgis/QGIS ${UPSTREAM_BRANCH}"
      -m "Upstream: ${UPSTREAM_URL}"
      -m "Upstream SHA: ${target_sha}"
    )
    if [[ "${partial}" == "true" ]]; then
      commit_args+=( -m "Partial sync: upstream ${upstream_sha} conflicts from $( candidate_at "${FIRST_CONFLICT_INDEX}" ) on." )
    fi
    git add -A
    git commit "${commit_args[@]}"

    log "Pushing sync branch ${SYNC_BRANCH}."
    if [[ "${DRY_RUN}" == "true" ]]; then
      log "Dry run: skipping push."
    else
      git push --force-with-lease origin "${SYNC_BRANCH}:${SYNC_BRANCH}"
    fi
  fi

  create_or_update_pr "${upstream_sha}" "${target_sha}" "${partial}"
  if [[ "${partial}" == "false" ]]; then
    close_issue_if_present "${CONFLICT_ISSUE_TITLE}" "The latest upstream sync produced a clean PR from \`${SYNC_BRANCH}\`."
  fi
}

# Same line on purpose: bash reads scripts while running them, and `git checkout` above may
# replace this file on disk.
main "$@"; exit $?
