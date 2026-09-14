#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=scripts/strata-backend-endpoints.sh
source "${ROOT}/scripts/strata-backend-endpoints.sh"

# The launcher mode is authoritative. Do not inherit STRATA_PLAN_ENDPOINT: a shell
# previously used for production must not silently send this desktop session to it.
STRATA_BACKEND_LOCAL="${STRATA_BACKEND_LOCAL%/}"
export STRATA_PLAN_ENDPOINT="${STRATA_BACKEND_LOCAL}/ai/messages"

BACKEND_DIR="${STRATA_BACKEND_DIR:-${ROOT}/../strata-be}"
BACKEND_READY_URL="${STRATA_BACKEND_LOCAL}/health/ready"
BACKEND_LIVE_URL="${STRATA_BACKEND_LOCAL}/health"
BACKEND_START_TIMEOUT="${STRATA_BACKEND_START_TIMEOUT:-30}"
BACKEND_STATE_DIR="${STRATA_BACKEND_STATE_DIR:-${TMPDIR:-/tmp}/strata-be}"
BACKEND_LOG="${STRATA_BACKEND_LOG:-${BACKEND_STATE_DIR}/strata-be-dev.log}"
BACKEND_PID_FILE="${BACKEND_STATE_DIR}/strata-be-dev.pid"

if ! [[ "${BACKEND_START_TIMEOUT}" =~ ^[1-9][0-9]*$ ]]; then
  echo "STRATA_BACKEND_START_TIMEOUT must be a positive number of seconds." >&2
  exit 1
fi

backend_is_ready() {
  curl --fail --silent --show-error --connect-timeout 1 --max-time 2 "${BACKEND_READY_URL}" >/dev/null 2>&1
}

backend_is_live() {
  curl --fail --silent --show-error --connect-timeout 1 --max-time 2 "${BACKEND_LIVE_URL}" >/dev/null 2>&1
}

wait_for_backend() {
  local backend_pid="${1:-}"
  local elapsed

  for (( elapsed = 0; elapsed < BACKEND_START_TIMEOUT; elapsed++ )); do
    if backend_is_ready; then
      return 0
    fi
    if [[ -n "${backend_pid}" ]] && ! kill -0 "${backend_pid}" 2>/dev/null; then
      return 1
    fi
    sleep 1
  done

  backend_is_ready
}

if backend_is_ready; then
  echo "Strata BE local is already ready at ${STRATA_BACKEND_LOCAL}." >&2
elif backend_is_live; then
  echo "Strata BE is listening at ${STRATA_BACKEND_LOCAL}; waiting for readiness." >&2
  if ! wait_for_backend; then
    echo "Strata BE did not become ready within ${BACKEND_START_TIMEOUT}s." >&2
    echo "Check database and task-queue configuration before launching Strata:" >&2
    echo "  cd ${BACKEND_DIR} && npm run db:up && npm run migrate:dev && npm run db:seed" >&2
    exit 1
  fi
  echo "Strata BE local is ready." >&2
else
  if [[ ! -d "${BACKEND_DIR}" ]]; then
    echo "Strata BE directory not found: ${BACKEND_DIR}" >&2
    echo "Set STRATA_BACKEND_DIR to the local strata-be checkout." >&2
    exit 1
  fi
  if [[ ! -f "${BACKEND_DIR}/.env" ]]; then
    echo "Missing local backend configuration: ${BACKEND_DIR}/.env" >&2
    echo "Configure strata-be first; the launcher never creates credentials automatically." >&2
    exit 1
  fi
  if [[ ! -d "${BACKEND_DIR}/node_modules" ]]; then
    echo "Backend dependencies are missing: ${BACKEND_DIR}/node_modules" >&2
    echo "Run: cd ${BACKEND_DIR} && npm ci" >&2
    exit 1
  fi

  mkdir -p "${BACKEND_STATE_DIR}"
  : > "${BACKEND_LOG}"
  echo "Starting Docker Postgres/Chroma for Strata BE..." >&2
  (
    cd "${BACKEND_DIR}"
    npm run db:up
  ) >> "${BACKEND_LOG}" 2>&1
  echo "Starting local Strata BE at ${STRATA_BACKEND_LOCAL} (log: ${BACKEND_LOG})" >&2
  (
    cd "${BACKEND_DIR}"
    exec npm run dev
  ) >> "${BACKEND_LOG}" 2>&1 &
  BACKEND_PID=$!
  printf '%s\n' "${BACKEND_PID}" > "${BACKEND_PID_FILE}"

  if ! wait_for_backend "${BACKEND_PID}"; then
    echo "Strata BE did not become ready within ${BACKEND_START_TIMEOUT}s; last log lines:" >&2
    tail -n 50 "${BACKEND_LOG}" >&2 || true
    exit 1
  fi
  echo "Strata BE local is ready (PID ${BACKEND_PID})." >&2
fi

exec "${ROOT}/scripts/_run-strata-app.sh" "$@"
