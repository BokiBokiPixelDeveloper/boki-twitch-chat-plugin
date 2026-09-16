#!/usr/bin/env bash
set -euo pipefail

die() {
  printf 'Fehler: %s\n' "$*" >&2
  exit 1
}

[[ $# -eq 1 ]] || die "Aufruf: $0 <version>"
version=$1
# SemVer without build metadata; numeric identifiers must not have leading zeros.
number='(0|[1-9][0-9]*)'
identifier='(0|[1-9][0-9]*|[0-9A-Za-z-]*[A-Za-z-][0-9A-Za-z-]*)'
[[ $version =~ ^$number\.$number\.$number(-$identifier(\.$identifier)*)?$ ]] ||
  die "Ungültige Version: $version (Beispiel: 0.1.0-alpha.10)"
tag="v$version"

root=$(git rev-parse --show-toplevel) || die 'Kein Git-Repository.'
cd "$root"
[[ $(git branch --show-current) == main ]] || die 'Releases sind nur von main erlaubt.'
require_clean() {
  local changes
  changes=$(git status --porcelain)
  if [[ -n $changes ]]; then
    printf '%s\n' "$changes" >&2
    die 'Der Working Tree muss sauber sein (auch keine untracked Dateien).'
  fi
}
require_clean
git pull --ff-only
require_clean

if git show-ref --verify --quiet "refs/tags/$tag"; then
  die "Tag $tag existiert bereits lokal."
fi
# Do not interpret a failed remote query as an absent tag.
remote_tag=$(git ls-remote --tags origin "refs/tags/$tag")
[[ -z $remote_tag ]] || die "Tag $tag existiert bereits auf origin."
git ls-files --error-unmatch -- VERSION buildspec.json >/dev/null
[[ -f VERSION && ! -L VERSION && -f buildspec.json && ! -L buildspec.json ]] ||
  die 'VERSION und buildspec.json müssen reguläre Dateien sein.'

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
    printf 'Abbruch nach dem Commit. Commit/Push/Tag-Stand bitte manuell prüfen; nichts wurde zurückgesetzt.\n' >&2
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
  die 'Projektversion in buildspec.json nicht eindeutig im erwarteten Format gefunden.'

restore=true
printf '%s\n' "$version" > VERSION
cat "$backup/buildspec.new" > buildspec.json
git --no-pager diff -- VERSION buildspec.json
if git diff --quiet -- VERSION buildspec.json; then
  die 'Die Versionsdateien enthalten bereits diese Version.'
fi
printf 'Release %s erstellen? [y/N] ' "$tag"
answer=''
if ! IFS= read -r answer || [[ $answer != y && $answer != Y ]]; then
  printf '\nRelease abgebrochen; Versionsdateien werden wiederhergestellt.\n'
  exit 1
fi

git add -- VERSION buildspec.json
git commit -m "chore: bump version to $version"
committed=true
git push
git tag "$tag"
git push origin "$tag"
printf '\nRelease %s wurde gepusht.\n' "$tag"
printf 'GitHub Actions sollte jetzt Build, Tests, Packaging und Veröffentlichung übernehmen.\n'
