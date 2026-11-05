#!/usr/bin/env bash
# Download the publisher's I-Quality profile for Brody's inference computer.
set -euo pipefail

venv="$HOME/.local/share/chela/download-venv"
[ -x "$venv/bin/hf" ] || { python3 -m venv "$venv" && "$venv/bin/pip" install huggingface_hub; }
source "$venv/bin/activate"

gemma_destination="${1:-$HOME/models/gemma-4-26B-A4B-heretic-APEX}"
gemma_filename='gemma-4-26B-A4B-heretic-APEX-I-Quality.gguf'
gemma_revision='8e3506b8b11a8ac2d666e0df473fa7ba4ec21497'
gemma_checksum='2def9529490558630809bdb1c9f3b82c6b8567409c850bbf4a3eeb5c35986de6'
# The vision encoder: without it the server refuses images.
vision_filename='mmproj.gguf'
vision_checksum='fc2ebf4c44528daa2cea7b39891712847ca5e4f87dcf578054a06c46bfe6da27'

mkdir -p -- "$gemma_destination"

hf download mudler/gemma-4-26B-A4B-it-heretic-APEX-GGUF \
    "$gemma_filename" "$vision_filename" \
    --revision "$gemma_revision" \
    --local-dir "$gemma_destination"

(
    cd -- "$gemma_destination"
    printf '%s  %s\n' "$gemma_checksum" "$gemma_filename" "$vision_checksum" "$vision_filename" | sha256sum --check
)

printf '\nDownloaded and verified: %s/{%s,%s}\n' "$gemma_destination" "$gemma_filename" "$vision_filename"
