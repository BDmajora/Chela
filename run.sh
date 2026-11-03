#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
build_dir=${BUILD:-build}
if [[ "$build_dir" != /* ]]; then
    build_dir="$project_dir/$build_dir"
fi

if [[ ! -x "$build_dir/chela" ]]; then
    "$project_dir/build.sh" build
fi

exec "$build_dir/chela" "$@"
