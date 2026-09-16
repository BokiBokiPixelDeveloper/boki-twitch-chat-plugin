#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

die() { printf 'Error: %s\n' "$*" >&2; exit 1; }
usage() {
  cat <<'EOF'
Bokis Twitch Chat Plugin — Linux x86_64 installer

  bash install.sh              Install or replace the plugin for this user
  bash install.sh --check      Verify the package and runtime without installing
  bash install.sh --uninstall  Remove plugin files; preserve settings and backups
  bash install.sh --help       Show this help

Requires native OBS Studio 32.x and compatible system libraries. The release is
built on Arch Linux; it is not a universal Linux binary. Flatpak, Snap, portable
OBS and system-wide installations are not supported by this installer.
Run as your normal user, without sudo. Close OBS before installing/removing.
EOF
}

action=install
[[ ${0##*/} != uninstall.sh ]] || action=uninstall
if (( $# )); then
  [[ $# == 1 ]] || die 'Expected at most one option. Use --help.'
  case $1 in
    --help|-h) usage; exit 0 ;;
    --check) action=check ;;
    --uninstall) action=uninstall ;;
    *) die "Unknown option: $1. Use --help." ;;
  esac
fi
[[ $EUID != 0 ]] || die 'Run as your normal user, without sudo.'
[[ $(uname -s) == Linux && $(uname -m) == x86_64 ]] || die 'This package requires Linux x86_64.'
[[ ${HOME:-} == /* ]] || die 'HOME must be an absolute path.'
[[ ! -e /.flatpak-info && -z ${SNAP:-} ]] || die 'This installer supports native OBS only, not Flatpak or Snap.'
for tool in cat dirname flock install ln mkdir mv od pgrep rm sha256sum sort; do
  command -v "$tool" >/dev/null || die "Required command is missing: $tool"
done

plugin=bokis-twitch-chat-plugin
config=${XDG_CONFIG_HOME:-}
[[ $config == /* ]] || config=$HOME/.config
cache=${XDG_CACHE_HOME:-}
[[ $cache == /* ]] || cache=$HOME/.cache
[[ $config != */.var/app/* && $config != */snap/* ]] || die 'A sandboxed OBS configuration is not supported.'
parent=$config/obs-studio/plugins
destination=$parent/$plugin
pending=$cache/$plugin/pending
transaction=$destination/.installer-transaction
# Keep this allowlist small: uninstall must never recursively delete user files.
# Fonts are embedded in the module; only their license is an external resource.
payload=(
  bin/64bit/bokis-twitch-chat-updater
  bin/64bit/bokis-twitch-chat-plugin.so
  data/licenses/NotoColorEmoji-OFL.txt
  uninstall.sh
)

# Allow the user's XDG base to be a symlink, but never follow symlinks within the
# plugin or updater directories, including dangling links and lock files.
check_path() {
  local base=$1 relative=$2 part
  local -a parts
  IFS=/ read -r -a parts <<< "$relative"
  for part in "${parts[@]}"; do
    base+=/$part
    [[ ! -L $base ]] || die "Refusing symbolic link: $base"
  done
}
check_target() {
  local file
  check_path "$config" "obs-studio/plugins/$plugin"
  for file in "${payload[@]}"; do
    check_path "$destination" "$file"
    [[ ! -e $destination/$file || -f $destination/$file ]] || die "Expected a regular file: $destination/$file"
  done
  check_path "$destination" bin/64bit/bokis-twitch-chat-plugin.so.use.lock
  [[ ! -e $destination/bin/64bit/bokis-twitch-chat-plugin.so.use.lock ||
     -f $destination/bin/64bit/bokis-twitch-chat-plugin.so.use.lock ]] || die 'The plugin lock is not a regular file.'
  [[ ! -e $transaction && ! -L $transaction ]] ||
    die "An interrupted installation needs recovery: $transaction. See INSTALL.md before retrying."
}
require_obs_closed() {
  if pgrep -x obs >/dev/null; then
    die 'Close all OBS instances completely, then run the installer again.'
  else
    [[ $? == 1 ]] || die 'Could not check running OBS processes.'
  fi
}
verify_payload() {
  local directory=$1 manifest=$2 actual file
  [[ -f $manifest && ! -L $manifest ]] || die 'Package SHA256SUMS is missing or is a symbolic link.'
  for file in "${payload[@]}"; do
    check_path "$directory" "$file"
    [[ -f $directory/$file ]] || die "Package file is missing: $file"
  done
  # Comparing the complete allowlisted manifest also rejects missing entries,
  # extra paths and path traversal, without interpreting manifest paths.
  actual=$(cd "$directory" && sha256sum -- "${payload[@]}" | sort -k2)
  [[ $actual == "$(cat -- "$manifest")" ]] || die 'Package checksum verification failed. Download and extract the complete release again.'
}
require_elf() {
  local header
  header=$(od -An -tx1 -N20 -- "$1")
  header=${header//[[:space:]]/}
  [[ ${#header} == 40 && $header == 7f454c460201*3e00 ]] || die "Not a Linux x86_64 ELF binary: $1"
}
check_runtime() {
  local obs version file dependencies
  for tool in ldd timeout; do
    command -v "$tool" >/dev/null || die "Required command is missing: $tool"
  done
  obs=$(command -v obs) || die 'Native OBS Studio was not found in PATH. Flatpak/Snap OBS requires a separate package.'
  require_elf "$obs"
  version=$(timeout 10 "$obs" --version 2>&1) || die 'Could not determine the installed OBS version.'
  [[ $version =~ OBS\ Studio\ -\ ([0-9]+)\. ]] || die "Unrecognized OBS version: $version"
  [[ ${BASH_REMATCH[1]} == 32 ]] || die "This release supports OBS 32.x; found: $version"
  for file in bin/64bit/bokis-twitch-chat-plugin.so bin/64bit/bokis-twitch-chat-updater; do
    require_elf "$1/$file"
    if ! dependencies=$(ldd "$1/$file" 2>&1) || [[ $dependencies == *'not found'* ]]; then
      printf '%s\n' "$dependencies" >&2
      die 'Required runtime libraries are unavailable. Use a build matching your OBS and Linux distribution.'
    fi
  done
  printf 'Runtime check passed: %s\n' "$version"
}

check_target
if [[ $action != uninstall ]]; then
  package=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
  [[ -d $package/plugin && ! -L $package/plugin ]] || die 'Extract the complete release ZIP and run its install.sh.'
  verify_payload "$package/plugin" "$package/SHA256SUMS"
  check_runtime "$package/plugin"
fi
printf 'Plugin location: %s\n' "$destination"
if [[ $action == check ]]; then
  printf 'Package and runtime checks passed. No files were installed.\n'
  exit 0
fi
if [[ $action == uninstall && ! -d $destination ]]; then
  printf 'No user installation found.\n'
  exit 0
fi
require_obs_closed

# Share the updater's lock order. Never unlink either lock: all participants must
# continue locking the same inode across installs, updates and removals.
check_path "$cache" "$plugin/pending/update.lock"
[[ ! -e $pending/update.lock || -f $pending/update.lock ]] || die 'The updater lock is not a regular file.'
mkdir -p -- "$pending"
exec 8>>"$pending/update.lock"
flock -n -x 8 || die 'An update or another installer is active. Wait for it to finish.'
for state in pending.json transaction.json; do
  [[ ! -e $pending/$state && ! -L $pending/$state ]] ||
    die "An update is pending or needs recovery: $pending/$state. Finish that update before installing/removing."
done
mkdir -p -- "$destination/bin/64bit"
exec 9>>"$destination/bin/64bit/bokis-twitch-chat-plugin.so.use.lock"
flock -n -x 9 || die 'The plugin is in use. Close all OBS instances completely.'
check_target

committed=false
changed=()
cleanup() {
  local status=$? file i failed=false
  trap - EXIT INT TERM
  if [[ $committed == false ]]; then
    for ((i=${#changed[@]}-1; i>=0; i--)); do
      file=${changed[i]}
      if [[ -f $transaction/old/$file ]]; then
        # A failed rename can leave the original inode already in place.
        if [[ ! $transaction/old/$file -ef $destination/$file ]]; then
          # Retain every backup until ALL restorations succeed. A later rollback
          # failure must not make an already restored file look newly installed.
          if ! ln -fT -- "$transaction/old/$file" "$transaction/restore" ||
             ! mv -fT -- "$transaction/restore" "$destination/$file"; then
            failed=true
          fi
        fi
      else
        rm -f -- "$destination/$file" || failed=true
      fi
    done
  fi
  if [[ $failed == true ]]; then
    printf 'Rollback failed. Keep OBS closed; recovery files are in %s. See INSTALL.md.\n' "$transaction" >&2
    exit 1
  fi
  rm -rf -- "$transaction" || status=1
  exit "$status"
}
mkdir -- "$transaction"
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# Prepare all new files and rollback links before changing any installed file.
# The transaction lives beside the targets, so each replacement is a rename.
for file in "${payload[@]}"; do
  mkdir -p -- "$transaction/old/$(dirname -- "$file")"
  if [[ -f $destination/$file ]]; then
    ln -- "$destination/$file" "$transaction/old/$file"
  fi
  if [[ $action == install ]]; then
    mode=0755
    [[ $file != data/* ]] || mode=0644
    install -D -m "$mode" -- "$package/plugin/$file" "$transaction/new/$file"
    mkdir -p -- "$destination/$(dirname -- "$file")"
  fi
done
if [[ $action == install ]]; then
  verify_payload "$transaction/new" "$package/SHA256SUMS"
fi
require_obs_closed
for file in "${payload[@]}"; do
  # Record intent before each operation so signals and ordinary errors restore it.
  printf '%s\n' "$file" >> "$transaction/changed"
  changed+=("$file")
  if [[ $action == install ]]; then
    mv -fT -- "$transaction/new/$file" "$destination/$file"
  else
    rm -f -- "$destination/$file"
  fi
done
committed=true
if [[ $action == install ]]; then
  printf 'Installed Bokis Twitch Chat Plugin and its updater. You can now start OBS.\n'
  printf 'To uninstall: bash %q\n' "$destination/uninstall.sh"
else
  printf 'Plugin removed. OBS settings, credentials, backups and lock files were preserved.\n'
fi
