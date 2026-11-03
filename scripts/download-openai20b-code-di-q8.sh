#!/usr/bin/env bash
# Verified CODE-DI Q8_0 variant; this is distinct from CODE2-Plus.
set -euo pipefail

source "$HOME/inferssd/.venv/bin/activate"

neo_destination="${1:-$HOME/models/OpenAI-20B-NEO-CODE-DI-Q8_0}"
neo_filename='OpenAI-20B-NEO-CODE-DI-Uncensored-Q8_0.gguf'
neo_revision='92d1bcc0244193a2cd11b9da138dfee9e5203d37'
neo_checksum='b0c1153ecf96e1e2d9f253ea28e0bba3979365182f2142d38f52bc140f5af082'

mkdir -p -- "$neo_destination"

hf download DavidAU/OpenAi-GPT-oss-20b-abliterated-uncensored-NEO-Imatrix-gguf \
    "$neo_filename" \
    --revision "$neo_revision" \
    --local-dir "$neo_destination"

(
    cd -- "$neo_destination"
    printf '%s  %s\n' "$neo_checksum" "$neo_filename" | sha256sum --check
)

printf '\nDownloaded and verified: %s/%s\n' "$neo_destination" "$neo_filename"
