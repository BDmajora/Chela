#!/usr/bin/env bash
# Optional development-only fallback for Debian/Ubuntu systems that have the
# runtime libraries but no development headers. Nothing is installed into the
# operating system; packages are unpacked into the ignored .deps/ directory.
#
#   bash scripts/local-deps.sh            core + native Wayland backend
#   bash scripts/local-deps.sh --qt       also the optional Qt 6 backend
#   bash scripts/local-deps.sh --gtk      also the optional GTK 3 backend
set -euo pipefail
cd "$(dirname "$0")/.."
task_deb_dir=$(mktemp -d /tmp/chela-headers-XXXXXX)
trap 'rm -rf "$task_deb_dir"' EXIT
task_packages=(libjson-c-dev libcurl4-openssl-dev libsqlite3-dev
    libwayland-dev libwayland-bin wayland-protocols libxkbcommon-dev
    libcairo2-dev libpixman-1-dev libfreetype-dev libfontconfig-dev libpng-dev)
for task_arg in "$@"; do
    case $task_arg in
    --qt) task_packages+=(qt6-base-dev) ;;
    --gtk)
        task_packages+=(libgtk-3-dev libglib2.0-dev libpango1.0-dev libharfbuzz-dev
            libatk1.0-dev libgdk-pixbuf-2.0-dev libepoxy-dev)
        # Recent Ubuntu releases split the GLib headers into this package.
        if apt-cache show libgio-2.0-dev >/dev/null 2>&1; then
            task_packages+=(libgio-2.0-dev)
        fi
        ;;
    *) printf 'Unknown option: %s\n' "$task_arg" >&2; exit 2 ;;
    esac
done
(cd "$task_deb_dir" && apt-get download "${task_packages[@]}")
mkdir -p .deps
for task_deb in "$task_deb_dir"/*.deb; do
    dpkg-deb -x "$task_deb" .deps
done
printf 'Development files extracted into .deps/. Run ./build.sh to build Chela.\n'
