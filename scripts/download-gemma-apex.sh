#!/usr/bin/env bash
# Download the publisher's I-Quality profile for Brody's inference computer.
set -euo pipefail

source "$HOME/inferssd/.venv/bin/activate"

gemma_destination="${1:-$HOME/models/gemma-4-26B-A4B-heretic-APEX}"
gemma_filename='gemma-4-26B-A4B-heretic-APEX-I-Quality.gguf'
gemma_revision='8e3506b8b11a8ac2d666e0df473fa7ba4ec21497'
gemma_checksum='2def9529490558630809bdb1c9f3b82c6b8567409c850bbf4a3eeb5c35986de6'

mkdir -p -- "$gemma_destination"

hf download mudler/gemma-4-26B-A4B-it-heretic-APEX-GGUF \
    "$gemma_filename" \
    --revision "$gemma_revision" \
    --local-dir "$gemma_destination"

(
    cd -- "$gemma_destination"
    printf '%s  %s\n' "$gemma_checksum" "$gemma_filename" | sha256sum --check
)

printf '\nDownloaded and verified: %s/%s\n' "$gemma_destination" "$gemma_filename"
