#!/usr/bin/env bash
# Read-only check of the AMD contest's published frozen-base requirement.
set -euo pipefail
ROOT=$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)
BASE=634888d9b037153fa143d2c913ba4b25503e1d62
REV=${1:-HEAD}
if ! git -C "$ROOT" cat-file -e "$BASE^{commit}" 2>/dev/null; then
  echo "Missing official AMD V5 object: $BASE" >&2
  echo "Fetch https://github.com/zihaomu/seekdb branch amd-oceanbase-official-contest-base-v5 first." >&2
  exit 2
fi
SHA=$(git -C "$ROOT" rev-parse --verify "$REV^{commit}")
if git -C "$ROOT" merge-base --is-ancestor "$BASE" "$SHA"; then
  printf 'AMD_V5_BASE=%s\nCOMMIT=%s\nANCESTRY=PASS\n' "$BASE" "$SHA"
else
  status=$?
  echo "AMD V5 ancestry check failed (exit=$status): $SHA" >&2
  exit "$status"
fi
