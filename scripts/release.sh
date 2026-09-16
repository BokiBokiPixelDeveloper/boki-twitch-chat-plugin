#!/usr/bin/env bash
set -euo pipefail

die() {
  printf 'Error: %s\n' "$*" >&2
  exit 1
}

[[ $# -eq 1 ]] || die "Usage: $0 <version>"
version=$1
# SemVer without build metadata; numeric identifiers must not have leading zeros.
number='(0|[1-9][0-9]*)'
identifier='(0|[1-9][0-9]*|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*)'
[[ $version =~ ^$number\.$number\.$number(-$identifier(\.$identifier)*)?$ ]] ||
  die "Invalid version: $version (example: 0.1.0-alpha.10)"
tag="v$version"

root=$(git rev-parse --show-toplevel) || die 'Not a Git repository.'
cd "$root"
[[ $(git branch --show-current) == main ]] || die 'Releases are only allowed from main.'
require_clean() {
  local changes
  changes=$(git status --porcelain)
  if [[ -n $changes ]]; then
    printf '%s\n' "$changes" >&2
    die 'The working tree must be clean, including untracked files.'
  fi
}
require_clean
git pull --ff-only
require_clean

if git show-ref --verify --quiet "refs/tags/$tag"; then
  die "Tag $tag already exists locally."
fi
# Do not interpret a failed remote query as an absent tag.
remote_tag=$(git ls-remote --tags origin "refs/tags/$tag")
[[ -z $remote_tag ]] || die "Tag $tag already exists on origin."
git ls-files --error-unmatch -- VERSION buildspec.json >/dev/null
[[ -f VERSION && ! -L VERSION && -f buildspec.json && ! -L buildspec.json ]] ||
  die 'VERSION and buildspec.json must be regular files.'

backup=$(mktemp -d)
restore=false
committed=false
cleanup() {
  local status=$?
  trap - EXIT
  if [[ $restore == true && $committed == false ]]; then
    # The index was clean before starting; undo our staging as well.
    git restore --staged -- VERSION buildspec.json || status=1
    cp -- "$backup/VERSION" VERSION || status=1
    cp -- "$backup/buildspec.json" buildspec.json || status=1
  fi
  rm -rf -- "$backup"
  if (( status != 0 )) && [[ $committed == true ]]; then
    printf 'Stopped after the commit. Check the commit, push, and tag state manually; nothing was reset.\n' >&2
  fi
  exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
cp -- VERSION "$backup/VERSION"
cp -- buildspec.json "$backup/buildspec.json"

# Match only the project's two-space-indented version field, never nested
# dependency fields. Fail closed if the expected layout is missing/ambiguous.
awk -v version="$version" '
  /^  "version"[[:space:]]*:/ {
    count++
    if ($0 !~ /^  "version"[[:space:]]*:[[:space:]]*"[^"\\]*",?[[:space:]]*$/) exit 1
    sub(/:[[:space:]]*"[^"\\]*"/, ": \"" version "\"")
  }
  { print }
  END { if (count != 1) exit 1 }
' buildspec.json > "$backup/buildspec.new" ||
  die 'The project version in buildspec.json was not found unambiguously in the expected format.'

restore=true
printf '%s\n' "$version" > VERSION
cat "$backup/buildspec.new" > buildspec.json
git --no-pager diff -- VERSION buildspec.json
if git diff --quiet -- VERSION buildspec.json; then
  die 'The version files already contain this version.'
fi
printf 'Create release %s? [y/N] ' "$tag"
answer=''
if ! IFS= read -r answer || [[ $answer != y && $answer != Y ]]; then
  printf '\nRelease canceled; restoring version files.\n'
  exit 1
fi

git add -- VERSION buildspec.json
git commit -m "chore: bump version to $version"
committed=true
git push
git tag "$tag"
git push origin "$tag"
printf '\nRelease %s was pushed.\n' "$tag"
printf 'GitHub Actions should now handle the build, tests, packaging, and publication.\n'
