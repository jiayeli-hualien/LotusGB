#!/bin/bash
# User-local installation from the generated LotusGB core bundle. macOS bash 3.2.
set -euo pipefail

fail() { printf 'LotusGB: %s\n' "$*" >&2; exit 1; }
usage() {
    printf '%s\n' 'Usage: bash "Install LotusGB.command" [--data-dir DIR] [--config FILE] [--core-dir DIR] [--info-dir DIR]'
}

bundle_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
data_dir="${HOME}/Library/Application Support/RetroArch"
config_arg=""; core_arg=""; info_arg=""
while [ "$#" -gt 0 ]; do
    case "$1" in
        --help|-h) usage; exit 0 ;;
        --data-dir|--config|--core-dir|--info-dir)
            [ "$#" -ge 2 ] || fail "Missing value for $1"
            case "$1" in
                --data-dir) data_dir="$2" ;;
                --config) config_arg="$2" ;;
                --core-dir) core_arg="$2" ;;
                --info-dir) info_arg="$2" ;;
            esac
            shift 2 ;;
        *) usage >&2; fail "Unknown option: $1" ;;
    esac
done
[ "$(uname -s)" = Darwin ] || fail 'This installer requires macOS.'
case "$(uname -m)" in arm64|x86_64) ;; *) fail 'Unsupported Mac architecture.' ;; esac
os_version="$(/usr/bin/sw_vers -productVersion)"
[ "${os_version%%.*}" -ge 11 ] || fail 'This Universal core targets macOS 11 or newer.'
[ "$EUID" -ne 0 ] || fail 'Run as your normal user; sudo is not needed.'
[ -d "$data_dir" ] || fail "RetroArch data directory does not exist: $data_dir. Open RetroArch once, or use --data-dir."
[ -f "$bundle_dir/SHA256SUMS" ] || fail 'Incomplete bundle: SHA256SUMS is missing.'
(cd "$bundle_dir" && /usr/bin/shasum -a 256 -c SHA256SUMS) || fail 'Bundle checksum verification failed; no core files were installed.'

config_path="${config_arg:-${data_dir}/config/retroarch.cfg}"
if [ -n "$config_arg" ] && [ ! -f "$config_path" ]; then
    fail "Config file does not exist: $config_path"
fi
read_setting() {
    [ -f "$config_path" ] || return 0 # A new frontend may not have saved a config yet.
    /usr/bin/awk -v key="$1" '
        $0 ~ "^[[:space:]]*" key "[[:space:]]*=" {
            value=$0
            sub("^[[:space:]]*" key "[[:space:]]*=[[:space:]]*", "", value)
            if (value ~ /^"/) {
                sub(/^"/, "", value)
                sub(/"[[:space:]]*(#.*)?$/, "", value)
                result=value
            }
        }
        END { print result }
    ' "$config_path"
}
resolve_path() {
    case "$1" in
        '~/'*) printf '%s/%s\n' "$HOME" "${1#\~/}" ;;
        /*) printf '%s\n' "$1" ;;
        *) fail "Expected an absolute or ~/ path, got: $1" ;;
    esac
}
configured_core="$(read_setting libretro_directory)"
configured_info="$(read_setting libretro_info_path)"
case "$configured_core" in ''|default) configured_core="$data_dir/cores" ;; esac
case "$configured_info" in ''|default) configured_info="$data_dir/info" ;; esac
core_dir="$(resolve_path "${core_arg:-$configured_core}")"
info_dir="$(resolve_path "${info_arg:-$configured_info}")"
core_name=lotusgb_libretro.dylib
info_name=lotusgb_libretro.info
[ -f "$bundle_dir/core/$core_name" ] && [ -f "$bundle_dir/core/$info_name" ] || fail 'Incomplete core payload.'

mkdir -p "$core_dir" "$info_dir"
backup_dir="$data_dir/backups/LotusGB/$(date -u +%Y%m%dT%H%M%SZ)-$$"
mkdir -p "$backup_dir"
core_tmp=""; info_tmp=""; core_installed=0; info_installed=0; had_core=0; had_info=0
cleanup() {
    status=$?
    trap - EXIT
    if [ "$status" -ne 0 ]; then
        printf 'Installation failed; restoring any replaced files. Backup: %s\n' "$backup_dir" >&2
        if [ "$core_installed" -eq 1 ]; then
            if [ "$had_core" -eq 1 ]; then
                if ! cp -p "$backup_dir/$core_name" "$core_dir/$core_name"; then
                    printf 'Could not restore the previous core; use the backup above.\n' >&2
                fi
            elif ! rm -f "$core_dir/$core_name"; then
                printf 'Could not remove the new core after failure.\n' >&2
            fi
        fi
        if [ "$info_installed" -eq 1 ]; then
            if [ "$had_info" -eq 1 ]; then
                if ! cp -p "$backup_dir/$info_name" "$info_dir/$info_name"; then
                    printf 'Could not restore the previous .info; use the backup above.\n' >&2
                fi
            elif ! rm -f "$info_dir/$info_name"; then
                printf 'Could not remove the new .info after failure.\n' >&2
            fi
        fi
    fi
    if [ -n "$core_tmp" ] && ! rm -f "$core_tmp"; then status=1; fi
    if [ -n "$info_tmp" ] && ! rm -f "$info_tmp"; then status=1; fi
    exit "$status"
}
trap cleanup EXIT
if [ -e "$core_dir/$core_name" ]; then
    [ -f "$core_dir/$core_name" ] || fail "Core destination is not a regular file: $core_dir/$core_name"
    cp -p "$core_dir/$core_name" "$backup_dir/$core_name"; had_core=1
fi
if [ -e "$info_dir/$info_name" ]; then
    [ -f "$info_dir/$info_name" ] || fail "Info destination is not a regular file: $info_dir/$info_name"
    cp -p "$info_dir/$info_name" "$backup_dir/$info_name"; had_info=1
fi
printf 'core_directory=%s\ninfo_directory=%s\n' "$core_dir" "$info_dir" > "$backup_dir/paths.txt"
core_tmp="$(mktemp "$core_dir/.lotusgb-install.XXXXXXXX")"
info_tmp="$(mktemp "$info_dir/.lotusgb-install.XXXXXXXX")"
cp "$bundle_dir/core/$core_name" "$core_tmp"
cp "$bundle_dir/core/$info_name" "$info_tmp"
chmod 755 "$core_tmp"; chmod 644 "$info_tmp"
# Request cache refresh before replacing either file, so an unwritable info
# directory fails before any core replacement. RetroArch consumes this marker.
touch "$info_dir/core_info.refresh"
mv -f "$core_tmp" "$core_dir/$core_name"; core_installed=1
mv -f "$info_tmp" "$info_dir/$info_name"; info_installed=1
cmp -s "$bundle_dir/core/$core_name" "$core_dir/$core_name" || fail 'Installed core verification failed.'
cmp -s "$bundle_dir/core/$info_name" "$info_dir/$info_name" || fail 'Installed info verification failed.'
printf '\n%s\n' 'LotusGB 安裝完成 / Installation complete (.gb + .gbc).'
printf 'Core: %s/%s\nInfo: %s/%s\nBackup: %s\n' "$core_dir" "$core_name" "$info_dir" "$info_name" "$backup_dir"
printf '%s\n' '請完全結束並重新開啟 RetroArch，再選擇 LotusGB。此視窗可關閉。' 'Quit and reopen RetroArch to load the new core and refreshed info.'
