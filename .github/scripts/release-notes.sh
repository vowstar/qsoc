#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>
#
# Render release notes for a tag from the conventional commits it contains.
# Usage: release-notes.sh <tag> [previous-tag]

set -euo pipefail

tag="${1:?usage: release-notes.sh <tag> [previous-tag]}"
prev="${2:-$(git describe --tags --abbrev=0 "${tag}^" 2>/dev/null || true)}"
repo="${GITHUB_REPOSITORY:-vowstar/qsoc}"
range="${prev:+${prev}..}${tag}"

work=$(mktemp)
trap 'rm -f "$work"' EXIT

if [ -z "$prev" ]; then
  printf 'Initial release.\n\n'
  printf '**Full Changelog**: https://github.com/%s/commits/%s\n' "$repo" "$tag"
  exit 0
fi

git log --no-merges --reverse --format='%H%x1f%s' "$range" |
  while IFS=$'\x1f' read -r sha subject; do
    case "$subject" in
      "chore: bump version to "*) continue ;;
    esac

    head=${subject%%:*}
    text=${subject#*:}
    text=${text# }
    scope=""
    case "$head" in
      *"("*")"*)
        scope=${head#*(}
        scope=${scope%%)*}
        head=${head%%(*}
        ;;
    esac
    breaking=""
    case "$subject" in
      *"!:"*) breaking="yes" ;;
    esac

    if [ -n "$breaking" ]; then
      bucket=0
    else
      case "$head" in
        feat) bucket=1 ;;
        fix) bucket=2 ;;
        perf) bucket=3 ;;
        refactor) bucket=4 ;;
        docs) bucket=5 ;;
        test) bucket=6 ;;
        build | ci | chore | style) bucket=7 ;;
        *)
          bucket=8
          scope=""
          text=$subject
          ;;
      esac
    fi

    if [ -n "$scope" ]; then
      printf '%s\x1f- **%s**: %s (%s)\n' "$bucket" "$scope" "$text" "${sha:0:7}"
    else
      printf '%s\x1f- %s (%s)\n' "$bucket" "$text" "${sha:0:7}"
    fi
  done >"$work"

titles=(
  "Breaking Changes"
  "Features"
  "Bug Fixes"
  "Performance"
  "Refactoring"
  "Documentation"
  "Tests"
  "Build & Tooling"
  "Other Changes"
)

for i in "${!titles[@]}"; do
  body=$(awk -F'\x1f' -v b="$i" '$1 == b { print $2 }' "$work" | sort -f)
  [ -n "$body" ] || continue
  printf '### %s\n\n%s\n\n' "${titles[$i]}" "$body"
done

if [ -n "$prev" ]; then
  printf '**Full Changelog**: https://github.com/%s/compare/%s...%s\n' \
    "$repo" "$prev" "$tag"
else
  printf '**Full Changelog**: https://github.com/%s/commits/%s\n' "$repo" "$tag"
fi
