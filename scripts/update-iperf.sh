#!/usr/bin/env bash
# Bump iperf-version.json to the newest upstream esnet/iperf release tag.
#
# Usage: scripts/update-iperf.sh [VERSION]
#   Without VERSION the newest "3.x[.y]" tag on github.com/esnet/iperf is used.
#
# Prints "updated=true|false" and "version=<ver>" and, when running in GitHub
# Actions, also writes them to $GITHUB_OUTPUT.
set -euo pipefail

cd "$(dirname "$0")/.."
pin=iperf-version.json
nix_flags=(--extra-experimental-features "nix-command flakes")

current=$(jq -r .version "$pin")

if [ $# -ge 1 ]; then
  latest=$1
else
  latest=$(git ls-remote --tags --refs https://github.com/esnet/iperf.git \
    | sed -n 's#.*refs/tags/\(3\.[0-9][0-9]*\(\.[0-9][0-9]*\)\{0,1\}\)$#\1#p' \
    | sort -V | tail -n1)
fi

if [ -z "$latest" ]; then
  echo "could not determine latest upstream iperf version" >&2
  exit 1
fi

emit() {
  echo "$1"
  if [ -n "${GITHUB_OUTPUT:-}" ]; then echo "$1" >>"$GITHUB_OUTPUT"; fi
}

emit "version=$latest"
emit "previous=$current"

if [ "$latest" = "$current" ]; then
  echo "iperf $current is already the newest version" >&2
  emit "updated=false"
  exit 0
fi

echo "updating iperf $current -> $latest" >&2
hash=$(nix "${nix_flags[@]}" flake prefetch --json "github:esnet/iperf/$latest" | jq -r .hash)

jq --arg v "$latest" --arg h "$hash" '.version = $v | .hash = $h' "$pin" >"$pin.tmp"
mv "$pin.tmp" "$pin"

emit "updated=true"
