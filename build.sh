#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

run_make() {
    for tool in make pkg-config; do
        if ! command -v "$tool" >/dev/null 2>&1; then
            printf 'Missing build tool: %s. Install build-essential and pkg-config.\n' "$tool" >&2
            exit 1
        fi
    done

    local build_jobs=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '1\n')}
    if [[ ! "$build_jobs" =~ ^[1-9][0-9]*$ ]]; then
        printf 'JOBS must be a positive integer.\n' >&2
        exit 1
    fi
    make --no-print-directory --silent -C "$project_dir" -j "$build_jobs" "$@"
}

build_project() {
    printf '\nBuilding Chela...\n'
    run_make "$@" all
    printf 'Build complete. Use run.sh to launch Chela.\n'
}

clean_project() {
    printf '\nCleaning build output...\n'
    make --no-print-directory --silent -C "$project_dir" clean "$@"
    printf 'Clean complete.\n'
}

# Commands also work without the menu, including existing Make targets/overrides.
if (( $# > 0 )); then
    command_name=$1
    shift
    case "$command_name" in
        build) build_project "$@" ;;
        clean) clean_project "$@" ;;
        rebuild) clean_project; build_project "$@" ;;
        *) run_make "$command_name" "$@" ;;
    esac
    exit 0
fi

while true; do
    printf '\nChela build menu\n  1) Build\n  2) Clean build output\n  3) Clean and rebuild\n  0) Exit\n\n'
    if ! read -r -p 'Choose an option: ' choice; then
        printf '\n'
        break
    fi
    case "$choice" in
        1) build_project ;;
        2) clean_project ;;
        3) clean_project; build_project ;;
        0|q|Q) break ;;
        *) printf 'Please choose 1, 2, 3, or 0.\n' ;;
    esac
done
