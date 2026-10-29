#!/usr/bin/env bash
# Shared Strata backend origins for the two desktop launch scripts.
#
# Override at runtime, e.g.:
#   STRATA_BACKEND_LOCAL=http://localhost:3002 ./scripts/run-strata-dev.sh
#   STRATA_BACKEND_PROD=https://my-service-xyz.a.run.app ./scripts/run-strata-prod.sh
#
# The mode script constructs STRATA_PLAN_ENDPOINT itself. This prevents a stale
# endpoint inherited from the shell from selecting the wrong environment.

STRATA_BACKEND_LOCAL="${STRATA_BACKEND_LOCAL:-http://localhost:3001}"
# Stable production origin, served by the Raspberry deployment.
STRATA_BACKEND_PROD="${STRATA_BACKEND_PROD:-https://api.getstrata.org}"
