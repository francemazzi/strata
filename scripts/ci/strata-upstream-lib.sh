#!/usr/bin/env bash
# Helpers shared by sync-qgis-upstream.sh and strata-upstream-forecast.sh.
# Sourced, not executed. Kept compatible with the bash 3.2 shipped with macOS:
# no mapfile, no associative arrays.

# Predicts the merge of <theirs> into <ours> without touching the working tree or the index.
# Writes the conflicted paths (one per line, empty when the merge is clean) to <files_out>
# and the resulting tree id to <tree_out>.
# Returns 0 when the merge is clean, 1 when it conflicts; any git error aborts the script.
predict_merge()
{
  local ours="$1"
  local theirs="$2"
  local files_out="$3"
  local tree_out="$4"
  local output
  local status=0

  output="$( git merge-tree --write-tree --name-only --no-messages "${ours}" "${theirs}" )" || status=$?
  if [[ ${status} -gt 1 ]]; then
    echo "git merge-tree failed (exit ${status}) for ${ours} + ${theirs}" >&2
    exit 1
  fi

  # sed reads the whole input: head would close the pipe early and trip pipefail.
  printf '%s\n' "${output}" | sed -n '1p' > "${tree_out}"
  printf '%s\n' "${output}" | tail -n +2 | sed '/^$/d' > "${files_out}"
  return "${status}"
}

# Prints the git merge-tree messages (CONFLICT lines and the like) for <ours> + <theirs>.
merge_messages()
{
  local ours="$1"
  local theirs="$2"
  local status=0
  local output

  output="$( git merge-tree --write-tree --name-only "${ours}" "${theirs}" )" || status=$?
  if [[ ${status} -gt 1 ]]; then
    return 0
  fi
  # Output: tree id, conflicted paths, a blank line, then the informational messages.
  printf '%s\n' "${output}" | awk 'seen { print } /^$/ { seen = 1 }'
}

# Counts conflict regions in a merge-tree result: one per conflict marker block,
# and one for each conflicted path without markers (modify/delete, rename, binary...).
count_conflict_regions()
{
  local tree="$1"
  local files="$2"
  local total=0
  local path
  local markers

  while IFS= read -r path; do
    [[ -z "${path}" ]] && continue
    markers="$( git cat-file -p "${tree}:${path}" 2>/dev/null | grep -c '^<<<<<<< ' || true )"
    if [[ -z "${markers}" || "${markers}" -eq 0 ]]; then
      markers=1
    fi
    total=$(( total + markers ))
  done < "${files}"

  echo "${total}"
}

# Makes a commit subject safe inside a Markdown code span: no backticks, no line breaks.
# Code spans also keep GitHub from turning @names and #numbers into mentions or links.
md_code()
{
  printf '%s' "$*" | tr '`\r\n' "'  "
}
