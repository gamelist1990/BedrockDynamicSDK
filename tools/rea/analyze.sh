#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "Usage: $0 <analyze|search|function|decompile|xrefs> <target.exe> [focus] [evidence-dir]" >&2
  exit 2
}

[[ $# -ge 2 ]] || usage
operation=$1
target=$2
focus=${3:-}
evidence_dir=${4:-"${HOME}/Documents/PEXData/REA"}
[[ -f "$target" ]] || { echo "Target executable not found: $target" >&2; exit 2; }
case "$operation" in analyze|search|function|decompile|xrefs) ;; *) usage ;; esac
if [[ "$operation" != analyze && -z "$focus" ]]; then usage; fi

mkdir -p "$evidence_dir"
target_name=$(basename "${target%.*}")
if command -v sha256sum >/dev/null 2>&1; then
  target_hash=$(sha256sum "$target" | cut -d ' ' -f 1 | cut -c 1-16)
else
  target_hash=$(shasum -a 256 "$target" | cut -d ' ' -f 1 | cut -c 1-16)
fi
target_identity="$target_name-$target_hash"
safe_focus=$(printf '%s' "${focus:-overview}" | sed 's/[^A-Za-z0-9._-]/_/g')
snapshot="$evidence_dir/$target_identity.rea-snapshot.json"
evidence="$evidence_dir/$target_identity-$operation-$safe_focus.json"
args=("$operation" "$target")
case "$operation" in
  search) args+=("$focus" --kind strings) ;;
  function|decompile|xrefs) args+=("$focus") ;;
esac
args+=(--provider ghidra --snapshot "$snapshot" --format json)

if command -v rea >/dev/null 2>&1; then
  rea "${args[@]}" > "$evidence"
else
  npx --yes rea-agents@latest "${args[@]}" > "$evidence"
fi
echo "Evidence: $evidence"
